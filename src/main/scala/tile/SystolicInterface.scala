package freechips.rocketchip.tile

import chisel3._
import chisel3.util._
import org.chipsalliance.cde.config.Parameters
import org.chipsalliance.diplomacy.bundlebridge.{BundleBridgeNode, BundleBridgeSource}

class SystolicBundle(implicit p: Parameters) extends Bundle {
  val west_in          = Decoupled(UInt(64.W))          // flows Mesh -> Tile
  val east_out         = Flipped(Decoupled(UInt(64.W))) // flows Tile -> Mesh
  val north_in         = Decoupled(UInt(64.W))          // flows Mesh -> Tile
  val south_out        = Flipped(Decoupled(UInt(64.W))) // flows Tile -> Mesh
  val systolic_enable  = Output(Bool())                 // flows Mesh -> Tile
  val systolic_stall   = Output(Bool())                 // flows Mesh -> Tile
  val systolic_master_ctrl = Input(Bool())              // flows Tile -> Mesh
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
  })

  // ------------------------------------------------------------------------
  // SIMPLIFIED PASS-THROUGH LOGIC (No Buffers for MVP)
  // ------------------------------------------------------------------------
  
  // Pipeline Registers
  val reg_a = RegInit(0.U(64.W))
  val reg_b = RegInit(0.U(64.W))
  val reg_valid_a = RegInit(false.B)
  val reg_valid_b = RegInit(false.B)

  // Handshaking Logic
  // For MVP, we assume "Always Ready" if enabled, to synchronize lockstep.
  // In a real flow control, we'd check downstream ready.
  
  io.west_in.ready := true.B
  io.north_in.ready := true.B
  
  when (io.core_data_wen) {
    reg_a := io.core_data_in
    reg_valid_a := true.B
  } .elsewhen (io.west_in.valid) {
    reg_a := io.west_in.bits
    reg_valid_a := true.B
  }
  
  when (io.north_in.valid) {
    reg_b := io.north_in.bits
    reg_valid_b := true.B
  }
  
  // Drive Outputs
  io.east_out.valid := reg_valid_a // Simple forward
  io.east_out.bits  := reg_a
  
  io.south_out.valid := reg_valid_b
  io.south_out.bits  := reg_b
  
  // Drive Core ALU
  io.alu_opA := reg_a
  io.alu_opB := reg_b

}
