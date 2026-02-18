#include <stdint.h>

#define UART0_BASE 0x10020000UL

static inline void mmio_write32(uintptr_t addr, uint32_t val) {
  *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uintptr_t addr) {
  return *(volatile uint32_t *)addr;
}

static inline void uart_putc(char c) {
    while (mmio_read32(UART0_BASE + 0x00) & 0x80000000);
    mmio_write32(UART0_BASE + 0x00, c);
}

void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

void uart_puthexm(uint64_t val) {
    for (int i = 15; i >= 0; i--) {
        int nibble = (val >> (i * 4)) & 0xf;
        uart_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
}

// Custom CSRs
#define CSR_SYSTOLIC_CTRL 0x800
#define CSR_SYSTOLIC_DATA 0x801

// Bit definitions for CSR 0x800
#define SYSTOLIC_MASTER_CTRL (1 << 0)
#define SYSTOLIC_SIMD_MODE   (1 << 1)

#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  __tmp; })

#define write_csr(reg, val) ({ \
  asm volatile ("csrw " #reg ", %0" :: "rK"(val)); })

int main(void) {
    uint64_t hartid;
    asm volatile("csrr %0, mhartid" : "=r"(hartid));

    // --- PHASE 1: MIMD Mode (Standard Boot) ---
    if (hartid == 0) {
        mmio_write32(UART0_BASE + 0x18, 867);
        mmio_write32(UART0_BASE + 0x08, 1);
        uart_puts("Core 0: Booted in MIMD mode.\n");
    }

    // --- PHASE 2: Transition to SIMD mode ---
    // In this mode, ALU operands will be fetched from the systolic mesh
    if (hartid == 0) {
        uart_puts("Core 0: Engaging SYSTOLIC_SIMD_MODE (ALU Hijack)...\n");
        // Enable both Master Ctrl and SIMD Mode
        write_csr(0x800, SYSTOLIC_MASTER_CTRL | SYSTOLIC_SIMD_MODE);
        
        // At this point, subsequent instructions on ALL cores that 
        // use the ALU will begin drawing data from their neighbors.
        uart_puts("Core 0: Now in SIMD mode.\n");
    } else {
        // Other cores can also enable their local "SIMD" mode independently if desired,
        // or follow the global stall signal from Core 0.
        // For this test, we have Core 0 toggle the global enable/stall via the mesh.
    }

    // All cores loop forever
    while (1) {
        asm volatile ("nop");
    }
    return 0;
}
