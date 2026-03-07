#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define MSTATUS_MIE 0x00000008

volatile char shared_results[4] __attribute__((aligned(64)));
volatile int core_ready[16] = {0};

void thread_entry(int cid, int nc) {
    // no-op: all harts continue to _init -> main
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);

    // Disable interrupts to prevent flushes
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    // Each core signs in
    core_ready[hartid] = 1;

    if (hartid == 0) {
        printf("Hello World!\n");

        for (int i = 1; i < 4; i++) {
            while (core_ready[i] == 0) { asm volatile("nop"); }
            printf("Hart %d is here.\n", i);
        }

        printf("It's a party in here!\n");
        
        for (int i = 0; i < 4; i++) {
            shared_results[i] = (i == 0) ? 0 : 'Z';
        }
        asm volatile("fence rw, rw" ::: "memory");

        printf("Array state BEFORE SIMD (Fixed Base: %p):\n", shared_results);
        for (int i = 0; i < 4; i++) {
            printf("  Slot %d: 0x%02x\n", i, (unsigned char)shared_results[i]);
        }

        printf("ACTIVATING SYSTOLIC SIMD (CSR 0x800)\n");
        asm volatile("csrw 0x800, %0 \n\t" : : "r"(2));
        
        // Massive initial gap removed via hardware CSR flush fix

        // Hardware global stall eliminates the need for NOP gaps and redundant stores.
        // Each instruction is held by the Leader until all Followers have consumed it.
        asm volatile(
            "csrr t0, mhartid   \n\t"
            "la   t1, shared_results \n\t"
            "slli t3, t0, 0     \n\t"
            "add  t1, t1, t3    \n\t"
            "li   t2, 'A'       \n\t"
            "add  t2, t2, t0    \n\t"
            "sb   t2, 0(t1)     \n\t"
            "fence rw, rw       \n\t"
            ::: "t0", "t1", "t2", "t3", "memory"
        );

        asm volatile("csrw 0x800, x0 \n\t");
        printf("DEACTIVATED SYSTOLIC SIMD\n\n");

        for (volatile int d = 0; d < 10000; d++);
        asm volatile("fence rw, rw" ::: "memory");

        printf("Array state AFTER SIMD (Proof of Lockstep):\n");
        int passed = 1;
        for (int i = 0; i < 4; i++) {
            char expected = 'A' + i;
            char actual = shared_results[i];
            printf("  Slot %d: '%c' (0x%02x)\n", i, actual, (unsigned char)actual);
            if (actual != expected) passed = 0;
        }

        if (passed) {
            printf("\nSIMD Lockstep Confirmed!\n");
            printf("\n*** PASSED ***\n");
        } else {
            printf("\nNot all cores modified their target memory slot!\n");
            printf("*** FAILED ***\n");
        }

    } else {
        while (1) {
            // Sleep until hardware systolic_enable wakes pipeline
            asm volatile("wfi");
        }
    }

    return 0;
}
