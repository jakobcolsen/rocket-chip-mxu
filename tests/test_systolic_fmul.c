/*
 * test_systolic_fmul.c — Debug Test for SYSTOLIC_FMUL_S
 *
 * Simplified: 
 *   Phase 1: MIMD - preload mesh east
 *   Phase 2: SIMD - load weight, execute SYSTOLIC_FMUL_S, store result + debug info
 *   Phase 3: Verify
 */

#include <stdio.h>
#include <stdint.h>
#include "systolic_mesh.h"

#define MSTATUS_MIE 0x00000008
#define NUM_CORES 4

// Results buffer: each core gets its own cache line (8 words)
// [0] = result (fmv.x.w ft1), [1] = weight (fmv.x.w ft0), [2] = debug flags
volatile uint64_t results[NUM_CORES][8] __attribute__((aligned(64)));
volatile int core_ready[NUM_CORES] = {0};

void thread_entry(int cid, int nc) { }

int main(void) {
    uint64_t hartid = read_csr(mhartid);

    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    // Enable FPU (mstatus.FS = Initial)
    set_csr(mstatus, 0x00006000);

    // Phase 1: Pre-load mesh data
    if (hartid == 0) {
        write_csr(0x801, 0x40E00000UL);  // IEEE 7.0f
    }

    core_ready[hartid] = 1;
    asm volatile("fence rw, rw" ::: "memory");

    if (hartid == 0) {
        for (int i = 1; i < NUM_CORES; i++) {
            while (core_ready[i] == 0) { asm volatile("nop"); }
        }

        for (int i = 0; i < NUM_CORES; i++) {
            results[i][0] = 0xDEADBEEFULL;
            results[i][1] = 0xDEADBEEFULL;
            results[i][2] = 0xDEADBEEFULL;
        }
        asm volatile("fence rw, rw" ::: "memory");

        printf("=== SYSTOLIC_FMUL_S Debug Test ===\n");

        // Phase 2: SIMD
        asm volatile(
            /* Activate SIMD */
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"

            /* Enable FPU on ALL cores (set mstatus.FS = Initial) */
            "li   t2, 0x6000           \n\t"
            "csrs mstatus, t2          \n\t"

            /* Read and store mstatus for debug */
            "csrr t6, mstatus          \n\t"
            "csrr t4, mhartid          \n\t"
            "slli t4, t4, 6            \n\t"
            "la   t5, results          \n\t"
            "add  t5, t5, t4           \n\t"
            "sd   t6, 24(t5)           \n\t"  /* results[hart][3] = mstatus */

            /* Load weight 5.0f into ft0 via integer path */
            "li   t0, 0x40A00000       \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"

            /* Debug: store t0 to results[hart][4] */
            "csrr t4, mhartid          \n\t"
            "slli t4, t4, 6            \n\t"
            "la   t5, results          \n\t"
            "add  t5, t5, t4           \n\t"
            "sd   t0, 32(t5)           \n\t"  /* results[hart][4] = t0 */

            "fmv.w.x ft0, t0           \n\t"

            /* NOPs to let fmv.w.x complete through FPU pipeline (latency=2) */
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"

            /* Store debug: what did ft0 get? */
            "fmv.x.w t1, ft0           \n\t"
            "csrr t4, mhartid          \n\t"
            "slli t4, t4, 6            \n\t"
            "la   t5, results          \n\t"
            "add  t5, t5, t4           \n\t"
            "sd   t1, 8(t5)            \n\t"  /* results[hart][1] = ft0 bits */

            /* SYSTOLIC_FMUL_S ft1, ft0 */
            SYSTOLIC_FMUL_S("ft1", "ft0")

            /* NOPs to let FMA complete (latency=3-4) */
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"
            "nop \n\t"

            /* Move FP result to integer register */
            "fmv.x.w t3, ft1          \n\t"

            /* Store result */
            "csrr t4, mhartid         \n\t"
            "slli t4, t4, 6           \n\t"
            "la   t5, results         \n\t"
            "add  t5, t5, t4          \n\t"
            "sd   t3, 0(t5)           \n\t"  /* results[hart][0] = ft1 result */

            /* Store debug flag */
            "li   t3, 0xCAFE          \n\t"
            "sd   t3, 16(t5)          \n\t"  /* results[hart][2] = marker */

            "fence rw, rw             \n\t"
            ::: "t0", "t1", "t2", "t3", "t4", "t5", "ft0", "ft1", "memory"
        );

        // Deactivate SIMD
        asm volatile("csrw 0x800, x0");

        // Drain
        asm volatile("fence rw, rw" ::: "memory");
        for (volatile int d = 0; d < 1000; d++);

        // Phase 3: Verify
        printf("Results:\n");
        int passed = 1;
        for (int i = 0; i < NUM_CORES; i++) {
            uint32_t result = (uint32_t)results[i][0];
            uint32_t weight = (uint32_t)results[i][1];
            uint32_t marker = (uint32_t)results[i][2];
            uint64_t mstat  = results[i][3];
            uint32_t t0val  = (uint32_t)results[i][4];
            printf("  Hart %d: res=0x%08lx wt=0x%08lx t0=0x%08lx mk=0x%lx ms=0x%lx\n",
                   i, (unsigned long)result, (unsigned long)weight, (unsigned long)t0val,
                   (unsigned long)marker, (unsigned long)mstat);
        }

        // Hart 0: 0.0*5.0=0.0, Hart 1: 7.0*5.0=35.0=0x420C0000
        uint32_t expected[NUM_CORES] = {0x00000000, 0x420C0000, 0x00000000, 0x00000000};
        for (int i = 0; i < NUM_CORES; i++) {
            uint32_t r = (uint32_t)results[i][0];
            if (r != expected[i]) {
                printf("  MISMATCH Hart %d: got=0x%08lx exp=0x%08lx\n",
                       i, (unsigned long)r, (unsigned long)expected[i]);
                passed = 0;
            }
        }

        if (passed) {
            printf("\n*** PASSED ***\n");
        } else {
            printf("\n*** FAILED ***\n");
        }

    } else {
        while (1) { asm volatile("wfi"); }
    }

    return 0;
}
