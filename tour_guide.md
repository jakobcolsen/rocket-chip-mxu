# Rocket Chip Systolic Array & SIMD Architecture: A Guided Tour

Welcome to the comprehensive tour guide of your custom Systolic Array and SIMD extension implementation on top of Rocket Chip! 

This guide assumes no prior knowledge of Rocket Chip's deep internals and will walk you through exactly how your multi-core architecture is constructed—from the highest-level configuration macros, down through the toolchains, into the standard Rocket pipeline, and finally to exactly how an instruction from Core 0 executes on Core 15.

---

## 🛠️ 1. The Toolchain: From Code to Silicon (Simulation)

Before diving into the hardware, it's critical to understand the software and hardware build processes that create your simulation environment.

### A. The Hardware Toolchain (Chisel to Verilator)
Rocket Chip is not written in Verilog; it is generated via a Scala-embedded domain-specific language called **Chisel**.
1. **Chisel Execution**: When you run `make` in Chipyard, Scala executes and creates an object graph of your CPU design based on the specified parameters.
2. **FIRRTL**: Once the graph is evaluated, Chisel emits an intermediate representation called FIRRTL (Flexible Intermediate Representation for RTL). Critical optimization passes (like dead code elimination and node deduction) run across this FIRRTL format.
3. **SystemVerilog Emission**: FIRRTL compiles down into massive, single-file SystemVerilog dumps (e.g., `TestHarness.sv`, `Rocket.sv`).
4. **Verilator Compilation**: Verilator parses the SystemVerilog and converts the entire hardware state machine into highly optimized C++ classes (`VTestHarness.h`, `VTestHarness.cc`). These are compiled by GCC to create the final executable simulator binary (`simulator-chipyard.harness-...`).

### B. The Software Toolchain (C to Hardware execution)
The software side involves cross-compiling your C tests (like `hello_simd.c`) to run "bare-metal" (without an OS) on your emulated hardware.
1. **RISC-V GCC**: Your test is compiled using `riscv64-unknown-elf-gcc`. It first links `crt.S` (C Runtime Assembly). This file is entirely responsible for waking up hardware threads, setting up a safe stack pointer (`sp`) and global pointer (`gp`), and then jumping to `main()`. It links the output into a final ELF binary executable.
2. **Execution & Memory Boot**: When you launch the simulator, a C++ module called `SimDRAM.cc` parses the ELF binary via the `+loadmem=binary.riscv` argument. It magically injects the binary data directly into the simulated DRAM before cycle 0.
3. **The Boot Sequence**: Upon processor reset, Rocket fetches its very first instructions from an internal, hardcoded BootROM located at memory address `0x10000`. The code inside the BootROM essentially wakes up the cores, puts followers (Harts 1-15) to sleep in a WFI (Wait For Interrupt) loop, and tells Hart 0 (the leader) to jump to the user payload lying in DRAM at `0x80000000`. 
   *(Note: We originally attempted bypassing this BootROM by overriding the reset vector, but standard BootROM sequencing proved necessary to properly initialize peripheral and pipeline states safely).*

---

## 🏗️ 2. High Level Architecture

Rocket Chip is a generator, meaning it uses parameters (Configs) to stamp out a hardware design. In our case, we are building a multi-core SoC (System on Chip) containing a 2D mesh of Rocket Cores. 

Normally, Rocket Cores are entirely independent and only communicate via memory (TileLink). Your custom architecture introduces a **dedicated side-band network** (a hardwired mesh) that connects these cores directly together to form a Systolic Array and enable SIMD (Single Instruction, Multiple Data) execution.

### The Component Stack
To achieve this, the codebase touches multiple hierarchical layers:
1. **`SystolicConfigs.scala`**: Tells the generator "Yes, please build a Quad-Core or 16-Core system and enable the Systolic mesh."
2. **`SystolicMesh.scala`**: The top-level SoC glue that creates the 2D physical wiring (North/South/East/West) between the tiles.
3. **`RocketTile.scala`**: The "box" around a Rocket Core. It exposes the mesh ports to the outside world.
4. **`SystolicInterface.scala`**: The hardware module inside the Tile that buffers mesh traffic and translates it into signals the CPU Core understands.
5. **`RocketCore.scala`**: The CPU itself. Modified to pause its own execution, listen to the mesh, and execute instructions handed down by a "Leader" core via Custom Control and Status Registers (CSRs).

