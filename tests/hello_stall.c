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

// Bit definitions for CSR 0x800
#define SYSTOLIC_MASTER_CTRL (1 << 0) // Stall Bit

#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  __tmp; })

#define write_csr(reg, val) ({ \
  asm volatile ("csrw " #reg ", %0" :: "rK"(val)); })

volatile uint32_t barrier = 0;
volatile uint32_t sync_flag = 0;

int main(void) {
    uint64_t hartid;
    asm volatile("csrr %0, mhartid" : "=r"(hartid));

    if (hartid == 0) {
        // Init UART
        mmio_write32(UART0_BASE + 0x18, 867);
        mmio_write32(UART0_BASE + 0x08, 1);
        uart_puts("--------------------------------\n");
        uart_puts("Hardware Stall Demo (Fork-Join)\n");
        uart_puts("--------------------------------\n");
    }

    // Phase 1: Synchronization
    if (hartid != 0) {
        // Slaves wait for signal
        while (barrier == 0);
    } else {
        // Master waits a bit then releases barrier
        for (volatile int i = 0; i < 10000; i++);
        barrier = 1;
    }

    // Phase 2: The "Freeze"
    if (hartid == 0) {
        uart_puts("Master: Freezing Slaves (Asserting Stall)...\n");
        // Set Bit 0 -> Global Stall = 1
        write_csr(0x800, SYSTOLIC_MASTER_CTRL); 
        
        // At this point, Cores 1-3 should be clock-gated (frozen).
        // Validating by changing shared state while they are 'asleep'
        sync_flag = 0xDEADBEEF;
        
        uart_puts("Master: Doing work while slaves are frozen...\n");
        for (volatile int i = 0; i < 5000; i++); // Pretend work

        uart_puts("Master: Unfreezing Slaves (Releasing Stall)...\n");
        // Clear Bit 0 -> Global Stall = 0
        write_csr(0x800, 0); 
    } else {
        // Slaves loop waiting for sync_flag
        // If effective, they won't see intermediate states, only the final state when woken
        while (sync_flag == 0);
        
        // Once un-stalled and seeing flag:
        uart_puts("Slave: Woke up! Saw flag update.\n");
    }
    
    // Final Barrier
    if (hartid == 0) {
       for (volatile int i = 0; i < 10000; i++);
       uart_puts("Demo Complete.\n");
    }

    while(1);
    return 0;
}
