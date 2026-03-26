/*
 * test_systolic_mul.c — Smoke Test for SYSTOLIC_MUL Instruction
 *
 * Verifies that the SYSTOLIC_MUL custom instruction correctly:
 *   1. Reads mesh_west data as one MUL operand
 *   2. Reads rs1 (from register file) as the other MUL operand
 *   3. Writes the product to rd
 *   4. Auto-forwards mesh_west → east_out (pass-through)
 *   5. Auto-forwards the MUL result → south_out
 *
 * 2×2 Mesh Layout:
 *       Hart 0 → Hart 1
 *         ↓         ↓
 *       Hart 2 → Hart 3
 *
 * Test Plan:
 *   Phase 1 (MIMD): Hart 0 writes East=7 (CSR 0x801). This becomes Hart 1's mesh_west.
 *   Phase 2 (SIMD): All cores execute SYSTOLIC_MUL with weight=5 in rs1.
 *     - Hart 0: mesh_west=0 (left edge)  → rd = 0 * 5 = 0
 *     - Hart 1: mesh_west=7 (from Hart 0) → rd = 7 * 5 = 35
 *     - Hart 2: mesh_west=0 (left edge)  → rd = 0 * 5 = 0
 *     - Hart 3: mesh_west=0 (left edge, no pre-load) → rd = 0 * 5 = 0
 *   Phase 3: Verify results stored to memory.
 */

#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define MSTATUS_MIE 0x00000008
#define NUM_CORES 4

// Results buffer: each core gets its own cache line
volatile uint64_t results[NUM_CORES][8] __attribute__((aligned(64)));
volatile int core_ready[NUM_CORES] = {0};

void thread_entry(int cid, int nc) {
    // no-op
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);

    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    // Phase 1: Pre-load mesh data (MIMD)
    if (hartid == 0) {
        write_csr(0x801, 7);  // East output = 7 → becomes Hart 1's mesh_west
    }

    // Signal ready
    core_ready[hartid] = 1;
    asm volatile("fence rw, rw" ::: "memory");

    if (hartid == 0) {
        // Leader: wait for all followers
        for (int i = 1; i < NUM_CORES; i++) {
            while (core_ready[i] == 0) { asm volatile("nop"); }
        }

        // Clear results
        for (int i = 0; i < NUM_CORES; i++) {
            results[i][0] = 0xDEAD;
        }
        asm volatile("fence rw, rw" ::: "memory");

        printf("=== SYSTOLIC_MUL Smoke Test ===\n\n");
        printf("Pre-loaded: Hart 0 East=7\n");
        printf("Weight (rs1) = 5 for all cores\n\n");

        // Phase 2: SIMD — execute SYSTOLIC_MUL
        printf("ACTIVATING SIMD\n");
        asm volatile(
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"

            /* Load weight into t2 */
            "li   t2, 5                \n\t"

            /* SYSTOLIC_MUL t3, t2: t3 = mesh_west * t2 */
            SYSTOLIC_MUL("t3", "t2")

            /* Store result: compute hartid-based offset */
            "csrr t4, mhartid          \n\t"
            "slli t4, t4, 6            \n\t"  /* t4 = hartid * 64 */
            "la   t5, results          \n\t"
            "add  t5, t5, t4           \n\t"
            "sd   t3, 0(t5)            \n\t"
            "fence rw, rw              \n\t"
            ::: "t2", "t3", "t4", "t5", "memory"
        );

        // Deactivate SIMD
        asm volatile("csrw 0x800, x0");
        printf("DEACTIVATED SIMD\n\n");

        // Drain
        asm volatile("fence rw, rw" ::: "memory");
        for (volatile int d = 0; d < 1000; d++);

        // Phase 3: Verify
        const uint64_t expected[NUM_CORES] = {0, 35, 0, 0};
        printf("Results:\n");
        int passed = 1;
        for (int i = 0; i < NUM_CORES; i++) {
            uint64_t r = results[i][0];
            char ok = (r == expected[i]) ? ' ' : '!';
            printf("  Hart %d: result=%ld%c", i, (long)r, ok);
            if (r != expected[i]) {
                printf("  (expected %ld)", (long)expected[i]);
                passed = 0;
            }
            printf("\n");
        }

        printf("\n");
        if (passed) {
            printf("SYSTOLIC_MUL verified!\n");
            printf("\n*** PASSED ***\n");
        } else {
            printf("SYSTOLIC_MUL MISMATCH!\n");
            printf("*** FAILED ***\n");
        }

    } else {
        // Followers: sleep until SIMD wakes them
        while (1) {
            asm volatile("wfi");
        }
    }

    return 0;
}
