#!/bin/bash
# run_benchmarks_firesim.sh — MXU Benchmark Suite on FireSim (Alveo U200)
#
# Handles:
#   1. XDMA driver check/reload
#   2. Bitstream existence verification
#   3. Workload JSON generation per benchmark
#   4. FireSim infrasetup + runworkload
#   5. Result parsing → results_firesim.csv
#
# Usage:
#   ./run_benchmarks_firesim.sh                  — run full suite
#   ./run_benchmarks_firesim.sh axpy_simd_64     — run single benchmark
#
# Prerequisites:
#   - Benchmark .riscv files built (run 'make' in this directory first)
#   - FPGA programmed with MXU bitstream (via firesim infrasetup or Vivado)

set -e

# ── Configuration ──────────────────────────────────────────────────────
CHIPYARD_DIR="/home/jolsen16/chipyard"
FIRESIM_DIR="$CHIPYARD_DIR/sims/firesim"
DEPLOY_DIR="$FIRESIM_DIR/deploy"
WORKLOAD_DIR="$DEPLOY_DIR/workloads"
BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_CSV="$BENCH_DIR/results_firesim.csv"
SIM_RUN_DIR="/home/jolsen16/FIRESIM_RUNS_DIR"

HW_CONFIG="alveo_u200_firesim_rocket_quadcore_mxu"
WORKLOAD_JSON_NAME="mxu_bench.json"

# XDMA driver locations (multiple versions)
XDMA_DRIVER_DIRS=(
    "/home/jolsen16/dma_ip_drivers/XDMA/linux-kernel/xdma"
    "/home/jolsen16/dma_ip_drivers_6x/XDMA/linux-kernel/xdma"
    "/home/jolsen16/dma_ip_drivers_xvsec/XDMA/linux-kernel/xdma"
)

echo "╔══════════════════════════════════════════════════════╗"
echo "║   MXU Benchmark Suite — FireSim (Alveo U200)        ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

# ── Step 0: Source FireSim environment ─────────────────────────────────
echo "[0/5] Sourcing FireSim environment..."
cd "$FIRESIM_DIR"
source sourceme-manager.sh --skip-ssh-setup 2>/dev/null || {
    echo "  Falling back to chipyard env..."
    cd "$CHIPYARD_DIR"
    source env.sh
}
cd "$BENCH_DIR"
echo "  ✓ Environment ready"
echo ""

# ── Step 1: XDMA Driver Check ─────────────────────────────────────────
echo "[1/5] Checking XDMA driver..."

xdma_loaded() {
    lsmod 2>/dev/null | grep -q "^xdma" && return 0
    ls /dev/xdma* 2>/dev/null | head -1 > /dev/null && return 0
    return 1
}

xdma_reload() {
    echo "  Attempting XDMA driver reload..."

    # Try to unload first (ignore errors)
    sudo rmmod xdma 2>/dev/null || true
    sleep 1

    # Find a working driver directory
    for XDMA_DIR in "${XDMA_DRIVER_DIRS[@]}"; do
        if [ -f "$XDMA_DIR/xdma.ko" ] || [ -f "$XDMA_DIR/Makefile" ]; then
            echo "  Trying driver from: $XDMA_DIR"

            # Build if needed
            if [ ! -f "$XDMA_DIR/xdma.ko" ]; then
                echo "  Building XDMA driver..."
                (cd "$XDMA_DIR" && sudo make clean && sudo make) 2>&1 | tail -3
            fi

            # Load
            if [ -f "$XDMA_DIR/xdma.ko" ]; then
                sudo insmod "$XDMA_DIR/xdma.ko" 2>/dev/null && {
                    sleep 2
                    if xdma_loaded; then
                        echo "  ✓ XDMA driver loaded from $XDMA_DIR"
                        return 0
                    fi
                }
            fi
        fi
    done

    echo "  ✗ Could not load XDMA driver from any known location"
    return 1
}

if xdma_loaded; then
    echo "  ✓ XDMA driver already loaded"
    # Show device nodes
    XDMA_DEVS=$(ls /dev/xdma* 2>/dev/null | head -5)
    [ -n "$XDMA_DEVS" ] && echo "  Devices: $(echo $XDMA_DEVS | tr '\n' ' ')"
else
    echo "  ✗ XDMA driver not loaded"
    xdma_reload || {
        echo ""
        echo "ERROR: XDMA driver is required for FireSim on Alveo U200."
        echo "Manual fix options:"
        echo "  1. cd /home/jolsen16/dma_ip_drivers/XDMA/linux-kernel/xdma"
        echo "  2. sudo make clean && sudo make"
        echo "  3. sudo insmod xdma.ko"
        echo ""
        echo "If that fails, the FPGA may need to be re-programmed first."
        exit 1
    }
fi
echo ""

# ── Step 2: Bitstream Check ───────────────────────────────────────────
echo "[2/5] Checking bitstream..."

