# SIMD Lockstep Post-Mortem: From "Fools SIMD" to Verified Truth

## 1. The "Fools SIMD" Discovery
During the initial verification of the Systolic Mesh instruction broadcast, a phenomenon termed "Fools SIMD" was identified. Follower cores appeared to be synchronized with the Leader, but execution traces revealed a deceptive failure mode:
*   **The Trap Deception**: Follower cores attempted to execute the `csrw 0x800` (SIMD Enable) instruction locally. In the default configuration, the `systolicSimdMode` CSR is only writable or has side-effects on the Leader core. This caused the Followers to trigger a Machine Mode trap.
*   **The Recovery Gap**: While the Leader initiated the SIMD payload, the Followers were occupied by the trap handler. They returned to their idle loops precisely as the Leader completed the payload, creating a superficial appearance of synchronization.
*   **The Silent Failure**: No memory was modified by the Followers during this period, as they were effectively trapped in an exception loop rather than executing the broadcasted stream.

---

## 2. Implementation Strategies (The Immediate Mitigation)

To achieve verified lockstep execution, several surgical modifications were applied to the Rocket Core and the software environment. These interventions bypassed standard pipeline dependencies to enable reliable instruction injection.

### A. Instruction Hijacking
The Followers' internal `ibuf` (Instruction Buffer) was bypassed. In `RocketCore.scala`, the team hijacked the `id_expanded_inst` and `id_raw_inst` signals to feed the broadcasted instruction directly into the Decode stage when `systolic_enable` is asserted. This ensures the core processes the Leader's stream regardless of its local Program Counter or Fetch state.

### B. Operand Mapping and Registry Control
Initial attempts saw Followers attempting to fetch operands from the Systolic Mesh registers. The implementation was refined to force Followers to use their local Register Files. This allowed broadcasted instructions (like `csrr t0, mhartid`) to correctly retrieve the local Hart ID and base addresses from each Follower's unique registers, even though the instruction itself originated globally.

### C. The Burst-Mode Synchronization (Temporal Redundancy)
The hardware's broadcast mechanism provides a single-cycle `valid` pulse per instruction. If a Follower core experiences a transient stall (e.g., a pipeline bubble from a local branch or a D-Cache hit/miss delay) during that specific cycle, it "misses" the instruction. 

To mitigate this without complex handshaking hardware, the implementation utilizes an **8-store burst**. By broadcasting each critical memory instruction eight times in succession, the Leader creates an 8-cycle "temporal window." Since the `sb` (store byte) operation is idempotent, a Follower that is stalled for up to 7 cycles will still successfully commit the instruction on the 8th cycle. This provides a robust margin against the 1-3 cycle jitters typical of the Rocket front-end.

### D. Pipeline Flush and Take-PC Masking
A critical challenge involved preventing Follower cores from "escaping" their assigned execution window.
*   **Take-PC Masking**: A hardware mask was applied to the `take_pc` signal for Followers. This prevents them from following the Leader's jump or branch instructions, ensuring they remain parked in their local spin-loops while their pipelines process the broadcasted instructions.
*   **Stability Trade-offs**: While a mask on `take_pc_mem_wb` (the definitive flush signal) provided the highest Follower stability, it introduced instability in the Leader core's UART/exception handling. The final implementation uses a surgical `take_pc` mask in the Decode stage, combined with a software-side "NOP Runway."

---

## 3. Memory Coherence and Layout
Parallel memory modification by un-synchronized cores often leads to false-sharing races. To resolve this:
*   **Cache-Line Isolation**: The result buffer was redesigned using a `padded_slot_t` structure, ensuring each core's target variable is isolated within its own 64-byte aligned cache line.
*   **Linker Section Alignment**: A dedicated `.shared_mem` section was defined in the linker script (`test.ld`) at a fixed, 4KB-aligned address. This bypassed compiler-driven relocation issues and guaranteed a stable memory map for all cores.

---

## 4. Implemented Hardware Solutions

The initial proof-of-concept relied on software mitigations (NOP runways, 8-store bursts) for pipeline desynchronization. A subsequent attempt used a RoCC-based barrier bitmap, which failed due to D-cache replays flushing the barrier instruction itself. All of these have since been replaced by clean hardware solutions:

