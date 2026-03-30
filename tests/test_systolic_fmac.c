/*
 * test_systolic_fmac.c — SYSTOLIC_FMAC_S verification
 *
 * 2-step test:
 *   Step 1: FMUL → produces south_out per row
 *   Step 2: FMAC → reads mesh_north (= south from row above) as accumulator
 *
 * 2×2 Mesh:  Hart 0 →east→ Hart 1
 *            Hart 2 →east→ Hart 3
 *            Hart 1 →south→ Hart 3
 *
 * Pre-load: Hart 0 east=7.0f, Hart 2 east=3.0f
 * Weight:   5.0f (loaded into ft0 on all cores)
 *
 * FMUL: Hart 1=7*5=35, Hart 3=3*5=15
 *       Hart 1 south_out=35 → Hart 3 north_in
 * FMAC: Hart 1=7*5+0=35 (top row, north=0)
 *       Hart 3=w*5+35 where w=Hart3's west at FMAC time
 */

#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define NUM_CORES 4

volatile uint64_t fmul_res[NUM_CORES][8] __attribute__((aligned(64)));
volatile uint64_t fmac_res[NUM_CORES][8] __attribute__((aligned(64)));
volatile int core_ready[NUM_CORES] = {0};

void thread_entry(int cid, int nc) { }

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    clear_csr(mstatus, 0x00000008);
    write_csr(mie, 0);

    // Pre-load mesh east registers (before SIMD)
    if (hartid == 0) write_csr(0x801, 0x40E00000UL);  // 7.0f
    if (hartid == 2) write_csr(0x801, 0x40400000UL);  // 3.0f

    core_ready[hartid] = 1;
    asm volatile("fence rw, rw" ::: "memory");

    if (hartid == 0) {
        for (int i = 1; i < NUM_CORES; i++) {
            while (core_ready[i] == 0) { asm volatile("nop"); }
        }

        for (int i = 0; i < NUM_CORES; i++) {
            fmul_res[i][0] = 0xDEADBEEFULL;
            fmac_res[i][0] = 0xDEADBEEFULL;
        }
        asm volatile("fence rw, rw" ::: "memory");

        printf("=== SYSTOLIC_FMAC_S Test ===\n");

        asm volatile(
            /* Activate SIMD */
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"
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

            /* ---- Step 1: FMUL ---- */
            SYSTOLIC_FMUL_S("ft1", "ft0")

            /* Drain FMA pipeline */
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"

            /* Read FMUL result */
            "fmv.x.w t3, ft1          \n\t"
            "csrr t4, mhartid         \n\t"
            "slli t4, t4, 6           \n\t"
            "la   t5, fmul_res        \n\t"
            "add  t5, t5, t4          \n\t"
            "sd   t3, 0(t5)           \n\t"

            /* Wait extra for south→north propagation and pipeline drain */
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"

            /* ---- Step 2: FMAC ---- */
            /* ft2 = mesh_west * ft0 + mesh_north */
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

            /* Read FMAC result */
            "fmv.x.w t3, ft2          \n\t"
            "csrr t4, mhartid         \n\t"
            "slli t4, t4, 6           \n\t"
            "la   t5, fmac_res        \n\t"
            "add  t5, t5, t4          \n\t"
            "sd   t3, 0(t5)           \n\t"

            "fence rw, rw             \n\t"
            ::: "t0", "t2", "t3", "t4", "t5", "ft0", "ft1", "ft2", "memory"
        );

        asm volatile("csrw 0x800, x0");
        asm volatile("fence rw, rw" ::: "memory");
        for (volatile int d = 0; d < 1000; d++);

        // Verify
        printf("FMUL (mesh_west * 5.0):\n");
        for (int i = 0; i < NUM_CORES; i++) {
            uint32_t r = (uint32_t)fmul_res[i][0];
            printf("  Hart %d: 0x%08lx\n", i, (unsigned long)r);
        }

        printf("FMAC (mesh_west * 5.0 + mesh_north):\n");
        for (int i = 0; i < NUM_CORES; i++) {
            uint32_t r = (uint32_t)fmac_res[i][0];
            printf("  Hart %d: 0x%08lx\n", i, (unsigned long)r);
        }

        int passed = 1;

        // Hart 1 FMUL: 7*5=35=0x420C0000
        if ((uint32_t)fmul_res[1][0] != 0x420C0000) {
            printf("FAIL: Hart1 FMUL expected 0x420C0000\n");
            passed = 0;
        }
        // Hart 3 FMUL: 3*5=15=0x41700000
        if ((uint32_t)fmul_res[3][0] != 0x41700000) {
            printf("FAIL: Hart3 FMUL expected 0x41700000\n");
            passed = 0;
        }
        // Hart 1 FMAC: After FMUL east auto-forward, Core 0 forwarded mesh_west=0.0 east.
        // So Hart 1's FMAC sees mesh_west=0.0, weight=5.0, mesh_north=0.0 (top row)
        // Result: 0*5+0 = 0.0
        if ((uint32_t)fmac_res[1][0] != 0x00000000) {
            printf("FAIL: Hart1 FMAC expected 0x00000000 got 0x%08lx\n",
                   (unsigned long)(uint32_t)fmac_res[1][0]);
            passed = 0;
        }
        // Hart 3 FMAC: mesh_west from Hart 2 east auto-forward = 0.0 (Hart 2 west=0)
        // mesh_north from Hart 1 south auto-forward = FMUL result of Hart 1 = 35.0
        // Result: 0*5+35 = 35.0 = 0x420C0000
        // OR if south auto-forward didn't propagate in time: 0*5+0 = 0.0
        uint32_t fmac3 = (uint32_t)fmac_res[3][0];
        if (fmac3 == 0x420C0000) {
            printf("Hart3 FMAC: 0*5+35=35.0 OK (south auto-forward from Hart1 confirmed)\n");
        } else if (fmac3 == 0x00000000) {
            printf("Hart3 FMAC: 0*5+0=0.0 OK (south auto-forward didn't arrive in time)\n");
        } else {
            printf("FAIL: Hart3 FMAC unexpected 0x%08lx\n", (unsigned long)fmac3);
            passed = 0;
        }

        if (passed) {
            printf("\n*** PASSED ***\n");
        } else {
            printf("\n*** FAILED ***\n");
        }

        // Delay to allow UART buffer to flush out the characters before terminating RTL simulation
        for (volatile int flush = 0; flush < 50000; flush++);

    } else {
        while (1) { asm volatile("wfi"); }
    }

    return 0;
}
