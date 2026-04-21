# Rocket Chip MXU: Architecture & Evaluation Guide

A comprehensive reference for the Systolic Mesh MXU extension to Rocket Chip — covering architecture, build system, benchmark suite, and performance evaluation.

---

## 1. Project Overview

This project extends a quad-core Rocket Chip SoC with a **2D Systolic Mesh** that enables three parallel execution modes beyond single-core scalar:

| Mode | Description |
|------|-------------|
| **Single-Core** | Baseline sequential execution on Core 0 only |
| **MIMD** | Independent 4-core fork-join (software synchronization) |
| **SIMD** | Hardware lockstep broadcast from Core 0 to all cores |
| **CSR Systolic** | SIMD + inter-core data flow via custom CSR mesh registers |

The mesh adds dedicated inter-tile wiring (East/West/North/South) and a global stall OR-tree for lockstep synchronization, without modifying the standard TileLink memory hierarchy.

---

## 2. Repository Structure

### Two-Repo Architecture

```
rocket-chip-mxu/          ← This repo (Rocket Chip fork)
├── src/main/scala/
│   ├── rocket/
│   │   └── RocketCore.scala      # SIMD pipeline injection, CSR mesh, stall logic
│   └── tile/
│       ├── RocketTile.scala       # Mesh port integration into tile boundary
│       └── SystolicInterface.scala # Shift registers, queue buffers, ALU snooping
├── chipyard-overlay/
│   ├── deploy.sh                  # Copies overlay into chipyard workspace
│   └── generators/chipyard/src/main/scala/
│       ├── SystolicConfigs.scala   # Config fragments (WithSystolicEnabled)
│       ├── SystolicMesh.scala      # Top-level 2D wiring + OR-trees
│       └── DigitalTop.scala.patch  # Mixes HasSystolicMesh into DigitalTop
├── tests/
│   ├── crt.S, syscalls.c, sys_stubs.c  # Bare-metal runtime support
│   ├── test.ld                          # Linker script
│   ├── systolic_mesh.h, util.h          # Hardware API headers
│   ├── test_systolic_mul.c              # Integer multiply unit test
│   ├── test_systolic_fmul.c             # FP multiply unit test
│   ├── test_systolic_fmac.c             # FP fused multiply-add unit test
│   └── benchmarks/                      # Full evaluation suite
└── POST_MORTEM.md                       # Detailed bug history & fixes
```

**Symlink**: `chipyard/generators/rocket-chip → rocket-chip-mxu/`

All edits to `rocket-chip-mxu/src/` are immediately visible to Chipyard's build.

---

## 3. Hardware Architecture

### 3.1 Component Stack

```
SystolicConfigs.scala    Config parameters (SystolicEnabledKey, core count)
        ↓
SystolicMesh.scala       2D wiring: East/West/North/South + OR-tree stall network
        ↓
RocketTile.scala         Diplomatic ports, SystolicInterface instantiation
        ↓
SystolicInterface.scala  Queue buffers, shift registers, ALU operand snooping
        ↓
RocketCore.scala         SIMD decode injection, CSR mesh read/write, stall_out
        ↓
FPU.scala                Systolic FP operand override, pipeline delay alignment
```

### 3.2 SIMD Execution Model

Core 0 is the hardcoded **Leader**. When software writes `2` to CSR `0x800`:

1. `systolicSimdMode` propagates through the mesh to all tiles as `systolic_enable`
2. Follower cores (hartid ≠ 0) freeze their local instruction fetch (`ibuf.ready = false`)
3. Followers receive the Leader's instruction, PC, and BTB prediction via the mesh
4. `id_effective_inst = Mux(id_systolic_follower, io.systolic_instruction_in, id_inst(0))`
5. Followers execute with their **own register files** (unique addresses, same instruction stream)
6. Branches, jumps, and AMOs are masked on followers to prevent control-flow divergence

### 3.3 Global Stall OR-Tree

Each core computes local stall conditions and drives `systolic_stall_out`:

```
Leader stall_out:   ctrl_stalld_local || take_pc_mem_wb
Follower stall_out: (ctrl_stalld_local && pipeline_active) || dcache_blocked
```

The mesh OR's all outputs: `global_stall = reduce(_ || _)`, broadcast back to all cores via `io.systolic_stall`. This freezes the Leader's decode stage until every core has consumed the current instruction. No instruction is ever dropped.

### 3.4 Store-Done Livelock Prevention

