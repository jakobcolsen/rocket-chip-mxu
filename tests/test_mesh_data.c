/*
 * test_mesh_data.c — Verify East→West mesh data path during SIMD
 *
 * Phase 1 (MIMD): Hart 0 writes East=7 via CSR 0x801
 * Phase 2 (SIMD): All cores read CSR 0x801 (mesh_west) and store results
 *   Hart 0: should read 0 (left edge)
 *   Hart 1: should read 7 (from Hart 0's East)
 */
#include <stdio.h>
#include <stdint.h>

#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); __tmp; })
#define write_csr(reg, val) ({ \
  asm volatile ("csrw " #reg ", %0" :: "rK"(val)); })
#define clear_csr(reg, bit) ({ unsigned long __tmp; \
  asm volatile ("csrrc %0, " #reg ", %1" : "=r"(__tmp) : "rK"(bit)); __tmp; })

#define MSTATUS_MIE 0x00000008
#define NUM_CORES 4

volatile uint64_t results[NUM_CORES][8] __attribute__((aligned(64)));
volatile int core_ready[NUM_CORES] = {0};

void thread_entry(int cid, int nc) { }

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    /* Phase 1: Hart 0 pre-loads East=7 */
    if (hartid == 0) {
        write_csr(0x801, 7);
    }

    core_ready[hartid] = 1;
    asm volatile("fence rw, rw" ::: "memory");

    if (hartid == 0) {
        for (int i = 1; i < NUM_CORES; i++)
            while (core_ready[i] == 0) asm volatile("nop");

        for (int i = 0; i < NUM_CORES; i++) results[i][0] = 0xDEAD;
        asm volatile("fence rw, rw" ::: "memory");

        printf("=== Mesh Data Path Test ===\n");
        printf("Hart 0 wrote East=7\n\n");

        /* Phase 2: SIMD — all cores read CSR 0x801 (mesh_west) */
        asm volatile(
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"
            /* Read mesh_west via CSR 0x801 */
            "csrr t3, 0x801            \n\t"
            /* Store to results[hartid] */
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

        printf("Results (CSR 0x801 = mesh_west):\n");
        int passed = 1;
        const uint64_t expected[NUM_CORES] = {0, 7, 0, 0};
        for (int i = 0; i < NUM_CORES; i++) {
            uint64_t r = results[i][0];
            printf("  Hart %d: mesh_west=%ld", i, (long)r);
            if (r != expected[i]) {
                printf(" (expected %ld) MISMATCH", (long)expected[i]);
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
