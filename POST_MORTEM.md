# SIMD Lockstep Post-Mortem

A record of the major hardware bugs discovered and resolved during development of the Systolic Mesh MXU extension. Each section documents the problem, root cause, and fix for future reference.

---

## 1. Fools SIMD — The Original Failure

During initial verification, follower cores appeared synchronized with the Leader, but execution traces revealed a deceptive failure:

- **Trap deception**: Followers executed `csrw 0x800` locally, triggering a Machine Mode trap (CSR only writable on Leader)
- **Recovery gap**: Followers returned from the trap handler just as the Leader completed, creating a superficial appearance of synchronization
- **Silent failure**: No memory was modified by followers during SIMD regions

**Fix**: The Follower-Fetch architecture (§2) completely bypasses local instruction fetch and injects the Leader's broadcast instruction directly into the decode stage.

---

## 2. Implemented Hardware Solutions

### i. Follower-Fetch PC Broadcast

Followers receive the Leader's 64-bit PC via the mesh: `id_effective_pc = Mux(id_systolic_follower, io.systolic_pc_in, ibuf.io.pc)`. The local I-Cache is frozen (`ibuf.ready := ... && !id_systolic_follower`) so it resumes seamlessly when SIMD ends.

### ii. WFI Wakeup Override

Followers sleep via `wfi`. When the Leader writes CSR `0x800`, the mesh asserts `systolic_enable`, waking all cores: `io.wfi := csr.io.status.wfi && !io.systolic_enable`.

### iii. Leader CSR Pipeline Flush

A decode-stage signal `id_sys_csr_write` detects writes to `0x800` and triggers `ex_reg_flush_pipe`, killing shadow pipeline instructions so the SIMD payload only enters after the CSR commits and the wakeup signal propagates.

### iv. Global Stall OR-Tree

**Problem**: When a Follower's D-cache misses, the pipeline replays and flushes the instruction. The Leader advances and broadcasts the *next* instruction — the missed store is permanently skipped.

**Fix**: A combinational OR-tree stall network:
- Each core drives `systolic_stall_out` from local conditions only (no global feedback — breaks combinational loop)
- The Mesh OR's all outputs: `global_stall = reduce(_ || _)`
- `global_stall` feeds back into every core's `ctrl_stalld`, freezing the Leader's decode until all cores have consumed the instruction

This replaced the original 8-store temporal redundancy bursts and the failed RoCC barrier bitmap.

### v. Store-Done Livelock Fix (Perfect Symmetry Problem)

**Problem**: When all SIMD cores store to the same cache line, TileLink grants one and nacks the rest. The Global Replay OR-tree forces all cores to flush and re-issue — recreating the identical collision forever.

**Fix** (two-part):
1. **Selective replay gating**: `replay_wb = local_replay_req || (global_replay_wb && is_leader)` — only the Leader (for re-broadcast) and locally-nacked followers replay. Successful followers skip replay; the stall OR-tree holds them.
2. **Store-Done D-Cache suppression**: A per-core `systolic_store_done` register + `systolic_done_pc` records committed stores. On re-broadcast, successful cores suppress their D-cache write. Only nacked cores retry.

Self-resolving in O(N) rounds — each round, at least one nacked core succeeds and drops out of contention.

### vi. Custom CSR Mesh Wiring (PTW Override Fix)

CSRs `0x801`/`0x802` were masked to 0 internally, preventing mesh data from reaching software. After setting Chisel masks to `BigInt("FFFFFFFFFFFFFFFF", 16)`, a secondary issue surfaced: the PTW's bidirectional `<>` connection overwrote the mesh `sdata` pins with defaults. Fixed by wiring PTW connections unidirectionally.

### vii. MUL Executed as ADD (Follower Decode Masking Bug)

**Problem**: The follower masking block set `id_ctrl.div := false.B` to block division. In Rocket's decode table, `id_ctrl.div` gates the **MulDiv unit** — both multiply and divide. Setting it false caused `MUL` to fall through to the ALU, which decoded it as `ADD`.

