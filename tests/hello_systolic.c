#include <stdint.h>

#define UART0_BASE 0x10020000UL
#define UART_TXDATA (UART0_BASE + 0x00)
#define UART_RXDATA (UART0_BASE + 0x04)
#define UART_TXCTRL (UART0_BASE + 0x08)
#define UART_RXCTRL (UART0_BASE + 0x0C)
#define UART_IE (UART0_BASE + 0x10)
#define UART_IP (UART0_BASE + 0x14)
#define UART_DIV (UART0_BASE + 0x18)

static inline void mmio_write32(uintptr_t addr, uint32_t val) {
  *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uintptr_t addr) {
  return *(volatile uint32_t *)addr;
}

static inline void uart_init(void) {
  // Enable TX and RX (bit0 enable). Leave watermark bits at 0.
  mmio_write32(UART_TXCTRL, 1);
  mmio_write32(UART_RXCTRL, 1);

  // Optional: set divisor if your platform doesn't pre-init it.
  // If you don't know the clock, DON'T guess. Leave it alone for now.
  // mmio_write32(UART_DIV, <div>);
}

static inline void uart_putc(char c) {
  // TXDATA[31] == FULL when 1.
  while (mmio_read32(UART_TXDATA) & 0x80000000u) {
  }
  mmio_write32(UART_TXDATA, (uint32_t)(uint8_t)c);
  asm volatile("fence iorw, iorw" ::: "memory");
}

/* Trap handler required by crt.S */
void handle_trap(uintptr_t cause, uintptr_t epc, uintptr_t regs[32]) {
  while (1)
    ; // Spin on trap
}

volatile uint32_t lock = 0;

void acquire_lock() {
  while (__sync_lock_test_and_set(&lock, 1)) {
    // Spin
  }
}

void release_lock() { __sync_lock_release(&lock); }

void print_hex(uint64_t val) {
  char hex[] = "0123456789ABCDEF";
  for (int i = 60; i >= 0; i -= 4) {
    uart_putc(hex[(val >> i) & 0xF]);
  }
}

int main(void) {
  uint64_t hartid;
  asm volatile("csrr %0, mhartid" : "=r"(hartid));

  // Initialize UART only once (core 0)
  if (hartid == 0) {
    uart_init();
    // Signal ready? For now just assume it's fast enough.
  }

  // Simple spin-wait to let core 0 init
  for (volatile int i = 0; i < 10000; i++)
    ;

  acquire_lock();
  uart_putc('C');
  uart_putc('o');
  uart_putc('r');
  uart_putc('e');
  uart_putc(' ');
  print_hex(hartid);
  uart_putc('\n');
  release_lock();

  for (;;)
    asm volatile("wfi");
}
