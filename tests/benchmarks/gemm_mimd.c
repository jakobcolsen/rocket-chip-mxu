/*
 * gemm_tiled_mimd.c — MIMD Fork-Join Register-Tiled GEMM
 *
 * C[M][N] += A[M][K] * B[K][N]   (single-precision floating-point)
 *
 * Row-partitioned across 4 cores (same as gemm_mimd.c), but each
 * core uses a register-tiled micro-kernel:
 *   - j-tiled by 4 (contiguous B row access → cache-line friendly)
 *   - i-tiled by 4 when rows_per_core >= 4 (full 4×4 tile)
 *   - Falls back to 1×4 tile for small N (DIM < 16)
 *
 * Compile: pass -DBENCH_N=<dim> for matrix dimension.
 */
#include "common.h"

#ifndef BENCH_N
#undef BENCH_N
#define BENCH_N 2
#endif

#define DIM BENCH_N
#define TILE 4
#define ALIAS_PAD 16
#define BLOCK_SIZE 32

/* ── Matrices (row-major) ─────────────────────────────────────────── */
float A[DIM * DIM + NUM_CORES * ALIAS_PAD] __attribute__((aligned(64)));
float B[DIM * DIM] __attribute__((aligned(64)));
volatile float C[DIM * DIM + NUM_CORES * ALIAS_PAD] __attribute__((aligned(64)));
float C_ref[DIM * DIM] __attribute__((aligned(64)));

static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } x; x.f = f; return x.u;
}

/* Fork-join control */
volatile int go       = 0;
volatile int warm_go  = 0;
volatile int done_cnt = 0;
volatile int warm_done_cnt = 0;

/*
 * Register-tiled kernel for a row slab [row_start, row_end).
 *
 * When the slab height is >= TILE, uses a 4×4 micro-kernel.
 * Otherwise falls back to a 1×4 tile (still 4× better than scalar).
 */
