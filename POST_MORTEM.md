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

### viii. Store-Nack Stall-Out Race — `dcache_blocked` Fix (Implemented)

Discovered during AXPY benchmark testing (2026-03-22). The first SIMD store on each follower was **silently dropped** — followers' array slices showed element 0 untouched while elements 1–15 were correct.

#### Root Cause
The follower `stall_out` signal was gated by `follower_pipeline_active` (§4.iv comment) to prevent WFI-related deadlock. After a store nack at WB:
1. `take_pc_wb` fires → pipeline kills in-flight instructions → all valid bits drop
2. `follower_pipeline_active = ex_reg_valid || mem_reg_valid || wb_reg_valid = false`
3. `stall_out = ctrl_stalld_local && false = false` — **backpressure dropped prematurely**
4. But `dcache_blocked` remains high while TileLink processes the Shared→Modified upgrade
5. Leader sees no backpressure, advances to the next instruction
6. Follower can't accept the re-broadcast (decode stalls on `id_ctrl.mem && dcache_blocked`)
7. Leader broadcasts the *next* instruction — **nacked store permanently lost**

The first store is most vulnerable because followers have cold D-caches (leader initialized the arrays). The Shared→Modified cache line upgrade nacks while TileLink probes the leader's L1.

#### Diagnostic Confirmation
Swapping the execution order of AXPY_ONE(0) and AXPY_ONE(4) moved the failure from element 0 to element 1 — proving it tracks the **first-executed store**, not a specific address.

#### Fix
Add `dcache_blocked` as an independent stall source that bypasses the `follower_pipeline_active` gate:
```scala
io.systolic_stall_out := io.systolic_enable && Mux(is_leader,
  ctrl_stalld_local || take_pc_mem_wb,
  (ctrl_stalld_local && follower_pipeline_active) || dcache_blocked
)
```
`dcache_blocked` is self-resolving (clears on TileLink grant, typically 3–10 cycles) and is never set by WFI, so the original deadlock scenario is unaffected. **Verified on 2026-03-22** — AXPY benchmark passes all 64 elements, `hello_simd_nopad` regression passes.

### ix. PipelinedMultiplier Desync in SIMD Mode (Open — SW Workaround)

Discovered during the same AXPY benchmark session. The RISC-V `mul` instruction produces **incorrect results on follower cores** during SIMD lockstep, while the leader's results are correct.

#### Root Cause
Rocket's `PipelinedMultiplier` (`Multiplier.scala:186`) uses Chisel `Pipe` primitives with `latency=2`:
```scala
val in = Pipe(io.req)                        // stage 1
io.resp.bits.data := Pipe(in.valid, muxed, latency-1).bits  // stage 2
```
`Pipe` is a blind shift register — **it has no stall or enable input**. When the global stall OR-tree freezes the main pipeline (e.g., during a follower D-cache miss), the multiplier's internal pipeline **keeps advancing**. The result arrives at `mul.io.resp` on a cycle when the mul instruction has NOT yet reached WB. By the time the instruction reaches WB, the multiplier output is stale.

The **leader** is unaffected because its D-cache is warm (it just initialized the arrays) — no cache misses, no pipeline stalls, the multiplier stays synchronized.

#### SW Workaround
Compute `a*X` via `slli` + `add` (e.g., `3*X = (X<<1) + X`). These are single-cycle ALU operations that stay in the main pipeline and cannot desync.

#### Future HW Fix
Add a stall/enable input to `PipelinedMultiplier` gated by `ctrl_stalld`, so the internal `Pipe` registers freeze when the main pipeline stalls:
```scala
class PipelinedMultiplier(width: Int, latency: Int, ...) extends Module {
  val io = IO(new Bundle {
    val req = Flipped(Valid(...))
    val resp = Valid(...)
    val stall = Input(Bool())  // NEW
  })
  // Replace Pipe() with stall-gated ShiftRegister
}
```

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
The Systolic Mesh SIMD architecture has evolved from a software-padded proof-of-concept into a fully hardware-synchronized lockstep system. Follower-Fetch PC broadcast provides accurate exception tracking, WFI wakeup enables zero-overhead idle waiting, CSR pipeline flushing eliminates Leader-side NOP delays, and the **global stall OR-tree** guarantees that no broadcasted instruction is ever dropped due to D-cache misses or pipeline replays. The D-cache contention livelock has been fully resolved via **Store-Done Tracking** (selective replay gating + per-core D-cache suppression), eliminating the need for software cache-line padding. The **store-nack stall_out race** (§4.viii) has been fixed by adding `dcache_blocked` to the follower stall backpressure, ensuring store instructions are never silently dropped during cache line upgrades. Custom CSRs dynamically expose real-time mesh data to the executable cores.

An AXPY benchmark (N=64, 4 cores, 16 elements/core) demonstrates **419 cycles SIMD vs 502 cycles MIMD** (~17% speedup) over a manual fork-join implementation. The `PipelinedMultiplier` desync (§4.ix) remains an open issue with a software workaround (`slli+add` instead of `mul`).

Fully verified on 2026-03-22 with Verilator (`VerilatorQuadRocketMXUConfig`).
