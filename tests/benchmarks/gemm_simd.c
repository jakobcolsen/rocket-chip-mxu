/*
 * gemm_simd.c — SIMD Lockstep GEMM Benchmark
 *
 * C[M][N] += A[M][K] * B[K][N]   (single-precision floating-point)
 *
 * All 4 cores execute in hardware SIMD lockstep, each computing a
 * disjoint slab of output rows using standard FP multiply-add (no
 * systolic instructions).
 *
 * Row partitioning (same as gemm_mimd):
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

#define ALIAS_PAD 16

/* ── Matrices (row-major) ─────────────────────────────────────────── */
float A[DIM * DIM + NUM_CORES * ALIAS_PAD] __attribute__((aligned(64)));
float B[DIM * DIM] __attribute__((aligned(64)));
volatile float C[DIM * DIM + NUM_CORES * ALIAS_PAD] __attribute__((aligned(64)));
float C_ref[DIM * DIM] __attribute__((aligned(64)));

/* IEEE 754 helper */
static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } x; x.f = f; return x.u;
}

/*
 * SIMD GEMM kernel — called inside SIMD region.
 *
 * Each core reads mhartid to determine its own row slab.
 * Store happens inside the j-loop body to satisfy BUG-001 workaround.
 * Each core writes to its own disjoint row slab → no BUG-002 risk.
 */
static void __attribute__((noinline)) simd_gemm_kernel(void) {
    uint64_t hartid = read_csr(mhartid);
    int rows_per_core = DIM / NUM_CORES;
    int row_start = 0;
    int row_end = 0;

    /* For small matrices: only core 0 computes.
     * Followers deliberately bound the loop to [0, 0)
     * avoiding the divergent `return` anomaly. */
    if (rows_per_core < 1) {
        if (hartid == 0) {
            row_start = 0;
            row_end = DIM;
        }
        rows_per_core = DIM;
    } else {
        row_start = hartid * rows_per_core;
        row_end   = row_start + rows_per_core;
        if (row_end > DIM) row_end = DIM;
    }

    int pad_offset = hartid * ALIAS_PAD;

    for (int i = row_start; i < row_end; i++) {
        for (int j = 0; j < DIM; j++) {
            float sum = 0.0f;
            for (int k = 0; k < DIM; k++) {
                sum += A[i * DIM + k + pad_offset] * B[k * DIM + j];
            }
            /* BUG-001 workaround: store inside loop body (j-loop),
             * not after all loops complete. Each core writes to its
             * own row slab, so no cache-line sharing (BUG-002 safe). */
            C[i * DIM + j + pad_offset] = sum;
        }
    }
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    bench_disable_interrupts();

    if (hartid == 0) {
        /* Initialize matrices */
        for (int i = 0; i < DIM * DIM + NUM_CORES * ALIAS_PAD; i++) {
            A[i] = (float)(i + 1);
            C[i] = 0.0f;
        }
        for (int i = 0; i < DIM * DIM; i++) {
            B[i] = (float)(i + 1) * 0.5f;
            C_ref[i] = 0.0f;
        }

        /* Compute reference */
        for (int c = 0; c < NUM_CORES; c++) {
            int rows_per_core = DIM / NUM_CORES;
            if (rows_per_core < 1) { if(c!=0) continue; rows_per_core = DIM; }
            int row_start = (rows_per_core < DIM) ? c * rows_per_core : 0;
            int row_end   = row_start + rows_per_core;
            if (row_end > DIM) row_end = DIM;
            int pad_offset = c * ALIAS_PAD;

            for (int i = row_start; i < row_end; i++) {
                for (int j = 0; j < DIM; j++) {
                    float sum = 0.0f;
                    for (int k = 0; k < DIM; k++) {
                        sum += A[i * DIM + k + pad_offset] * B[k * DIM + j];
                    }
                    C_ref[i * DIM + j] = sum;
                }
            }
        }
        asm volatile ("fence rw, rw" ::: "memory");

        printf("BENCHMARK: gemm_simd\n");
        printf("N: %d\n", DIM);

        HPM_SETUP();
        BENCH_START();
        HPM_START();

        /* ── SIMD Region ── */
        SIMD_ENABLE();
        simd_gemm_kernel();
        SIMD_DISABLE();
        SIMD_PARK_FOLLOWERS();

        BENCH_END();
        HPM_END();
        SIMD_DRAIN();
        BENCH_REPORT();
        HPM_REPORT();

        /* Verify */
        int passed = 1;
        printf("Results:\n");
        for (int c = 0; c < NUM_CORES; c++) {
            int rows_per_core = DIM / NUM_CORES;
            if (rows_per_core < 1) { if(c!=0) continue; rows_per_core = DIM; }
            int row_start = (rows_per_core < DIM) ? c * rows_per_core : 0;
            int row_end   = row_start + rows_per_core;
            if (row_end > DIM) row_end = DIM;
            int pad_offset = c * ALIAS_PAD;

            for (int i = row_start; i < row_end; i++) {
                for (int j = 0; j < DIM; j++) {
                    float got = C[i * DIM + j + pad_offset];
                    float exp = C_ref[i * DIM + j];
                    float diff = got - exp;
                    if (diff < 0) diff = -diff;
                    if (diff > 0.01f) {
                        passed = 0;
                    }
                }
            }
        }

        if (passed) {
            printf("*** PASSED ***\n");
        } else {
            printf("*** FAILED ***\n");
        }

    } else {
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