**Symptom**: AXPY produced `Y[i] = X[i] + scalar` instead of `Y[i] = X[i] * scalar`. Every follower element matched the `ADD` pattern.

**Fix**: Removed both `id_ctrl.mul := false.B` and `id_ctrl.div := false.B` from the follower masking block. The decoder already reads `id_effective_inst` (the broadcast instruction), so the stale-ibuf concern was invalid.

### viii. dcache_blocked Livelock (Cache-Line Contention)

The `dcache_blocked` term in follower `stall_out` created a secondary livelock when combined with Store-Done Tracking and same-cache-line writes:

1. Store contention → one follower succeeds, others nacked
2. Nacked followers retry → TileLink sends Probe to successful follower
3. Probe sets `dcache_blocked` on successful follower
4. `dcache_blocked` in `stall_out` → OR-tree → all cores freeze (including retrying cores)
5. Probe clears → all advance together → back to step 1 (Perfect Symmetry restored)

**Fix**: Software cache-line padding. Each core's output buffer is aligned to 64-byte boundaries. The hardware uses plain `dcache_blocked` (no gating). This is a documented architectural constraint: *"SIMD stores must target separate cache lines."*

### ix. FPU Pipeline Synchronization

**Problem**: Integer systolic ALU evaluates in 1 cycle, but the FP FMA pipe takes 3 cycles. East auto-forwarding (Matrix A) fired at EX stage while South auto-forwarding (Matrix C) fired at WB stage, misaligning wavefronts by 3 cycles.

Additionally, `SYSTOLIC_FMAC_S` doesn't read a third register (`ren3=N`), so `FPUFMAPipe` treated it as 2-operand and zeroed the `mesh_north` accumulation value.

**Fix**:
1. Extended the FPU's `wbInfo` shift-register with a `sys_opA` field. Both East and South auto-forwards now fire synchronously at Writeback.
2. Force `req.ren3 := true.B` for FMAC instructions to prevent zeroing of the mesh_north operand.

---

## 3. Abandoned Approaches

### Decoupled Front-End (Abandoned)

An instruction FIFO between fetch/decode and execute would have been more robust but required a fundamental Rocket pipeline overhaul. Abandoned in favor of the simpler Store-Done Tracking fix.

### HartID-Based Replay Backoff (Superseded)

A `hartid * K` cycle backoff to stagger replays conflicted with the stall OR-tree: every `take_pc_mem_wb` re-synchronized all cores, canceling the backoff.

### SIMD Branches (Tabled)

Un-masking `id_ctrl.branch` for followers caused double-computation: taken branches flush speculative instructions on the Leader, but followers (with `branch := false.B`) don't flush — speculative instructions execute on followers. A `leader_flush` signal was prototyped but didn't resolve deeper pipeline timing issues. Unrolled SIMD kernels remain the current approach.

---

## 4. Verification Status

All hardware fixes verified on the `mxu` branch with Verilator `VerilatorQuadRocketMXUConfig`:

**Unit Tests** (custom instructions):
- `test_systolic_mul` — integer mesh multiply ✓
- `test_systolic_fmul` — FP mesh multiply ✓
- `test_systolic_fmac` — FP fused multiply-add with 2D wave propagation ✓

**Benchmark Suite** (`run_all_benchmarks.sh`):
- `gemm_single` (N=4,8,16,32) — single-core baseline, all PASSED ✓
- `gemm_mimd` (N=4,8,16,32) — 4-core fork-join, all PASSED ✓
- `gemm_simd` (N=4,8,16,32) — SIMD lockstep, all PASSED ✓
- `gemm_csr` (N=4,8,16,32) — CSR systolic reduction, all PASSED ✓

**MIMD Parity**: MXU and vanilla Rocket simulators produce identical cycle counts for all MIMD and single-core benchmarks, proving zero regression from the mesh extension.

**CSR Latency Microbenchmark**: CSR mesh transfer = ~5.3 cycles/element; L2 shared memory coherence = ~58 cycles/round-trip. The mesh is 10× faster for point-to-point inter-core transfers.
