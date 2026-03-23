/*
 * bench_axpy_simd.c — Hardware SIMD Lockstep AXPY Benchmark
 *
 * Compares against bench_axpy_mimd.c (manual MIMD fork-join).
 * All 4 cores compute Y[i] = a*X[i] + Y[i] on their own slice,
 * coordinated via the Systolic Mesh hardware broadcast (CSR 0x800).
 *
 * The SIMD broadcast cannot contain branches (followers mask them),
 * so the 16-iteration inner loop is fully unrolled in inline asm.
 *
 * Build:
 *   riscv64-unknown-elf-gcc -march=rv64g -mabi=lp64 -static -mcmodel=medany \
 *     -fvisibility=hidden -nostdlib -nostartfiles -T test.ld \
 *     crt.S bench_axpy_simd.c syscalls.c sys_stubs.c \
 *     -o bench_axpy_simd.riscv -I. -O2
 */

#include <stdio.h>
#include <stdint.h>

#define MSTATUS_MIE  0x00000008
#define NUM_CORES    4
#define N            64          /* total elements */
#define SLICE        (N / NUM_CORES)  /* 16 per core */
#define SCALAR_A     3

/* ── CSR helpers ─────────────────────────────────────────────────── */
#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); __tmp; })
#define write_csr(reg, val) ({ \
  asm volatile ("csrw " #reg ", %0" :: "rK"(val)); })
#define clear_csr(reg, bit) ({ unsigned long __tmp; \
  asm volatile ("csrrc %0, " #reg ", %1" : "=r"(__tmp) : "rK"(bit)); __tmp; })

/* ── Shared data — each 16-int32 slice = 64 bytes = 1 cache line ── */
volatile int32_t X[N]  __attribute__((aligned(64)));
volatile int32_t Y[N]  __attribute__((aligned(64)));

volatile int core_ready[16] = {0};

void thread_entry(int cid, int nc) {
    /* no-op */
}

/*
 * Fully-unrolled AXPY for one element at word offset `off` from base.
 *   t0 = base pointer into X (for this core's slice)
 *   t1 = base pointer into Y (for this core's slice)
 *   t3, t4 = temporaries
 *
 * NOTE: We avoid the RISC-V `mul` instruction because Rocket's
 * PipelinedMultiplier has a 2-stage latency with late writeback.
 * During SIMD lockstep, the multiplier result appears to be lost
 * on follower cores (the global stall/replay network doesn't
 * correctly synchronize the pipelined multiplier response path).
 * Instead, we compute 3*X as (X<<1)+X using single-cycle ALU ops.
 *
 * Each element: lw t3, off(t0);  slli t4, t3, 1;  add t3, t4, t3;
 *               lw t4, off(t1);  add t3, t3, t4;  sw t3, off(t1)
 */
#define AXPY_ONE(byte_off) \
    "lw   t3, " #byte_off "(t0) \n\t" \
    "slli t4, t3, 1             \n\t" \
    "add  t3, t4, t3            \n\t" \
    "lw   t4, " #byte_off "(t1) \n\t" \
    "add  t3, t3, t4            \n\t" \
    "sw   t3, " #byte_off "(t1) \n\t"

int main(void) {
    uint64_t hartid = read_csr(mhartid);

    /* Disable interrupts */
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);

    /* Each core signs in */
    core_ready[hartid] = 1;

    if (hartid == 0) {
        /* ── LEADER ──────────────────────────────────────────────── */
        printf("=== AXPY Benchmark: Hardware SIMD Lockstep ===\n");
        printf("N=%d, SLICE=%d, a=%d, cores=%d\n", N, SLICE, SCALAR_A, NUM_CORES);

        /* Wait for all followers to be alive */
        for (int i = 1; i < NUM_CORES; i++) {
            while (core_ready[i] == 0) { asm volatile ("nop"); }
        }

        /* Initialize arrays: X[i] = i+1, Y[i] = 100+i */
        for (int i = 0; i < N; i++) {
            X[i] = i + 1;
            Y[i] = 100 + i;
        }
        asm volatile ("fence rw, rw" ::: "memory");

        /* ── START TIMING ──────────────────────────────────────── */
        uint64_t cyc_start = read_csr(mcycle);

        /*
         * Broadcast AXPY kernel — all 4 cores execute this in lockstep.
         *
         * SIMD enable (csrw 0x800, 2) is INSIDE this asm block to
         * guarantee zero compiler-inserted instructions between the
         * enable and the first kernel instruction. The CSR flush
         * hardware kills the pipeline shadow after the csrw, so
         * the first real instruction (csrr t5) enters a clean pipe.
         *
         * Register plan (same instruction stream, different data per core):
         *   t0 = &X[hartid * SLICE]
         *   t1 = &Y[hartid * SLICE]
         *   t3, t4 = temporaries for AXPY computation
         *   t5 = hartid (from csrr), then hartid * 64 (from slli)
         *   t2 = SIMD enable constant (2), reused freely after csrw
         */
        asm volatile (
            /* Activate SIMD mode */
            "li   t2, 2                \n\t"
            "csrw 0x800, t2            \n\t"

            /* Compute slice base addresses */
            "csrr t5, mhartid          \n\t"
            "slli t5, t5, 6            \n\t"  /* t5 = hartid * 64 bytes */
            "la   t0, X               \n\t"
            "add  t0, t0, t5           \n\t"
            "la   t1, Y               \n\t"
            "add  t1, t1, t5           \n\t"

            /* Unrolled AXPY: 16 elements × 4 bytes = offsets 0..60 */
            AXPY_ONE(0)
            AXPY_ONE(4)
            AXPY_ONE(8)
            AXPY_ONE(12)
            AXPY_ONE(16)
            AXPY_ONE(20)
            AXPY_ONE(24)
            AXPY_ONE(28)
            AXPY_ONE(32)
            AXPY_ONE(36)
            AXPY_ONE(40)
            AXPY_ONE(44)
            AXPY_ONE(48)
            AXPY_ONE(52)
            AXPY_ONE(56)
            AXPY_ONE(60)

            "fence rw, rw              \n\t"
            :
            :
            : "t0", "t1", "t2", "t3", "t4", "t5", "memory"
        );

        /* Deactivate SIMD */
        asm volatile ("csrw 0x800, x0");

        /* ── STOP TIMING ───────────────────────────────────────── */
        uint64_t cyc_end = read_csr(mcycle);
        uint64_t elapsed = cyc_end - cyc_start;

        /* Small drain to let stores propagate */
        for (volatile int d = 0; d < 1000; d++);
        asm volatile ("fence rw, rw" ::: "memory");

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
        /* Sleep until hardware SIMD wakeup, then park forever */
        while (1) {
            asm volatile ("wfi");
        }
    }

    return 0;
}
