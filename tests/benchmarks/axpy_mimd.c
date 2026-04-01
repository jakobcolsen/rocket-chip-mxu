/*
 * axpy_mimd.c — MIMD Fork-Join AXPY Benchmark
 *
 * Y[i] = a * X[i] + Y[i]   for i = 0..N-1
 *
 * All 4 cores compute their slice via software fork-join.
 * Leader wakes followers with a flag, followers signal done with atomic counter.
 *
 * Compile: pass -DBENCH_N=<size> to set problem size.
 */
#include "common.h"

#define SCALAR_A  3
#define SLICE     (BENCH_N / NUM_CORES)

/* Cache-line aligned arrays */
volatile int32_t X[BENCH_N] __attribute__((aligned(64)));
volatile int32_t Y[BENCH_N] __attribute__((aligned(64)));

/* Fork-join control */
volatile int go       = 0;
volatile int done_cnt = 0;

static void __attribute__((noinline)) axpy_slice(int start, int end) {
    for (int i = start; i < end; i++) {
        Y[i] = SCALAR_A * X[i] + Y[i];
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

        printf("BENCHMARK: axpy_mimd\n");
        printf("N: %d\n", BENCH_N);

        BENCH_START();

        /* Wake followers */
        go = 1;
        asm volatile ("fence rw, rw" ::: "memory");

        /* Leader computes its own slice */
        axpy_slice(0, SLICE);

        /* Wait for all followers to finish */
        mimd_barrier_wait(&done_cnt, NUM_CORES - 1);

        BENCH_END();
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
        /* Follower: wait for go flag */
        while (go == 0) { asm volatile ("nop"); }
        asm volatile ("fence rw, rw" ::: "memory");

        /* Compute my slice */
        int start = hartid * SLICE;
        int end   = start + SLICE;
        axpy_slice(start, end);

        asm volatile ("fence rw, rw" ::: "memory");
        __sync_fetch_and_add(&done_cnt, 1);

        /* Park */
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