---

## ⚙️ 3. The Configuration (`SystolicConfigs.scala`)

This file is where we define the hardware parameters using Chisel `Config` fragments.

```scala
class WithSystolicEnabled extends Config((site, here, up) => {
  case SystolicEnabledKey => true
})
```
- **What it does:** Sets a global parameter `SystolicEnabledKey` to `true`. Throughout the codebase, you will see `if (p(SystolicEnabledKey))`—this is how the generator knows whether to actually generate the physical wires for the mesh or strip them out completely to save silicon area.

```scala
class VerilatorQuadRocketMXUConfig extends Config(
  new WithSystolicEnabled ++
  // new WithResetVectorToDRAM ++ // Note: Deliberately Removed!
  new chipyard.config.WithNoTileClockGaters ++
  new chipyard.QuadRocketConfig)
```
- **What it does:** This is the top-level target you actually compile. It mixes properties together: 
  1. Your Systolic Mesh (`WithSystolicEnabled`)
  2. Clock Gating disabled (to prevent Verilator simulation stalls)
  3. The base Quad-Core Rocket layout (`QuadRocketConfig`).
  *(Notice that we removed `WithResetVectorToDRAM` to allow the built-in minimal BootROM to initialize the system cleanly before jumping to memory, as this was much safer than a blind DRAM jump).*

---

## 🕸️ 4. The Top-Level Mesh (`SystolicMesh.scala`)

This is where the magic of the 2D grid happens. It iterates over all instantiated Rocket Tiles and stitches them together.

### Sinks and Sources
```scala
val systolic_input_sources = if (p(SystolicEnabledKey)) {
  rocket_tiles.sortBy(_.tileId).map { t =>
    val s = BundleBridgeSource(() => new SystolicInputBundle)
    t.systolic_in_node.foreach(_ := s)
    s
  }
} // ... and similarly for systolic_output_sinks ...
```
- **What it does:** Rocket Chip uses a framework called "Diplomacy" to draw wires between modules before they are fully compiled. `BundleBridgeSource` and `BundleBridgeSink` are Diplomatic nodes. This code iterates over every tile (0 to N) and creates a physical plug (node) on the outside of each tile for incoming mesh traffic and outgoing mesh traffic.

### The Wiring Matrix
Further down, inside `if (count == 4 || count == 16)`, the code calculates `val dim = math.sqrt(count).toInt` (e.g., a 2x2 or 4x4 grid).
It uses nested `for (r <- 0 until dim)` loops to wire East-West and North-South logic.

```scala
if (c > 0) {
  val left_out = sys_outputs(r * dim + c - 1)
  curr_in.west_in <> left_out.east_out
}
```
- **What it does:** If the core is not on the left edge (`c > 0`), it reaches out to the tile sitting immediately to its left (`c - 1`), grabs its `east_out` bundle, and plugs it into its own `west_in` bundle using the `<>` (bulk connect) operator. 

### Global Instruction & Stall Broadcast
```scala
val global_stall = sys_outputs(0).systolic_master_ctrl
val global_simd_mode = sys_outputs(0).systolic_simd_mode
val global_instruction = sys_outputs(0).instruction
...
if (r == 0 && c == 0) { ... } else {
  curr_in.systolic_stall := global_stall
  curr_in.instruction := global_instruction
}
```
- **What it does:** Tile 0 is heavily hardcoded here as the "Leader" (the master controller). Its `output` ports for `instruction`, `simd_mode`, and `stall` are broadcast identically to the `input` ports of *every single other tile* in the array. This is the backbone of your SIMD lock-step feature!

---

## 🛡️ 5. Tile-Level Integration (`RocketTile.scala`)

Standard Rocket Chip separates the core logic (`RocketCore`) from the cache and external interfaces (the `RocketTile`).

```scala
val systolic_in_node  = if (p(SystolicEnabledKey)) Some(BundleBridgeSink[SystolicInputBundle]()) else None
val systolic_out_node = if (p(SystolicEnabledKey)) Some(BundleBridgeSource(() => new SystolicOutputBundle)) else None
```
- **What it does:** Creates the Diplomatic ports on the boundary of the Tile so `SystolicMesh` can hook into them.