### i. Follower-Fetch PC Broadcast (Implemented)
The Follower's Decode stage now receives the Leader's 64-bit Program Counter via `io.systolic_pc_in`, routed through the Systolic Mesh. The multiplexer `id_effective_pc = Mux(id_systolic_follower, io.systolic_pc_in, ibuf.io.pc)` ensures that `mepc` accurately tracks the SIMD payload during exceptions. The Follower's local I-Cache is frozen (`ibuf.io.inst(0).ready := ... && !id_systolic_follower`) so it can seamlessly resume local execution when SIMD mode is deactivated.

### ii. WFI Wakeup Override (Implemented)
Followers no longer need to spin in active NOP loops. They sleep via the standard RISC-V `wfi` instruction. When the Leader writes to CSR `0x800`, the mesh asserts `io.systolic_enable` on all tiles. The clock-gate output is modified: `io.wfi := csr.io.status.wfi && !io.systolic_enable`, instantly waking any sleeping Follower.

### iii. Leader CSR Pipeline Flush (Implemented)
The Leader no longer needs a NOP runway after `csrw 0x800`. A decode-stage signal `id_sys_csr_write` detects writes to address `0x800` and is OR'd into `id_csr_flush`. This triggers `ex_reg_flush_pipe`, killing the shadow pipeline behind the CSR write and guaranteeing the SIMD payload only enters the pipeline after the CSR has committed and the wakeup signal has propagated.

### iv. Global Stall OR-Tree (Implemented — replaces NOP gaps, 8-store bursts, and barrier bitmap)
The root cause of dropped instructions was identified: when a Follower's D-cache misses, the pipeline replays and flushes the in-flight instruction. Meanwhile, the Leader advanced and started broadcasting the *next* instruction. The Follower received this new instruction, permanently skipping the missed store.

The fix is a **combinational OR-tree stall network**:
*   Each core computes `ctrl_stalld_local` (all local hazards: D-cache miss, structural stall, scoreboard, etc.) and drives `systolic_stall_out` when it cannot accept the current instruction.
*   The Mesh OR's all `systolic_stall_out` signals: `val global_stall = sys_outputs.map(_.systolic_stall_out).reduce(_ || _)`.
*   `global_stall` is broadcast to every core's `systolic_stall` input, which is injected into `ctrl_stalld`: `val ctrl_stalld = ctrl_stalld_local || (io.systolic_stall && io.systolic_enable)`.
*   This freezes the Leader's Decode stage so the broadcasted instruction stays on the wire until **all** Followers have digested it.
*   **Combinational loop avoidance**: `systolic_stall_out` is driven from `ctrl_stalld_local` (no global feedback), not from `ctrl_stalld` (which includes the feedback). This breaks the `ctrl_stalld → stall_out → mesh → stall_in → ctrl_stalld` loop.

The previous RoCC-based barrier bitmap (`WithSystolicRoCC`) has been removed from the configuration.

### v. D-Cache Contention Livelock — Store-Done Tracking (Implemented)
During lock-step execution, overlapping cache misses from multiple cores interacting with the same cache line led to infinite coherence livelocks (the "Perfect Symmetry" problem — see §5.ii for full analysis). This was initially worked around with 64-byte software padding, then permanently resolved with a **two-part hardware fix**:

1. **Selective Replay Gating**: `replay_wb = local_replay_req || (global_replay_wb && is_leader)` — only the Leader (for re-broadcast) and locally-nacked Followers participate in replays. Successful Followers skip the replay entirely; the stall OR-tree holds them.
2. **Store-Done D-Cache Suppression**: A per-core `systolic_store_done` register + `systolic_done_pc` records when a SIMD store has been committed. When the same instruction re-enters the EX stage (via re-broadcast), writes are suppressed: `io.dmem.req.valid := ex_reg_valid && ex_ctrl.mem && !suppress_done_store`. Only nacked cores (where the flag was never set) retry their D-cache.

This makes the livelock **self-resolving in O(N) rounds**: each round, TileLink grants at least one nacked core. That core sets `systolic_store_done` and drops out of contention. The Leader re-broadcasts on every round (D-cache suppressed), keeping the instruction on the mesh for remaining nacked Followers. Cache-line padding is no longer required.

