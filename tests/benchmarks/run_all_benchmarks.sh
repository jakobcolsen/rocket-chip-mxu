#!/bin/bash
#
# run_all_benchmarks.sh — Automated thesis evaluation suite
#
# Runs all GEMM benchmarks on MXU and vanilla Rocket simulators,
# parses output into results_v2.csv for plotting.
#
# Usage: ./run_all_benchmarks.sh
#
BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
CHIPYARD="${CHIPYARD:-$HOME/chipyard}"

# Source env BEFORE strict mode (env.sh has unbound vars)
source "${CHIPYARD}/env.sh" 2>/dev/null || true
set -eo pipefail
export SIM_MXU="${CHIPYARD}/sims/verilator/simulator-chipyard.harness-VerilatorQuadRocketMXUConfig"
export SIM_VANILLA="${CHIPYARD}/sims/verilator/simulator-chipyard.harness-VerilatorQuadRocketBaselineConfig"
export RESULTS="${BENCH_DIR}/results.csv"
export TIMEOUT=28800  # 8 hours per benchmark
export BENCH_DIR

BENCHMARKS="gemm_single gemm_mimd gemm_simd gemm_csr"
SIZES="16 32 64 128"

# CSV header
echo "benchmark,N,simulator,cycles,instret,dcache_miss,systolic_stall,load_use,dcache_blocked,csr_interlock,fp_muladd,status" > "$RESULTS"

echo "=== Thesis Evaluation Suite ==="
echo "Results: $RESULTS"
echo ""

# Build all benchmarks
echo "--- Building benchmarks ---"
cd "$BENCH_DIR"
make -j$(nproc) 2>&1 | tail -5
echo ""

# Generate jobs
JOBS_FILE=$(mktemp)

for bench in $BENCHMARKS; do
    for sz in $SIZES; do
        echo "$SIM_MXU mxu $bench $sz" >> "$JOBS_FILE"
    done
done

for bench in gemm_single gemm_mimd; do
    for sz in $SIZES; do
        echo "$SIM_VANILLA vanilla $bench $sz" >> "$JOBS_FILE"
    done
done

echo "--- Running 30 benchmarks in parallel across 6 cores... ---"

# Run in parallel
cat "$JOBS_FILE" | xargs -n 4 -P 6 bash -c '
    source "${CHIPYARD}/env.sh" 2>/dev/null || true
    sim="$1"
    sim_name="$2"
    bench="$3"
    sz="$4"
    binary="${BENCH_DIR}/${bench}_${sz}.riscv"

    if [ ! -f "$binary" ]; then
        echo "  SKIP: $binary not found"
        exit 0
    fi
    if [ ! -f "$sim" ]; then
        echo "  SKIP: simulator $sim not found"
        exit 0
    fi

    output=$(timeout "$TIMEOUT" "$sim" +permissive +loadmem="$binary" +permissive-off "$binary" 2>&1) || true
    
    cycles=$(echo "$output" | grep "^CYCLES:" | awk "{print \$2}")
    instret=$(echo "$output" | grep "^INSTRET:" | awk "{print \$2}")
    dcache_miss=$(echo "$output" | grep "^DCACHE_MISS:" | awk "{print \$2}")
    systolic_stall=$(echo "$output" | grep "^SYSTOLIC_STALL:" | awk "{print \$2}")
    load_use=$(echo "$output" | grep "^LOAD_USE:" | awk "{print \$2}")
    dcache_blocked=$(echo "$output" | grep "^DCACHE_BLOCKED:" | awk "{print \$2}")
    csr_interlock=$(echo "$output" | grep "^CSR_INTERLOCK:" | awk "{print \$2}")
    fp_muladd=$(echo "$output" | grep "^FP_MULADD:" | awk "{print \$2}")
    status=$(echo "$output" | grep -o "PASSED\|FAILED" | head -1)

    parsed="${cycles:-0},${instret:-0},${dcache_miss:-0},${systolic_stall:-0},${load_use:-0},${dcache_blocked:-0},${csr_interlock:-0},${fp_muladd:-0},${status:-ERROR}"
    
    # Atomic append to CSV
    echo "${bench},${sz},${sim_name},${parsed}" >> "$RESULTS"
    # Atomic print to stdout
    echo "[${sim_name}] ${bench}_${sz}: ${cycles:-0} cycles, ${status:-ERROR}"
' _

rm -f "$JOBS_FILE"

echo ""
echo "=== Complete! Results in $RESULTS ==="
cat "$RESULTS"