# Check the hwdb entry for our config
BITSTREAM_TAR=$(grep -A5 "^${HW_CONFIG}:" "$DEPLOY_DIR/config_hwdb.yaml" \
    | grep "bitstream_tar:" | head -1 | sed 's/.*bitstream_tar: *//' | tr -d '"')

if [ -z "$BITSTREAM_TAR" ] || [ "$BITSTREAM_TAR" = "null" ]; then
    echo "  ✗ No bitstream_tar found for '$HW_CONFIG' in config_hwdb.yaml"
    echo "  You need to build the bitstream first:"
    echo "    cd $FIRESIM_DIR && firesim buildbitstream"
    exit 1
fi

# Check if it's a local file
if [[ "$BITSTREAM_TAR" == file://* ]]; then
    LOCAL_PATH="${BITSTREAM_TAR#file://}"
    if [ -f "$LOCAL_PATH" ]; then
        echo "  ✓ Bitstream found: $(basename $LOCAL_PATH)"
        echo "    $(du -h "$LOCAL_PATH" | cut -f1) at $LOCAL_PATH"
    else
        echo "  ✗ Bitstream file missing: $LOCAL_PATH"
        echo "  Rebuild with: firesim buildbitstream"
        exit 1
    fi
else
    echo "  ℹ Bitstream is remote: $BITSTREAM_TAR"
    echo "  (will be downloaded by firesim infrasetup)"
fi

# Check if FPGA is currently programmed (via xdma device presence)
if xdma_loaded && ls /dev/xdma0_* 2>/dev/null | head -1 > /dev/null; then
    echo "  ✓ FPGA appears programmed (xdma0 devices present)"
else
    echo "  ⚠ FPGA may not be programmed — firesim infrasetup will handle this"
fi
echo ""

# ── Step 3: Verify runtime config ─────────────────────────────────────
echo "[3/5] Verifying runtime configuration..."

CURRENT_HW=$(grep "default_hw_config:" "$DEPLOY_DIR/config_runtime.yaml" \
    | awk '{print $2}')
if [ "$CURRENT_HW" != "$HW_CONFIG" ]; then
    echo "  ⚠ config_runtime.yaml has hw_config='$CURRENT_HW'"
    echo "    Expected: '$HW_CONFIG'"
    echo "    Updating..."
    sed -i "s/default_hw_config: .*/default_hw_config: $HW_CONFIG/" \
        "$DEPLOY_DIR/config_runtime.yaml"
    echo "  ✓ Updated"
else
    echo "  ✓ Hardware config: $HW_CONFIG"
fi

# Verify platform
PLATFORM=$(grep "default_platform:" "$DEPLOY_DIR/config_runtime.yaml" | awk '{print $2}')
echo "  ✓ Platform: $PLATFORM"
echo "  ✓ Sim dir: $SIM_RUN_DIR"
echo ""

# ── Step 4: Build benchmarks if needed ─────────────────────────────────
echo "[4/5] Checking benchmark binaries..."
cd "$BENCH_DIR"

RISCV_COUNT=$(ls *.riscv 2>/dev/null | wc -l)
if [ "$RISCV_COUNT" -eq 0 ]; then
    echo "  Building benchmarks..."
    make -j$(nproc) 2>&1 | tail -3
    RISCV_COUNT=$(ls *.riscv 2>/dev/null | wc -l)
fi
echo "  ✓ $RISCV_COUNT benchmark binaries available"
echo ""

# ── Step 5: Run benchmarks ─────────────────────────────────────────────
echo "[5/5] Running benchmarks via FireSim..."

# Determine which benchmarks to run
if [ -n "$1" ]; then
    TARGETS="$1.riscv"
else
    TARGETS=$(ls *.riscv 2>/dev/null | sort)
fi

TOTAL=$(echo "$TARGETS" | wc -w)
echo "  Running $TOTAL benchmark(s)..."
echo ""

# Initialize CSV
echo "benchmark,mode,N,cycles,instret,ipc,ops_per_cycle,pass" > "$RESULTS_CSV"

# Helper functions (same as Verilator version)
bench_mode() {
    case "$1" in
        *_simd*)     echo "simd" ;;
        *_mimd*)     echo "mimd" ;;
        *_csr*)      echo "csr_systolic" ;;
        *_systolic*) echo "eu_systolic" ;;
        *)           echo "unknown" ;;
    esac
}

ops_per_elem() {
    case "$1" in
        axpy_*|dot_*) echo 2 ;;
        gemm_*)       echo 2 ;;
        *)            echo 1 ;;
    esac
}

COUNT=0
PASSED=0
FAILED=0

