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

    # 4. Set up stack pointer (8KB per core)
    # Use 0x80100000 (1MB offset) to be safe within smaller DRAM configs
    csrr a0, mhartid
    li  t0, 0x2000      # 8KB stack size per hart
    mul t1, a0, t0
    li  sp, 0x80100000  # Stack base (Top of stack for Hart 0)
    sub sp, sp, t1      # sp = 0x80100000 - (hartid * 8KB)

    # 5. Call _init (which calls thread_entry then main)
    # a0 = hartid, a1 = num_harts (approximate)
    csrr a0, mhartid
    li a1, 4
    jal ra, _init

    # 6. Exit with return value of main
    jal ra, exit

.section .tohost,"aw",@progbits
.align 6
.global tohost
tohost: .quad 0
.global fromhost
fromhost: .quad 0
