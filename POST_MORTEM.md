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

### v. Scalability Notes
*   **4–16 cores**: The combinational OR-tree is `log₂(N)` gate delays deep (~2–4 levels). Negligible compared to ALU or D-cache critical path.
*   **64+ cores**: A registered hierarchical stall tree (grouping cores into sub-arrays) would be needed to meet timing at high clock frequencies. This adds ~1 cycle latency per tree level.
*   **Performance**: The stall only fires on actual D-cache misses. For compute-heavy SIMD kernels (ALU-bound), it is essentially free. Compared to the old 16-NOP gaps per instruction, throughput is significantly improved.

---

## Conclusion
The Systolic Mesh SIMD architecture has evolved from a software-padded proof-of-concept into a fully hardware-synchronized lockstep system. Follower-Fetch PC broadcast provides accurate exception tracking, WFI wakeup enables zero-overhead idle waiting, CSR pipeline flushing eliminates Leader-side NOP delays, and the **global stall OR-tree** guarantees that no broadcasted instruction is ever dropped due to D-cache misses or pipeline replays. The SIMD payload in `hello_simd.c` now uses a single `sb` instruction with zero NOP gaps — verified on 2026-03-04 with Verilator.
