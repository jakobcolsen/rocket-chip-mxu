/*
 * test_fmac_only.c — Minimal FMAC-only test
 *
 * Pre-load Hart 0's east register with 7.0, then do FMAC in SIMD.
 * No FMUL first — only FMAC, so no auto-forward contamination.
 *
 * FMAC = mesh_west * weight + mesh_north
 *   Hart 0: west=0.0 (left edge), north=0.0 (top edge), weight=5.0
 *           Result: 0*5+0 = 0.0
 *   Hart 1: west=7.0 (from Hart 0 east preload), north=0.0 (top edge), weight=5.0
 *           Result: 7*5+0 = 35.0 = 0x420C0000
 */

#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define NUM_CORES 4

volatile uint64_t results[NUM_CORES][8] __attribute__((aligned(64)));
volatile int core_ready[NUM_CORES] = {0};

void thread_entry(int cid, int nc) { }

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    clear_csr(mstatus, 0x00000008);
    write_csr(mie, 0);

    // Enable FPU BEFORE SIMD (so followers already have FPU enabled)
    set_csr(mstatus, 0x00006000);

    // Pre-load mesh east register
    if (hartid == 0) write_csr(0x801, 0x40E00000UL);  // 7.0f

    core_ready[hartid] = 1;
    asm volatile("fence rw, rw" ::: "memory");

    if (hartid == 0) {
        for (int i = 1; i < NUM_CORES; i++) {
            while (core_ready[i] == 0) { asm volatile("nop"); }
        }

        for (int i = 0; i < NUM_CORES; i++) {
            results[i][0] = 0xDEADBEEFULL;
        }
        asm volatile("fence rw, rw" ::: "memory");

        printf("=== FMAC-Only Test ===\n");

        asm volatile(
            /* Activate SIMD */
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"

            /* Ensure FPU enabled on all cores */
            "li   t2, 0x6000           \n\t"
            "csrs mstatus, t2          \n\t"

            /* Load weight 5.0f into ft0 */
            "li   t0, 0x40A00000       \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "fmv.w.x ft0, t0           \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"

            /* FMAC: ft2 = mesh_west * ft0 + mesh_north */
            SYSTOLIC_FMAC_S("ft2", "ft0")

            /* Drain FMA pipeline */
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"

            /* Read result */
            "fmv.x.w t3, ft2          \n\t"
            "csrr t4, mhartid         \n\t"
            "slli t4, t4, 6           \n\t"
            "la   t5, results         \n\t"
            "add  t5, t5, t4          \n\t"
            "sd   t3, 0(t5)           \n\t"

            "fence rw, rw             \n\t"
            ::: "t0", "t2", "t3", "t4", "t5", "ft0", "ft2", "memory"
        );

        asm volatile("csrw 0x800, x0");
        asm volatile("fence rw, rw" ::: "memory");
        for (volatile int d = 0; d < 1000; d++);

        printf("FMAC results:\n");
        int passed = 1;
        for (int i = 0; i < NUM_CORES; i++) {
            uint32_t r = (uint32_t)results[i][0];
            printf("  Hart %d: 0x%08lx\n", i, (unsigned long)r);
        }

        // Hart 1: 7.0 * 5.0 + 0.0 = 35.0 = 0x420C0000
        uint32_t h1 = (uint32_t)results[1][0];
        if (h1 != 0x420C0000) {
            printf("FAIL: Hart1 expected 0x420C0000 got 0x%08lx\n", (unsigned long)h1);
            passed = 0;
        }

        // Hart 0: 0 * 5 + 0 = 0
        uint32_t h0 = (uint32_t)results[0][0];
        if (h0 != 0x00000000) {
            printf("FAIL: Hart0 expected 0x00000000 got 0x%08lx\n", (unsigned long)h0);
            passed = 0;
        }

        if (passed) {
            printf("\n*** PASSED ***\n");
        } else {
            printf("\n*** FAILED ***\n");
        }

        // Delay for UART flush
        for (volatile int flush = 0; flush < 50000; flush++);

    } else {
        while (1) { asm volatile("wfi"); }
    }

    return 0;
}
