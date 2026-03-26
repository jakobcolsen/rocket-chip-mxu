/*
 * test_systolic_mul_v3.c — Verify SYSTOLIC_MUL actually writes rd
 * Pre-load t3=0xCAFE, then SYSTOLIC_MUL t3, t2.
 * If t3 stays 0xCAFE, the MulDiv never wrote back.
 * If t3 becomes 0, the MulDiv executed but with wrong operand.
 * If t3 becomes 35 on Hart 1, everything is working.
 */
#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define MSTATUS_MIE 0x00000008
#define NUM_CORES 4

volatile uint64_t results[NUM_CORES][8] __attribute__((aligned(64)));
volatile int core_ready[NUM_CORES] = {0};

void thread_entry(int cid, int nc) { }

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    if (hartid == 0) write_csr(0x801, 7);

    core_ready[hartid] = 1;
    asm volatile("fence rw, rw" ::: "memory");

    if (hartid == 0) {
        for (int i = 1; i < NUM_CORES; i++)
            while (core_ready[i] == 0) asm volatile("nop");
        for (int i = 0; i < NUM_CORES; i++) results[i][0] = 0xDEAD;
        asm volatile("fence rw, rw" ::: "memory");

        printf("=== SYSTOLIC_MUL v3 Test (rd overwrite check) ===\n");
        printf("Hart 0 East=7, weight=5\n\n");

        asm volatile(
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"
            /* Pre-load t3 with known sentinel */
            "li   t3, 0xCAFE           \n\t"
            /* Load weight */
            "li   t2, 5                \n\t"
            /* SYSTOLIC_MUL: should overwrite t3 */
            SYSTOLIC_MUL("t3", "t2")
            /* Store result */
            "csrr t4, mhartid          \n\t"
            "slli t4, t4, 6            \n\t"
            "la   t5, results          \n\t"
            "add  t5, t5, t4           \n\t"
            "sd   t3, 0(t5)            \n\t"
            "fence rw, rw              \n\t"
            ::: "t2", "t3", "t4", "t5", "memory"
        );
        asm volatile("csrw 0x800, x0");

        for (volatile int d = 0; d < 1000; d++);
        asm volatile("fence rw, rw" ::: "memory");

        printf("Results (0xCAFE=MulDiv never wrote, 0=wrong operand, 35=correct):\n");
        for (int i = 0; i < NUM_CORES; i++) {
            uint64_t r = results[i][0];
            printf("  Hart %d: 0x%lx (%ld)\n", i, (unsigned long)r, (long)r);
        }

        if (results[1][0] == 35) printf("\n*** PASSED ***\n");
        else printf("\n*** FAILED ***\n");
    } else {
        while (1) asm volatile("wfi");
    }
    return 0;
}
