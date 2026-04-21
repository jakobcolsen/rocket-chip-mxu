#!/bin/bash
# run_benchmarks_firesim.sh — Run MXU Benchmarks via FireSim (Alveo U200 FPGA)
#
# Compiles benchmarks, runs each through FireSim FPGA simulation,
# parses cycle/instret counts from UART log, and writes results to results_firesim.csv.
#
# Usage:
#   ./run_benchmarks_firesim.sh                        — run full suite
#   ./run_benchmarks_firesim.sh gemm_csr_256           — run single benchmark
#   ./run_benchmarks_firesim.sh gemm_csr_256 gemm_mimd_256  — run specific benchmarks
#
# Prerequisites:
#   - chipyard env sourced
#   - FireSim bitstream built for FireSimQuadRocketMXUConfig
#   - Custom XDMA driver built: ~/dma_ip_drivers/XDMA/linux-kernel/xdma/xdma.ko

set -e

# ── Configuration ──────────────────────────────────────────────────────
CHIPYARD_DIR="${CHIPYARD:-$HOME/chipyard}"
FIRESIM_DIR="$CHIPYARD_DIR/sims/firesim"
DEPLOY_DIR="$FIRESIM_DIR/deploy"
WORKLOADS_DIR="$DEPLOY_DIR/workloads"
RUNTIME_YAML="$DEPLOY_DIR/config_runtime.yaml"
SIM_SLOT_DIR="${HOME}/FIRESIM_RUNS_DIR/sim_slot_0"

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_CSV="$BENCH_DIR/results_firesim.csv"
POLL_INTERVAL=5     # seconds between completion checks
MAX_WAIT=3600       # max seconds to wait per benchmark

echo "=== MXU FireSim Benchmark Suite ==="
echo "FireSim:   $FIRESIM_DIR"
echo "Results:   $RESULTS_CSV"
echo ""

# ── Build benchmarks ──────────────────────────────────────────────────
echo "[1/4] Building benchmarks..."
cd "$BENCH_DIR"
make -j$(nproc) 2>&1 | tail -5
echo ""

# ── Initialize CSV ─────────────────────────────────────────────────────
echo "benchmark,mode,N,cycles,instret,ipc,ops_per_cycle,pass" > "$RESULTS_CSV"

# ── Helpers ────────────────────────────────────────────────────────────
extract_metric() {
    local output="$1"
    local tag="$2"
    echo "$output" | grep "^${tag}:" | tail -1 | awk '{print $2}'
}

ops_per_elem() {
    local bench="$1"
    case "$bench" in
        axpy_*)  echo 2 ;;
        dot_*)   echo 2 ;;
        gemm_*)  echo 2 ;;
        *)       echo 1 ;;
    esac
}

bench_mode() {
    local bench="$1"
    case "$bench" in
        *_simd*)     echo "simd" ;;
        *_mimd*)     echo "mimd" ;;
        *_csr*)      echo "csr_systolic" ;;
        *_systolic*) echo "eu_systolic" ;;
        *)           echo "unknown" ;;
    esac
}

# Create a FireSim workload JSON for a given binary
create_workload() {
    local bench_name="$1"
    local binary_path="$2"
    local wl_dir="$WORKLOADS_DIR/$bench_name"
    
    mkdir -p "$wl_dir"
    
    # Symlink binary into workload directory
    ln -sf "$binary_path" "$wl_dir/${bench_name}.riscv"
    
    # Create workload JSON
    cat > "$WORKLOADS_DIR/${bench_name}.json" <<EOF
{
    "benchmark_name": "$bench_name",
    "common_simulation_outputs": [
        "uartlog"
    ],
    "common_bootbinary": "${bench_name}.riscv",
    "common_rootfs": null
}
EOF
}

# Update config_runtime.yaml workload_name
set_workload() {
    local wl_name="$1"
    sed -i "s/workload_name: .*/workload_name: ${wl_name}.json/" "$RUNTIME_YAML"
}

# Source FireSim environment once and export the function
FIRESIM_ENV_SCRIPT="$FIRESIM_DIR/sourceme-manager.sh"

