#!/usr/bin/env bash
#==============================================================================
# tinykv vs Valkey / Redis Reproducible Benchmark Runner
#
# Runs identical workloads across tinykv and valkey-server (or redis-server):
#   - Concurrency: 50 clients (-c 50)
#   - Pipelining: P=1 and P=16
#   - Value Sizes: 16 bytes and 256 bytes (-d 16, -d 256)
#   - Workloads: SET and GET
#   - Repetitions: 5 runs each -> records the MEDIAN
#==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TINYKV_BIN="${ROOT_DIR}/build/tinykv"
TINYKV_PORT=16379
VALKEY_PORT=16380

NUM_REQUESTS="${NUM_REQUESTS:-100000}"
RUNS="${RUNS:-5}"
CLIENTS=50

# Detect available benchmark CLI
if command -v valkey-benchmark >/dev/null 2>&1; then
    BENCHMARK_TOOL="valkey-benchmark"
elif command -v redis-benchmark >/dev/null 2>&1; then
    BENCHMARK_TOOL="redis-benchmark"
else
    echo "[ERROR] Neither valkey-benchmark nor redis-benchmark found on PATH." >&2
    exit 1
fi

# Detect comparison server
if command -v valkey-server >/dev/null 2>&1; then
    COMP_SERVER_BIN="valkey-server"
    COMP_SERVER_NAME="Valkey"
elif command -v redis-server >/dev/null 2>&1; then
    COMP_SERVER_BIN="redis-server"
    COMP_SERVER_NAME="Redis"
else
    echo "[ERROR] Neither valkey-server nor redis-server found on PATH." >&2
    exit 1
fi

if [[ ! -x "${TINYKV_BIN}" ]]; then
    echo "[ERROR] tinykv binary not found at ${TINYKV_BIN}. Build Release first." >&2
    exit 1
fi

# System metadata
KERNEL_VERSION="$(uname -r)"
CPU_MODEL="$(lscpu | grep 'Model name' | sed -E 's/Model name:\s+//')"
COMPILER_VERSION="$(g++ --version | head -n 1)"
COMP_SERVER_VERSION="$(${COMP_SERVER_BIN} --version | head -n 1)"

echo "================================================================="
echo " tinykv Benchmark Suite"
echo "================================================================="
echo " System:   ${CPU_MODEL}"
echo " Kernel:   ${KERNEL_VERSION}"
echo " Compiler: ${COMPILER_VERSION}"
echo " Tool:     $(${BENCHMARK_TOOL} --version 2>&1 | head -n 1)"
echo " Baseline: ${COMP_SERVER_VERSION}"
echo " Settings: -n ${NUM_REQUESTS} -c ${CLIENTS} (${RUNS} runs per test, median)"
echo "================================================================="

# Helper to compute median from a list of numbers
compute_median() {
    local -a vals=($(for v in "$@"; do echo "$v"; done | sort -n))
    local count=${#vals[@]}
    local mid=$(( count / 2 ))
    if (( count % 2 == 1 )); then
        echo "${vals[mid]}"
    else
        # average of two middle values via python/awk
        awk -v a="${vals[mid-1]}" -v b="${vals[mid]}" 'BEGIN { printf "%.2f", (a + b) / 2 }'
    fi
}

# Run benchmark for a specific target port and parse RPS and p99
# Usage: run_bench_case <port> <cmd: SET|GET> <pipe> <size>
run_bench_case() {
    local port="$1"
    local cmd="$2"
    local pipe="$3"
    local size="$4"

    local -a rps_list=()
    local -a p99_list=()

    for ((r=1; r<=RUNS; r++)); do
        # Run benchmark with CSV output
        local out
        out="$(${BENCHMARK_TOOL} -p "${port}" -h 127.0.0.1 -t "${cmd}" -n "${NUM_REQUESTS}" -c "${CLIENTS}" -P "${pipe}" -d "${size}" --csv 2>&1)"
        
        # Extract row corresponding to the command: "SET","rps",...
        local line
        line="$(echo "${out}" | grep -E "^\"${cmd}\"" || true)"
        if [[ -z "${line}" ]]; then
            echo "[ERROR] Failed to parse benchmark output:" >&2
            echo "${out}" >&2
            exit 1
        fi

        # CSV fields: "test","rps","avg_latency_ms","min_latency_ms","p50_latency_ms","p95_latency_ms","p99_latency_ms","max_latency_ms"
        local rps
        local p99
        rps="$(echo "${line}" | awk -F',' '{gsub(/"/, "", $2); print $2}')"
        p99="$(echo "${line}" | awk -F',' '{gsub(/"/, "", $7); print $7}')"

        rps_list+=("${rps}")
        p99_list+=("${p99}")
    done

    local med_rps
    local med_p99
    med_rps="$(compute_median "${rps_list[@]}")"
    med_p99="$(compute_median "${p99_list[@]}")"

    echo "${med_rps} ${med_p99}"
}

