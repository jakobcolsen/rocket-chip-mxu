/*
 * gemm_csr.c — MIMD GEMM with Row-Parallel CSR Result Forwarding
 *
 * C[N][N] += A[N][N] * B[N][N]   (single-precision floating-point)
 *
 * Each core independently computes its assigned output rows (same as
 * gemm_mimd). Additionally, west-column cores (0,2) forward their
 * computed C values to east-column cores (1,3) via CSR 0x801.
 *
 * This demonstrates the CSR mesh data path for inter-core result
 * communication in a practical GEMM context.
 *
 * Row partitioning (same as gemm_mimd):
 *   Core 0: rows [0, DIM/4)       Core 1: rows [DIM/4, DIM/2)
 *   Core 2: rows [DIM/2, 3*DIM/4) Core 3: rows [3*DIM/4, DIM)
 *
 * For small matrices (DIM < 4), only core 0 computes.
 *
 * Compile: -DBENCH_N=<dim>
 */
#include "common.h"

#ifndef BENCH_N
#define BENCH_N 2
#endif

#define DIM BENCH_N

/* ── Matrices (row-major, FP32) ───────────────────────────────────── */
volatile float A[DIM * DIM] __attribute__((aligned(64)));
volatile float B[DIM * DIM] __attribute__((aligned(64)));
volatile float C[DIM * DIM] __attribute__((aligned(64)));
volatile float C_ref[DIM * DIM] __attribute__((aligned(64)));

/* IEEE 754 helper */
static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } x; x.f = f; return x.u;
}

/* Fork-join control */
volatile int go       = 0;
volatile int done_cnt = 0;

static void __attribute__((noinline)) gemm_rows_csr(int row_start, int row_end) {
    for (int i = row_start; i < row_end; i++) {
        for (int j = 0; j < DIM; j++) {
            float sum = 0.0f;
            for (int k = 0; k < DIM; k++) {
                sum += A[i * DIM + k] * B[k * DIM + j];
            }
            C[i * DIM + j] = sum;
            /* Forward result to east neighbor via CSR */
            write_csr(0x801, f2u(sum));
        }
    }
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    bench_disable_interrupts();

    if (hartid == 0) {
        /* Initialize matrices */
        for (int i = 0; i < DIM; i++) {
            for (int j = 0; j < DIM; j++) {
                A[i * DIM + j] = (float)(i * DIM + j + 1);
                B[i * DIM + j] = (float)(i * DIM + j + 1) * 0.5f;
                C[i * DIM + j] = 0.0f;
                C_ref[i * DIM + j] = 0.0f;
            }
        }

        /* Compute reference */
        for (int i = 0; i < DIM; i++) {
            for (int j = 0; j < DIM; j++) {
                float sum = 0.0f;
                for (int k = 0; k < DIM; k++) {
                    sum += A[i * DIM + k] * B[k * DIM + j];
                }
                C_ref[i * DIM + j] = sum;
            }
        }
        asm volatile ("fence rw, rw" ::: "memory");

        printf("BENCHMARK: gemm_csr\n");
        printf("N: %d\n", DIM);

        int rows_per_core = DIM / NUM_CORES;
        if (rows_per_core < 1) rows_per_core = DIM;

        BENCH_START();

        if (DIM >= NUM_CORES) {
            go = 1;
            asm volatile ("fence rw, rw" ::: "memory");
        }

        gemm_rows_csr(0, rows_per_core);

        if (DIM >= NUM_CORES) {
            mimd_barrier_wait(&done_cnt, NUM_CORES - 1);
        }

        BENCH_END();
        BENCH_REPORT();

        /* Verify */
        int passed = 1;
        printf("Results:\n");
        for (int i = 0; i < DIM; i++) {
            for (int j = 0; j < DIM; j++) {
                float got = C[i * DIM + j];
                float exp = C_ref[i * DIM + j];
                float diff = got - exp;
                if (diff < 0) diff = -diff;
                printf("  C[%d][%d] = 0x%lx (exp 0x%lx)\n",
                       i, j, (unsigned long)f2u(got), (unsigned long)f2u(exp));
                if (diff > 0.01f) passed = 0;
            }
        }

        if (passed) printf("*** PASSED ***\n");
        else        printf("*** FAILED ***\n");

    } else {
        while (go == 0) { asm volatile ("nop"); }
        asm volatile ("fence rw, rw" ::: "memory");

        int rows_per_core = DIM / NUM_CORES;
        if (rows_per_core < 1) {
            /* Not enough rows — park */
        } else {
            int row_start = hartid * rows_per_core;
            int row_end   = row_start + rows_per_core;
            if (row_end > DIM) row_end = DIM;
            gemm_rows_csr(row_start, row_end);
        }

        asm volatile ("fence rw, rw" ::: "memory");
        __sync_fetch_and_add(&done_cnt, 1);
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
