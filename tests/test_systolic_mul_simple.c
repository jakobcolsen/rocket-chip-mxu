/*
 * test_systolic_mul_simple.c — Minimal single-core test for SYSTOLIC_MUL decode
 * 
 * Tests whether SYSTOLIC_MUL decodes and executes correctly on Hart 0 alone,
 * WITHOUT any SIMD mode. mesh_west=0 (left edge), so result should be 0.
 * If we get an illegal instruction trap, something is wrong with the decode.
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

/* SYSTOLIC_MUL rd, rs1: rd = mesh_west * rs1 */
#define SYSTOLIC_MUL(rd, rs1)  \
    ".insn r 0x5b, 0x0, 0x00, " rd ", " rs1 ", x0\n\t"

void thread_entry(int cid, int nc) { }

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    if (hartid != 0) {
        while (1) { asm volatile("wfi"); }
    }

    printf("=== SYSTOLIC_MUL Simple Decode Test ===\n");

    /* Test 1: Execute SYSTOLIC_MUL in isolation (no SIMD) */
    uint64_t result = 0xDEADBEEF;
    printf("Test 1: SYSTOLIC_MUL without SIMD (mesh_west should be 0)\n");
    
    asm volatile(
        "li   t2, 5                \n\t"
        SYSTOLIC_MUL("t3", "t2")
        "mv   %0, t3              \n\t"
        : "=r"(result)
        :
        : "t2", "t3"
    );
    
    printf("  Result: %ld (expected 0, mesh_west=0)\n", (long)result);
    
    /* Test 2: Check that a normal MUL still works */
    uint64_t mul_result = 0;
    asm volatile(
        "li   t2, 5                \n\t"
        "li   t3, 7                \n\t"
        "mul  t4, t2, t3           \n\t"
        "mv   %0, t4              \n\t"
        : "=r"(mul_result)
        :
        : "t2", "t3", "t4"
    );
    printf("  Normal MUL: 5*7=%ld (expected 35)\n", (long)mul_result);

    /* Test 3: Pre-load East, check if mesh_west is visible via CSR read */
    write_csr(0x801, 42);  /* Write East=42 (this core's east output) */
    uint64_t west_val;
    asm volatile("csrr %0, 0x801" : "=r"(west_val));
    printf("  CSR 0x801 read after write: %ld\n", (long)west_val);

    if (result == 0 && mul_result == 35) {
        printf("\n*** PASSED ***\n");
    } else {
        printf("\n*** FAILED ***\n");
    }

    return 0;
}
