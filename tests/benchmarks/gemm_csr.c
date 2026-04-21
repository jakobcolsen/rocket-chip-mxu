/*
 * gemm_csr.c — Systolic K-Split GEMM via CSR Mesh Reduction
 *
 * C[M][N] += A[M][K] * B[K][N]   (single-precision floating-point)
 *
 * True systolic data flow: the K reduction dimension is split across
 * 4 cores. Each core computes a local partial sum for its K-slice,
 * then partial sums cascade through the 2D mesh using BOTH axes:
 *
 *   Step 1 — East (CSR 0x801): accumulate across columns
 *     Core 0 → Core 1:  Core 1 acc = partial_k0 + partial_k1
 *     Core 2 → Core 3:  Core 3 acc = partial_k2 + partial_k3
 *
 *   Step 2 — South (CSR 0x802): accumulate across rows
 *     Core 1 → Core 3:  Core 3 final = (k0+k1) + (k2+k3) ✓
 *
 * All cores store results to per-hartid sections of C.
 * Core 3 (hartid = NUM_CORES-1) holds the fully-reduced output.
 *
 * Compile: pass -DBENCH_N=<dim> for matrix dimension.
 */
#include "common.h"

#ifndef BENCH_N
#undef BENCH_N
#define BENCH_N 2
#endif

#define DIM BENCH_N
#define ALIAS_PAD 16

/* ── Matrices (row-major) ─────────────────────────────────────────── */
float A[DIM * DIM] __attribute__((aligned(64)));
float B[DIM * DIM] __attribute__((aligned(64)));
/* Per-core output: each core stores to C[hartid * (DIM*DIM + ALIAS_PAD) + ...] */
volatile float C[NUM_CORES * (DIM * DIM + ALIAS_PAD)] __attribute__((aligned(64)));
float C_ref[DIM * DIM] __attribute__((aligned(64)));

/* IEEE 754 helpers */
static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } x; x.f = f; return x.u;
}
static inline float u2f(uint32_t u) {
    union { float f; uint32_t u; } x; x.u = u; return x.f;
}

/*
 * Systolic K-split reduction kernel.
 *
 * Each core handles DIM/NUM_CORES of the K dimension.
 * After local partial sums, 2-step mesh reduction:
 *   East accumulate → South accumulate → Core 3 has full dot product.
 * ALL cores store to per-hartid C sections (avoids branch divergence).
 */
static void __attribute__((noinline)) systolic_reduction_kernel(void) {
    uint64_t hartid = read_csr(mhartid);

    /* K-split bounds for this core */
    int k_per_core = (DIM + NUM_CORES - 1) / NUM_CORES;
    int k_start = hartid * k_per_core;
    int k_end   = k_start + k_per_core;
    if (k_end > DIM) k_end = DIM;
    if (k_start > DIM) k_start = DIM;

    /* Per-core output base */
    int c_base = hartid * (DIM * DIM + ALIAS_PAD);

    for (int i = 0; i < DIM; i++) {
        for (int j = 0; j < DIM; j++) {
            /* ── Local partial sum for my K-slice ── */
            float my_partial = 0.0f;
            for (int k = k_start; k < k_end; k++) {
                my_partial += A[i * DIM + k] * B[k * DIM + j];
            }

            /* ── Step 1: Accumulate East (CSR 0x801) ──
             * Core 0 → Core 1, Core 2 → Core 3 */
            write_csr(0x801, f2u(my_partial));
                        /* natural pipeline latency is sufficient — no NOPs needed */
            float acc = my_partial + u2f(read_csr(0x801));

            /* ── Step 2: Accumulate South (CSR 0x802) ──
             * Core 1 → Core 3 */
            write_csr(0x802, f2u(acc));
                        /* natural pipeline latency is sufficient — no NOPs needed */
            float result = acc + u2f(read_csr(0x802));

            /* ALL cores store to their own section (no branch needed) */
            C[c_base + i * DIM + j] = result;
        }
    }
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    bench_disable_interrupts();

    if (hartid == 0) {
        /* Initialize matrices */
        for (int i = 0; i < DIM * DIM; i++) {
            A[i] = (float)(i + 1);
            B[i] = (float)(i + 1) * 0.5f;
            C_ref[i] = 0.0f;
        }
        for (int i = 0; i < NUM_CORES * (DIM * DIM + ALIAS_PAD); i++) {
            C[i] = 0.0f;
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

        HPM_SETUP();
        BENCH_START();
        HPM_START();

        SIMD_ENABLE();
        systolic_reduction_kernel();
        asm volatile ("fence rw, rw" ::: "memory");
        SIMD_DISABLE();
        SIMD_PARK_FOLLOWERS();

        BENCH_END();
        HPM_END();
        SIMD_DRAIN();
        asm volatile ("fence rw, rw" ::: "memory");
        BENCH_REPORT();
        HPM_REPORT();

        /* Verify Core 3's output section (fully reduced) */
        int core3_base = (NUM_CORES - 1) * (DIM * DIM + ALIAS_PAD);
        int passed = 1;
        for (int i = 0; i < DIM; i++) {
            for (int j = 0; j < DIM; j++) {
                float got = C[core3_base + i * DIM + j];
                float exp = C_ref[i * DIM + j];
                float diff = got - exp;
                if (diff < 0) diff = -diff;
                if (diff > 0.01f && passed) {
                    passed = 0;
                    printf("  FAIL C[%d][%d]=%f exp=%f\n", i, j, got, exp);
                }
            }
        }
        if (passed) printf("*** PASSED ***\n");
        else        printf("*** FAILED ***\n");

    } else {
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