# Run firesim infrasetup (only needs to happen once per session)
run_infrasetup() {
    echo -n "    infrasetup..."
    bash -c "
        cd $FIRESIM_DIR
        source sourceme-manager.sh --skip-ssh-setup 2>/dev/null
        firesim infrasetup
    " > /tmp/firesim_infrasetup.log 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo " FAILED (exit code $rc)"
        echo "    Check /tmp/firesim_infrasetup.log"
        tail -5 /tmp/firesim_infrasetup.log | sed 's/^/    /'
        return 1
    fi
    echo " done"
    return 0
}

# Run FireSim runworkload synchronously (blocks until simulation completes)
run_firesim_benchmark() {
    local bench_name="$1"
    local need_infrasetup="$2"  # "yes" or "no"

    if [ "$need_infrasetup" = "yes" ]; then
        if ! run_infrasetup; then
            return 1
        fi
    fi

    echo -n "    simulating..."
    # Run synchronously — firesim runworkload blocks until the sim finishes
    bash -c "
        cd $FIRESIM_DIR
        source sourceme-manager.sh --skip-ssh-setup 2>/dev/null
        firesim runworkload
    " > /tmp/firesim_run_${bench_name}.log 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo " FAILED (exit code $rc)"
        echo "    Check /tmp/firesim_run_${bench_name}.log"
        tail -5 /tmp/firesim_run_${bench_name}.log | sed 's/^/    /'
        return 1
    fi
    echo " done"
    return 0
}

# Find the latest results directory for a benchmark
find_results_dir() {
    local bench_name="$1"
    ls -1dt "$DEPLOY_DIR/results-workload/"*"-${bench_name}/" 2>/dev/null | head -1
}

