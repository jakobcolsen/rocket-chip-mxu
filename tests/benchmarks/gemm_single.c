/*
 * gemm_single.c — Single-Core Sequential GEMM Baseline
 *
 * Runs ONLY on Core 0. No SIMD, no MIMD, no mesh.
 * Provides the Amdahl's-law denominator:
 *   speedup = single_cycles / mode_cycles
 *
 * Same CFLAGS, matrix init, and verification as other modes.
 */
#include "common.h"

#ifndef BENCH_N
#undef BENCH_N
#define BENCH_N 2
#endif

#define DIM BENCH_N

float A[DIM * DIM] __attribute__((aligned(64)));
float B[DIM * DIM] __attribute__((aligned(64)));
float C[DIM * DIM] __attribute__((aligned(64)));
float C_ref[DIM * DIM] __attribute__((aligned(64)));

static void __attribute__((noinline)) gemm_sequential(void) {
    for (int i = 0; i < DIM; i++) {
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
        /* Initialize */
        for (int i = 0; i < DIM * DIM; i++) {
            A[i] = (float)(i + 1);
            B[i] = (float)(i + 1) * 0.5f;
            C[i] = 0.0f;
        }

        /* Reference */
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

        printf("BENCHMARK: gemm_single\n");
        printf("N: %d\n", DIM);

        HPM_SETUP();
        BENCH_START();
        HPM_START();

        gemm_sequential();

        BENCH_END();
        HPM_END();
        BENCH_REPORT();
        HPM_REPORT();

        /* Verify */
        int passed = 1;
        for (int i = 0; i < DIM * DIM; i++) {
            float diff = C[i] - C_ref[i];
            if (diff < 0) diff = -diff;
            if (diff > 0.01f && passed) {
                passed = 0;
                printf("  FAIL C[%d]=%f exp=%f\n", i, C[i], C_ref[i]);
            }
        }
        if (passed) printf("*** PASSED ***\n");
        else        printf("*** FAILED ***\n");

    } else {
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
