/*
 * test_systolic_mul_v2.c — SYSTOLIC_MUL + CSR read comparison
 * 
 * Tests whether io.systolic_opA (reg_west) has the correct value
 * when SYSTOLIC_MUL executes on Hart 1.
 * 
 * Phase 1: Hart 0 writes East=7 via CSR
 * Phase 2 (SIMD): Read CSR 0x801 first (verify mesh_west), then SYSTOLIC_MUL
 */
#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define MSTATUS_MIE 0x00000008
#define NUM_CORES 4

volatile uint64_t csr_results[NUM_CORES][8] __attribute__((aligned(64)));
volatile uint64_t mul_results[NUM_CORES][8] __attribute__((aligned(64)));
volatile int core_ready[NUM_CORES] = {0};

void thread_entry(int cid, int nc) { }

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    if (hartid == 0) {
        write_csr(0x801, 7);
    }

    core_ready[hartid] = 1;
    asm volatile("fence rw, rw" ::: "memory");

    if (hartid == 0) {
        for (int i = 1; i < NUM_CORES; i++)
            while (core_ready[i] == 0) asm volatile("nop");

        for (int i = 0; i < NUM_CORES; i++) {
            csr_results[i][0] = 0xDEAD;
            mul_results[i][0] = 0xDEAD;
        }
        asm volatile("fence rw, rw" ::: "memory");

        printf("=== SYSTOLIC_MUL v2 Test ===\n");
        printf("Hart 0 East=7, weight=5\n\n");

        asm volatile(
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"

            /* Step 1: Read mesh_west via CSR 0x801 → store to csr_results */
            "csrr t3, 0x801            \n\t"
            "csrr t4, mhartid          \n\t"
            "slli t4, t4, 6            \n\t"
            "la   t5, csr_results      \n\t"
            "add  t5, t5, t4           \n\t"
            "sd   t3, 0(t5)            \n\t"

            /* Step 2: SYSTOLIC_MUL t3, t2 (weight=5) → store to mul_results */
            "li   t2, 5                \n\t"
            SYSTOLIC_MUL("t3", "t2")
            "csrr t4, mhartid          \n\t"
            "slli t4, t4, 6            \n\t"
            "la   t5, mul_results      \n\t"
            "add  t5, t5, t4           \n\t"
            "sd   t3, 0(t5)            \n\t"
            "fence rw, rw              \n\t"
            ::: "t2", "t3", "t4", "t5", "memory"
        );
        asm volatile("csrw 0x800, x0");

        for (volatile int d = 0; d < 1000; d++);
        asm volatile("fence rw, rw" ::: "memory");

        printf("CSR read (mesh_west via 0x801):\n");
        for (int i = 0; i < NUM_CORES; i++)
            printf("  Hart %d: %ld\n", i, (long)csr_results[i][0]);
        
        printf("SYSTOLIC_MUL result (mesh_west * 5):\n");
        int passed = 1;
        const uint64_t expected_mul[NUM_CORES] = {0, 35, 0, 0};
        for (int i = 0; i < NUM_CORES; i++) {
            uint64_t r = mul_results[i][0];
            printf("  Hart %d: %ld", i, (long)r);
            if (r != expected_mul[i]) {
                printf(" (expected %ld) MISMATCH", (long)expected_mul[i]);
                passed = 0;
            }
            printf("\n");
        }

        if (passed) printf("\n*** PASSED ***\n");
        else printf("\n*** FAILED ***\n");
    } else {
        while (1) asm volatile("wfi");
    }
    return 0;
}