# ── Determine which benchmarks to run ──────────────────────────────────
if [ $# -gt 0 ]; then
    BENCH_LIST="$@"
else
    # Default: all compiled .riscv binaries
    BENCH_LIST=""
    for f in "$BENCH_DIR"/*.riscv; do
        [ -f "$f" ] || continue
        bn=$(basename "$f" .riscv)
        BENCH_LIST="$BENCH_LIST $bn"
    done
fi

TOTAL=$(echo $BENCH_LIST | wc -w)
echo "[2/4] Running $TOTAL benchmarks on FireSim FPGA..."
echo ""

# ── Run each benchmark ─────────────────────────────────────────────────
COUNT=0
PASSED=0
FAILED_COUNT=0

for BENCH_NAME in $BENCH_LIST; do
    COUNT=$((COUNT + 1))
    BINARY="${BENCH_NAME}.riscv"
    BINARY_PATH="$BENCH_DIR/$BINARY"
    
    # Extract base name and size
    BASE_NAME=$(echo "$BENCH_NAME" | sed 's/_[0-9]*$//')
    SIZE=$(echo "$BENCH_NAME" | grep -o '[0-9]*$')
    MODE=$(bench_mode "$BASE_NAME")
    
    printf "  [%d/%d] %-30s\n" "$COUNT" "$TOTAL" "$BENCH_NAME"
    
    if [ ! -f "$BINARY_PATH" ]; then
        echo "    ERROR: Binary not found: $BINARY_PATH"
        echo "$BASE_NAME,$MODE,$SIZE,N/A,N/A,0,0,MISSING" >> "$RESULTS_CSV"
        FAILED_COUNT=$((FAILED_COUNT + 1))
        continue
    fi
    
    # Create workload
    create_workload "$BENCH_NAME" "$BINARY_PATH"
    
    # Set this workload as active
    set_workload "$BENCH_NAME"
    
    # Run FireSim (infrasetup only needed for the first benchmark)
    if [ $COUNT -eq 1 ]; then
        NEED_SETUP="yes"
    else
        NEED_SETUP="yes"  # always re-setup since workload changed
    fi
    if ! run_firesim_benchmark "$BENCH_NAME" "$NEED_SETUP"; then
        echo "$BASE_NAME,$MODE,$SIZE,N/A,N/A,0,0,FAIL" >> "$RESULTS_CSV"
        FAILED_COUNT=$((FAILED_COUNT + 1))
        continue
    fi
    
    # Find results and parse uartlog
    RESULTS_DIR=$(find_results_dir "$BENCH_NAME")
    UARTLOG=""
    if [ -n "$RESULTS_DIR" ]; then
        UARTLOG=$(find "$RESULTS_DIR" -name "uartlog" 2>/dev/null | head -1)
    fi
    # Fallback: check sim_slot_0 uartlog
    if [ -z "$UARTLOG" ] || [ ! -s "$UARTLOG" ]; then
        UARTLOG="$SIM_SLOT_DIR/uartlog"
    fi
    
    if [ -f "$UARTLOG" ]; then
        UART_OUTPUT=$(cat "$UARTLOG")
    else
        UART_OUTPUT=""
    fi
    
    # Parse results
    CYCLES=$(extract_metric "$UART_OUTPUT" "CYCLES")
    INSTRET=$(extract_metric "$UART_OUTPUT" "INSTRET")
    
    if echo "$UART_OUTPUT" | grep -q '\*\*\* PASSED \*\*\*'; then
        PASS="PASS"
        PASSED=$((PASSED + 1))
    else
        PASS="FAIL"
        FAILED_COUNT=$((FAILED_COUNT + 1))
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
    
    printf "    %-6s  %s cycles  IPC=%s\n" "$PASS" "$CYCLES" "$IPC"
    
    echo "$BASE_NAME,$MODE,$SIZE,$CYCLES,$INSTRET,$IPC,$OPS_PER_CYCLE,$PASS" >> "$RESULTS_CSV"
done

echo ""

# ── Restore original workload ──────────────────────────────────────────
set_workload "systolic"

# ── Summary ────────────────────────────────────────────────────────────
echo "[3/4] Summary"
echo "  Total:  $COUNT"
echo "  Passed: $PASSED"
echo "  Failed: $FAILED_COUNT"
echo ""
echo "Results written to: $RESULTS_CSV"
echo ""

# Print results table
echo "[4/4] Results"
echo "┌──────────────────────────────┬──────────────┬───────┬─────────────┬─────────┬───────┐"
printf "│ %-28s │ %-12s │ %5s │ %11s │ %7s │ %-5s │\n" "Benchmark" "Mode" "N" "Cycles" "IPC" "Pass"
echo "├──────────────────────────────┼──────────────┼───────┼─────────────┼─────────┼───────┤"
tail -n +2 "$RESULTS_CSV" | while IFS=, read -r bench mode n cycles instret ipc opc pass; do
    printf "│ %-28s │ %-12s │ %5s │ %11s │ %7s │ %-5s │\n" "$bench" "$mode" "$n" "$cycles" "$ipc" "$pass"
done
echo "└──────────────────────────────┴──────────────┴───────┴─────────────┴─────────┴───────┘"
echo ""

# ── Speedup comparison ─────────────────────────────────────────────────
echo "Speedup Analysis (CSR vs MIMD for same size):"
echo "─────────────────────────────────────────────"
tail -n +2 "$RESULTS_CSV" | grep ",csr_systolic," | while IFS=, read -r cb cm cn cc ci cipc copc cp; do
    MIMD_NAME=$(echo "$cb" | sed 's/_csr/_mimd/')
    MIMD_LINE=$(grep "^$MIMD_NAME,mimd,$cn," "$RESULTS_CSV" 2>/dev/null | head -1)
    if [ -n "$MIMD_LINE" ]; then
        MC=$(echo "$MIMD_LINE" | cut -d, -f4)
        if [ "$cc" != "N/A" ] && [ "$MC" != "N/A" ] && [ "$cc" -gt 0 ] 2>/dev/null; then
            RATIO=$(awk "BEGIN {printf \"%.2f\", $cc / $MC}")
            printf "  %-20s N=%-5s  MIMD=%s  CSR=%s  CSR/MIMD=%sx\n" \
                   "$(echo $cb | sed 's/_csr//')" "$cn" "$MC" "$cc" "$RATIO"
        fi
    fi
done
echo ""
