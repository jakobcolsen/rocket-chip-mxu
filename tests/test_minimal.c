#include <stdint.h>

#define UART0_BASE 0x10020000

void mmio_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}

void uart_putc(char c) {
    while ((*(volatile uint32_t*)(UART0_BASE + 0x00)) & 0x80000000);
    mmio_write32(UART0_BASE + 0x00, c);
}

void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

int main(void) {
    uint64_t hartid;
    asm volatile("csrr %0, mhartid" : "=r"(hartid));

    if (hartid == 0) {
        // Init UART
        mmio_write32(UART0_BASE + 0x18, 867);
        mmio_write32(UART0_BASE + 0x08, 1);
        uart_puts("\n\nMinimal UART Test Started\n");
        for (int i = 0; i < 10; i++) {
            uart_puts("Core 0 heartbeat...\n");
        }
        uart_puts("Minimal UART Test Complete\n");
    }

    while(1) {
        asm volatile("wfi");
    }
    return 0;
}