### vi. Custom CSR Mesh Wiring and PTW Override Fix (Implemented)
Custom CSRs `0x801` and `0x802` were originally masked to strictly 0 internally, preventing them from catching dynamic values from the mesh `sdata` pins. After setting their Chisel masks to `BigInt("FFFFFFFFFFFFFFFF", 16)`, a secondary issue was found where the `csr.io.customCSRs` bundle inputs were being overwritten by false defaults from a bidirectional `<>` connection to the Page Table Walker (PTW). The fix explicitly wires the PTW connection uni-directionally, preserving the dynamic mesh data inputs and enabling dynamic hardware-to-software data routing.

### vii. Scalability Notes
*   **4–16 cores**: The combinational OR-tree is `log₂(N)` gate delays deep (~2–4 levels). Negligible compared to ALU or D-cache critical path.
*   **64+ cores**: A registered hierarchical stall tree (grouping cores into sub-arrays) would be needed to meet timing at high clock frequencies. This adds ~1 cycle latency per tree level.
*   **Performance**: The stall only fires on actual D-cache misses. For compute-heavy SIMD kernels (ALU-bound), it is essentially free. Compared to the old 16-NOP gaps per instruction, throughput is significantly improved.

### viii. Follower Decode Masking Bug — MUL Executed as ADD (Fixed 2026-03-23, Re-enabled 2026-03-25)
The constrained SIMD masking (§2.A / tour_guide §8.D) forcefully disables dangerous control-flow operations on followers: `id_ctrl.branch := false.B`, `id_ctrl.jal := false.B`, etc. One of these overrides was `id_ctrl.div := false.B`, intended to block division on followers.

**The bug**: In Rocket's decode table, `id_ctrl.div` controls the **MulDiv unit** — it gates **both** multiply and divide instructions. Setting `div = false` caused `MUL` instructions to bypass the MulDiv unit entirely. The instruction fell through to the ALU, which decoded the opcode `0110011` + funct3 `000` as `ADD`. The result: `Y[i] = X[i] + scalar` instead of `Y[i] = X[i] * scalar`.

**Diagnosis**: The AXPY benchmark (`bench_axpy_simd.c`) showed `Y[16]=136, expected 167`. Manual analysis: `136 - 116(init) = 20 = 17(X[16]) + 3(scalar)`. Every follower element matched the pattern `X[i] + a` — the signature of ADD replacing MUL. The encoding difference between `MUL` and `ADD` is a single bit: funct7[0] (bit 25 of the instruction word).

**Original fix (2026-03-23)**: Commented out `id_ctrl.div := false.B` — but was reverted because the "stale ibuf" concern remained.

**Permanent fix (2026-03-25)**: Both `id_ctrl.mul := false.B` and `id_ctrl.div := false.B` removed from the follower masking block. The original concern ("decoder reads STALE ibuf") is no longer valid: the decode table input is `id_effective_inst` (line 402), which is already the broadcast instruction for followers. MUL/DIV now route correctly to the MulDiv unit on all cores.

### ix. dcache_blocked and Cache-Line Contention (Resolved via Software Padding — 2026-03-24)
The `dcache_blocked` term in follower `stall_out` (added in commit `39c45b8e1` to prevent store loss during TileLink upgrades) created a livelock when combined with Store-Done Tracking (§4.v) and same-cache-line writes:

1. A SIMD store causes cache contention. One follower succeeds (`systolic_store_done = true`), others are nacked.
2. Nacked followers replay and retry. TileLink sends a **Probe** to the successful follower to invalidate its cached copy.
3. The Probe sets `dcache_blocked = true` on the successful follower.
4. `dcache_blocked` in `stall_out` → global OR-tree → **all cores freeze**, including the nacked follower trying to retry.
5. When the Probe clears, `dcache_blocked` drops, stall releases → all cores advance together → back to step 1. Perfect Symmetry is restored.

**Attempted hardware fixes** (all failed to simultaneously fix both AXPY and systolic_flow):
- `dcache_blocked && !systolic_store_done` — broke AXPY (permanent stall suppression after first memory op → instruction drops)
- PC-aware `in_done_replay` with `systolic_replay_active` — AXPY cold L1 misses triggered nacks, latching the replay flag and suppressing stalls
- Auto-clearing `systolic_store_done` — race condition cleared flag too early

**Root cause**: The livelock only occurs when multiple SIMD cores write to the **same 64-byte cache line**. This is analogous to GPU shared memory bank conflicts — a well-understood constraint in parallel computing.