```scala
val systolic_if = Module(new SystolicInterface)
systolic_in_node.foreach { in => systolic_if.io.west_in <> in.bundle.west_in }
// ... wires up North, enables, stall, etc ...
```
- **What it does:** Instantiates your custom hardware `SystolicInterface` inside the tile and connects it to the Diplomatic ports.

```scala
core.io.systolic_enable := systolic_if.io.systolic_enable
core.io.systolic_instruction_in := systolic_if.io.core_inst_in
```
- **What it does:** This is the bridge deeper inwards. It pushes the SIMD broadcast signals from the `SystolicInterface` exactly one level deeper, directly into `RocketCore.scala`.

---

## 🎛️ 6. The Hardware Interface (`SystolicInterface.scala`)

This module brokers all the handshaking and defines the `SystolicInputBundle` and `SystolicOutputBundle`. 

```scala
val q_west  = Module(new Queue(UInt(64.W), 1))
q_west.io.enq.valid := io.west_in.valid
...
```
- **What it does:** Instead of passing wires combinationally (which creates massive timing delays across the chip), the interface instantiates a 1-entry `Queue` (a skid buffer/register) on the incoming West and North ties. This means data takes exactly 1 clock cycle to pass into the tile, cleanly separating the timing domains.

```scala
val injected_a = io.core_data_wen && io.systolic_enable
val source_a_valid = q_west.io.deq.valid || injected_a
val source_a_bits  = Mux(injected_a, io.core_data_in, q_west.io.deq.bits)
```
- **What it does:** This is a multiplexer. The data flowing out the East port (Matrix A) can either come from the West Queue (`q_west`) **OR** the core can actively overwrite it (`injected_a`) by writing to a CSR (`core_data_in`).

```scala
io.alu_opA := source_a_bits
io.alu_opB := Mux(q_north.io.deq.valid, q_north.io.deq.bits, 0.U)
```
- **What it does:** "Snoops" the data as it flows through the node and exposes it to the Core's ALU. This way, if a custom instruction executes inside the CPU, it can just grab `alu_opA` and `alu_opB` to multiply them immediately, without waiting for memory.

---

## 🧠 7. Understanding the Baseline Rocket Pipeline

Before seeing how SIMD hacks the pipeline, you need to understand the normal 5-stage Rocket CPU lifecycle. Rocket is an in-order, scalar pipeline consisting of:
1. **IF (Instruction Fetch)**: The PC queries the internal Instruction Cache (`ibuf`). The cache returns a block of up to 4 instructions.
2. **ID (Instruction Decode)**: Calculates what the instruction does (Add, Load, Branch) and reads operands from the physical integer or floating-point register file. 
3. **EX (Execute)**: The ALU calculates data (for math) or an effective address (for memory loads/stores). Branches evaluate their conditions and trigger PC redirects if a misprediction occurred.
4. **MEM (Memory)**: For Loads/Stores, data is flushed out to the Data Cache (`dcache`).
5. **WB (Writeback)**: Data returning from memory or ALU is committed to the architectural register file.

---

## 🔀 8. The CPU Core SIMD Extension (`RocketCore.scala`)

Your SIMD features essentially hijack the standard 5-stage pipeline at specific, critical moments.

### A. Custom CSRs (Control & Status Registers)
To give software control over this hardware, Rocket uses Custom CSRs.
```scala
def systolicCSR = CustomCSR(0x800, BigInt(3), Some(BigInt(0))) 
def systolicDataCSR = CustomCSR(0x801, BigInt(0), Some(BigInt(0)))
```
- **What it does:** Defines hardware registers mapped to addresses `0x800` and `0x801`. 
  - `0x800` (Control): Bit 0 represents `systolicMasterCtrl` (Global Stall), and Bit 1 represents `systolicSimdMode` (SIMD broadcast active).
  - `0x801` (Data): A 64-bit register for the core to inject data into the systolic array. `systolicDataWen` triggers high for exactly one clock cycle when software writes to this CSR using `csrw 0x801, a0`.

### B. The "Follower" Override
How does Core 1 know it should stop running its own code and listen to Core 0?
```scala
val is_leader = io.hartid === 0.U
val id_systolic_follower = io.systolic_enable && !is_leader
```
- **What it does:** Calculates if the current CPU is a follower. Note the hardcoded `hartid === 0.U`. This means Tile 0 can NEVER be a follower. If `systolic_enable` (which comes from `0x800` on Tile 0 via the Mesh) is high, every core *except* Core 0 flips into "Follower Mode".

