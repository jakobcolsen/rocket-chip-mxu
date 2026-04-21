#!/bin/bash
#
# run_all_benchmarks.sh — Automated thesis evaluation suite
#
# Runs all GEMM benchmarks on MXU and vanilla Rocket simulators,
# parses output into results.csv for plotting.
#
# Usage: ./run_all_benchmarks.sh
#
BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
CHIPYARD="${CHIPYARD:-$HOME/chipyard}"

# Source env BEFORE strict mode (env.sh has unbound vars)
source "${CHIPYARD}/env.sh" 2>/dev/null || true
set -eo pipefail
SIM_MXU="${CHIPYARD}/sims/verilator/simulator-chipyard.harness-VerilatorQuadRocketMXUConfig"
SIM_VANILLA="${CHIPYARD}/sims/verilator/simulator-chipyard.harness-VerilatorQuadRocketBaselineConfig"
RESULTS="${BENCH_DIR}/results.csv"
TIMEOUT=7200  # 2 hours per benchmark

BENCHMARKS="gemm_single gemm_mimd gemm_simd gemm_csr"
SIZES="4 8 16 32"

# CSV header
echo "benchmark,N,simulator,cycles,instret,dcache_miss,systolic_stall,load_use,dcache_blocked,csr_interlock,fp_muladd,status" > "$RESULTS"

parse_output() {
    local output="$1"
    local cycles=$(echo "$output" | grep "^CYCLES:" | awk '{print $2}')
    local instret=$(echo "$output" | grep "^INSTRET:" | awk '{print $2}')
    local dcache_miss=$(echo "$output" | grep "^DCACHE_MISS:" | awk '{print $2}')
    local systolic_stall=$(echo "$output" | grep "^SYSTOLIC_STALL:" | awk '{print $2}')
    local load_use=$(echo "$output" | grep "^LOAD_USE:" | awk '{print $2}')
    local dcache_blocked=$(echo "$output" | grep "^DCACHE_BLOCKED:" | awk '{print $2}')
    local csr_interlock=$(echo "$output" | grep "^CSR_INTERLOCK:" | awk '{print $2}')
    local fp_muladd=$(echo "$output" | grep "^FP_MULADD:" | awk '{print $2}')
    local status=$(echo "$output" | grep -o 'PASSED\|FAILED' | head -1)

    echo "${cycles:-0},${instret:-0},${dcache_miss:-0},${systolic_stall:-0},${load_use:-0},${dcache_blocked:-0},${csr_interlock:-0},${fp_muladd:-0},${status:-ERROR}"
}

run_bench() {
    local sim="$1" sim_name="$2" bench="$3" sz="$4"
    local binary="${BENCH_DIR}/${bench}_${sz}.riscv"

    if [ ! -f "$binary" ]; then
        echo "  SKIP: $binary not found"
        return
    fi
    if [ ! -f "$sim" ]; then
        echo "  SKIP: simulator $sim not found"
        return
    fi

    echo -n "  Running ${bench}_${sz} on ${sim_name}... "
    local output
    output=$(timeout "$TIMEOUT" "$sim" +permissive +loadmem="$binary" +permissive-off "$binary" 2>&1) || true
    local parsed=$(parse_output "$output")
    echo "${bench},${sz},${sim_name},${parsed}" >> "$RESULTS"
    echo "done ($(echo "$parsed" | cut -d, -f1) cycles, $(echo "$parsed" | cut -d, -f9))"
}

echo "=== Thesis Evaluation Suite ==="
echo "Results: $RESULTS"
echo ""

# Build all benchmarks
echo "--- Building benchmarks ---"
cd "$BENCH_DIR"
make -j$(nproc) 2>&1 | tail -5
echo ""

# Run on MXU simulator
echo "--- MXU Simulator ---"
for bench in $BENCHMARKS; do
    for sz in $SIZES; do
        run_bench "$SIM_MXU" "mxu" "$bench" "$sz"
    done
done
echo ""

# Run MIMD + single on vanilla Rocket (regression baseline)
echo "--- Vanilla Rocket Simulator (regression baseline) ---"
for bench in gemm_single gemm_mimd; do
    for sz in $SIZES; do
        run_bench "$SIM_VANILLA" "vanilla" "$bench" "$sz"
    done
done
echo ""

echo "=== Complete! Results in $RESULTS ==="
cat "$RESULTS"
