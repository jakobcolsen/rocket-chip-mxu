/*
 * gemm_tiled_single.c — Single-Core Register-Tiled GEMM
 *
 * C[M][N] += A[M][K] * B[K][N]   (single-precision floating-point)
 *
 * Uses a 4×4 register tile: 16 FP accumulators stay in registers
 * across the entire K-loop, amortizing loads over 16 FMAs per k.
 * B row tiles are contiguous in memory → cache-line friendly.
 *
 * Requires: DIM is a multiple of 4.
 */
#include "common.h"

#ifndef BENCH_N
#undef BENCH_N
#define BENCH_N 2
#endif

#define DIM BENCH_N
#define TILE 4
#define BLOCK_SIZE 32

float A[DIM * DIM] __attribute__((aligned(64)));
float B[DIM * DIM] __attribute__((aligned(64)));
float C[DIM * DIM] __attribute__((aligned(64)));
float C_ref[DIM * DIM] __attribute__((aligned(64)));

/*
 * 4×4 register-tiled micro-kernel.
 *
 * Loop order: i-tiles → j-tiles → k (inner)
 *   - 16 C accumulators live in FP registers for the entire k-loop
 *   - A column (4 loads, stride=DIM): amortized over TILE j-tiles
 *   - B row   (4 loads, contiguous): cache-line friendly
 *   - 16 FMAs per k iteration → 4× better compute-to-load ratio
 */
static void __attribute__((noinline)) gemm_tiled(void) {
    for (int jj = 0; jj < DIM; jj += BLOCK_SIZE) {
        for (int kk = 0; kk < DIM; kk += BLOCK_SIZE) {
            for (int i = 0; i < DIM; i += TILE) {
                int j_end = (jj + BLOCK_SIZE < DIM) ? jj + BLOCK_SIZE : DIM;
                for (int j = jj; j < j_end; j += TILE) {
                    /* 4×4 accumulator tile — stays in registers */
                    float c00 = C[(i+0)*DIM + (j+0)]; float c01 = C[(i+0)*DIM + (j+1)];
                    float c02 = C[(i+0)*DIM + (j+2)]; float c03 = C[(i+0)*DIM + (j+3)];
                    float c10 = C[(i+1)*DIM + (j+0)]; float c11 = C[(i+1)*DIM + (j+1)];
                    float c12 = C[(i+1)*DIM + (j+2)]; float c13 = C[(i+1)*DIM + (j+3)];
                    float c20 = C[(i+2)*DIM + (j+0)]; float c21 = C[(i+2)*DIM + (j+1)];
                    float c22 = C[(i+2)*DIM + (j+2)]; float c23 = C[(i+2)*DIM + (j+3)];
                    float c30 = C[(i+3)*DIM + (j+0)]; float c31 = C[(i+3)*DIM + (j+1)];
                    float c32 = C[(i+3)*DIM + (j+2)]; float c33 = C[(i+3)*DIM + (j+3)];

                    int k_end = (kk + BLOCK_SIZE < DIM) ? kk + BLOCK_SIZE : DIM;
                    for (int k = kk; k < k_end; k++) {
                        /* A column tile: 4 strided loads */
                        float a0 = A[(i+0)*DIM + k];
                        float a1 = A[(i+1)*DIM + k];
                        float a2 = A[(i+2)*DIM + k];
                        float a3 = A[(i+3)*DIM + k];

                        /* B row tile: 4 contiguous loads (cache-line!) */
                        float b0 = B[k*DIM + (j+0)];
                        float b1 = B[k*DIM + (j+1)];
                        float b2 = B[k*DIM + (j+2)];
                        float b3 = B[k*DIM + (j+3)];

                        /* 4×4 outer product: 16 FMAs */
                        c00 += a0*b0; c01 += a0*b1; c02 += a0*b2; c03 += a0*b3;
                        c10 += a1*b0; c11 += a1*b1; c12 += a1*b2; c13 += a1*b3;
                        c20 += a2*b0; c21 += a2*b1; c22 += a2*b2; c23 += a2*b3;
                        c30 += a3*b0; c31 += a3*b1; c32 += a3*b2; c33 += a3*b3;
                    }

                    /* Store tile */
                    C[(i+0)*DIM + (j+0)] = c00; C[(i+0)*DIM + (j+1)] = c01;
                    C[(i+0)*DIM + (j+2)] = c02; C[(i+0)*DIM + (j+3)] = c03;
                    C[(i+1)*DIM + (j+0)] = c10; C[(i+1)*DIM + (j+1)] = c11;
                    C[(i+1)*DIM + (j+2)] = c12; C[(i+1)*DIM + (j+3)] = c13;
                    C[(i+2)*DIM + (j+0)] = c20; C[(i+2)*DIM + (j+1)] = c21;
                    C[(i+2)*DIM + (j+2)] = c22; C[(i+2)*DIM + (j+3)] = c23;
                    C[(i+3)*DIM + (j+0)] = c30; C[(i+3)*DIM + (j+1)] = c31;
                    C[(i+3)*DIM + (j+2)] = c32; C[(i+3)*DIM + (j+3)] = c33;
                }
            }
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

        /* Reference (naive triple loop) */
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

        /* Warm up L1 cache */
        volatile float dummy = 0.0f;
        for (int i = 0; i < DIM * DIM; i++) {
            dummy += A[i] + B[i] + C[i];
        }

        BENCH_START();
        HPM_START();

        gemm_tiled();

        BENCH_END();
        HPM_END();
        BENCH_REPORT();
        HPM_REPORT();

        /* Verify */
        int passed = 1;
        for (int i = 0; i < DIM * DIM; i++) {
            float diff = C[i] - C_ref[i];
            if (diff < 0) diff = -diff;
            float mag = C_ref[i]; if (mag < 0) mag = -mag; if (mag < 1.0f) mag = 1.0f;
            if (diff / mag > 1e-4f && passed) {
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
