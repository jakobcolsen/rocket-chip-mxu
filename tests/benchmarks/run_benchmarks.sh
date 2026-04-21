#!/bin/bash
# run_benchmarks.sh — MXU Benchmark Suite Orchestrator
#
# Compiles all benchmarks, runs each through Verilator,
# parses cycle/instret counts, computes derived metrics,
# and writes results to results.csv.
#
# Usage:
#   ./run_benchmarks.sh                  — run full suite
#   ./run_benchmarks.sh axpy_simd_64     — run single benchmark
#
# Prerequisites:
#   - chipyard env sourced
#   - Verilator simulator already built for VerilatorQuadRocketMXUConfig

set -e

# ── Configuration ──────────────────────────────────────────────────────
CHIPYARD_DIR="${CHIPYARD:-$HOME/chipyard}"
SIM_DIR="$CHIPYARD_DIR/sims/verilator"
CONFIG="VerilatorQuadRocketMXUConfig"
BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_CSV="$BENCH_DIR/results.csv"
TIMEOUT=3600 # seconds per simulation

# Find the simulator binary
SIM_BIN=$(find "$SIM_DIR" -name "simulator-chipyard.harness-$CONFIG" -type f 2>/dev/null | head -1)
if [ -z "$SIM_BIN" ]; then
    echo "ERROR: Cannot find Verilator simulator binary for $CONFIG"
    echo "Build it first: cd $SIM_DIR && make CONFIG=$CONFIG"
    exit 1
fi

echo "=== MXU Benchmark Suite ==="
echo "Simulator: $SIM_BIN"
echo "Results:   $RESULTS_CSV"
echo ""

# ── Build ──────────────────────────────────────────────────────────────
echo "[1/3] Building benchmarks..."
cd "$BENCH_DIR"
make -j$(nproc) 2>&1 | tail -5
echo ""

# ── Initialize CSV ─────────────────────────────────────────────────────
echo "benchmark,mode,N,cycles,instret,ipc,ops_per_cycle,pass" > "$RESULTS_CSV"

# ── Helper: extract metric from simulation output ──────────────────────
extract_metric() {
    local output="$1"
    local tag="$2"
    echo "$output" | grep "^${tag}:" | tail -1 | awk '{print $2}'
}

# ── Helper: determine ops per element for a benchmark ──────────────────
ops_per_elem() {
    local bench="$1"
    case "$bench" in
        axpy_*)  echo 2 ;;    # 1 mul + 1 add
        dot_*)   echo 2 ;;    # 1 mul + 1 add
        gemm_*)  echo 2 ;;    # per inner-product step: 1 mul + 1 add (× K)
        *)       echo 1 ;;
    esac
}

# ── Helper: determine execution mode from benchmark name ───────────────
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

# ── Determine which benchmarks to run ──────────────────────────────────
if [ -n "$1" ]; then
    TARGETS="$1.riscv"
else
    TARGETS=$(ls *.riscv 2>/dev/null | sort)
fi

if [ -z "$TARGETS" ]; then
    echo "ERROR: No .riscv binaries found. Run 'make' first."
    exit 1
fi

TOTAL=$(echo "$TARGETS" | wc -w)
echo "[2/3] Running $TOTAL benchmarks..."
echo ""

# ── Run each benchmark ─────────────────────────────────────────────────
COUNT=0
PASSED=0
FAILED=0

