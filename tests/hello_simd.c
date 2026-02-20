#include <stdint.h>
#include "systolic_mesh.h"
#include "util.h"

#define UART0_BASE 0x10020000

// Simple unrolled print to avoid loops/branches in SIMD mode
void blind_putc(char c) {
    // Write directly to TXDATA. 
    // Followers: Branch logic is disabled (NOP), so they won't check ready bit.
    // Leader: Will check ready bit if compiled with branches, but for this test
    //         we assume FIFO has space or we allow overwrite.
    *(volatile uint32_t*)UART0_BASE = c;
    
    // Tiny delay to maybe help UART settle (executed as ALUs)
    asm volatile("nop; nop; nop; nop;");
}

/*
 * Hello World from all cores.
 * 
 * In SIMD Mode (Constrained):
 * - MEM is enabled (Stores work)
 * - BRANCH is disabled (jumps/branches become NOPs)
 * 
 * Strategy:
 * - Unroll the print so it's a linear sequence of instructions.
 * - Followers 'blind fire' into the UART.
 * - Leader controls the flow.
 */
int main(void) {
    uint64_t hartid;
    asm volatile("csrr %0, mhartid" : "=r"(hartid));

    if (hartid == 0) {
        // Init UART
        *(volatile uint32_t*)(UART0_BASE + 0x18) = 867;
        *(volatile uint32_t*)(UART0_BASE + 0x08) = 1;
        
        blind_putc('\n');
        blind_putc('S'); blind_putc('I'); blind_putc('M'); blind_putc('D');
        blind_putc(' '); blind_putc('S'); blind_putc('t'); blind_putc('a'); blind_putc('r'); blind_putc('t');
        blind_putc('\n');
        
        // Wait for others
        for (volatile int i = 0; i < 50000; i++);
        
        // Enable SIMD Mode (Bit 1=1) + Clear Stall (Bit 0=0)
        asm volatile("csrw 0x800, %0" : : "r"(2));
        
        // --- SIMD REGION (Linear Execution) ---
        
        // All cores run this. 
        // Logic: "H" + hartid.
        // Hart 0 prints 'H', Hart 1 prints 'I', etc.
        char my_char = 'A' + hartid;
        blind_putc(my_char);
        blind_putc(' ');
        
        // --- END SIMD REGION ---
        
        // Disable SIMD
        asm volatile("csrw 0x800, x0");
        
        blind_putc('\n');
        blind_putc('D'); blind_putc('o'); blind_putc('n'); blind_putc('e');
        blind_putc('\n');
        
        // Spin forever
        while(1);
    } else {
        // Followers just spin. 
        // When SIMD is enabled, they are hijacked into the leader's stream.
        // They will execute the 'blind_putc' instructions.
        while(1);
    }
    
    return 0;
}
