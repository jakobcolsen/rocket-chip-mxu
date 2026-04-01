/*
 * common.h — Shared benchmark infrastructure
 *
 * Provides CSR helpers, timing macros, and verification utilities
 * for all MXU benchmarks. Include from tests/benchmarks/*.c.
 */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stdio.h>
#include <stdint.h>

/* ── CSR Helpers ──────────────────────────────────────────────────────── */

#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); __tmp; })
#define write_csr(reg, val) ({ \
  asm volatile ("csrw " #reg ", %0" :: "rK"(val)); })
#define clear_csr(reg, bit) ({ unsigned long __tmp; \
  asm volatile ("csrrc %0, " #reg ", %1" : "=r"(__tmp) : "rK"(bit)); __tmp; })
#define set_csr(reg, bit) ({ unsigned long __tmp; \
  asm volatile ("csrrs %0, " #reg ", %1" : "=r"(__tmp) : "rK"(bit)); __tmp; })

/* ── Constants ────────────────────────────────────────────────────────── */

#define NUM_CORES       4
#define MSTATUS_MIE     0x00000008
#define MSTATUS_FS_INIT 0x00006000

/* Default problem size if not set via -DBENCH_N=xxx */
#ifndef BENCH_N
#define BENCH_N 64
#endif

/* ── Benchmark Timing ─────────────────────────────────────────────────── */

static volatile uint64_t _bench_cyc0, _bench_cyc1;
static volatile uint64_t _bench_inst0, _bench_inst1;

#define BENCH_START() do { \
    asm volatile ("fence" ::: "memory"); \
    _bench_inst0 = read_csr(minstret); \
    _bench_cyc0  = read_csr(mcycle); \
} while(0)

#define BENCH_END() do { \
    _bench_cyc1  = read_csr(mcycle); \
    _bench_inst1 = read_csr(minstret); \
    asm volatile ("fence" ::: "memory"); \
} while(0)

#define BENCH_REPORT() do { \
    printf("CYCLES: %lu\n",  (unsigned long)(_bench_cyc1  - _bench_cyc0)); \
    printf("INSTRET: %lu\n", (unsigned long)(_bench_inst1 - _bench_inst0)); \
} while(0)

/* ── SIMD Helpers ─────────────────────────────────────────────────────── */

#define SIMD_ENABLE()  asm volatile ("csrw 0x800, %0" :: "r"(2))
#define SIMD_DISABLE() asm volatile ("csrw 0x800, x0")

/* Park follower cores after SIMD region ends */
#define SIMD_PARK_FOLLOWERS() \
    asm volatile ( \
        "csrr t0, mhartid \n\t" \
        "beqz t0, 1f      \n\t" \
        "2: wfi           \n\t" \
        "j 2b             \n\t" \
        "1:               \n\t" \
        ::: "t0" \
    )

/* Drain pipeline after SIMD region */
#define SIMD_DRAIN() do { \
    for (volatile int _d = 0; _d < 1000; _d++); \
    asm volatile ("fence rw, rw" ::: "memory"); \
} while(0)

/* ── Synchronization for MIMD ─────────────────────────────────────────── */

/* Busy-wait barrier using atomic counter */
static inline void mimd_barrier_wait(volatile int *done_cnt, int expected) {
    while (*done_cnt < expected) {
        asm volatile ("nop");
    }
    asm volatile ("fence rw, rw" ::: "memory");
}

/* ── Common setup — call from main() ──────────────────────────────────── */

static inline void bench_disable_interrupts(void) {
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);
}

/* Stub for crt.S thread_entry */
void thread_entry(int cid, int nc) { /* no-op */ }

/* ── Systolic custom instructions (from systolic_mesh.h) ──────────────── */

/* SYSTOLIC_MUL rd, rs1: rd = mesh_west * rs1 (integer) */
#define SYSTOLIC_MUL(rd, rs1) \
    ".insn r 0x5b, 0x0, 0x00, " rd ", " rs1 ", x0\n\t"

/* SYSTOLIC_FMUL_S fd, fs1: fd = mesh_west * fs1 (FP single) */
#define SYSTOLIC_FMUL_S(fd, fs1) \
    ".insn r 0x5b, 0x0, 0x04, " fd ", " fs1 ", f0\n\t"

/* SYSTOLIC_FMAC_S fd, fs1: fd = mesh_west * fs1 + mesh_north (FP FMA) */
#define SYSTOLIC_FMAC_S(fd, fs1) \
    ".insn r 0x5b, 0x0, 0x08, " fd ", " fs1 ", f0\n\t"

#endif /* BENCH_COMMON_H */
