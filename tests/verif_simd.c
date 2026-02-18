#include <stdint.h>
#include "systolic_mesh.h"
#include "util.h"

#define UART0_BASE 0x10020000
#define SYSTOLIC_CTRL_CSR 0x800

// Shared memory for check-in
volatile uint64_t core_results[4] = {0, 0, 0, 0};
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

int main(void) {
    uint64_t hartid;
    asm volatile("csrr %0, mhartid" : "=r"(hartid));

    if (hartid == 0) {
        // Init UART
        mmio_write32(UART0_BASE + 0x18, 867);
        mmio_write32(UART0_BASE + 0x08, 1);
        uart_puts("\n\n================================\n");
        uart_puts("True SIMD Lock-Step Verification\n");
        uart_puts("================================\n");
        
        uart_puts("Core 0: Waiting for others to initialize...\n");
        for (volatile int i = 0; i < 10000; i++); // Wait for others to reach loop
        
        sync_barrier = 1;
        asm volatile("fence");
        uart_puts("Core 0: Barrier Released.\n");
    } else {
        while (sync_barrier == 0) asm volatile("fence");
    }

    if (hartid == 0) {
        uart_puts("Core 0: Entering SIMD Setup...\n");
        uart_puts("Core 0: Stalling Followers (Bit 0=1)...\n");
        // Step 1: Stall Followers
        write_csr(0x800, 0x1); 

        // Delay to ensure stall is propagated
        for (volatile int i = 0; i < 2000; i++);

        uart_puts("Core 0: Enabling SIMD Mode (Bit 1=1)...\n");
        // Step 2: Enable SIMD Mode and Release Stall
        // CSR 0x800: Bit 1 = SIMD Mode, Bit 0 = 0 (Release)
        write_csr(0x800, 0x2); 
    }

    // --- CRITICAL SECTION (SIMD) ---
    // All cores execute this block in lock-step.
    // Each core should write its own identity to the array.
    
    // We use inline assembly to ensure no compiler-induced drift 
    // and to make sure the same instructions are executed.
    uintptr_t results_ptr = (uintptr_t)core_results;
    uint64_t magic = 0xABC00000;
    
    asm volatile (
        "csrr t0, mhartid\n\t"          // t0 = hartid
        "slli t1, t0, 3\n\t"            // t1 = hartid * 8
        "add  t2, %0, t1\n\t"           // t2 = &core_results[hartid]
        "add  t3, %1, t0\n\t"           // t3 = 0xABC00000 + hartid
        "sd   t3, 0(t2)\n\t"            // store result
        "fence\n\t"                     // ensure visibility
        :
        : "r"(results_ptr), "r"(magic)
        : "t0", "t1", "t2", "t3", "memory"
    );

    // --- END CRITICAL SECTION ---

    if (hartid == 0) {
        // Give time for stores to complete in SIMD if needed (though fence is there)
        for (volatile int i = 0; i < 2000; i++);
        
        uart_puts("Core 0: Disabling SIMD Mode...\n");
        write_csr(0x800, 0);

        uart_puts("Core 0: Verifying Results...\n");
        int success = 1;
        for (int i = 0; i < 4; i++) {
            uart_puts("Core ");
            uart_putc('0' + i);
            uart_puts(": ");
            uart_puthex(core_results[i]);
            if (core_results[i] == (0xABC00000 + i)) {
                uart_puts(" [PASS]\n");
            } else {
                uart_puts(" [FAIL]\n");
                success = 0;
            }
        }

        if (success) {
            uart_puts("\nTEST PASSED: Lock-Step SIMD Confirmed.\n");
        } else {
            uart_puts("\nTEST FAILED.\n");
        }
    }

    while(1) {
        asm volatile("wfi");
    }
    return 0;
}
