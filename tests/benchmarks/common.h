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

/* ── Hardware Performance Monitor Counters ─────────────────────────── */
/* Rocket EventSets encoding: selector = (event_mask << 8) | set_id
 *   set_id=0: instruction types (load, store, fp mul-add, ...)
 *   set_id=1: pipeline events (load-use, D$ blocked, CSR interlock, ...)
 *   set_id=2: cache events (I$ miss, D$ miss, ...)
 * event_mask: bitmask selecting which event(s) within the set.
 */

#define HPM_EVT_DCACHE_MISS     ((1UL << 1) << 8 | 2)  /* set 2, bit 1 */
#define HPM_EVT_SYSTOLIC_STALL  ((1UL << 11) << 8 | 1) /* set 1, bit 11 */
#define HPM_EVT_LOAD_USE        ((1UL << 0) << 8 | 1)  /* set 1, bit 0 */
#define HPM_EVT_DCACHE_BLOCKED  ((1UL << 4) << 8 | 1)  /* set 1, bit 4 */
#define HPM_EVT_CSR_INTERLOCK   ((1UL << 2) << 8 | 1)  /* set 1, bit 2 */
#define HPM_EVT_FP_MULADD       ((1UL << 15) << 8 | 0) /* set 0, bit 15 */

static volatile uint64_t _hpm3_0, _hpm3_1, _hpm4_0, _hpm4_1;
static volatile uint64_t _hpm5_0, _hpm5_1, _hpm6_0, _hpm6_1;
static volatile uint64_t _hpm7_0, _hpm7_1, _hpm8_0, _hpm8_1;

#define HPM_SETUP() do { \
    write_csr(mhpmevent3, HPM_EVT_DCACHE_MISS); \
    write_csr(mhpmevent4, HPM_EVT_SYSTOLIC_STALL); \
    write_csr(mhpmevent5, HPM_EVT_LOAD_USE); \
    write_csr(mhpmevent6, HPM_EVT_DCACHE_BLOCKED); \
    write_csr(mhpmevent7, HPM_EVT_CSR_INTERLOCK); \
    write_csr(mhpmevent8, HPM_EVT_FP_MULADD); \
} while(0)

#define HPM_START() do { \
    _hpm3_0 = read_csr(mhpmcounter3); \
    _hpm4_0 = read_csr(mhpmcounter4); \
    _hpm5_0 = read_csr(mhpmcounter5); \
    _hpm6_0 = read_csr(mhpmcounter6); \
    _hpm7_0 = read_csr(mhpmcounter7); \
    _hpm8_0 = read_csr(mhpmcounter8); \
} while(0)

#define HPM_END() do { \
    _hpm3_1 = read_csr(mhpmcounter3); \
    _hpm4_1 = read_csr(mhpmcounter4); \
    _hpm5_1 = read_csr(mhpmcounter5); \
    _hpm6_1 = read_csr(mhpmcounter6); \
    _hpm7_1 = read_csr(mhpmcounter7); \
    _hpm8_1 = read_csr(mhpmcounter8); \
} while(0)

#define HPM_REPORT() do { \
    printf("DCACHE_MISS: %lu\n",    (unsigned long)(_hpm3_1 - _hpm3_0)); \
    printf("SYSTOLIC_STALL: %lu\n", (unsigned long)(_hpm4_1 - _hpm4_0)); \
    printf("LOAD_USE: %lu\n",       (unsigned long)(_hpm5_1 - _hpm5_0)); \
    printf("DCACHE_BLOCKED: %lu\n", (unsigned long)(_hpm6_1 - _hpm6_0)); \
    printf("CSR_INTERLOCK: %lu\n",  (unsigned long)(_hpm7_1 - _hpm7_0)); \
    printf("FP_MULADD: %lu\n",      (unsigned long)(_hpm8_1 - _hpm8_0)); \
} while(0)

/* ── SIMD Helpers ─────────────────────────────────────────────────────── */

#define SIMD_ENABLE()  asm volatile ("csrw 0x800, %0" :: "r"(2))
#define SIMD_ENABLE_LOADGATE() asm volatile ("csrw 0x800, %0" :: "r"(6))  // bits 2+1: SIMD + load gate
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
    set_csr(mstatus, MSTATUS_FS_INIT);
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