When multiple SIMD cores store to the same cache line, TileLink grants one and nacks the rest. The **Store-Done Tracker** breaks the resulting "Perfect Symmetry" livelock:

- Cores that successfully commit a store record its PC in `systolic_done_pc`
- On re-broadcast, successful cores suppress their D-cache write (`!suppress_done_store`)
- Only nacked cores retry → self-resolving in O(N) rounds

### 3.5 CSR Mesh Registers

| CSR | Direction | Purpose |
|-----|-----------|---------|
| `0x800` | Control | Bit 1 = SIMD mode, Bit 2 = load gate |
| `0x801` | East/West | Write injects data East; Read returns data from West neighbor |
| `0x802` | North/South | Write injects data South; Read returns data from North neighbor |

### 3.6 Custom Instructions

| Instruction | Encoding | Operation |
|-------------|----------|-----------|
| `SYSTOLIC_MUL rd, rs1` | `0x5b, funct7=0x00` | `rd = mesh_west × rs1` (integer) |
| `SYSTOLIC_FMUL_S fd, fs1` | `0x5b, funct7=0x04` | `fd = mesh_west × fs1` (FP single) |
| `SYSTOLIC_FMAC_S fd, fs1` | `0x5b, funct7=0x08` | `fd = mesh_west × fs1 + mesh_north` (FP FMA) |

Results auto-forward through the mesh: East (Matrix A) and South (Matrix C accumulator) fire synchronously at Writeback.

### 3.7 Known Hardware Constraints

1. **SIMD stores must target separate 64-byte cache lines** — same-line writes from multiple cores cause serialization overhead (analogous to GPU bank conflicts)
2. **End-of-function stores** — the last store before `SIMD_DISABLE` may be dropped for followers; workaround is to store inside the loop body
3. **No SIMD branches** — `id_ctrl.branch/jal/jalr` are masked on followers; kernels must be branchless or unrolled

---

## 4. Benchmark Suite

### 4.1 GEMM Variants

| Benchmark | File | Description |
|-----------|------|-------------|
| `gemm_single` | `gemm_single.c` | Sequential on Core 0 — Amdahl's law baseline |
| `gemm_mimd` | `gemm_mimd.c` | 4-core fork-join, row-partitioned C matrix |
| `gemm_simd` | `gemm_simd.c` | SIMD lockstep, row-partitioned C matrix |
| `gemm_csr` | `gemm_csr.c` | SIMD + CSR mesh K-split reduction |

### 4.2 Other Benchmarks

| Benchmark | File | Description |
|-----------|------|-------------|
| `axpy_simd` | `axpy_simd.c` | SIMD Y = aX + Y (memory-bound) |
| `axpy_mimd` | `axpy_mimd.c` | MIMD Y = aX + Y |
| `csr_latency_microbench` | `csr_latency_microbench.c` | Isolates CSR mesh vs shared-memory transfer latency |

### 4.3 Problem Sizes

- **AXPY**: N = 64, 256, 1024, 4096
- **GEMM**: N = 2, 4, 8, 16, 32

### 4.4 Hardware Performance Monitors (HPM)

All benchmarks collect 6 HPM counters via Rocket's `mhpmcounter3–8`:

| Counter | Event | What it measures |
|---------|-------|------------------|
| `dcache_miss` | Set 2, bit 1 | L1 D-cache misses |
| `systolic_stall` | Set 1, bit 11 | Cycles frozen by global SIMD stall OR-tree |
| `load_use` | Set 1, bit 0 | Load-use pipeline interlocks |
| `dcache_blocked` | Set 1, bit 4 | Cycles D-cache unavailable |
| `csr_interlock` | Set 1, bit 2 | CSR read/write pipeline stalls |
| `fp_muladd` | Set 0, bit 15 | FP fused multiply-add operations issued |

### 4.5 Simulators

| Config | Binary | Purpose |
|--------|--------|---------|
| `VerilatorQuadRocketMXUConfig` | MXU simulator | Full mesh-enabled quad-core |
| `VerilatorQuadRocketBaselineConfig` | Vanilla simulator | Standard quad-core (regression baseline) |

---

## 5. Build & Run

### 5.1 Environment Setup

```bash
cd $CHIPYARD && source env.sh
```

### 5.2 Build Benchmarks

```bash
cd tests/benchmarks
make -j$(nproc)          # builds all .riscv binaries
```

### 5.3 Run Full Evaluation Suite

