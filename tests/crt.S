.section .text.init
.global _start
_start:
    # 1. Set up trap vector early
    la t0, handle_trap
    csrw mtvec, t0

    # 2. Initialize global pointer (gp)
    # This is critical for relaxation and small data access
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    # 3. Clear other registers (optional but good practice)
    li  ra, 0
    li  tp, 0
    li  t0, 0
    li  t1, 0
    li  t2, 0
    li  s0, 0
    li  s1, 0
    li  a0, 0
    li  a1, 0
    li  a2, 0
    li  a3, 0
    li  a4, 0
    li  a5, 0
    li  a6, 0
    li  a7, 0
    li  s2, 0
    li  s3, 0
    li  s4, 0
    li  s5, 0
    li  s6, 0
    li  s7, 0
    li  s8, 0
    li  s9, 0
    li  s10, 0
    li  s11, 0
    li  t3, 0
    li  t4, 0
    li  t5, 0
    li  t6, 0

    # 4. Set up stack pointer (8KB per core, starting from HIGH MEMORY)
    # We use 0x88000000 (128MB offset) to avoid colliding with text at 0x80000000
    csrr a0, mhartid
    li t0, 0x2000      # 8KB stack size per hart
    mul t1, a0, t0
    li sp, 0x88000000  # Stack base (Top of stack for Hart 0)
    sub sp, sp, t1     # sp = 0x88000000 - (hartid * 8KB)

    # 5. Jump to main
    tail main

.section .text
.global handle_trap
handle_trap:
    # Print trap info to UART (0x10020000)
    # This is a minimal debug handler
    li t0, 0x10020000
    
    # Check for reasons (mcause)
    csrr t1, mcause
    # (Simple print loop could go here if we had a function, 
    #  but we want to be minimal and robust)

    # Infinite loop to catch the trap
1:  j 1b

.section .tohost,"aw",@progbits
.align 6
.global tohost
tohost: .quad 0
.global fromhost
fromhost: .quad 0