static void __attribute__((noinline)) gemm_tiled_rows(int row_start, int row_end, int pad_offset) {
    int slab_height = row_end - row_start;
    int leftover_start = row_start + (slab_height / TILE) * TILE;

    for (int jj = 0; jj < DIM; jj += BLOCK_SIZE) {
        for (int kk = 0; kk < DIM; kk += BLOCK_SIZE) {

            if (slab_height >= TILE) {
                /* Full 4×4 tiles */
                for (int i = row_start; i <= row_end - TILE; i += TILE) {
                    int j_end = (jj + BLOCK_SIZE < DIM) ? jj + BLOCK_SIZE : DIM;
                    for (int j = jj; j < j_end; j += TILE) {
                        float c00 = C[(i+0)*DIM+(j+0)+pad_offset]; float c01 = C[(i+0)*DIM+(j+1)+pad_offset];
                        float c02 = C[(i+0)*DIM+(j+2)+pad_offset]; float c03 = C[(i+0)*DIM+(j+3)+pad_offset];
                        float c10 = C[(i+1)*DIM+(j+0)+pad_offset]; float c11 = C[(i+1)*DIM+(j+1)+pad_offset];
                        float c12 = C[(i+1)*DIM+(j+2)+pad_offset]; float c13 = C[(i+1)*DIM+(j+3)+pad_offset];
                        float c20 = C[(i+2)*DIM+(j+0)+pad_offset]; float c21 = C[(i+2)*DIM+(j+1)+pad_offset];
                        float c22 = C[(i+2)*DIM+(j+2)+pad_offset]; float c23 = C[(i+2)*DIM+(j+3)+pad_offset];
                        float c30 = C[(i+3)*DIM+(j+0)+pad_offset]; float c31 = C[(i+3)*DIM+(j+1)+pad_offset];
                        float c32 = C[(i+3)*DIM+(j+2)+pad_offset]; float c33 = C[(i+3)*DIM+(j+3)+pad_offset];

                        int k_end = (kk + BLOCK_SIZE < DIM) ? kk + BLOCK_SIZE : DIM;
                        for (int k = kk; k < k_end; k++) {
                            float a0 = A[(i+0)*DIM + k];
                            float a1 = A[(i+1)*DIM + k];
                            float a2 = A[(i+2)*DIM + k];
                            float a3 = A[(i+3)*DIM + k];

                            float b0 = B[k*DIM + (j+0)];
                            float b1 = B[k*DIM + (j+1)];
                            float b2 = B[k*DIM + (j+2)];
                            float b3 = B[k*DIM + (j+3)];

                            c00+=a0*b0; c01+=a0*b1; c02+=a0*b2; c03+=a0*b3;
                            c10+=a1*b0; c11+=a1*b1; c12+=a1*b2; c13+=a1*b3;
                            c20+=a2*b0; c21+=a2*b1; c22+=a2*b2; c23+=a2*b3;
                            c30+=a3*b0; c31+=a3*b1; c32+=a3*b2; c33+=a3*b3;
                        }

                        C[(i+0)*DIM+(j+0)+pad_offset]=c00; C[(i+0)*DIM+(j+1)+pad_offset]=c01;
                        C[(i+0)*DIM+(j+2)+pad_offset]=c02; C[(i+0)*DIM+(j+3)+pad_offset]=c03;
                        C[(i+1)*DIM+(j+0)+pad_offset]=c10; C[(i+1)*DIM+(j+1)+pad_offset]=c11;
                        C[(i+1)*DIM+(j+2)+pad_offset]=c12; C[(i+1)*DIM+(j+3)+pad_offset]=c13;
                        C[(i+2)*DIM+(j+0)+pad_offset]=c20; C[(i+2)*DIM+(j+1)+pad_offset]=c21;
                        C[(i+2)*DIM+(j+2)+pad_offset]=c22; C[(i+2)*DIM+(j+3)+pad_offset]=c23;
                        C[(i+3)*DIM+(j+0)+pad_offset]=c30; C[(i+3)*DIM+(j+1)+pad_offset]=c31;
                        C[(i+3)*DIM+(j+2)+pad_offset]=c32; C[(i+3)*DIM+(j+3)+pad_offset]=c33;
                    }
                }
            }

            /* Handle leftover rows (slab_height not multiple of TILE) with 1×4 */
            if (leftover_start < row_end) {
                for (int i = leftover_start; i < row_end; i++) {
                    int j_end = (jj + BLOCK_SIZE < DIM) ? jj + BLOCK_SIZE : DIM;
                    for (int j = jj; j < j_end; j += TILE) {
                        float c0 = C[i*DIM+(j+0)+pad_offset];
                        float c1 = C[i*DIM+(j+1)+pad_offset];
                        float c2 = C[i*DIM+(j+2)+pad_offset];
                        float c3 = C[i*DIM+(j+3)+pad_offset];
                        
                        int k_end = (kk + BLOCK_SIZE < DIM) ? kk + BLOCK_SIZE : DIM;
                        for (int k = kk; k < k_end; k++) {
                            float a = A[i*DIM + k];
                            c0 += a * B[k*DIM + (j+0)];
                            c1 += a * B[k*DIM + (j+1)];
                            c2 += a * B[k*DIM + (j+2)];
                            c3 += a * B[k*DIM + (j+3)];
                        }
                        
                        C[i*DIM+(j+0)+pad_offset]=c0; C[i*DIM+(j+1)+pad_offset]=c1;
                        C[i*DIM+(j+2)+pad_offset]=c2; C[i*DIM+(j+3)+pad_offset]=c3;
                    }
                }
            }

        }
    }
}

