#include <stdint.h>
#include "systolic_mesh.h"
#include "util.h"

#define UART0_BASE 0x10020000
#define SYSTOLIC_CTRL_CSR 0x800

#define NCORES 16

// Shared memory for check-in
volatile uint64_t core_results[NCORES] = {0};
volatile uint64_t sync_barrier = 0;

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

void uart_puthex(uint64_t h) {
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        int d = h & 0xf;
        buf[i] = d < 10 ? '0' + d : 'a' + d - 10;
        h >>= 4;
    }
    buf[16] = '\0';
    uart_puts(buf);
}

void uart_putdec(uint64_t n) {
    if (n == 0) {
        uart_putc('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        uart_putc(buf[j]);
    }
}

int main(void) {
    uint64_t hartid;
    asm volatile("csrr %0, mhartid" : "=r"(hartid));

    // Initialize specific registers for the test
    // t3 will be our accumulator
    // Set seed based on hartid
    uint64_t seed = 0x100 + hartid;
    asm volatile("mv t3, %0" : : "r"(seed));

    if (hartid == 0) {
        // Init UART
        mmio_write32(UART0_BASE + 0x18, 867);
        mmio_write32(UART0_BASE + 0x08, 1);
        uart_puts("\n\n================================\n");
        uart_puts("Constrained SIMD Verification\n");
        uart_puts("================================\n");
        
        uart_puts("Core 0: Waiting for others to initialize...\n");
        for (volatile int i = 0; i < 50000; i++); // Wait for others to reach barrier
        
        // DO NOT Release barrier yet.
    } else {
        // Followers spin here.
        // In SIMD mode, they will be hijacked from this loop.
        // Upon exit, they will resume this loop until barrier is 1.
        while (sync_barrier == 0);
    }

    if (hartid == 0) {
        uart_puts("Core 0: Enabling Global SIMD Mode (Bit 1)...\n");
        // Enable SIMD Mode (Bit 1 = 1). Bit 0 (Stall) = 0.
        // This drives global `systolic_enable` -> `RocketCore.io.systolic_enable`
        write_csr(0x800, 0x2);
        
        // Delay to ensure mode propagates
        for (volatile int i = 0; i < 1000; i++) asm volatile("nop");

        uart_puts("Core 0: Broadcasting ALU ops...\n");
        
        // --- SIMD REGION ---
        // Only safe ALU operations here.
        // No memory, branches, CSRs, or fences.
        
        // 1. Add 1 to t3
        // Leader: t3 = 0x100 + 1 = 0x101
        // Follower: t3 = (0x100+id) + 1
        asm volatile("addi t3, t3, 1");
        
        // 2. Double t3
        // Leader: 0x101 * 2 = 0x202
        // Follower: (0x100+id+1) * 2
        asm volatile("add t3, t3, t3");
        
        // 3. Xor with magic constant
        // 0x555
        asm volatile("xori t3, t3, 0x555");
        
        // --- END SIMD REGION ---
        
        uart_puts("Core 0: Disabling SIMD Mode...\n");
        write_csr(0x800, 0);
        
        // Delay for propagation
        for (volatile int i = 0; i < 1000; i++) asm volatile("nop");
        
        uart_puts("Core 0: Releasing Barrier...\n");
        sync_barrier = 1;
        asm volatile("fence");
    } else {
        // Followers wait for barrier to be 1 (after SIMD session)
        while (sync_barrier == 0);
    }
    
    // Post-SIMD Verification
    // All cores dump their t3 to memory
    uintptr_t results_ptr = (uintptr_t)core_results;
    asm volatile (
        "csrr t0, mhartid\n\t"          // t0 = hartid
        "slli t1, t0, 3\n\t"            // t1 = hartid * 8
        "add  t2, %0, t1\n\t"           // t2 = &core_results[hartid]
        "sd   t3, 0(t2)\n\t"            // store t3 (result)
        "fence\n\t"
        :
        : "r"(results_ptr)
        : "t0", "t1", "t2", "memory"
    );
    
    // Leader verifies
    if (hartid == 0) {
        for (volatile int i = 0; i < 2000; i++); // Wait for stores
        
        int success = 1;
        uart_puts("\nResults:\n");
        for (int i = 0; i < NCORES; i++) {
            uart_puts("Core ");
            uart_putdec(i);
            uart_puts(": ");
            uart_puthex(core_results[i]);
            
            // Expected: ((0x100 + id + 1) * 2) ^ 0x555
            uint64_t expected = ((0x100 + i + 1) * 2) ^ 0x555;
            
            if (core_results[i] == expected) {
                uart_puts(" [PASS]");
            } else {
                uart_puts(" [FAIL] Exp: ");
                uart_puthex(expected);
                success = 0;
            }
            uart_puts("\n");
        }
        
        if (success) {
            uart_puts("\nTEST PASSED: Constrained SIMD verified.\n");
        } else {
            uart_puts("\nTEST FAILED.\n");
        }
        
        // Stop
        while(1);
    } else {
        while(1);
    }

    return 0;
}
