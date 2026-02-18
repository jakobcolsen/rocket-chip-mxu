#!/bin/bash
set -e

# --- Configuration ---
FIRESIM_DIR="/home/jolsen16/chipyard/sims/firesim"
DEPLOY_DIR="$FIRESIM_DIR/deploy"
WORKLOAD_JSON="hello_systolic.json"

echo "=== Systolic Workload Runner ==="
echo "Note: This script assumes you manually program the FPGA via Vivado."

# 0. Environment Verification
echo ""
echo "[0/3] Verifying Environment..."
if grep -q "default_platform: XilinxAlveoU200InstanceDeployManager" $DEPLOY_DIR/config_runtime.yaml; then
    echo "      [OK] Platform: XilinxAlveoU200InstanceDeployManager"
else
    echo "      [WARNING] Platform mismatch in config_runtime.yaml"
fi

if grep -q "default_simulation_dir: /home/jolsen16/FIRESIM_RUNS_DIR" $DEPLOY_DIR/config_runtime.yaml; then
    echo "      [OK] Sim Dir: ~/FIRESIM_RUNS_DIR"
else
    echo "      [WARNING] Sim Dir mismatch in config_runtime.yaml"
fi

# 1. Find Latest Bitstream
echo ""
echo "[1/3] Bitstream Search..."
# User mentioned ~/firesim-build, checks that before standard location.
SEARCH_DIR="$DEPLOY_DIR/results-build"
if [ -d "/home/jolsen16/firesim-build" ]; then
    SEARCH_DIR="/home/jolsen16/firesim-build"
    echo "      Using user build dir: $SEARCH_DIR"
fi

LATEST_BUILD=$(ls -td $SEARCH_DIR/*/ | head -1)
if [ -z "$LATEST_BUILD" ]; then
    echo "Error: No build results found in $SEARCH_DIR"
    exit 1
fi

BITSTREAM=$(find $LATEST_BUILD -name "*.bit" | head -1)
if [ -z "$BITSTREAM" ]; then
    echo "Warning: Could not find .bit file in $LATEST_BUILD"
else
    echo "      Found Bitstream: $BITSTREAM"
    echo "      (You should use this file in Vivado Hardware Manager)"
fi

# 2. Manual Programming Pause
echo ""
echo "[2/3] MANUAL ACTION REQUIRED:"
echo "      1. Open Vivado Hardware Manager."
echo "      2. Program the device Xilinx Alveo U200."
echo "      3. Select the bitstream file listed above."
echo ""
read -p "      Have you programmed the FPGA? (y/n) " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborting."
    exit 1
fi

# 3. Execution
echo ""
echo "[3/3] Execution:"
cd $DEPLOY_DIR

# Source Environment Variables
# sourceme-manager.sh expects to be in the firesim root
cd $FIRESIM_DIR
source sourceme-manager.sh
cd $DEPLOY_DIR

# Check if we need to update workload in config_runtime.yaml
# We can't easily edit YAML with bash securely without yq, so we warn.
CURRENT_WORKLOAD=$(grep "workload_name:" config_runtime.yaml | awk '{print $2}')
if [ "$CURRENT_WORKLOAD" != "$WORKLOAD_JSON" ]; then
    echo "      [WARNING] config_runtime.yaml workload is '$CURRENT_WORKLOAD'."
    echo "      It should be '$WORKLOAD_JSON'."
    read -p "      Update it manually now? Press Enter when ready."
fi

# echo "      Running: firesim infrasetup (to prepare workload)..."
# firesim infrasetup

echo "      Running: firesim runworkload..."
firesim runworkload