for BINARY in $TARGETS; do
    COUNT=$((COUNT + 1))
    BENCH_NAME="${BINARY%.riscv}"
    BASE_NAME=$(echo "$BENCH_NAME" | sed 's/_[0-9]*$//')
    SIZE=$(echo "$BENCH_NAME" | grep -o '[0-9]*$')
    MODE=$(bench_mode "$BASE_NAME")

    printf "  [%d/%d] %-30s " "$COUNT" "$TOTAL" "$BENCH_NAME"

    # Create workload JSON for this benchmark
    BINARY_ABS="$BENCH_DIR/$BINARY"
    cat > "$WORKLOAD_DIR/$WORKLOAD_JSON_NAME" <<EOF
{
    "benchmark_name": "mxu_bench_${BENCH_NAME}",
    "common_simulation_outputs": ["uartlog"],
    "common_bootbinary": "$BINARY_ABS",
    "common_rootfs": null
}
EOF

    # Update config_runtime.yaml to use our workload
    sed -i "s/workload_name: .*/workload_name: $WORKLOAD_JSON_NAME/" \
        "$DEPLOY_DIR/config_runtime.yaml"

    # Run firesim infrasetup + runworkload
    cd "$DEPLOY_DIR"
    firesim infrasetup > /tmp/firesim_infrasetup_$$.log 2>&1 || {
        printf "INFRA_ERR\n"
        echo "    infrasetup failed — check /tmp/firesim_infrasetup_$$.log"
        FAILED=$((FAILED + 1))
        echo "$BASE_NAME,$MODE,$SIZE,N/A,N/A,0,0,INFRA_ERR" >> "$RESULTS_CSV"
        cd "$BENCH_DIR"
        continue
    }

    firesim runworkload > /tmp/firesim_runworkload_$$.log 2>&1 || true
    cd "$BENCH_DIR"

    # Find the most recent uartlog
    LATEST_RUN=$(ls -td "$SIM_RUN_DIR"/mxu_bench_${BENCH_NAME}*/ 2>/dev/null | head -1)
    UARTLOG=""
    if [ -n "$LATEST_RUN" ]; then
        UARTLOG=$(find "$LATEST_RUN" -name "uartlog" -type f 2>/dev/null | head -1)
    fi

    # Parse results from uartlog
    CYCLES=""
    INSTRET=""
    PASS="FAIL"

    if [ -n "$UARTLOG" ] && [ -f "$UARTLOG" ]; then
        CYCLES=$(grep "^CYCLES:" "$UARTLOG" 2>/dev/null | tail -1 | awk '{print $2}')
        INSTRET=$(grep "^INSTRET:" "$UARTLOG" 2>/dev/null | tail -1 | awk '{print $2}')

        if grep -q '\*\*\* PASSED \*\*\*' "$UARTLOG" 2>/dev/null; then
            PASS="PASS"
            PASSED=$((PASSED + 1))
        else
            FAILED=$((FAILED + 1))
        fi
    else
        echo "    ⚠ No uartlog found"
        FAILED=$((FAILED + 1))
    fi

    # Compute derived metrics
    IPC="0"
    OPS_PER_CYCLE="0"
    if [ -n "$CYCLES" ] && [ "$CYCLES" -gt 0 ] 2>/dev/null; then
        if [ -n "$INSTRET" ] && [ "$INSTRET" -gt 0 ] 2>/dev/null; then
            IPC=$(awk "BEGIN {printf \"%.3f\", $INSTRET / $CYCLES}")
        fi
        OPE=$(ops_per_elem "$BASE_NAME")
        if [ -n "$SIZE" ] && [ "$SIZE" -gt 0 ] 2>/dev/null; then
            OPS_PER_CYCLE=$(awk "BEGIN {printf \"%.3f\", ($SIZE * $OPE) / $CYCLES}")
        fi
    fi

    [ -z "$CYCLES" ]  && CYCLES="N/A"
    [ -z "$INSTRET" ] && INSTRET="N/A"

    printf "%-6s  %s cycles  IPC=%s\n" "$PASS" "$CYCLES" "$IPC"

    echo "$BASE_NAME,$MODE,$SIZE,$CYCLES,$INSTRET,$IPC,$OPS_PER_CYCLE,$PASS" >> "$RESULTS_CSV"
done

echo ""

# ── Restore original workload config ──────────────────────────────────
sed -i "s/workload_name: .*/workload_name: systolic.json/" \
    "$DEPLOY_DIR/config_runtime.yaml"

# ── Summary ────────────────────────────────────────────────────────────
echo "═══════════════════════════════════════════════════════"
echo "  Summary:  $PASSED passed, $FAILED failed (of $COUNT)"
echo "  Results:  $RESULTS_CSV"
echo "═══════════════════════════════════════════════════════"
echo ""

# Print results table
echo "┌──────────────────────────────┬──────────────┬───────┬─────────┬─────────┬───────┐"
printf "│ %-28s │ %-12s │ %5s │ %7s │ %7s │ %-5s │\n" "Benchmark" "Mode" "N" "Cycles" "IPC" "Pass"
echo "├──────────────────────────────┼──────────────┼───────┼─────────┼─────────┼───────┤"
tail -n +2 "$RESULTS_CSV" | while IFS=, read -r bench mode n cycles instret ipc opc pass; do
    printf "│ %-28s │ %-12s │ %5s │ %7s │ %7s │ %-5s │\n" "$bench" "$mode" "$n" "$cycles" "$ipc" "$pass"
done
echo "└──────────────────────────────┴──────────────┴───────┴─────────┴─────────┴───────┘"
echo ""
