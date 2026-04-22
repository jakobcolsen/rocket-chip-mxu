/*
 * gemm_tiled_csr.c — CSR Systolic K-Split Register-Tiled GEMM
 *
 * C[M][N] += A[M][K] * B[K][N]   (single-precision floating-point)
 *
 * K-split across 4 cores (same reduction strategy as gemm_csr.c),
 * but each core's local partial-sum computation uses register tiling:
 *   - j-tiled by 4 (contiguous B access → cache-line friendly)
 *   - 4 accumulators per row stay in registers across the K-slice
 *
 * After local tiled computation, 2-step CSR mesh reduction per element:
 *   Step 1 — East (CSR 0x801): accumulate across columns
 *   Step 2 — South (CSR 0x802): accumulate across rows
 *
 * Compile: pass -DBENCH_N=<dim> for matrix dimension.
 */
#include "common.h"

#ifndef BENCH_N
#undef BENCH_N
#define BENCH_N 2
#endif

#define DIM BENCH_N
#define TILE 4
#define ALIAS_PAD 16
#define BLOCK_SIZE 32

/* ── Matrices (row-major) ─────────────────────────────────────────── */
float A[DIM * DIM + NUM_CORES * ALIAS_PAD] __attribute__((aligned(64)));
float B[DIM * DIM] __attribute__((aligned(64)));
/* Per-core output sections with alias padding */
volatile float C[NUM_CORES * (DIM * DIM + ALIAS_PAD)] __attribute__((aligned(64)));
float C_ref[DIM * DIM + NUM_CORES * ALIAS_PAD] __attribute__((aligned(64)));

static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } x; x.f = f; return x.u;
}
static inline float u2f(uint32_t u) {
    union { float f; uint32_t u; } x; x.u = u; return x.f;
}

/*
 * Tiled CSR systolic reduction kernel.
 *
 * Phase 1: Each core computes local partial sums for its K-slice
 *          using j-tiled 1×4 micro-kernel (4 accumulators in regs).
 * Phase 2: Per-element 2-step mesh reduction via CSR 0x801/0x802.
 *
 * The tiling benefit is in Phase 1 — same per-element reduction
 * overhead as the scalar version, but much faster local computation.
 */
static void __attribute__((noinline)) tiled_csr_kernel(void) {
    uint64_t hartid = read_csr(mhartid);

    /* K-split bounds */
    int k_per_core = (DIM + NUM_CORES - 1) / NUM_CORES;
    int k_start = hartid * k_per_core;
    int k_end   = k_start + k_per_core;
    if (k_end > DIM) k_end = DIM;
    if (k_start > DIM) k_start = DIM;

    int c_base = hartid * (DIM * DIM + ALIAS_PAD);

    for (int ii = 0; ii < DIM; ii += BLOCK_SIZE) {
        for (int jj = 0; jj < DIM; jj += BLOCK_SIZE) {
            int i_end = (ii + BLOCK_SIZE < DIM) ? ii + BLOCK_SIZE : DIM;
            for (int i = ii; i < i_end; i++) {
                /* Process j in tiles of 4 for cache-line-friendly B access */
                int j_end = (jj + BLOCK_SIZE < DIM) ? jj + BLOCK_SIZE : DIM;
                for (int j = jj; j < j_end; j += TILE) {
                    /* Phase 1: Local partial sums with 1x4 register tile */
                    float p0 = 0.0f, p1 = 0.0f, p2 = 0.0f, p3 = 0.0f;
                    for (int k = k_start; k < k_end; k++) {
                        float a = A[i * DIM + k];
                        p0 += a * B[k * DIM + (j+0)];
                        p1 += a * B[k * DIM + (j+1)];
                        p2 += a * B[k * DIM + (j+2)];
                        p3 += a * B[k * DIM + (j+3)];
                    }

                    /* Phase 2: CSR mesh reduction (per element, same as scalar) */
                    /* Each of the 4 tile elements goes through the 2-step reduce */
                    float partials[4] = { p0, p1, p2, p3 };
                    for (int t = 0; t < TILE; t++) {
                        /* Step 1: East accumulate (CSR 0x801) */
                        write_csr(0x801, f2u(partials[t]));
                        float acc = partials[t] + u2f(read_csr(0x801));

                        /* Step 2: South accumulate (CSR 0x802) */
                        write_csr(0x802, f2u(acc));
                        float result = acc + u2f(read_csr(0x802));

                        C[c_base + i * DIM + (j+t)] = result;
                    }
                }
            }
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

        /* Warm up L1 cache for ALL cores */
        SIMD_ENABLE();
        volatile float dummy = 0.0f;
        for (int i = 0; i < DIM * DIM + NUM_CORES * ALIAS_PAD; i++) dummy += A[i] + C[i];
        for (int i = 0; i < DIM * DIM; i++) dummy += B[i];
        SIMD_DISABLE();
        SIMD_DRAIN();

        BENCH_START();
        HPM_START();

        SIMD_ENABLE();
        tiled_csr_kernel();
        asm volatile ("fence rw, rw" ::: "memory");
        SIMD_DISABLE();
        SIMD_PARK_FOLLOWERS();

        BENCH_END();
        HPM_END();
        SIMD_DRAIN();
        asm volatile ("fence rw, rw" ::: "memory");
        BENCH_REPORT();
        HPM_REPORT();

        /* Verify Core 3's output (fully reduced) */
        int core3_base = (NUM_CORES - 1) * (DIM * DIM + ALIAS_PAD);
        int passed = 1;
        for (int i = 0; i < DIM; i++) {
            for (int j = 0; j < DIM; j++) {
                float got = C[core3_base + i * DIM + j];
                float exp = C_ref[i * DIM + j];
                float diff = got - exp;
                if (diff < 0) diff = -diff;
                float mag = exp; if (mag < 0) mag = -mag; if (mag < 1.0f) mag = 1.0f;
                if (diff / mag > 1e-4f && passed) {
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