**Fix**: Software cache-line padding. Each core's output buffer is aligned to 64-byte boundaries so cores write to separate cache lines. The hardware stall_out uses plain `dcache_blocked` (no gating). This is simple, correct for all workloads, and requires no D-cache modifications. The constraint is documented as a design requirement: *"SIMD stores must target separate cache lines."*

**Verified on 2026-03-24** with `hello_systolic_flow` (padded `results[4][8]` with `__attribute__((aligned(64)))`) — all 4 harts, correct data flow ✓.

### x. FPU Systolic Pipeline Synchronization (Completed 2026-03-30)
Floating point systolic math (`SYSTOLIC_FMUL_S`, `SYSTOLIC_FMAC_S`) introduced a critical pipeline delay. 

**The bug**: The integer systolic ALU evaluates in 1 cycle, allowing East and South auto-forwards to fire near-simultaneously. The single-precision FPU FMA pipe takes 3 cycles. Initially, East auto-forwarding (Matrix A) was wired to fire when the instruction entered the Execute (`EX`) stage, but South auto-forwarding (Matrix C accumulation) naturally fired when the instruction finished math and hit the Writeback (`WB`) stage. This misaligned the Matrix A and Matrix C wavefronts by 3 cycles across the mesh.
Additionally, the internal `FPUFMAPipe` uses the `ren3` (read-enable register 3) signal to identify 3-operand instructions. Since our custom `SYSTOLIC_FMAC_S` does not read a third value from the standard register file (`ren3=N` in the decoder), the FMAPipe treated it as a 2-operand instruction and unconditionally zeroed out the injected `mesh_north` accumulation value.

**The Fix**:
1. **Pipeline Synchronization**: The `mesh_west` operand was pipelined down the FPU by extending the internal `wbInfo` shift-register with a new `sys_opA` field. The East auto-forward was delayed to the `WB` stage, ensuring it fires synchronously with the South auto-forward.
2. **Operand Override**: During systolic operand injection in `FPU.fuInput()`, `req.ren3 := true.B` is forcefully asserted for `FMAC` instructions. This overrides the decoder and prevents `FPUFMAPipe` from zeroing out the `mesh_north` data payload.

**Verified on 2026-03-30** with `test_systolic_fmac` — fully coherent 2D wave propagation across the mesh.

---

## 5. Future Work / Abandoned Approaches

### i. Decoupling Front-End from Execution (Abandoned)

A fundamentally more robust (but more complex) solution would have been to decouple the instruction fetch/decode stage from the execution stages using an instruction FIFO. This was abandoned due to the significant overhaul required of the Rocket pipeline's control logic, and because the Store-Done Tracking fix (§4.v) resolved the livelock with minimal hardware changes.

### ii. HartID-Based Replay Backoff (Superseded by §4.v)

#### The Problem: Why Global Replay Enforces the Livelock
The Global Replay Network (§4.iv) correctly fixes data corruption (no instruction is ever dropped), but it inadvertently **enforces** the D-cache livelock when cores contend on the same cache line:
1. **The Conflict**: A SIMD memory instruction causes all cores to issue a D-cache request for the same cache line in the same cycle.
2. **The Nack**: TileLink can only grant exclusive access to one core at a time. The L2 grants one and the other L1 D-caches receive `s2_nack`.
3. **The Synchronized Replay**: The Global Replay OR-tree detects the nack and forces **all** cores — including the one that succeeded — to flush and refetch.
4. **The Loop**: Because work is perfectly lockstepped, all cores arrive back at the Memory stage at *exactly* the same cycle. They repeat the exact same conflict. No hardware jitter exists to break the tie.

This is the **Perfect Symmetry** problem: the very mechanism that keeps cores in lockstep also prevents the D-cache coherence protocol from ever making forward progress.

#### Why Not Remove Global Replay (Option B)?
An initial attempt removed the global replay OR-tree entirely, relying on the stall OR-tree alone. This **fundamentally failed** because the global replay serves a critical re-broadcast function: when a follower nacks at WB (3+ pipeline stages deep), the leader must replay to put the instruction back into its Decode stage for re-broadcast. The stall OR-tree can only freeze the *current* decode instruction — it cannot rewind to a past instruction.

