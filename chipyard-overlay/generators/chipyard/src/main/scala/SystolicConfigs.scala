package chipyard

import org.chipsalliance.cde.config.{Config}
import freechips.rocketchip.devices.tilelink.{BootROMLocated}
import freechips.rocketchip.subsystem.{HasTilesExternalResetVectorKey}
import freechips.rocketchip.tile.SystolicEnabledKey

class WithResetVectorToDRAM extends Config((site, here, up) => {
  case HasTilesExternalResetVectorKey => false
  case BootROMLocated(x) => up(BootROMLocated(x)).map(_.copy(hang = 0x80000000L, driveResetVector = true))
})

class WithResetVectorToBootROM extends Config((site, here, up) => {
  case HasTilesExternalResetVectorKey => false
  case BootROMLocated(x) => up(BootROMLocated(x)).map(_.copy(hang = 0x10000L, driveResetVector = true))
})

class WithSystolicEnabled extends Config((site, here, up) => {
  case SystolicEnabledKey => true
})

class VerilatorQuadRocketMXUConfig extends Config(
  new chipyard.config.WithNPerfCounters(6) ++
  new WithSystolicEnabled ++
  new WithResetVectorToBootROM ++
  new chipyard.config.WithNoTileClockGaters ++
  new chipyard.QuadRocketConfig)

// Vanilla Rocket (no MXU) for regression baseline
class VerilatorQuadRocketBaselineConfig extends Config(
  new chipyard.config.WithNPerfCounters(6) ++
  new WithResetVectorToBootROM ++
  new chipyard.config.WithNoTileClockGaters ++
  new chipyard.QuadRocketConfig)
