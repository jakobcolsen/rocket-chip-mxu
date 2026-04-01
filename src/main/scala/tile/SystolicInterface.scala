package freechips.rocketchip.tile

import chisel3._
import chisel3.util._
import org.chipsalliance.cde.config.Parameters
import org.chipsalliance.diplomacy.bundlebridge.{BundleBridgeNode, BundleBridgeSource}

// Flows from Mesh Network -> Tile
class SystolicInputBundle(implicit p: Parameters) extends Bundle {
  val west_in          = UInt(64.W)       // Shift register data (was Decoupled)
  val north_in         = UInt(64.W)       // Shift register data (was Decoupled)
  val systolic_enable  = Bool()
  val systolic_stall   = Bool()           // Global stall from mesh OR-tree
  val systolic_replay  = Bool()           // Global replay from mesh OR-tree
  val instruction      = UInt(32.W)       // Instruction from Leader
  val instruction_valid = Bool()          // Leader is executing
  val pc               = UInt(64.W)       // Leader's Program Counter
  val systolic_btb_taken = Bool()         // Leader's BTB prediction (1=taken)
  val systolic_flush   = Bool()           // Leader's pipeline flush broadcast
}

// Flows from Tile -> Mesh Network
class SystolicOutputBundle(implicit p: Parameters) extends Bundle {
  val east_out         = UInt(64.W)       // Shift register data (was Decoupled)
  val south_out        = UInt(64.W)       // Shift register data (was Decoupled)
  val systolic_master_ctrl = Bool()
  val systolic_simd_mode   = Bool()       // Broadcast Leader Mode
  val systolic_stall_out   = Bool()       // Core needs the array to stall
  val systolic_replay_out  = Bool()       // Core needs the array to replay
  val instruction      = UInt(32.W)       // Instruction from Leader
  val instruction_valid = Bool()          // Leader is executing
  val pc               = UInt(64.W)       // Leader's Program Counter
  val systolic_btb_taken_out = Bool()     // Leader's BTB prediction (1=taken)
  val systolic_flush_out = Bool()         // Leader's pipeline flush broadcast
}

class SystolicInterface(implicit p: Parameters) extends Module {
  val io = IO(new Bundle {
    // Neighbor Connections (Shift Register Data Flow)
    // West -> East (Matrix A)
    val west_in  = Input(UInt(64.W))
    val east_out = Output(UInt(64.W))

    // North -> South (Matrix B)
    val north_in  = Input(UInt(64.W))
    val south_out = Output(UInt(64.W))

    // Core Read Ports (snooped from shift registers)
    val alu_opA = Output(UInt(64.W))     // West register value (read via CSR 0x801)
    val alu_opB = Output(UInt(64.W))     // North register value (read via CSR 0x802)

    // Core Write Ports (driven by CSR writes)
    val core_east_data  = Input(UInt(64.W))  // Value to write to East output register
    val core_east_wen   = Input(Bool())      // Write-enable for East (CSR 0x801 write)
    val core_south_data = Input(UInt(64.W))  // Value to write to South output register
    val core_south_wen  = Input(Bool())      // Write-enable for South (CSR 0x802 write)

    // Control
    val systolic_enable = Input(Bool())
    val systolic_stall  = Input(Bool())
    val systolic_replay = Input(Bool())

    // Instruction and PC Broadcast
    val instruction_out = Output(UInt(32.W))
    val instruction_valid_out = Output(Bool())
    val pc_out          = Output(UInt(64.W))
    val instruction_in  = Input(UInt(32.W))
    val instruction_valid_in = Input(Bool())
    val pc_in           = Input(UInt(64.W))

    val core_inst_out   = Input(UInt(32.W))
    val core_inst_valid_out = Input(Bool())
    val core_pc_out     = Input(UInt(64.W))
    val core_inst_in    = Output(UInt(32.W))
    val core_inst_valid_in = Output(Bool())
    val core_pc_in      = Output(UInt(64.W))

    // Global Stall & Replay Backpressure
    val core_stall_out  = Input(Bool())
    val stall_out       = Output(Bool())
    val core_replay_out = Input(Bool())
    val replay_out      = Output(Bool())
    val core_btb_taken_out = Input(Bool()) // Leader btb_taken from core
    val btb_taken_out   = Output(Bool())   // Leader btb_taken to mesh
    val core_flush_out  = Input(Bool())
    val flush_out       = Output(Bool())
    val flush_in        = Input(Bool())
    val core_flush_in   = Output(Bool())

    // Systolic ALU-to-ALU Auto-Forward (from core)
    val core_east_auto_data  = Input(UInt(64.W))
    val core_east_auto_wen   = Input(Bool())
    val core_south_auto_data = Input(UInt(64.W))
    val core_south_auto_wen  = Input(Bool())
  })

  // ------------------------------------------------------------------------
  // Shift Register Data Path (replaces Queue-based handshake)
  // ------------------------------------------------------------------------

  // Input shift registers: latch upstream value on every non-stalled clock edge
  val reg_west  = RegInit(0.U(64.W))
  val reg_north = RegInit(0.U(64.W))

  when (!io.systolic_stall) {
    reg_west  := io.west_in
    reg_north := io.north_in
  }

  // Output registers: written by core via CSR
  val reg_east  = RegInit(0.U(64.W))
  val reg_south = RegInit(0.U(64.W))

  when (io.core_east_wen)  { reg_east  := io.core_east_data }
  .elsewhen (io.core_east_auto_wen) { reg_east := io.core_east_auto_data }
  when (io.core_south_wen) { reg_south := io.core_south_data }
  .elsewhen (io.core_south_auto_wen) { reg_south := io.core_south_auto_data }

  // Drive outputs to neighbors
  io.east_out  := reg_east
  io.south_out := reg_south

  // Drive core read ports (snoop shift registers)
  io.alu_opA := reg_west
  io.alu_opB := reg_north

  // Instruction and PC Broadcast Logic
  io.instruction_out       := io.core_inst_out
  io.instruction_valid_out := io.core_inst_valid_out
  io.pc_out                := io.core_pc_out

  io.core_inst_in          := io.instruction_in
  io.core_inst_valid_in    := io.instruction_valid_in
  io.core_pc_in            := io.pc_in

  // Global Stall & Replay Backpressure (combinational passthrough)
  io.stall_out := io.core_stall_out
  io.replay_out := io.core_replay_out
  io.btb_taken_out := io.core_btb_taken_out
  io.flush_out := io.core_flush_out
  io.core_flush_in := io.flush_in
}