#### Why Not HartID-Based Backoff?
A `hartid * K` cycle backoff was proposed to stagger replays. This conflicts with the stall OR-tree: every time a replaying core causes `take_pc_mem_wb`, the stall network re-synchronizes all cores, effectively canceling the backoff. The two mechanisms fight each other.

#### Implemented Solution: Store-Done Tracking (§4.v)
The actual fix uses two mechanisms:
1. **Selective replay gating** — successful followers skip replay entirely
2. **Per-core store-done flag** — cores that already committed the store suppress their D-cache request on re-broadcast

This breaks Perfect Symmetry without removing the essential re-broadcast mechanism. Self-resolving in O(N) rounds. **Verified on 2026-03-12** with both padded and unpadded tests.

## 6. Conclusion
The Systolic Mesh SIMD architecture has evolved from a software-padded proof-of-concept into a hardware-synchronized lockstep system. Follower-Fetch PC broadcast provides accurate exception tracking, WFI wakeup enables zero-overhead idle waiting, CSR pipeline flushing eliminates Leader-side NOP delays, and the **global stall OR-tree** guarantees that no broadcasted instruction is ever dropped due to D-cache misses or pipeline replays. Custom CSRs dynamically expose real-time mesh data to the executable cores.

D-cache contention on same-cache-line SIMD stores is handled via **software cache-line alignment** — a well-understood constraint analogous to GPU shared memory bank padding. This avoids D-cache modifications while keeping the hardware extension minimal (~200 lines across 2 Scala files).

The AXPY benchmark now uses real `mul` instructions — `id_ctrl.mul`/`id_ctrl.div` masks were removed for followers since the decoder already reads `id_effective_inst` (the broadcast instruction). See §4.viii for full history.

**Verified on 2026-03-25** on the `mxu` branch with Verilator `VerilatorQuadRocketMXUConfig`:
- `benchmark_axpy_simd` — AXPY with real **`mul`** (Y = 3*X + Y), all 64 elements correct, **409 cycles** ✓
- `benchmark_axpy_mimd` — MIMD AXPY (software fork-join), all 64 elements correct, **452 cycles** ✓
- `hello_systolic_flow` — Systolic data through 2×2 mesh with **cache-line-padded stores**, all values correct ✓

**SIMD speedup over MIMD: 9.5%** (409 vs 452 cycles) — with hyper-optimized MIMD and all software tricks.

---

## 7. Next Steps

### Phase 1: SIMD Loops (Investigation — Tabled)
Attempted enabling C for loops in SIMD mode by un-masking `id_ctrl.branch` for followers. The leader evaluates branches and re-fetches; followers should follow the broadcast. However, taken branches cause the leader to flush speculative fall-through instructions from its pipeline, while followers (with `branch := false.B`) don't flush — the speculative instructions execute on followers, causing double-computation (6×X instead of 3×X on every taken-branch iteration).

A dedicated `leader_flush` signal was prototyped (leader broadcasts `take_pc_mem` to followers via the mesh, followers add it to `ctrl_killx`) but did not resolve the issue — the 2× pattern persisted even on the **leader's** own elements, suggesting the root cause is deeper (possibly related to pipeline timing or the BTB/no-BTB misprediction path).

**Status**: Tabled. Unrolled SIMD is the current approach.

### Phase 2: MUL Re-enablement ✅ (Completed 2026-03-25)
`id_ctrl.mul` and `id_ctrl.div` masks removed from follower masking block. The decoder already reads `id_effective_inst` (broadcast instruction), so the stale-ibuf concern was invalid. Verified with real `mul` in AXPY — 409 cycles, all 64 elements correct.

### Phase 3: FPU Systolic Integration ✅ (Completed 2026-03-30)
With the integer logic robust, the FPU pipeline was tapped. Native floating-point single-precision `SYSTOLIC_FMUL_S` and `SYSTOLIC_FMAC_S` instructions were embedded alongside operand injection directly into the Rocket `FPU.scala` architecture. Pipeline desynchronization between A and C matrices was resolved via custom `wbInfo` shift-register metadata.

### Phase 4: GEMM Kernel
The hardware is now completely verified for floating point operations. The immediate next step is to write a true 2D blocked **GEMM (General Matrix Multiply)** kernel utilizing the full 4 or 16-core configuration.
