/*
 * dot_simd.c — SIMD Lockstep Dot Product Benchmark
 *
 * result = Σ X[i] * Y[i]   for i = 0..N-1
 *
 * Strategy: same memory layout as axpy_simd (which is proven to work).
 * Each core accumulates its slice then writes the sum back to Y[start].
 * Leader reads Y[0], Y[SLICE], Y[2*SLICE], Y[3*SLICE] to sum.
 *
 * Compile: pass -DBENCH_N=<size> to set problem size.
 */
#include "common.h"

#define SLICE (BENCH_N / NUM_CORES)

/* Cache-line aligned arrays — same layout as axpy_simd */
volatile int32_t X[BENCH_N] __attribute__((aligned(64)));
volatile int32_t Y[BENCH_N] __attribute__((aligned(64)));

/* Separate results array, cache-line aligned per core (stride 16) */
volatile int32_t results[NUM_CORES * 16] __attribute__((aligned(64)));

static void __attribute__((noinline)) simd_dot_kernel(void) {
    uint64_t hartid = read_csr(mhartid);
    int start = hartid * SLICE;
    int end   = start + SLICE;
    int32_t sum = 0;
    for (int i = start; i < end; i++) {
        sum += X[i] * Y[i];
        /* Store every 16 iterations to prevent store queue overflow.
         * MUST store on the last iteration (i == end - 1) INSIDE the loop 
         * to prevent BUG-001 (dropped end-of-SIMD store). */
        if ((i & 15) == 0 || i == end - 1) {
            results[hartid * 16] = sum;
        }
    }
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    bench_disable_interrupts();

    if (hartid == 0) {
        /* Initialize arrays with small values to avoid overflow */
        for (int i = 0; i < BENCH_N; i++) {
            X[i] = (i % 7) + 1;
            Y[i] = (i % 5) + 1;
        }
        asm volatile ("fence rw, rw" ::: "memory");

        /* Pre-compute reference BEFORE SIMD (Y gets overwritten) */
        int64_t expected = 0;
        for (int i = 0; i < BENCH_N; i++) {
            expected += (int64_t)X[i] * (int64_t)Y[i];
        }

        printf("BENCHMARK: dot_simd\n");
        printf("N: %d\n", BENCH_N);

        BENCH_START();

        SIMD_ENABLE();
        simd_dot_kernel();
        SIMD_DISABLE();
        SIMD_PARK_FOLLOWERS();

        /* Leader reduces: read results[c*16] for each core */
        asm volatile ("fence rw, rw" ::: "memory");
        int64_t total = 0;
        for (int c = 0; c < NUM_CORES; c++) {
            total += results[c * 16];
        }

        BENCH_END();
        SIMD_DRAIN();
        BENCH_REPORT();

        printf("Result: %ld, Expected: %ld\n", (long)total, (long)expected);
        if (total == expected) {
            printf("*** PASSED ***\n");
        } else {
            printf("*** FAILED ***\n");
        }

    } else {
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
