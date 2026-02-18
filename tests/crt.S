
.section .text.init
.global _start
_start:
    # 1. Clear all registers
    li  ra, 0
    li  sp, 0
    li  gp, 0
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

    # 1.5 Set up trap vector
    la t0, handle_trap
    csrw mtvec, t0

    # 2. Set up stack pointer (8KB per core, starting from 0x80020000)
    csrr a0, mhartid
    li t0, 0x2000      # 8KB
    mul t1, a0, t0
    li sp, 0x80020000
    sub sp, sp, t1

    # 3. Jump to main
    tail main

.section .text
.global handle_trap
handle_trap:
    # For now, just loop forever on traps
    1: j 1b

.section .tohost,"aw",@progbits
.align 6
.global tohost
tohost: .quad 0
.global fromhost
fromhost: .quad 0
