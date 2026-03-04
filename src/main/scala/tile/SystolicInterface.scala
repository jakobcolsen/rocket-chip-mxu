package freechips.rocketchip.tile

import chisel3._
import chisel3.util._
import org.chipsalliance.cde.config.Parameters
import org.chipsalliance.diplomacy.bundlebridge.{BundleBridgeNode, BundleBridgeSource}

// Flows from Mesh Network -> Tile
class SystolicInputBundle(implicit p: Parameters) extends Bundle {
  val west_in          = Decoupled(UInt(64.W))
  val north_in         = Decoupled(UInt(64.W))
  val systolic_enable  = Bool()
  val systolic_stall   = Bool()      // Global stall from mesh OR-tree
  val instruction      = UInt(32.W)  // Instruction from Leader
  val instruction_valid = Bool()     // Leader is executing
  val pc               = UInt(64.W)  // Leader's Program Counter
}

// Flows from Tile -> Mesh Network
class SystolicOutputBundle(implicit p: Parameters) extends Bundle {
  val east_out         = Decoupled(UInt(64.W))
  val south_out        = Decoupled(UInt(64.W))
  val systolic_master_ctrl = Bool()
  val systolic_simd_mode   = Bool()  // Broadcast Leader Mode
  val systolic_stall_out   = Bool()  // Core needs the array to stall
  val instruction      = UInt(32.W)  // Instruction from Leader
  val instruction_valid = Bool()     // Leader is executing
  val pc               = UInt(64.W)  // Leader's Program Counter
}

class SystolicInterface(implicit p: Parameters) extends Module {
  val io = IO(new Bundle {
    // Neighbor Connections (Data Flow)
    // West -> East (Matrix A)
    val west_in  = Flipped(Decoupled(UInt(64.W)))
    val east_out = Decoupled(UInt(64.W))
    
    // North -> South (Matrix B)
    val north_in = Flipped(Decoupled(UInt(64.W)))
    val south_out = Decoupled(UInt(64.W))

    // Core Interactions
    val alu_opA = Output(UInt(64.W))
    val alu_opB = Output(UInt(64.W))
    
    // Control
    val systolic_enable = Input(Bool())
    val systolic_stall  = Input(Bool())
    
    // Core Data Injection (for Software-driven Mesh)
    val core_data_in  = Input(UInt(64.W))
    val core_data_wen = Input(Bool())

    // Instruction and PC Broadcast
    val instruction_out = Output(UInt(32.W)) // To Mesh (Leader)
    val instruction_valid_out = Output(Bool()) // To Mesh (Leader)
    val pc_out          = Output(UInt(64.W)) // Leader's PC to Mesh
    val instruction_in  = Input(UInt(32.W))  // From Mesh (Follower)
    val instruction_valid_in = Input(Bool()) // From Mesh (Follower)
    val pc_in           = Input(UInt(64.W))  // Leader's PC from Mesh

    val core_inst_out   = Input(UInt(32.W))  // From Core (Leader Source)
    val core_inst_valid_out = Input(Bool())  // From Core (Leader Source)
    val core_pc_out     = Input(UInt(64.W))  // Leader's PC from Core
    val core_inst_in    = Output(UInt(32.W)) // To Core (Follower)
    val core_inst_valid_in = Output(Bool())  // To Core (Follower)
    val core_pc_in      = Output(UInt(64.W)) // Leader's PC to Core Follower

    // Global Stall Backpressure
    val core_stall_out  = Input(Bool())      // From Core: core needs array to stall
    val stall_out       = Output(Bool())     // To Mesh: passthrough (combinational)
  })

  // ------------------------------------------------------------------------
  // Proper Decoupled Handshaking Logic
  // ------------------------------------------------------------------------
  
  // Design Choice: Use 1-entry Queues (skid buffers) to break combinational paths
  // and handle ready/valid clean-up.
  
  val q_west  = Module(new Queue(UInt(64.W), 1))
  val q_north = Module(new Queue(UInt(64.W), 1))
  
  // Inputs feed into queues
  q_west.io.enq.valid := io.west_in.valid
  q_west.io.enq.bits  := io.west_in.bits
  io.west_in.ready    := q_west.io.enq.ready
  
  q_north.io.enq.valid := io.north_in.valid
  q_north.io.enq.bits  := io.north_in.bits
  io.north_in.ready    := q_north.io.enq.ready
  
  // Pipeline Registers / State
  // We only consume from queues if we can output OR if we are just latching internal ops
  // For this simple systolic node, we forward inputs -> outputs.
  
  // 1. Data Injection Override
  // If core writes, it takes priority and behaves like a "valid" input source
  val injected_a = io.core_data_wen && io.systolic_enable
  val source_a_valid = q_west.io.deq.valid || injected_a
  val source_a_bits  = Mux(injected_a, io.core_data_in, q_west.io.deq.bits)
  
  // 2. Output Logic (East)
  // We forward A (West/Core) -> East
  io.east_out.valid := source_a_valid && !io.systolic_stall
  io.east_out.bits  := source_a_bits
  
  // We consume West if we are running and either logic injected (fake consumption) or East accepted
  // Note: core injection doesn't pop West queue, it overrides it.
  q_west.io.deq.ready := !injected_a && io.east_out.ready && !io.systolic_stall

  // 3. Output Logic (South)
  // We forward B (North) -> South
  io.south_out.valid := q_north.io.deq.valid && !io.systolic_stall
  io.south_out.bits  := q_north.io.deq.bits
  
  q_north.io.deq.ready := io.south_out.ready && !io.systolic_stall
  
  // 4. Drive Core ALU
  // We snoop the data being passed through.
  // Ideally, this should be latched when the handshake happens.
  // For now, asynchronous snoop of the data presented to outputs.
  io.alu_opA := source_a_bits
  io.alu_opB := Mux(q_north.io.deq.valid, q_north.io.deq.bits, 0.U)

  // 5. Instruction and PC Broadcast Logic
  // Leader: instruction_out = core_inst_out (from IB/Core)
  io.instruction_out       := io.core_inst_out
  io.instruction_valid_out := io.core_inst_valid_out
  io.pc_out                := io.core_pc_out

  // Follower: core_inst_in = instruction_in (from Mesh)
  io.core_inst_in          := io.instruction_in
  io.core_inst_valid_in    := io.instruction_valid_in
  io.core_pc_in            := io.pc_in

  // 6. Global Stall Backpressure (combinational passthrough)
  io.stall_out := io.core_stall_out
}
