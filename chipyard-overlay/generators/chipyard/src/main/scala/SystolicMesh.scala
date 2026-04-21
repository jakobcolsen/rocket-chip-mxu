package chipyard

import chisel3._
import chisel3.util._
import freechips.rocketchip.tile.{RocketTile, SystolicInputBundle, SystolicOutputBundle, SystolicEnabledKey}
import freechips.rocketchip.subsystem.BaseSubsystem
import freechips.rocketchip.diplomacy.{LazyModule, BundleBridgeSource, BundleBridgeSink}

trait HasSystolicMesh { this: freechips.rocketchip.subsystem.InstantiatesHierarchicalElements =>
  implicit val p: org.chipsalliance.cde.config.Parameters
  
  lazy val rocket_tiles = totalTiles.values.toSeq.collect { case t: RocketTile => t }

  // Tile INPUTS (West/North/Ctrl) need a SOURCE driving them
  val systolic_input_sources = if (p(SystolicEnabledKey)) {
    rocket_tiles.sortBy(_.tileId).map { t =>
      val s = BundleBridgeSource(() => new SystolicInputBundle)
      t.systolic_in_node.foreach(_ := s)
      s
    }
  } else Seq.empty

  // Tile OUTPUTS (East/South/MasterCtrl) need a SINK receiving them
  val systolic_output_sinks = if (p(SystolicEnabledKey)) {
    rocket_tiles.sortBy(_.tileId).map { t =>
      val s = BundleBridgeSink[SystolicOutputBundle]()
      t.systolic_out_node.foreach(s := _)
      s
    }
  } else Seq.empty
}

trait HasSystolicMeshModule { this: ChipyardSystemModule =>
  // Access the bundles
  val sys_inputs  = outer.asInstanceOf[HasSystolicMesh].systolic_input_sources.map(_.bundle)
  val sys_outputs = outer.asInstanceOf[HasSystolicMesh].systolic_output_sinks.map(_.bundle)

  try {
    val count = sys_inputs.length
    if (count > 0) { // Only wire if sources exist (implied enabled)
      if (count == 4 || count == 16) {
        val dim = math.sqrt(count).toInt
      println(s"Systolic Mesh: Configuring ${dim}x${dim} mesh")

      for (r <- 0 until dim) {
        for (c <- 0 until dim) {
          val idx = r * dim + c
          val curr_in  = sys_inputs(idx)
          val curr_out = sys_outputs(idx)

          // --- East-West Connection (A Matrix) ---
          if (c > 0) {
            val left_out = sys_outputs(r * dim + c - 1)
            curr_in.west_in := left_out.east_out
          } else {
            curr_in.west_in := 0.U  // Left edge: no upstream data
          }

          // --- North-South Connection (B Matrix) ---
          if (r > 0) {
            val up_out = sys_outputs((r - 1) * dim + c)
            curr_in.north_in := up_out.south_out
          } else {
            curr_in.north_in := 0.U // Top edge: no upstream data
          }

          // --- Control Signals ---
          // Source 0 (Tile 0) drives global mode
          val global_simd_mode = sys_outputs(0).systolic_simd_mode

          // Instruction Broadcast: Source 0 (Tile 0) drives global instruction
          val global_instruction = sys_outputs(0).instruction
          val global_instruction_valid = sys_outputs(0).instruction_valid
          val global_flush = sys_outputs(0).systolic_flush_out
          val global_pc = sys_outputs(0).pc
          if (r == 0 && c == 0) {
            curr_in.instruction := 0.U
            curr_in.instruction_valid := false.B
            curr_in.pc := 0.U
            curr_in.systolic_flush := false.B
          } else {
            curr_in.instruction := global_instruction
            curr_in.instruction_valid := global_instruction_valid
            curr_in.pc := global_pc
            curr_in.systolic_flush := global_flush
          }

          // Drive Enable using effective_enable (defined below, after drain logic)
        }
      }

      // --- Global Stall OR-Tree (replaces old barrier bitmap) ---
      // When ANY core asserts systolic_stall_out, the entire array freezes.
      // This is a combinational OR across all tile outputs — the result is
      // broadcast to every core's systolic_stall input, freezing the Leader's
      // Decode stage so it holds the current instruction until all Followers
      // have digested it.
      val global_stall = sys_outputs.map(_.systolic_stall_out).reduce(_ || _)
      val global_replay = sys_outputs.map(_.systolic_replay_out).reduce(_ || _)

      // --- SIMD Pipeline Drain (BUG-001 fix) ---
      // When the leader disables SIMD mode (csrw 0x800, 0), keep systolic_enable
      // asserted until ALL cores have finished their in-flight D-cache operations.
      // This prevents follower stores in EX/MEM from being killed prematurely.
      val raw_simd_mode = sys_outputs(0).systolic_simd_mode
      val all_mem_idle = !sys_outputs.map(_.simd_mem_active).reduce(_ || _)
      
      // To prevent a one-cycle glitch where effective_enable drops low before
      // the drain register catches it, we define effective_enable combinatorially:
      // It is high if raw_simd_mode is high, OR if it was high last cycle and
      // memory operations are not yet idle.
      val effective_enable = Wire(Bool())
      val effective_enable_prev = RegNext(effective_enable, false.B)
      effective_enable := raw_simd_mode || (effective_enable_prev && !all_mem_idle)

      // --- SIMD Memory Token Ring (BUG-002 fix) ---
      // A 2-bit counter that rotates when the current token holder has no
      // in-flight D-cache operation. Only the core whose hartid matches
      // the token may issue D-cache requests — this serializes memory
      // access and prevents D-cache contention.
      val mem_token = RegInit(0.U(2.W))
      val current_holder_mem_active = VecInit(sys_outputs.map(_.simd_mem_active))(mem_token)
      when (effective_enable && !global_stall && !current_holder_mem_active) {
        mem_token := Mux(mem_token === (count - 1).U, 0.U, mem_token + 1.U)
      }

      for (i <- 0 until count) {
        sys_inputs(i).systolic_enable := effective_enable
        sys_inputs(i).systolic_stall := global_stall
        sys_inputs(i).systolic_replay := global_replay
        sys_inputs(i).simd_mem_token := Mux(effective_enable, mem_token, i.U)
      }

    } else {
      println(s"Systolic Mesh: tile count $count is not 4 or 16. Skipping wiring.")
      sys_inputs.foreach { in =>
        in.west_in := 0.U
        in.north_in := 0.U
        in.systolic_enable := false.B
        in.systolic_stall  := false.B
        in.systolic_replay := false.B
        in.pc              := 0.U
        in.instruction := 0.U
        in.instruction_valid := false.B
        in.systolic_flush := false.B
        in.simd_mem_token := 0.U
      }
    }
    } // End if (count > 0)
  } catch {
    case e: Exception =>
      println(s"SYSTOLIC MESH ERROR: ${e.getMessage}")
      e.printStackTrace()
      throw e
  }
}
