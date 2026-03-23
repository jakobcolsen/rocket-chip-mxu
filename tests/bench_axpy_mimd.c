/*
 * bench_axpy_mimd.c — Manual MIMD Fork-Join AXPY Benchmark
 *
 * Compares against bench_axpy_simd.c (hardware SIMD lockstep).
 * All 4 cores compute Y[i] = a*X[i] + Y[i] on their own slice,
 * coordinated via polling (wakeup) + sense-reversing barrier (join).
 *
 * Build:
 *   riscv64-unknown-elf-gcc -march=rv64g -mabi=lp64 -static -mcmodel=medany \
 *     -fvisibility=hidden -nostdlib -nostartfiles -T test.ld \
 *     crt.S bench_axpy_mimd.c syscalls.c sys_stubs.c \
 *     -o bench_axpy_mimd.riscv -I. -O2
 */

#include <stdio.h>
#include <stdint.h>

#define MSTATUS_MIE  0x00000008
#define NUM_CORES    4
#define N            64          /* total elements */
#define SLICE        (N / NUM_CORES)  /* 16 per core */
#define SCALAR_A     3

/* ── CSR helpers (from encoding.h via util.h) ───────────────────────── */
#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); __tmp; })
#define write_csr(reg, val) ({ \
  asm volatile ("csrw " #reg ", %0" :: "rK"(val)); })
#define clear_csr(reg, bit) ({ unsigned long __tmp; \
  asm volatile ("csrrc %0, " #reg ", %1" : "=r"(__tmp) : "rK"(bit)); __tmp; })

/* ── Shared data — each 16-int32 slice = 64 bytes = 1 cache line ─── */
volatile int32_t X[N]  __attribute__((aligned(64)));
volatile int32_t Y[N]  __attribute__((aligned(64)));

/* Fork-join control */
volatile int go       = 0;      /* leader sets to wake followers */
volatile int done_cnt = 0;      /* follower increment when finished */

/* ── Per-core AXPY kernel ──────────────────────────────────────────── */
static void __attribute__((noinline)) axpy_slice(int start, int end) {
    for (int i = start; i < end; i++) {
        Y[i] = SCALAR_A * X[i] + Y[i];
    }
}

void thread_entry(int cid, int nc) {
    /* no-op — all harts fall through to main */
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);

    /* Disable interrupts */
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    if (hartid == 0) {
        /* ── LEADER ──────────────────────────────────────────────── */
        printf("=== AXPY Benchmark: Manual MIMD Fork-Join ===\n");
        printf("N=%d, SLICE=%d, a=%d, cores=%d\n", N, SLICE, SCALAR_A, NUM_CORES);

        /* Initialize arrays: X[i] = i+1, Y[i] = 100+i */
        for (int i = 0; i < N; i++) {
            X[i] = i + 1;
            Y[i] = 100 + i;
        }
        asm volatile ("fence rw, rw" ::: "memory");

        /* ── START TIMING ──────────────────────────────────────── */
        uint64_t cyc_start = read_csr(mcycle);

        /* Wake followers */
        go = 1;
        asm volatile ("fence rw, rw" ::: "memory");

        /* Leader computes its own slice (elements 0..15) */
        axpy_slice(0, SLICE);

        /* Wait for all followers to finish */
        while (done_cnt < NUM_CORES - 1) {
            asm volatile ("nop");
        }
        asm volatile ("fence rw, rw" ::: "memory");

        /* ── STOP TIMING ───────────────────────────────────────── */
        uint64_t cyc_end = read_csr(mcycle);
        uint64_t elapsed = cyc_end - cyc_start;

        printf("Parallel region: %lu cycles\n", (unsigned long)elapsed);

        /* ── VERIFY ────────────────────────────────────────────── */
        int passed = 1;
        for (int i = 0; i < N; i++) {
            int32_t expected = SCALAR_A * (i + 1) + (100 + i);
            if (Y[i] != expected) {
                printf("  MISMATCH Y[%d] = %d, expected %d\n",
                       i, (int)Y[i], (int)expected);
                passed = 0;
            }
        }

        if (passed) {
            printf("Verification PASSED — all %d elements correct.\n", N);
            printf("\n*** PASSED ***\n");
        } else {
            printf("Verification FAILED!\n");
            printf("*** FAILED ***\n");
        }

    } else {
        /* ── FOLLOWER ────────────────────────────────────────── */

        /* Spin-wait for leader to set go flag */
        while (go == 0) {
            asm volatile ("nop");
        }
        asm volatile ("fence rw, rw" ::: "memory");

        /* Compute my slice */
        int start = hartid * SLICE;
        int end   = start + SLICE;
        axpy_slice(start, end);

        asm volatile ("fence rw, rw" ::: "memory");

        /* Signal completion */
        __sync_fetch_and_add(&done_cnt, 1);

        /* Park */
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
