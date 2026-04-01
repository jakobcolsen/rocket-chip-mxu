/*
 * gemm_systolic.c — Systolic GEMM via SYSTOLIC_FMUL_S
 *
 * C[N][N] += A[N][N] * B[N][N]   (single-precision floating-point)
 *
 * For each output element C[i][j]:
 *   MIMD: Leader pre-loads A-row and B-col into shared arrays
 *   SIMD: Single burst — k-loop reads from shared arrays,
 *         injects via CSR, FMUL, register accumulate, one store
 *   MIMD: Leader reads Hart 1's result
 *
 * Optimizations vs previous version:
 *   - Register-based FP accumulation (no memory load/store per k)
 *   - Minimal NOPs (2 post-FMUL instead of 4+2)
 *   - No drain delay (removed for(d<100) loop)
 *   - SIMD_PARK_FOLLOWERS removed (hw wfi mask handles return)
 *
 * Note: FMAC (mesh_west * fs1 + mesh_north) accumulates spatially
 * across mesh rows, not temporally across k. We use FMUL + fadd.s.
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

/* Shared operands — leader pre-loads before SIMD enable */
volatile uint32_t shared_a[DIM] __attribute__((aligned(64)));
volatile uint32_t shared_b[DIM] __attribute__((aligned(64)));

/* Per-core result (cache-line padded per core) */
volatile uint32_t tile_out[NUM_CORES * 16] __attribute__((aligned(64)));

/* IEEE 754 helpers */
static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } x; x.f = f; return x.u;
}
static inline float u2f(uint32_t u) {
    union { float f; uint32_t u; } x; x.u = u; return x.f;
}

/*
 * systolic_dot — one SIMD burst computing dot(A_row, B_col)
 *
 * shared_a[] and shared_b[] pre-loaded by leader.
 * Accumulates in FP register, stores once at end.
 */
static float __attribute__((noinline)) systolic_dot(void) {
    asm volatile ("fence rw, rw" ::: "memory");

    SIMD_ENABLE();

    /* k-loop: inject, FMUL, accumulate in register */
    float acc = 0.0f;
    for (int k = 0; k < DIM; k++) {
        uint32_t a_bits = shared_a[k];
        uint32_t b_bits = shared_b[k];

        write_csr(0x801, a_bits);

        uint32_t res_bits;
        asm volatile (
            "fmv.w.x ft0, %[bw]   \n\t"
            SYSTOLIC_FMUL_S("ft1", "ft0")
            "nop \n\t" "nop \n\t"
            "fmv.x.w %[res], ft1   \n\t"
            : [res] "=r" (res_bits)
            : [bw] "r" (b_bits)
            : "ft0", "ft1"
        );

        acc += u2f(res_bits);
    }

    /* Store result per-core INSIDE the function (before SIMD disable) */
    tile_out[read_csr(mhartid) * 16] = f2u(acc);

    SIMD_DISABLE();

    asm volatile ("fence rw, rw" ::: "memory");

    return u2f(tile_out[1 * 16]);
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

        for (int i = 0; i < DIM; i++) {
            for (int j = 0; j < DIM; j++) {
                /* Pre-load operands (MIMD, only leader runs this) */
                for (int k = 0; k < DIM; k++) {
                    shared_a[k] = f2u(A[i * DIM + k]);
                    shared_b[k] = f2u(B[k * DIM + j]);
                }

                C[i * DIM + j] = systolic_dot();
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