int main(void) {
    uint64_t hartid = read_csr(mhartid);
    bench_disable_interrupts();

    if (hartid == 0) {
        /* Initialize matrices */
        for (int i = 0; i < DIM * DIM + NUM_CORES * ALIAS_PAD; i++) {
            A[i] = (float)(i + 1);
            C[i] = 0.0f;
        }
        for (int i = 0; i < DIM * DIM; i++) {
            B[i] = (float)(i + 1) * 0.5f;
            C_ref[i] = 0.0f;
        }

        /* Compute reference (per-core padded, like MIMD) */
        for (int c = 0; c < NUM_CORES; c++) {
            int rows_per_core = DIM / NUM_CORES;
            if (rows_per_core < 1) { if(c!=0) continue; rows_per_core = DIM; }
            int rs = (rows_per_core < DIM) ? c * rows_per_core : 0;
            int re = rs + rows_per_core;
            if (re > DIM) re = DIM;
            int po = c * ALIAS_PAD;

            for (int i = rs; i < re; i++) {
                for (int j = 0; j < DIM; j++) {
                    float sum = 0.0f;
                    for (int k = 0; k < DIM; k++) {
                        sum += A[i * DIM + k] * B[k * DIM + j];
                    }
                    C_ref[i * DIM + j] = sum;
                }
            }
        }
        asm volatile ("fence rw, rw" ::: "memory");

        printf("BENCHMARK: gemm_mimd\n");
        printf("N: %d\n", DIM);

        int rows_per_core = DIM / NUM_CORES;
        if (rows_per_core < 1) rows_per_core = DIM;

        HPM_SETUP();

        if (DIM >= NUM_CORES) {
            warm_go = 1;
            asm volatile ("fence rw, rw" ::: "memory");
        }

        /* Warm up L1 cache */
        volatile float dummy = 0.0f;
        for (int i = 0; i < DIM * DIM + NUM_CORES * ALIAS_PAD; i++) dummy += A[i] + C[i];
        for (int i = 0; i < DIM * DIM; i++) dummy += B[i];
        
        __sync_fetch_and_add(&warm_done_cnt, 1);
        if (DIM >= NUM_CORES) {
            mimd_barrier_wait(&warm_done_cnt, NUM_CORES);
        }

        BENCH_START();
        HPM_START();

        if (DIM >= NUM_CORES) {
            go = 1;
            asm volatile ("fence rw, rw" ::: "memory");
        }

        gemm_tiled_rows(0, rows_per_core, 0);

        if (DIM >= NUM_CORES) {
            mimd_barrier_wait(&done_cnt, NUM_CORES - 1);
        }

        BENCH_END();
        HPM_END();
        BENCH_REPORT();
        HPM_REPORT();

        /* Verify */
        int passed = 1;
        for (int c = 0; c < NUM_CORES; c++) {
            int rpc = DIM / NUM_CORES;
            if (rpc < 1) { if(c!=0) continue; rpc = DIM; }
            int rs = (rpc < DIM) ? c * rpc : 0;
            int re = rs + rpc;
            if (re > DIM) re = DIM;
            int po = c * ALIAS_PAD;

            for (int i = rs; i < re; i++) {
                for (int j = 0; j < DIM; j++) {
                    float got = C[i * DIM + j + po];
                    float exp = C_ref[i * DIM + j];
                    float diff = got - exp;
                    if (diff < 0) diff = -diff;
                    float mag = exp; if (mag < 0) mag = -mag; if (mag < 1.0f) mag = 1.0f;
                    if (diff / mag > 1e-4f && passed) {
                        passed = 0;
                        printf("  FAIL C[%d][%d]=%f exp=%f\n", i, j, got, exp);
                    }
                }
            }
        }
        if (passed) printf("*** PASSED ***\n");
        else        printf("*** FAILED ***\n");

    } else {
        /* Follower: wait for warm_go flag */
        while (warm_go == 0) { asm volatile ("nop"); }
        asm volatile ("fence rw, rw" ::: "memory");

        /* Warm up L1 cache */
        volatile float dummy = 0.0f;
        for (int i = 0; i < DIM * DIM + NUM_CORES * ALIAS_PAD; i++) dummy += A[i] + C[i];
        for (int i = 0; i < DIM * DIM; i++) dummy += B[i];

        __sync_fetch_and_add(&warm_done_cnt, 1);

        /* Follower: wait for go flag */
        while (go == 0) { asm volatile ("nop"); }
        asm volatile ("fence rw, rw" ::: "memory");

        int rows_per_core = DIM / NUM_CORES;
        if (rows_per_core < 1) {
            /* Not enough rows — park */
        } else {
            int row_start = hartid * rows_per_core;
            int row_end   = row_start + rows_per_core;
            if (row_end > DIM) row_end = DIM;
            gemm_tiled_rows(row_start, row_end, hartid * ALIAS_PAD);
        }

        asm volatile ("fence rw, rw" ::: "memory");
        __sync_fetch_and_add(&done_cnt, 1);
        while (1) { asm volatile ("wfi"); }
    }

    return 0;
}