# Temporary log files
LOG_TINYKV="/tmp/tinykv_bench.log"
LOG_VALKEY="/tmp/valkey_bench.log"

# Matrix configurations: (Pipeline, Size, Command)
CONFIGS=(
    "1 16 set"
    "1 16 get"
    "1 256 set"
    "1 256 get"
    "16 16 set"
    "16 16 get"
    "16 256 set"
    "16 256 get"
)

# Start tinykv
echo "[INFO] Starting tinykv on port ${TINYKV_PORT}..."
"${TINYKV_BIN}" --port "${TINYKV_PORT}" --bind 127.0.0.1 > "${LOG_TINYKV}" 2>&1 &
PID_TINYKV=$!
sleep 0.5

# Start Valkey (pure in-memory, no persistence, no protected mode)
echo "[INFO] Starting ${COMP_SERVER_NAME} on port ${VALKEY_PORT}..."
${COMP_SERVER_BIN} --port "${VALKEY_PORT}" --bind 127.0.0.1 --save "" --appendonly no --protected-mode no > "${LOG_VALKEY}" 2>&1 &
PID_VALKEY=$!
sleep 0.5

# Ensure cleanup on exit
cleanup() {
    echo "[INFO] Shutting down benchmark servers..."
    kill -TERM "${PID_TINYKV}" 2>/dev/null || true
    kill -TERM "${PID_VALKEY}" 2>/dev/null || true
    wait "${PID_TINYKV}" 2>/dev/null || true
    wait "${PID_VALKEY}" 2>/dev/null || true
}
trap cleanup EXIT

RESULTS_FILE="${SCRIPT_DIR}/results.md"
TMP_RESULTS="${SCRIPT_DIR}/results.tmp.md"

cat <<EOF > "${TMP_RESULTS}"
# tinykv vs ${COMP_SERVER_NAME} Benchmark Results

## Environment & System Info
- **CPU Model**: \`${CPU_MODEL}\`
- **Linux Kernel**: \`${KERNEL_VERSION}\`
- **Compiler**: \`${COMPILER_VERSION}\`
- **tinykv Version**: \`v0.1.0 (Release)\`
- **Comparison Server**: \`${COMP_SERVER_VERSION}\`
- **Benchmark Tool**: \`$(${BENCHMARK_TOOL} --version 2>&1 | head -n 1)\`
- **Parameters**: \`-n ${NUM_REQUESTS}\`, \`-c ${CLIENTS}\`, 5 runs per configuration (reporting median)

## Performance Comparison Table

| Workload | Pipeline | Value Size | tinykv (req/sec) | ${COMP_SERVER_NAME} (req/sec) | tinykv p99 (ms) | ${COMP_SERVER_NAME} p99 (ms) | Speedup (%) |
|:---------|:--------:|:----------:|:----------------:|:-----------------------------:|:---------------:|:----------------------------:|:-----------:|
EOF

echo ""
echo "Running Benchmark Matrix (5 iterations each, computing median)..."
echo "-----------------------------------------------------------------"

for cfg in "${CONFIGS[@]}"; do
    read -r pipe size cmd <<< "${cfg}"
    CMD_UPPER="$(echo "${cmd}" | tr '[:lower:]' '[:upper:]')"
    printf "Testing %-4s | Pipeline: %-2s | Size: %3s B ... " "${CMD_UPPER}" "${pipe}" "${size}"

    # tinykv
    read -r tiny_rps tiny_p99 <<< "$(run_bench_case "${TINYKV_PORT}" "${CMD_UPPER}" "${pipe}" "${size}")"

    # valkey
    read -r valkey_rps valkey_p99 <<< "$(run_bench_case "${VALKEY_PORT}" "${CMD_UPPER}" "${pipe}" "${size}")"

    # Calculate speedup percentage: (tiny_rps / valkey_rps - 1) * 100
    local_ratio="$(awk -v t="${tiny_rps}" -v v="${valkey_rps}" 'BEGIN { printf "%+.1f%%", ((t / v) - 1.0) * 100.0 }')"

    printf "tinykv: %10.1f rps | %s: %10.1f rps (%s)\n" "${tiny_rps}" "${COMP_SERVER_NAME}" "${valkey_rps}" "${local_ratio}"

    echo "| ${CMD_UPPER} | P=${pipe} | ${size} B | ${tiny_rps} | ${valkey_rps} | ${tiny_p99} ms | ${valkey_p99} ms | ${local_ratio} |" >> "${TMP_RESULTS}"
done

mv "${TMP_RESULTS}" "${RESULTS_FILE}"
echo "-----------------------------------------------------------------"
echo "[SUCCESS] Benchmark complete! Real results written to bench/results.md"