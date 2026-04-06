/*
 * gemm_systolic.c — Optimized Systolic GEMM via SYSTOLIC_FMUL_S
 *
 * C[N][N] += A[N][N] * B[N][N]   (single-precision floating-point)
 *
 * Optimization v3: Zero-NOP pipelined approach.
 * The Rocket FPU scoreboard should stall the readback instruction
 * until the FMUL result is available, making explicit NOPs unnecessary.
 *
 * Additionally: 2×2 tile batching (4 dot products per SIMD burst).
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

/* Shared operands for a 2×2 tile — 2 A-rows, 2 B-cols */
volatile uint32_t shared_a0[DIM] __attribute__((aligned(64)));
volatile uint32_t shared_a1[DIM] __attribute__((aligned(64)));
volatile uint32_t shared_b0[DIM] __attribute__((aligned(64)));
volatile uint32_t shared_b1[DIM] __attribute__((aligned(64)));

/* Per-core results */
volatile uint32_t tile_out[NUM_CORES * 16] __attribute__((aligned(64)));

/* IEEE 754 helpers */
static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } x; x.f = f; return x.u;
}
static inline float u2f(uint32_t u) {
    union { float f; uint32_t u; } x; x.u = u; return x.f;
}

/*
 * systolic_tile_2x2 — one SIMD burst computing 4 dot products
 *
 * Zero-NOP: rely on FPU scoreboard to stall fmv.x.w until result ready.
 */
static void __attribute__((noinline)) systolic_tile_2x2(void) {
    asm volatile ("fence rw, rw" ::: "memory");

    SIMD_ENABLE();

    float acc00 = 0.0f, acc01 = 0.0f, acc10 = 0.0f, acc11 = 0.0f;

#pragma GCC unroll 16
    for (int k = 0; k < DIM; k++) {
        uint32_t a0 = shared_a0[k];
        uint32_t a1 = shared_a1[k];
        uint32_t b0 = shared_b0[k];
        uint32_t b1 = shared_b1[k];
        uint32_t r;

        /* DP 1: a0 * b0 — no NOPs, scoreboard handles hazard */
        write_csr(0x801, a0);
        asm volatile (
            "fmv.w.x ft0, %[bw]\n\t"
            SYSTOLIC_FMUL_S("ft1", "ft0")
            "fmv.x.w %[res], ft1\n\t"
            : [res] "=r" (r) : [bw] "r" (b0) : "ft0", "ft1"
        );
        acc00 += u2f(r);

        /* DP 2: a0 * b1 */
        write_csr(0x801, a0);
        asm volatile (
            "fmv.w.x ft0, %[bw]\n\t"
            SYSTOLIC_FMUL_S("ft1", "ft0")
            "fmv.x.w %[res], ft1\n\t"
            : [res] "=r" (r) : [bw] "r" (b1) : "ft0", "ft1"
        );
        acc01 += u2f(r);

        /* DP 3: a1 * b0 */
        write_csr(0x801, a1);
        asm volatile (
            "fmv.w.x ft0, %[bw]\n\t"
            SYSTOLIC_FMUL_S("ft1", "ft0")
            "fmv.x.w %[res], ft1\n\t"
            : [res] "=r" (r) : [bw] "r" (b0) : "ft0", "ft1"
        );
        acc10 += u2f(r);

        /* DP 4: a1 * b1 */
        write_csr(0x801, a1);
        asm volatile (
            "fmv.w.x ft0, %[bw]\n\t"
            SYSTOLIC_FMUL_S("ft1", "ft0")
            "fmv.x.w %[res], ft1\n\t"
            : [res] "=r" (r) : [bw] "r" (b1) : "ft0", "ft1"
        );
        acc11 += u2f(r);
    }

    /* Store all 4 results per-core */
    uint64_t hid = read_csr(mhartid);
    tile_out[hid * 16 + 0] = f2u(acc00);
    tile_out[hid * 16 + 1] = f2u(acc01);
    tile_out[hid * 16 + 2] = f2u(acc10);
    tile_out[hid * 16 + 3] = f2u(acc11);

    SIMD_DISABLE();

    asm volatile ("fence rw, rw" ::: "memory");
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

        /* Scalar reference */
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

        printf("BENCHMARK: gemm_systolic\n");
        printf("N: %d\n", DIM);

        BENCH_START();

        /* Process in 2×2 output tiles */
        for (int ti = 0; ti < DIM; ti += 2) {
            for (int tj = 0; tj < DIM; tj += 2) {
                /* Pre-load 2 A-rows and 2 B-columns */
                for (int k = 0; k < DIM; k++) {
                    shared_a0[k] = f2u(A[ti * DIM + k]);
                    shared_a1[k] = (ti + 1 < DIM) ? f2u(A[(ti+1) * DIM + k]) : 0;
                    shared_b0[k] = f2u(B[k * DIM + tj]);
                    shared_b1[k] = (tj + 1 < DIM) ? f2u(B[k * DIM + (tj+1)]) : 0;
                }

                systolic_tile_2x2();

                /* Read Core 1's results */
                C[ti * DIM + tj]         = u2f(tile_out[1 * 16 + 0]);
                if (tj + 1 < DIM)
                    C[ti * DIM + (tj+1)] = u2f(tile_out[1 * 16 + 1]);
                if (ti + 1 < DIM)
                    C[(ti+1) * DIM + tj] = u2f(tile_out[1 * 16 + 2]);
                if (ti + 1 < DIM && tj + 1 < DIM)
                    C[(ti+1) * DIM + (tj+1)] = u2f(tile_out[1 * 16 + 3]);
            }
        }

        BENCH_END();
        BENCH_REPORT();

        /* Verify */
        int passed = 1;
        printf("Results:\n");
        for (int i = 0; i < DIM; i++) {
            for (int j = 0; j < DIM; j++) {
                uint32_t got_bits = f2u(C[i * DIM + j]);
                uint32_t exp_bits = f2u(C_ref[i * DIM + j]);
                float diff = C[i * DIM + j] - C_ref[i * DIM + j];
                if (diff < 0) diff = -diff;
                printf("  C[%d][%d] = 0x%lx (exp 0x%lx)\n",
                       i, j, (unsigned long)got_bits, (unsigned long)exp_bits);
                if (diff > 0.01f) passed = 0;
            }
        }

        if (passed) printf("*** PASSED ***\n");
        else        printf("*** FAILED ***\n");

        for (volatile int flush = 0; flush < 50000; flush++);

    } else {
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