for BINARY in $TARGETS; do
    COUNT=$((COUNT + 1))
    BENCH_NAME="${BINARY%.riscv}"

    # Extract base name and size from pattern: <name>_<size>
    BASE_NAME=$(echo "$BENCH_NAME" | sed 's/_[0-9]*$//')
    SIZE=$(echo "$BENCH_NAME" | grep -o '[0-9]*$')
    MODE=$(bench_mode "$BASE_NAME")

    printf "  [%d/%d] %-30s " "$COUNT" "$TOTAL" "$BENCH_NAME"

    # Run simulation
    # Plusargs must be between +permissive/+permissive-off (Chipyard convention)
    # Binary as positional arg AFTER +permissive-off for HTIF tohost termination
    SIM_OUTPUT=$(timeout "$TIMEOUT" "$SIM_BIN" \
        +permissive +loadmem="$BENCH_DIR/$BINARY" +permissive-off \
        "$BENCH_DIR/$BINARY" </dev/null 2>&1) || true

    # Parse results
    CYCLES=$(extract_metric "$SIM_OUTPUT" "CYCLES")
    INSTRET=$(extract_metric "$SIM_OUTPUT" "INSTRET")

    if echo "$SIM_OUTPUT" | grep -q '\*\*\* PASSED \*\*\*'; then
        PASS="PASS"
        PASSED=$((PASSED + 1))
    else
        PASS="FAIL"
        FAILED=$((FAILED + 1))
    fi

    # Compute derived metrics
    IPC="0"
    OPS_PER_CYCLE="0"
    if [ -n "$CYCLES" ] && [ "$CYCLES" -gt 0 ] 2>/dev/null; then
        if [ -n "$INSTRET" ] && [ "$INSTRET" -gt 0 ] 2>/dev/null; then
            # IPC = instret / cycles (fixed point: multiply by 1000 for 3 decimal places)
            IPC=$(awk "BEGIN {printf \"%.3f\", $INSTRET / $CYCLES}")
        fi

        # Ops per cycle
        OPE=$(ops_per_elem "$BASE_NAME")
        if [ -n "$SIZE" ] && [ "$SIZE" -gt 0 ] 2>/dev/null; then
            OPS_PER_CYCLE=$(awk "BEGIN {printf \"%.3f\", ($SIZE * $OPE) / $CYCLES}")
        fi
    fi

    # Handle missing metrics
    [ -z "$CYCLES" ]  && CYCLES="N/A"
    [ -z "$INSTRET" ] && INSTRET="N/A"

    printf "%-6s  %s cycles  IPC=%s\n" "$PASS" "$CYCLES" "$IPC"

    # Write to CSV
    echo "$BASE_NAME,$MODE,$SIZE,$CYCLES,$INSTRET,$IPC,$OPS_PER_CYCLE,$PASS" >> "$RESULTS_CSV"

    # Secondary run on standard config if it's a MIMD benchmark
    if [ "$MODE" = "mimd" ]; then
        STANDARD_SIM_BIN=$(find "$SIM_DIR" -name "simulator-chipyard.harness-QuadRocketConfig" -type f 2>/dev/null | head -1)
        if [ -n "$STANDARD_SIM_BIN" ]; then
            printf "  [CONTROL] %-30s " "${BENCH_NAME} (StandardQuad)"
            SIM_OUTPUT=$(timeout "$TIMEOUT" "$STANDARD_SIM_BIN" +permissive +loadmem="$BENCH_DIR/$BINARY" +permissive-off "$BENCH_DIR/$BINARY" </dev/null 2>&1) || true
            CYCLES=$(extract_metric "$SIM_OUTPUT" "CYCLES")
            INSTRET=$(extract_metric "$SIM_OUTPUT" "INSTRET")
            if echo "$SIM_OUTPUT" | grep -q '\*\*\* PASSED \*\*\*'; then PASS="PASS"; PASSED=$((PASSED + 1)); else PASS="FAIL"; FAILED=$((FAILED + 1)); fi
            IPC="0"; OPS_PER_CYCLE="0"
            if [ -n "$CYCLES" ] && [ "$CYCLES" -gt 0 ] 2>/dev/null; then
                if [ -n "$INSTRET" ] && [ "$INSTRET" -gt 0 ] 2>/dev/null; then IPC=$(awk "BEGIN {printf \"%.3f\", $INSTRET / $CYCLES}"); fi
                OPE=$(ops_per_elem "$BASE_NAME")
                if [ -n "$SIZE" ] && [ "$SIZE" -gt 0 ] 2>/dev/null; then OPS_PER_CYCLE=$(awk "BEGIN {printf \"%.3f\", ($SIZE * $OPE) / $CYCLES}"); fi
            fi
            [ -z "$CYCLES" ] && CYCLES="N/A"
            [ -z "$INSTRET" ] && INSTRET="N/A"
            printf "%-6s  %s cycles  IPC=%s\n" "$PASS" "$CYCLES" "$IPC"
            echo "$BASE_NAME,${MODE}_standard,$SIZE,$CYCLES,$INSTRET,$IPC,$OPS_PER_CYCLE,$PASS" >> "$RESULTS_CSV"
        fi
    fi
done

echo ""

# ── Summary ────────────────────────────────────────────────────────────
echo "[3/3] Summary"
echo "  Total:  $COUNT"
echo "  Passed: $PASSED"
echo "  Failed: $FAILED"
echo ""
echo "Results written to: $RESULTS_CSV"
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

# ── Speedup comparison (SIMD vs MIMD for same benchmark+size) ──────────
echo "Speedup Analysis (SIMD vs MIMD):"
echo "─────────────────────────────────"
tail -n +2 "$RESULTS_CSV" | grep ",simd," | while IFS=, read -r sb sm sn sc si sipc sopc sp; do
    MIMD_NAME=$(echo "$sb" | sed 's/_simd/_mimd/')
    MIMD_LINE=$(grep "^$MIMD_NAME,mimd,$sn," "$RESULTS_CSV" 2>/dev/null | head -1)
    if [ -n "$MIMD_LINE" ]; then
        MC=$(echo "$MIMD_LINE" | cut -d, -f4)
        if [ "$sc" != "N/A" ] && [ "$MC" != "N/A" ] && [ "$sc" -gt 0 ] 2>/dev/null; then
            SPEEDUP=$(awk "BEGIN {printf \"%.2f\", $MC / $sc}")
            EFF=$(awk "BEGIN {printf \"%.1f\", ($MC / $sc) / 4 * 100}")
            printf "  %-20s N=%-5s  MIMD=%s  SIMD=%s  Speedup=%sx  ParEff=%s%%\n" \
                   "$(echo $sb | sed 's/_simd//')" "$sn" "$MC" "$sc" "$SPEEDUP" "$EFF"
        fi
    fi
done
echo ""