### C. Hijacking the Instruction Buffer (Fetch / Decode)
Normally, instructions come from the Instruction Fetch stage (the `ibuf`). 
```scala
val id_effective_inst = Mux(id_systolic_follower, io.systolic_instruction_in, id_inst(0))
val id_effective_valid = Mux(id_systolic_follower, io.systolic_instruction_valid_in, ibuf.io.inst(0).valid)
```
- **What it does:** This is the most crucial hack in the codebase. Inside the **Decode (ID)** stage, we create an `id_effective_inst` multiplexer. If the core is a follower, it completely ignores the instruction fetched by its local Program Counter (`id_inst(0)`) and blindly feeds the pipeline whatever 32-bit `systolic_instruction_in` is being broadcasted over the mesh from Core 0. 

```scala
ibuf.io.inst(0).ready := !ctrl_stalld && !(customCSRs.systolicSimdMode && !is_leader)
```
- **What it does:** It tells the **Instruction Fetch (IF)** unit: "Hey, do NOT pop the next instruction off the cache queue." This freezes the Follower's local Program Counter (PC) safely in place while the downstream pipeline digests the injected SIMD instructions.

### D. Pipeline Control and Safety Overrides
When injecting foreign instructions into a pipeline, things can go disastrously wrong if you don't suppress local safety checks.
```scala
val id_xcpt = Mux(id_systolic_follower, false.B, id_xcpt_raw)
```
- **What it does:** If an instruction comes from Core 0, the local cache might throw an exception (e.g., page fault). We explicitly mask off (`false.B`) any decode exceptions thrown by the local follower because we aren't executing the local instruction anyway!

```scala
val ex_op1 = Mux(io.systolic_enable && customCSRs.systolicSimdMode && !is_leader, io.systolic_opA.asSInt, ex_op1_normal)
```
- **What it does:** In the **Execute (EX)** stage, if SIMD mode is active, the ALU inputs (`op1` and `op2`) are hijacked. Instead of reading from the CPU register file (e.g., register `t0`), the inputs are hard-wired to read from the East-West and North-South flows passing through the `SystolicInterface`. 

### E. Masking Replays & Kills
```scala
ctrl_killd := !id_effective_valid || (ibuf.io.inst(0).bits.replay && !id_systolic_follower) || take_pc_mem_wb || ctrl_stalld || csr.io.interrupt
```
- **What it does:** The standard Rocket Chip will "kill" an instruction (flush it from the pipeline) if the Instruction Buffer marks it for "replay" (meaning the cache wasn't ready). Because Followers freeze their `ibuf`, it often stays in a "replay" state eternally. The added `&& !id_systolic_follower` ensures that Followers ignore their local freeze state and execute the broadcasted instruction anyway.

### F. Broadcasting from the Leader
```scala
io.systolic_instruction_out := id_inst(0)
io.systolic_instruction_valid_out := is_leader && !ctrl_killd
```
- **What it does:** At the very bottom of the core file, we wire the outputs. Core 0 (`is_leader`) takes the instruction currently in its decode stage (`id_inst(0)`) and sends it out to the Mesh (`systolic_instruction_out`). It is only flagged as `valid` if it wasn't killed by a stall or branch misprediction (`!ctrl_killd`).

---

## 🚀 Summary of the Data Flow
1. **Core 0 (Leader)** boots up and writes `3` to CSR `0x800`.
2. This sets `systolicSimdMode` to high in Core 0's hardware.
3. This signal propagates out through `RocketTile` -> `SystolicMesh`, hitting all other cores.
4. **Cores 1-15 (Followers)** receive this signal, evaluate `id_systolic_follower = true`, freeze their local instruction caches (`ibuf.ready = false`), and mask their local exceptions.
5. **Core 0** executes an instruction (e.g., `add t0, t1, t2`).
6. As that instruction passes through Core 0's **Decode** stage, it is broadcast out onto the mesh.
7. **Cores 1-15** receive the instruction through `id_effective_inst` and execute it precisely in lock-step, utilizing data flowing dynamically through the `SystolicInterface` queues rather than their local register files.
