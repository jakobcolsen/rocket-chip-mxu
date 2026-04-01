/*
 * dot_mimd.c — MIMD Fork-Join Dot Product Benchmark
 *
 * result = Σ X[i] * Y[i]   for i = 0..N-1
 *
 * All 4 cores compute partial sums via software fork-join,
 * then leader reduces the partial sums.
 *
 * Compile: pass -DBENCH_N=<size> to set problem size.
 */
#include "common.h"

#define SLICE (BENCH_N / NUM_CORES)

/* Cache-line aligned arrays */
volatile int32_t X[BENCH_N] __attribute__((aligned(64)));
volatile int32_t Y[BENCH_N] __attribute__((aligned(64)));

/* Each core's partial sum in its own cache line */
volatile int64_t partial_sums[NUM_CORES][8] __attribute__((aligned(64)));

/* Fork-join control */
volatile int go       = 0;
volatile int done_cnt = 0;

static void __attribute__((noinline)) dot_slice(int start, int end, int core) {
    int64_t sum = 0;
    for (int i = start; i < end; i++) {
        sum += (int64_t)X[i] * (int64_t)Y[i];
    }
    partial_sums[core][0] = sum;
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    bench_disable_interrupts();

    if (hartid == 0) {
        /* Initialize arrays */
        for (int i = 0; i < BENCH_N; i++) {
            X[i] = (i % 7) + 1;
            Y[i] = (i % 5) + 1;
        }
        for (int i = 0; i < NUM_CORES; i++) {
            partial_sums[i][0] = 0;
        }
        asm volatile ("fence rw, rw" ::: "memory");

        printf("BENCHMARK: dot_mimd\n");
        printf("N: %d\n", BENCH_N);

        BENCH_START();

        /* Wake followers */
        go = 1;
        asm volatile ("fence rw, rw" ::: "memory");

        /* Leader computes its own slice */
        dot_slice(0, SLICE, 0);

        /* Wait for all followers to finish */
        mimd_barrier_wait(&done_cnt, NUM_CORES - 1);

        /* Reduce partial sums */
        int64_t total = 0;
        for (int c = 0; c < NUM_CORES; c++) {
            total += partial_sums[c][0];
        }

        BENCH_END();
        BENCH_REPORT();

        /* Verify */
        int64_t expected = 0;
        for (int i = 0; i < BENCH_N; i++) {
            expected += (int64_t)((i % 7) + 1) * (int64_t)((i % 5) + 1);
        }

        printf("Result: %ld, Expected: %ld\n", (long)total, (long)expected);
        if (total == expected) {
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
        dot_slice(start, end, hartid);

        asm volatile ("fence rw, rw" ::: "memory");
        __sync_fetch_and_add(&done_cnt, 1);

        /* Park */
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