```bash
./run_all_benchmarks.sh   # runs all GEMM variants (N=4,8,16,32) on MXU + vanilla
```

Results written to `results.csv`. Progress logged to `run_log.txt`.

### 5.4 Run Individual Benchmark

```bash
$SIM_BIN +permissive +loadmem=gemm_mimd_16.riscv +permissive-off gemm_mimd_16.riscv
```

### 5.5 Generate Plots

```bash
python3 plot_results.py   # generates 6 thesis-quality figures in plots/
```

Figures produced:
1. Cycles vs Matrix Size (grouped bar, log scale)
2. Normalized Execution Time vs MIMD
3. Cycles per FLOP (compute utilization)
4. D-cache Miss Count
5. Pipeline Stall Breakdown (stacked bar per size)
6. MXU vs Vanilla Rocket Regression

### 5.6 Build Simulators

```bash
cd $CHIPYARD/sims/verilator

# MXU quad-core
make CONFIG=VerilatorQuadRocketMXUConfig

# Vanilla quad-core (regression baseline)
make CONFIG=VerilatorQuadRocketBaselineConfig
```

### 5.7 Vivado Area Synthesis (OOC)

Generate Verilog, then synthesize in Vivado for LUT/FF/BRAM/DSP counts:

```bash
cd $CHIPYARD/sims/verilator
make CONFIG=VerilatorQuadRocketMXUConfig verilog
```

Source files: `generated-src/.../gen-collateral/*.sv` — add all to Vivado, set top module to `DigitalTop`. Any UltraScale+ part works for area-only synthesis (LUT fabric is identical across the family).

---

## 6. Key Performance Insights

### MIMD Parity (Zero Regression)

MIMD benchmarks produce identical cycle counts on the MXU and vanilla Rocket simulators, proving the mesh extension adds zero overhead to standard multi-core execution.

### SIMD vs MIMD

- **Small N (≤4)**: SIMD wins — broadcast eliminates I-cache thrashing across cores
- **Large N (≥16)**: MIMD wins — independent L1 caches pipeline around misses; SIMD lockstep is bottlenecked by the slowest core

### CSR Mesh Overhead

The CSR mesh transfers data 10× faster than L2 shared memory coherence (~5.3 cycles/element vs ~58 cycles for a coherent round-trip). However, CSR GEMM trails MIMD at scale because:

1. **4 CSR round-trips per (i,j) element** inside the innermost loop
2. Each CSR write triggers a pipeline replay on the leader → global SIMD stall → all cores freeze
3. The `systolic_stall` HPM counter captures this global amplification (not just local CSR interlock)

The fix is algorithmic: tile the reduction to amortize CSR overhead over larger local compute blocks.

### Memory-Bound vs Compute-Bound

- **AXPY** (memory-bound): MIMD > SIMD — independent cores hide latency via asynchronous cache misses
- **GEMM** (compute-bound at small N): SIMD competitive — broadcast reduces fetch overhead

---

## 7. File Reference

### Scala (Hardware)

| File | Purpose |
|------|---------|
| `src/main/scala/rocket/RocketCore.scala` | SIMD injection, CSR mesh, stall_out, store-done tracking |
| `src/main/scala/tile/RocketTile.scala` | Mesh port wiring into tile boundary |
| `src/main/scala/tile/SystolicInterface.scala` | Queue buffers, shift registers, ALU operand exposure |
| `src/main/scala/rocket/FPU.scala` | Systolic FP operand override, pipeline alignment |
| `src/main/scala/rocket/CustomInstructions.scala` | SYSTOLIC_MUL/FMUL/FMAC decode entries |
| `chipyard-overlay/.../SystolicMesh.scala` | 2D grid wiring, OR-tree stall, instruction broadcast |
| `chipyard-overlay/.../SystolicConfigs.scala` | Config fragments for MXU and baseline |

### C (Software)

| File | Purpose |
|------|---------|
| `tests/benchmarks/common.h` | CSR helpers, HPM macros, SIMD enable/disable, timing |
| `tests/benchmarks/gemm_*.c` | GEMM kernel variants |
| `tests/benchmarks/axpy_*.c` | AXPY kernel variants |
| `tests/benchmarks/csr_latency_microbench.c` | CSR vs shared-memory latency measurement |
| `tests/benchmarks/run_all_benchmarks.sh` | Automated evaluation suite |
| `tests/benchmarks/plot_results.py` | Publication-quality figure generation |
| `tests/test_systolic_*.c` | Hardware unit tests for custom instructions |
