/*
 * bench_axpy_simd_loop.c — SIMD AXPY benchmark using a C for loop
 * Tests whether compiler-generated branch instructions work in SIMD mode.
 */
#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define N          1024
#define NUM_CORES   4
#define SLICE      (N / NUM_CORES)   /* 16 elements per core */
#define SCALAR_A    3

/* Cache-line aligned arrays (64 bytes per core slice) */
volatile int32_t X[N] __attribute__((aligned(64)));
volatile int32_t Y[N] __attribute__((aligned(64)));

void thread_entry(int cid, int nc) { /* no-op */ }

void run_simd_axpy(void) {
    uint64_t hartid = read_csr(mhartid);
    int start = hartid * SLICE;
    int end = start + SLICE;
    for (int i = start; i < end; i++) {
        Y[i] += SCALAR_A * X[i];
    }
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);

    /* Disable interrupts to keep lockstep clean */
    clear_csr(mstatus, 0x00000008); 
    write_csr(mie, 0);

    if (hartid == 0) {
        /* Leader initialises arrays */
        for (int i = 0; i < N; i++) {
            X[i] = i + 1;
            Y[i] = 100 + i;
        }
        asm volatile("fence rw, rw" ::: "memory");

        printf("=== AXPY SIMD Loop Test ===\n");
        printf("N=%d, SLICE=%d, a=%d, cores=%d\n", N, SLICE, SCALAR_A, NUM_CORES);

        uint64_t cyc_start = read_csr(mcycle);

        /* ── SIMD REGION ── */
        /* Activate SIMD */
        asm volatile("csrw 0x800, 2");

        /* Cores branch and execute ABI-safe C loop together */
        run_simd_axpy();

        /* Deactivate SIMD */
        asm volatile("csrw 0x800, x0");

        /* Followers go to sleep immediately */
        asm volatile(
            "csrr t0, mhartid \n\t"
            "beqz t0, 1f      \n\t"
            "2: wfi           \n\t"
            "j 2b             \n\t"
            "1:               \n\t"
            ::: "t0"
        );

        uint64_t cyc_end = read_csr(mcycle);
        uint64_t elapsed = cyc_end - cyc_start;

        /* Drain */
        for (volatile int d = 0; d < 1000; d++);
        asm volatile("fence rw, rw" ::: "memory");

        printf("Parallel region: %lu cycles\n", (unsigned long)elapsed);

        /* Verify */
        int passed = 1;
        for (int i = 0; i < N; i++) {
            int32_t expected = SCALAR_A * (i + 1) + (100 + i);
            if (Y[i] != expected) {
                printf("  MISMATCH Y[%d] = %d, expected %d\n",
                       i, (int)Y[i], (int)expected);
                passed = 0;
            }
        }

        if (passed) {
            printf("Verification PASSED — all %d elements correct.\n", N);
            printf("\n*** PASSED ***\n");
            return 0;
        } else {
            printf("Verification FAILED!\n");
            printf("*** FAILED ***\n");
            return 1;
        }

    } else {
        /* Followers: sleep until SIMD wakes them */
        while (1) { asm volatile("wfi"); }
    }
}
