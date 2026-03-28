#!/usr/bin/env bash
#
# Comprehensive allocator benchmark runner
#

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
OPERATIONS=10000000
THREAD_COUNTS=(1 2 4 8 16)
SIZE_RANGES=("16:64" "64:256" "256:1024" "1024:4096")
OUTPUT_DIR="results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] [ALLOCATORS...]

Run comprehensive allocator benchmarks.

OPTIONS:
    -n COUNT        Number of operations (default: $OPERATIONS)
    -t THREADS      Comma-separated thread counts (default: 1,2,4,8,16)
    -s SIZES        Comma-separated size ranges (default: 16:64,64:256,...)
    -o DIR          Output directory (default: $OUTPUT_DIR)
    -q              Quick mode: fewer iterations
    -h              Show this help

ALLOCATORS:
    libc            System malloc
    umem            libumem allocator
    jemalloc        jemalloc allocator
    tcmalloc        Google TCMalloc
    mimalloc        Microsoft mimalloc
    all             Test all available allocators (default)

EXAMPLES:
    $0                              # Test all allocators
    $0 umem libc                    # Compare umem vs libc
    $0 -q -n 1000000                # Quick test with 1M operations
    $0 -t 1,4,16 umem               # Test umem with specific thread counts

EOF
    exit 1
}

# Parse arguments
QUICK_MODE=0
ALLOCATORS=()

while [[ $# -gt 0 ]]; do
    case $1 in
        -n)
            OPERATIONS="$2"
            shift 2
            ;;
        -t)
            IFS=',' read -ra THREAD_COUNTS <<< "$2"
            shift 2
            ;;
        -s)
            IFS=',' read -ra SIZE_RANGES <<< "$2"
            shift 2
            ;;
        -o)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -q)
            QUICK_MODE=1
            OPERATIONS=1000000
            THREAD_COUNTS=(1 4)
            SIZE_RANGES=("16:256" "256:4096")
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            ALLOCATORS+=("$1")
            shift
            ;;
    esac
done

# Default to all allocators if none specified
if [[ ${#ALLOCATORS[@]} -eq 0 ]]; then
    ALLOCATORS=("all")
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"
RESULT_FILE="$OUTPUT_DIR/bench_${TIMESTAMP}.csv"
LOG_FILE="$OUTPUT_DIR/bench_${TIMESTAMP}.log"

echo -e "${GREEN}Allocator Benchmark Suite${NC}"
echo "================================"
echo "Operations:    $OPERATIONS"
echo "Thread counts: ${THREAD_COUNTS[*]}"
echo "Size ranges:   ${SIZE_RANGES[*]}"
echo "Allocators:    ${ALLOCATORS[*]}"
echo "Output:        $RESULT_FILE"
echo "Log:           $LOG_FILE"
echo ""

# Check if benchmark binary exists
if [[ ! -f ./bench_allocators ]]; then
    echo -e "${YELLOW}Building benchmark...${NC}"
    if ! make -j$(nproc) >> "$LOG_FILE" 2>&1; then
        echo -e "${RED}Build failed. Check $LOG_FILE${NC}"
        exit 1
    fi
    echo -e "${GREEN}Build successful${NC}"
fi

# Write CSV header
./bench_allocators -H > "$RESULT_FILE"

# Run benchmarks
total_runs=$((${#ALLOCATORS[@]} * 4 * ${#THREAD_COUNTS[@]}))
current_run=0

for allocator in "${ALLOCATORS[@]}"; do
    echo -e "${GREEN}Testing $allocator${NC}"

    # Single-threaded workload with different sizes
    for size_range in "${SIZE_RANGES[@]}"; do
        current_run=$((current_run + 1))
        echo -ne "  [${current_run}/${total_runs}] single-thread ($size_range)... "

        if ./bench_allocators -a "$allocator" -w single -n "$OPERATIONS" \
                -s "$size_range" -c >> "$RESULT_FILE" 2>> "$LOG_FILE"; then
            echo -e "${GREEN}✓${NC}"
        else
            echo -e "${RED}✗${NC}"
        fi
    done

    # Multi-threaded workload
    for threads in "${THREAD_COUNTS[@]}"; do
        for size_range in "${SIZE_RANGES[@]}"; do
            current_run=$((current_run + 1))
            ops_per_thread=$((OPERATIONS / threads))
            echo -ne "  [${current_run}/${total_runs}] multi-thread (t=$threads, $size_range)... "

            if ./bench_allocators -a "$allocator" -w multi -t "$threads" \
                    -n "$ops_per_thread" -s "$size_range" -c >> "$RESULT_FILE" 2>> "$LOG_FILE"; then
                echo -e "${GREEN}✓${NC}"
            else
                echo -e "${RED}✗${NC}"
            fi
        done
    done
done

echo ""
echo -e "${GREEN}Benchmark complete!${NC}"
echo "Results: $RESULT_FILE"
echo "Log:     $LOG_FILE"
echo ""

# Generate summary
echo "Generating summary..."
python3 - "$RESULT_FILE" <<'PYTHON_SCRIPT' || true
import sys
import csv
from collections import defaultdict

if len(sys.argv) < 2:
    sys.exit(0)

results_file = sys.argv[1]
allocators = defaultdict(lambda: {"throughput": [], "latency_p99": []})

try:
    with open(results_file) as f:
        reader = csv.DictReader(f)
        for row in reader:
            alloc = row["allocator"]
            allocators[alloc]["throughput"].append(float(row["ops_per_sec"]))
            allocators[alloc]["latency_p99"].append(float(row["lat_p99"]))

    print("\n========== SUMMARY ==========")
    print(f"{'Allocator':<15} {'Avg Throughput':<20} {'Avg p99 Latency':<20}")
    print("-" * 60)

    for alloc in sorted(allocators.keys()):
        data = allocators[alloc]
        avg_throughput = sum(data["throughput"]) / len(data["throughput"])
        avg_p99 = sum(data["latency_p99"]) / len(data["latency_p99"])
        print(f"{alloc:<15} {avg_throughput:>15.2f} ops/s {avg_p99:>15.0f} ns")

except Exception as e:
    print(f"Could not generate summary: {e}")
    pass
PYTHON_SCRIPT

echo ""
echo "To analyze results:"
echo "  - View CSV: cat $RESULT_FILE"
echo "  - Import to Excel/LibreOffice/R/Python for detailed analysis"
