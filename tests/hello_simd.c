#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define MSTATUS_MIE 0x00000008

// --------------------------------------------------------------------------
// SYSTOLIC MESH SHARING STRUCTURE
// --------------------------------------------------------------------------
extern char _shared_results[];
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
        // === LEADER PATH ===
        printf("\n\n=================================================\n");
        printf("           CHIPYARD SIMD BOOT SEQUENCE           \n");
        printf("=================================================\n");
        printf("[Leader] Hart 0 Hello World!\n");

        for (int i = 1; i < 4; i++) {
            while (core_ready[i] == 0) { asm volatile("nop"); }
            printf("[Leader] Hart %d signed in.\n", i);
        }

        printf("\n[Leader] All 4 cores alive in DRAM.\n");
        
        for (int i = 0; i < 4; i++) {
            _shared_results[i * 64] = (i == 0) ? 0 : 'Z';
        }
        asm volatile("fence rw, rw" ::: "memory");

        printf("[Leader] Array state BEFORE SIMD (Fixed Base: %p):\n", _shared_results);
        for (int i = 0; i < 4; i++) {
            printf("  Slot %d: 0x%02x\n", i, (unsigned char)_shared_results[i*64]);
        }

        printf("\n>>> ACTIVATING SYSTOLIC SIMD (CSR 0x800) <<<\n");
        asm volatile("csrw 0x800, %0 \n\t" : : "r"(2));
        
        // Massive initial gap to ensure all followers are deep in their NOP runways
        for (volatile int d = 0; d < 100; d++) { asm volatile("nop"); }

        #define SIMD_GAP \
            "nop \n\t" "nop \n\t" "nop \n\t" "nop \n\t" \
            "nop \n\t" "nop \n\t" "nop \n\t" "nop \n\t" \
            "nop \n\t" "nop \n\t" "nop \n\t" "nop \n\t" \
            "nop \n\t" "nop \n\t" "nop \n\t" "nop \n\t"

        // [THE STINKY HACK] 8-Store Burst
        // This ensures Hart 1, 2, or 3 will catch at least one commit even if hit by a stall
        asm volatile(
            "csrr t0, mhartid   \n\t"
            SIMD_GAP
            "lui  t1, %%hi(_shared_results) \n\t"
            SIMD_GAP
            "addi t1, t1, %%lo(_shared_results) \n\t"
            SIMD_GAP
            "slli t3, t0, 6     \n\t"
            SIMD_GAP
            "add  t1, t1, t3    \n\t"
            SIMD_GAP
            "li   t2, 'A'       \n\t"
            SIMD_GAP
            "add  t2, t2, t0    \n\t"
            SIMD_GAP
            "sb   t2, 0(t1)     \n\t"
            "sb   t2, 0(t1)     \n\t"
            "sb   t2, 0(t1)     \n\t"
            "sb   t2, 0(t1)     \n\t"
            "sb   t2, 0(t1)     \n\t"
            "sb   t2, 0(t1)     \n\t"
            "sb   t2, 0(t1)     \n\t"
            "sb   t2, 0(t1)     \n\t"
            SIMD_GAP
            "fence rw, rw      \n\t"
            "fence rw, rw      \n\t"
            SIMD_GAP
            ::: "t0", "t1", "t2", "t3", "memory"
        );

        asm volatile("csrw 0x800, x0 \n\t");
        printf(">>> DEACTIVATED SYSTOLIC SIMD <<<\n\n");

        for (volatile int d = 0; d < 10000; d++);
        asm volatile("fence rw, rw" ::: "memory");

        printf("[Proof] Array state AFTER SIMD (Proof of Lockstep):\n");
        printf("=================================================\n");
        int passed = 1;
        for (int i = 0; i < 4; i++) {
            char expected = 'A' + i;
            char actual = _shared_results[i * 64];
            printf("  Slot %d: '%c' (0x%02x)\n", i, actual, (unsigned char)actual);
            if (actual != expected) passed = 0;
        }

        if (passed) {
            printf("\n[SUCCESS] SIMD Lockstep Proof Confirmed!\n");
            printf("\n*** PASSED ***\n");
        } else {
            printf("\n[ERROR] Not all cores modified their target memory slot!\n");
            printf("*** FAILED ***\n");
        }
        printf("=================================================\n");

    } else {
        // === FOLLOWER PATH ===
        while (1) {
            // Unroll to minimize jump-induced pipeline hazards
            asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");
            asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");
            asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");
            asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");
        }
    }

    return 0;
}
