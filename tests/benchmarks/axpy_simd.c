/*
 * axpy_simd.c — SIMD Lockstep AXPY Benchmark
 *
 * Y[i] = a * X[i] + Y[i]   for i = 0..N-1
 *
 * All 4 cores compute their slice in hardware SIMD lockstep.
 * Uses C for-loop (branches work in SIMD via leader flush broadcast).
 *
 * Compile: pass -DBENCH_N=<size> to set problem size.
 */
#include "common.h"

#define SCALAR_A  3
#define SLICE     (BENCH_N / NUM_CORES)

/* Cache-line aligned arrays */
volatile int32_t X[BENCH_N] __attribute__((aligned(64)));
volatile int32_t Y[BENCH_N] __attribute__((aligned(64)));

static void __attribute__((noinline)) simd_axpy_kernel(void) {
    uint64_t hartid = read_csr(mhartid);
    int start = hartid * SLICE;
    int end   = start + SLICE;
    for (int i = start; i < end; i++) {
        Y[i] += SCALAR_A * X[i];
    }
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    bench_disable_interrupts();

    if (hartid == 0) {
        /* Initialize arrays */
        for (int i = 0; i < BENCH_N; i++) {
            X[i] = i + 1;
            Y[i] = 100 + i;
        }
        asm volatile ("fence rw, rw" ::: "memory");

        printf("BENCHMARK: axpy_simd\n");
        printf("N: %d\n", BENCH_N);

        BENCH_START();

        /* ── SIMD Region ── */
        SIMD_ENABLE();
        simd_axpy_kernel();
        SIMD_DISABLE();
        SIMD_PARK_FOLLOWERS();

        BENCH_END();
        SIMD_DRAIN();
        BENCH_REPORT();

        /* Verify */
        int passed = 1;
        for (int i = 0; i < BENCH_N; i++) {
            int32_t expected = SCALAR_A * (i + 1) + (100 + i);
            if (Y[i] != expected) {
                printf("  MISMATCH Y[%d] = %d, expected %d\n",
                       i, (int)Y[i], (int)expected);
                passed = 0;
                if (i > 5) { printf("  ... (more errors)\n"); break; }
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
