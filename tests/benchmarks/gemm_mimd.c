/*
 * gemm_mimd.c — MIMD Fork-Join GEMM Benchmark
 *
 * C[M][N] += A[M][K] * B[K][N]   (single-precision floating-point)
 *
 * Standard parallel GEMM: each core computes a subset of output rows.
 * No mesh data paths are used — this is pure shared-memory MIMD.
 *
 * Row partitioning:
 *   Core 0: rows [0, DIM/4)
 *   Core 1: rows [DIM/4, DIM/2)
 *   Core 2: rows [DIM/2, 3*DIM/4)
 *   Core 3: rows [3*DIM/4, DIM)
 *
 * For small matrices (DIM < 4), only core 0 computes.
 *
 * Compile: pass -DBENCH_N=<dim> for matrix dimension (default 2).
 */
#include "common.h"

#ifndef BENCH_N
#undef BENCH_N
#define BENCH_N 2
#endif

#define DIM BENCH_N

/* ── Matrices (row-major) ─────────────────────────────────────────── */
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

static void __attribute__((noinline)) gemm_rows(int row_start, int row_end) {
    for (int i = row_start; i < row_end; i++) {
        for (int j = 0; j < DIM; j++) {
            float sum = 0.0f;
            for (int k = 0; k < DIM; k++) {
                sum += A[i * DIM + k] * B[k * DIM + j];
            }
            C[i * DIM + j] = sum;
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

        printf("BENCHMARK: gemm_mimd\n");
        printf("N: %d\n", DIM);

        /* Determine row partitioning */
        int rows_per_core = DIM / NUM_CORES;
        if (rows_per_core < 1) rows_per_core = DIM;  /* small matrix: single core */

        BENCH_START();

        /* Wake followers (only if enough rows to parallelize) */
        if (DIM >= NUM_CORES) {
            go = 1;
            asm volatile ("fence rw, rw" ::: "memory");
        }

        /* Leader computes its rows */
        gemm_rows(0, rows_per_core);

        /* Wait for followers */
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
                if (diff > 0.01f) {
                    passed = 0;
                }
            }
        }

        if (passed) {
            printf("*** PASSED ***\n");
        } else {
            printf("*** FAILED ***\n");
        }

    } else {
        /* Follower: wait for go flag */
        while (go == 0) { asm volatile ("nop"); }
        asm volatile ("fence rw, rw" ::: "memory");

        /* Compute my rows */
        int rows_per_core = DIM / NUM_CORES;
        if (rows_per_core < 1) {
            /* Not enough rows — park immediately */
        } else {
            int row_start = hartid * rows_per_core;
            int row_end   = row_start + rows_per_core;
            if (row_end > DIM) row_end = DIM;
            gemm_rows(row_start, row_end);
        }

        asm volatile ("fence rw, rw" ::: "memory");
        __sync_fetch_and_add(&done_cnt, 1);

        /* Park */
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
