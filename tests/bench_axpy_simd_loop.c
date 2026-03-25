/*
 * bench_axpy_simd_loop.c — SIMD AXPY benchmark using a C for loop
 * Tests whether compiler-generated branch instructions work in SIMD mode.
 */
#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define N          64
#define NUM_CORES   4
#define SLICE      (N / NUM_CORES)   /* 16 elements per core */
#define SCALAR_A    3

/* Cache-line aligned arrays (64 bytes per core slice) */
volatile int32_t X[N] __attribute__((aligned(64)));
volatile int32_t Y[N] __attribute__((aligned(64)));

void thread_entry(int cid, int nc) { /* no-op */ }

int main(void) {
    uint64_t hartid = read_csr(mhartid);

    /* Disable interrupts */
    clear_csr(mstatus, 0x8);
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
        asm volatile(
            /* Activate SIMD */
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"

            /* Compute per-core slice base addresses */
            "csrr t5, mhartid          \n\t"
            "slli t5, t5, 6            \n\t"  /* hartid * 64 bytes */
            "la   t0, X               \n\t"
            "add  t0, t0, t5           \n\t"  /* t0 = &X[slice] */
            "la   t1, Y               \n\t"
            "add  t1, t1, t5           \n\t"  /* t1 = &Y[slice] */

            /* Loop: 16 elements, 4 bytes each */
            "li   t5, 64              \n\t"  /* t5 = 16 * 4 = byte limit */
            "li   t2, 0               \n\t"  /* t2 = byte offset */
        "1:                            \n\t"
            "add  t3, t0, t2           \n\t"  /* &X[slice + i] */
            "lw   t3, 0(t3)            \n\t"  /* t3 = X[i] */
            "slli t4, t3, 1            \n\t"  /* t4 = 2*X[i] */
            "add  t3, t4, t3           \n\t"  /* t3 = 3*X[i] */
            "add  t4, t1, t2           \n\t"  /* &Y[slice + i] */
            "lw   t4, 0(t4)            \n\t"  /* t4 = Y[i] */
            "add  t3, t3, t4           \n\t"  /* t3 = 3*X[i] + Y[i] */
            "add  t4, t1, t2           \n\t"  /* &Y[slice + i] */
            "sw   t3, 0(t4)            \n\t"  /* Y[i] = result */
            "addi t2, t2, 4            \n\t"  /* offset += 4 */
            "bne  t2, t5, 1b           \n\t"  /* loop if offset < 64 */

            "fence rw, rw              \n\t"
            ::: "t0", "t1", "t2", "t3", "t4", "t5", "memory"
        );

        /* Deactivate SIMD */
        asm volatile("csrw 0x800, x0");

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
        } else {
            printf("Verification FAILED!\n");
            printf("*** FAILED ***\n");
        }

    } else {
        /* Followers: sleep until SIMD wakes them */
        while (1) { asm volatile("wfi"); }
    }

    return 0;
}
