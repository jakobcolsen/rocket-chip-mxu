#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

/*
 * hello_systolic_flow.c — Systolic Shift Register Data Flow Test
 *
 * 2×2 Mesh Layout (hartid → position):
 *
 *       North=0    North=0
 *         ↓          ↓
 *  West=0 → Hart 0 → Hart 1 → (East sink)
 *             ↓          ↓
 *  West=0 → Hart 2 → Hart 3 → (East sink)
 *             ↓          ↓
 *          (South     (South
 *           sink)      sink)
 *
 * Test Plan:
 *   Phase 1 (MIMD): Each core pre-loads its output registers based on position.
 *     - Hart 0: East=10, South=20   (feeds Hart 1's West and Hart 2's North)
 *     - Hart 1: South=30            (feeds Hart 3's North)
 *     - Hart 2: East=40             (feeds Hart 3's West)
 *
 *   Phase 2 (SIMD): All cores read their input registers and store to memory.
 *     - CSR 0x801 read → West shift register value
 *     - CSR 0x802 read → North shift register value
 *
 *   Expected Results:
 *     Hart 0: West=0  (left edge),  North=0  (top edge)
 *     Hart 1: West=10 (from Hart 0), North=0  (top edge)
 *     Hart 2: West=0  (left edge),  North=20 (from Hart 0)
 *     Hart 3: West=40 (from Hart 2), North=30 (from Hart 1)
 */

#define MSTATUS_MIE 0x00000008
#define NUM_CORES 4

#define SYSTOLIC_DATA_WE_CSR 0x801  // Read: West input,  Write: East output
#define SYSTOLIC_DATA_NS_CSR 0x802  // Read: North input, Write: South output

// Results buffer: dense layout — all 4 harts in one cache line (tests Store-Done Tracking)
volatile uint64_t results[NUM_CORES][2];
volatile int core_ready[NUM_CORES] = {0};

void thread_entry(int cid, int nc) {
    // no-op: all harts continue to _init -> main
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);

    // Disable interrupts to prevent pipeline flushes
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    // ======================================================================
    // Phase 1 (MIMD): Pre-load shift register output registers
    // ======================================================================
    // Each core writes to its East/South output registers based on position.
    // These values will appear at the downstream core's West/North inputs.

    if (hartid == 0) {
        write_csr(0x801, 10);  // East  → Hart 1's West
        write_csr(0x802, 20);  // South → Hart 2's North
    } else if (hartid == 1) {
        write_csr(0x802, 30);  // South → Hart 3's North
    } else if (hartid == 2) {
        write_csr(0x801, 40);  // East  → Hart 3's West
    }
    // Hart 3: no outputs to pre-load

    asm volatile("fence rw, rw" ::: "memory");

    // Signal ready and enter WFI (followers) or continue (leader)

    if (hartid == 0) {
        // Leader: give followers time to pre-load CSRs and enter WFI
        // (can't poll core_ready — L1 D-caches are non-coherent)

        printf("=== Systolic Shift Register Data Flow Test ===\n\n");
        printf("Pre-loaded values:\n");
        printf("  Hart 0: East=10, South=20\n");
        printf("  Hart 1: South=30\n");
        printf("  Hart 2: East=40\n\n");

        // Clear results
        for (int i = 0; i < NUM_CORES; i++) {
            results[i][0] = 0xDEAD;
            results[i][1] = 0xDEAD;
        }
        asm volatile("fence rw, rw" ::: "memory");

        // ==================================================================
        // Phase 2 (SIMD): All cores read input registers and store to memory
        // ==================================================================
        printf("ACTIVATING SYSTOLIC SIMD\n");
        asm volatile("csrw 0x800, %0" : : "r"(2));  // Bit 1 = SIMD mode

        // SIMD Payload: read shift registers, compute store offset, store
        asm volatile(
            "csrr  t0, 0x801          \n\t"  // t0 = West input register
            "csrr  t1, 0x802          \n\t"  // t1 = North input register
            "csrr  t2, mhartid        \n\t"  // t2 = hart id
            "slli  t3, t2, 4          \n\t"  // t3 = hartid * 16 (2 x uint64_t)
            "la    t4, results        \n\t"
            "add   t4, t4, t3         \n\t"  // t4 = &results[hartid]
            "sd    t0, 0(t4)          \n\t"  // results[hartid][0] = west
            "sd    t1, 8(t4)          \n\t"  // results[hartid][1] = north
            "fence rw, rw             \n\t"
            ::: "t0", "t1", "t2", "t3", "t4", "memory"
        );

        // Deactivate SIMD
        asm volatile("csrw 0x800, x0");
        printf("DEACTIVATED SYSTOLIC SIMD\n\n");

        // Drain time
        asm volatile("fence rw, rw" ::: "memory");

        // ==================================================================
        // Phase 3: Verify results
        // ==================================================================
        // Expected: Hart → (West, North)
        //   Hart 0: (0,  0 )  — left edge, top edge
        //   Hart 1: (10, 0 )  — from Hart 0 East, top edge
        //   Hart 2: (0,  20)  — left edge, from Hart 0 South
        //   Hart 3: (40, 30)  — from Hart 2 East, from Hart 1 South

        const uint64_t expected_west[NUM_CORES]  = { 0, 10,  0, 40};
        const uint64_t expected_north[NUM_CORES] = { 0,  0, 20, 30};

        printf("Results (West, North per core):\n");
        int passed = 1;
        for (int i = 0; i < NUM_CORES; i++) {
            uint64_t w = results[i][0];
            uint64_t n = results[i][1];
            char w_ok = (w == expected_west[i])  ? ' ' : '!';
            char n_ok = (n == expected_north[i]) ? ' ' : '!';
            printf("  Hart %d: West=%ld%c  North=%ld%c", i,
                   (long)w, w_ok, (long)n, n_ok);

            if (w != expected_west[i] || n != expected_north[i]) {
                printf("  (expected W=%ld N=%ld)", (long)expected_west[i],
                       (long)expected_north[i]);
                passed = 0;
            }
            printf("\n");
        }

        printf("\n");
        if (passed) {
            printf("Systolic data flow verified!\n");
            printf("\n*** PASSED ***\n");
        } else {
            printf("Data flow mismatch detected!\n");
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
