# Makefile for MXU Benchmark Suite
#
# Cross-compiles each benchmark at multiple problem sizes.
# Usage:
#   make                  — build all benchmarks at all sizes
#   make axpy_simd_64     — build single benchmark
#   make clean            — remove all .riscv files
#
# Requires: riscv64-unknown-elf-gcc (from chipyard env.sh)

CC       = riscv64-unknown-elf-gcc
CFLAGS   = -march=rv64g -mabi=lp64 -static -mcmodel=medany \
           -fvisibility=hidden -nostdlib -nostartfiles -O2
LDSCRIPT = ../test.ld
SUPPORT  = ../crt.S ../syscalls.c ../sys_stubs.c
INCLUDES = -I. -I..

# ── Problem sizes ──────────────────────────────────────────────────────
AXPY_SIZES = 64 256 1024 4096
DOT_SIZES  = 64 256 1024 4096
GEMM_SIZES = 2 4 8

# ── Generate target lists ─────────────────────────────────────────────
AXPY_SIMD_TARGETS = $(foreach s,$(AXPY_SIZES),axpy_simd_$(s).riscv)
AXPY_MIMD_TARGETS = $(foreach s,$(AXPY_SIZES),axpy_mimd_$(s).riscv)
DOT_SIMD_TARGETS  = $(foreach s,$(DOT_SIZES),dot_simd_$(s).riscv)
DOT_MIMD_TARGETS  = $(foreach s,$(DOT_SIZES),dot_mimd_$(s).riscv)
GEMM_CSR_TARGETS  = $(foreach s,$(GEMM_SIZES),gemm_csr_$(s).riscv)
GEMM_SYS_TARGETS  = $(foreach s,$(GEMM_SIZES),gemm_systolic_$(s).riscv)
GEMM_MIMD_TARGETS = $(foreach s,$(GEMM_SIZES),gemm_mimd_$(s).riscv)

ALL_TARGETS = $(AXPY_SIMD_TARGETS) $(AXPY_MIMD_TARGETS) \
              $(DOT_SIMD_TARGETS) $(DOT_MIMD_TARGETS) \
              $(GEMM_CSR_TARGETS) $(GEMM_SYS_TARGETS) $(GEMM_MIMD_TARGETS)

.PHONY: all clean

all: $(ALL_TARGETS)

# ── Pattern rules ──────────────────────────────────────────────────────
# Each target: <benchmark>_<size>.riscv compiled from <benchmark>.c with -DBENCH_N=<size>

# AXPY
axpy_simd_%.riscv: axpy_simd.c common.h $(SUPPORT)
	$(CC) $(CFLAGS) -T $(LDSCRIPT) -DBENCH_N=$* $(INCLUDES) $(SUPPORT) $< -o $@

axpy_mimd_%.riscv: axpy_mimd.c common.h $(SUPPORT)
	$(CC) $(CFLAGS) -T $(LDSCRIPT) -DBENCH_N=$* $(INCLUDES) $(SUPPORT) $< -o $@

# Dot product
dot_simd_%.riscv: dot_simd.c common.h $(SUPPORT)
	$(CC) $(CFLAGS) -T $(LDSCRIPT) -DBENCH_N=$* $(INCLUDES) $(SUPPORT) $< -o $@

dot_mimd_%.riscv: dot_mimd.c common.h $(SUPPORT)
	$(CC) $(CFLAGS) -T $(LDSCRIPT) -DBENCH_N=$* $(INCLUDES) $(SUPPORT) $< -o $@

# GEMM
gemm_csr_%.riscv: gemm_csr.c common.h $(SUPPORT)
	$(CC) $(CFLAGS) -T $(LDSCRIPT) -DBENCH_N=$* $(INCLUDES) $(SUPPORT) $< -o $@

gemm_systolic_%.riscv: gemm_systolic.c common.h $(SUPPORT)
	$(CC) $(CFLAGS) -T $(LDSCRIPT) -DBENCH_N=$* $(INCLUDES) $(SUPPORT) $< -o $@

gemm_mimd_%.riscv: gemm_mimd.c common.h $(SUPPORT)
	$(CC) $(CFLAGS) -T $(LDSCRIPT) -DBENCH_N=$* $(INCLUDES) $(SUPPORT) $< -o $@

clean:
	rm -f *.riscv
