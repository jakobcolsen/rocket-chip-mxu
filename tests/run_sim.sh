#!/bin/bash
BINARY=${1:-/home/jolsen16/rocket-chip-mxu/tests/hello_simd.riscv}
LOG=${2:-/home/jolsen16/rocket-chip-mxu/tests/hello_simd_output.log}
cd /home/jolsen16/chipyard
source env.sh
stdbuf -oL -eL /home/jolsen16/chipyard/sims/verilator/simulator-chipyard.harness-VerilatorQuadRocketMXUConfig +permissive +uart_tx_printf=1 +permissive-off $BINARY 2>&1 | tee $LOG
