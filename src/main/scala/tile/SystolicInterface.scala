package freechips.rocketchip.tile

import chisel3._
import chisel3.util._
import org.chipsalliance.cde.config.Parameters

class SystolicMeshToTileBundle(implicit p: Parameters) extends Bundle {
  val west_in         = Flipped(Decoupled(UInt(64.W)))
  val north_in        = Flipped(Decoupled(UInt(64.W)))
  val systolic_enable = Input(Bool())
  val systolic_stall  = Input(Bool())
}

class SystolicTileToMeshBundle(implicit p: Parameters) extends Bundle {
  val east_out             = Decoupled(UInt(64.W))
  val south_out            = Decoupled(UInt(64.W))
  val systolic_master_ctrl = Output(Bool())
}

class SystolicInterface(implicit p: Parameters) extends Module {
  val io = IO(new Bundle {
    // Neighbor Connections (Data Flow)
    val west_in   = Flipped(Decoupled(UInt(64.W)))
    val east_out  = Decoupled(UInt(64.W))
    val north_in  = Flipped(Decoupled(UInt(64.W)))
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

  val flowAllowed = io.systolic_enable && !io.systolic_stall

  // 1-deep canonical Decoupled stage for West->East path.
  // Core injection has priority over mesh input when both are present.
  val westBits = Reg(UInt(64.W))
  val westValid = RegInit(false.B)
  val westPop = westValid && flowAllowed && io.east_out.ready
  val westCanLoad = !westValid || westPop

  io.west_in.ready := westCanLoad && !io.core_data_wen

  when (westCanLoad) {
    when (io.core_data_wen) {
      westBits := io.core_data_in
      westValid := true.B
    } .elsewhen (io.west_in.fire) {
      westBits := io.west_in.bits
      westValid := true.B
    } .otherwise {
      westValid := false.B
    }
  }

  io.east_out.valid := westValid && flowAllowed
  io.east_out.bits := westBits

  // 1-deep canonical Decoupled stage for North->South path.
  val northBits = Reg(UInt(64.W))
  val northValid = RegInit(false.B)
  val northPop = northValid && flowAllowed && io.south_out.ready
  val northCanLoad = !northValid || northPop

  io.north_in.ready := northCanLoad

  when (northCanLoad) {
    when (io.north_in.fire) {
      northBits := io.north_in.bits
      northValid := true.B
    } .otherwise {
      northValid := false.B
    }
  }

  io.south_out.valid := northValid && flowAllowed
  io.south_out.bits := northBits

  io.alu_opA := Mux(westValid, westBits, 0.U)
  io.alu_opB := Mux(northValid, northBits, 0.U)
}
