#!/usr/bin/env bash
#
# Comprehensive Coverage Report Generator for libumem
#
# This script builds libumem with coverage enabled, runs all tests,
# generates detailed HTML reports, and validates coverage thresholds.
#
# Usage:
#   ./scripts/generate-coverage-report.sh [OPTIONS]
#
# Options:
#   --min-coverage N    Minimum coverage threshold (default: 95)
#   --with-experimental Include experimental features
#   --branch-coverage   Enable branch coverage analysis
#   --clean             Remove previous coverage data
#   --help              Show this help message
#

set -euo pipefail

# Colors for output
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly RED='\033[0;31m'
readonly BLUE='\033[0;34m'
readonly NC='\033[0m' # No Color

# Default configuration
MIN_COVERAGE=${MIN_COVERAGE:-95}
BUILD_DIR="build-coverage"
COVERAGE_DIR="test/coverage"
WITH_EXPERIMENTAL=0
BRANCH_COVERAGE=0
CLEAN=0

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --min-coverage)
            MIN_COVERAGE="$2"
            shift 2
            ;;
        --with-experimental)
            WITH_EXPERIMENTAL=1
            shift
            ;;
        --branch-coverage)
            BRANCH_COVERAGE=1
            shift
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --help)
            grep '^#' "$0" | tail -n +3 | head -n -1 | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Run with --help for usage information"
            exit 1
            ;;
    esac
done

# Check for required tools
check_dependencies() {
    local missing_deps=()

    for cmd in lcov genhtml bc; do
        if ! command -v "$cmd" &> /dev/null; then
            missing_deps+=("$cmd")
        fi
    done

    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        echo -e "${RED}Error: Missing required dependencies:${NC}"
        for dep in "${missing_deps[@]}"; do
            echo "  - $dep"
        done
        echo ""
        echo "Install with:"
        echo "  Debian/Ubuntu: sudo apt-get install lcov bc"
        echo "  Fedora/RHEL:   sudo dnf install lcov bc"
        echo "  macOS:         brew install lcov bc"
        exit 1
    fi
}

# Print section header
print_header() {
    echo ""
    echo -e "${BLUE}======================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}======================================${NC}"
}

# Print step
print_step() {
    echo -e "${GREEN}▶${NC} $1"
}

# Print warning
print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

# Print error
print_error() {
    echo -e "${RED}✗${NC} $1"
}

# Print success
print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

# Clean previous build and coverage data
clean_previous() {
    print_step "Cleaning previous build and coverage data..."

    if [[ -d "$BUILD_DIR" ]]; then
        rm -rf "$BUILD_DIR"
        print_success "Removed build directory: $BUILD_DIR"
    fi

    if [[ -d "$COVERAGE_DIR" ]]; then
        rm -rf "$COVERAGE_DIR"
        print_success "Removed coverage directory: $COVERAGE_DIR"
    fi

    # Clean gcov files in source tree
    find . -name "*.gcda" -delete
    find . -name "*.gcno" -delete
    find . -name "*.gcov" -delete

    print_success "Cleaned coverage data files"
}

# Configure build with coverage
configure_with_coverage() {
    print_step "Configuring build with coverage enabled..."

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    local configure_flags="--enable-coverage"

    if [[ $WITH_EXPERIMENTAL -eq 1 ]]; then
        configure_flags+=" --enable-percpu-caching"
        configure_flags+=" --enable-numa"
        configure_flags+=" --enable-rseq"
        configure_flags+=" --enable-htm"
        print_warning "Including experimental features"
    fi

    ../configure $configure_flags || {
        print_error "Configuration failed"
        exit 1
    }

    print_success "Configuration complete"
}

# Build with coverage instrumentation
build_with_coverage() {
    print_step "Building with coverage instrumentation..."

    local nproc_count
    nproc_count=$(nproc 2>/dev/null || echo 4)

    make -j"$nproc_count" || {
        print_error "Build failed"
        exit 1
    }

    print_success "Build complete"
}

# Run all tests
run_tests() {
    print_step "Running test suite..."

    # Run make check (runs basic tests)
    if ! make check; then
        print_error "Tests failed"
        return 1
    fi

    print_success "Basic tests passed"

    # Run additional test programs if they exist
    local test_programs=(
        "./test/test_main"
        "./test/property/prop_alloc_free2"
        "./test/property/prop_cache"
        "./test/property/prop_fragmentation"
        "./test/integration/test_multithreaded"
        "./test/integration/test_signals"
        "./test/integration/test_oom"
        "./test/integration/test_realloc_bootstrap"
        "./test/integration/test_debug_features"
        "./test/integration/test_threading_stress"
    )

    for test_prog in "${test_programs[@]}"; do
        if [[ -x "$test_prog" ]]; then
            print_step "Running $(basename "$test_prog")..."
            if "$test_prog"; then
                print_success "$(basename "$test_prog") passed"
            else
                print_warning "$(basename "$test_prog") failed (continuing)"
            fi
        fi
    done

    print_success "Test suite complete"
    return 0
}

# Capture coverage data
capture_coverage() {
    print_step "Capturing coverage data..."

    lcov --capture \
         --directory . \
         --output-file coverage.info \
         --rc lcov_branch_coverage=$BRANCH_COVERAGE \
         2>&1 | grep -v "ignoring data for external file" || true

    print_success "Coverage data captured"
}

# Filter coverage data
filter_coverage() {
    print_step "Filtering coverage data..."

    # Remove system files, test files, and external code
    lcov --remove coverage.info \
         '/usr/*' \
         '*/test/*' \
         '*/examples/*' \
         '*/amd64/umem_genasm.c' \
         --output-file coverage.info.cleaned \
         --rc lcov_branch_coverage=$BRANCH_COVERAGE \
         2>&1 | grep -v "Deleted" || true

    print_success "Coverage data filtered"
}

# Generate HTML report
generate_html_report() {
    print_step "Generating HTML coverage report..."

    mkdir -p "../$COVERAGE_DIR"

    local genhtml_opts=(
        coverage.info.cleaned
        --output-directory "../$COVERAGE_DIR"
        --title "libumem Coverage Report"
        --legend
        --show-details
        --demangle-cpp
    )

    if [[ $BRANCH_COVERAGE -eq 1 ]]; then
        genhtml_opts+=(
            --branch-coverage
            --rc lcov_branch_coverage=1
        )
    fi

    genhtml "${genhtml_opts[@]}" > /dev/null 2>&1

    print_success "HTML report generated at $COVERAGE_DIR/index.html"
}

# Extract coverage metrics
extract_metrics() {
    print_step "Extracting coverage metrics..."

    # Extract line coverage
    local line_info
    line_info=$(lcov --summary coverage.info.cleaned 2>&1 | grep "lines")
    local line_coverage
    line_coverage=$(echo "$line_info" | awk '{print $2}' | sed 's/%//')
    local line_hit
    line_hit=$(echo "$line_info" | awk '{print $4}')
    local line_total
    line_total=$(echo "$line_info" | awk '{print $6}')

    # Extract function coverage
    local func_info
    func_info=$(lcov --summary coverage.info.cleaned 2>&1 | grep "functions")
    local func_coverage
    func_coverage=$(echo "$func_info" | awk '{print $2}' | sed 's/%//')
    local func_hit
    func_hit=$(echo "$func_info" | awk '{print $4}')
    local func_total
    func_total=$(echo "$func_info" | awk '{print $6}')

    # Extract branch coverage if enabled
    local branch_coverage_val="N/A"
    local branch_hit="N/A"
    local branch_total="N/A"
    if [[ $BRANCH_COVERAGE -eq 1 ]]; then
        local branch_info
        branch_info=$(lcov --summary coverage.info.cleaned 2>&1 | grep "branches" || echo "")
        if [[ -n "$branch_info" ]]; then
            branch_coverage_val=$(echo "$branch_info" | awk '{print $2}' | sed 's/%//')
            branch_hit=$(echo "$branch_info" | awk '{print $4}')
            branch_total=$(echo "$branch_info" | awk '{print $6}')
        fi
    fi

    # Return metrics
    echo "$line_coverage|$line_hit|$line_total|$func_coverage|$func_hit|$func_total|$branch_coverage_val|$branch_hit|$branch_total"
}

# Display coverage summary
display_summary() {
    local metrics="$1"
    IFS='|' read -r line_cov line_hit line_total func_cov func_hit func_total branch_cov branch_hit branch_total <<< "$metrics"

    print_header "Coverage Summary"

    echo ""
    echo "  Line Coverage:     $line_cov% ($line_hit / $line_total lines)"
    echo "  Function Coverage: $func_cov% ($func_hit / $func_total functions)"
    if [[ "$branch_cov" != "N/A" ]]; then
        echo "  Branch Coverage:   $branch_cov% ($branch_hit / $branch_total branches)"
    fi
    echo ""
}

# Display file-level coverage
display_file_coverage() {
    print_header "File-Level Coverage"
    echo ""

    lcov --list coverage.info.cleaned 2>&1 | \
        grep -v "ignoring data" | \
        grep -v "^$" | \
        head -n 50
}

# Check coverage threshold
check_threshold() {
    local metrics="$1"
    IFS='|' read -r line_cov _ _ func_cov _ _ _ _ _ <<< "$metrics"

    print_header "Coverage Threshold Check"

    local line_check=0
    local func_check=0

    # Check line coverage
    if (( $(echo "$line_cov >= $MIN_COVERAGE" | bc -l) )); then
        print_success "Line coverage ($line_cov%) meets threshold (≥$MIN_COVERAGE%)"
        line_check=1
    else
        print_error "Line coverage ($line_cov%) below threshold (≥$MIN_COVERAGE%)"
        echo "  Gap: $(echo "$MIN_COVERAGE - $line_cov" | bc)% remaining"
    fi

    # Check function coverage
    if (( $(echo "$func_cov >= $MIN_COVERAGE" | bc -l) )); then
        print_success "Function coverage ($func_cov%) meets threshold (≥$MIN_COVERAGE%)"
        func_check=1
    else
        print_error "Function coverage ($func_cov%) below threshold (≥$MIN_COVERAGE%)"
        echo "  Gap: $(echo "$MIN_COVERAGE - $func_cov" | bc)% remaining"
    fi

    if [[ $line_check -eq 1 && $func_check -eq 1 ]]; then
        return 0
    else
        return 1
    fi
}

# Identify low-coverage files
identify_gaps() {
    print_header "Low Coverage Files (<80%)"
    echo ""

    lcov --list coverage.info.cleaned 2>&1 | \
        awk 'NR>4 && $2 ~ /%/ && $2 != "100.0%" {
            cov = $2;
            sub(/%/, "", cov);
            if (cov+0 < 80.0) print
        }' | \
        head -n 20
}

# Generate coverage badge data
generate_badge_data() {
    local metrics="$1"
    IFS='|' read -r line_cov _ _ _ _ _ _ _ _ <<< "$metrics"

    local badge_file="../$COVERAGE_DIR/badge.svg"
    local color="red"

    if (( $(echo "$line_cov >= 90" | bc -l) )); then
        color="brightgreen"
    elif (( $(echo "$line_cov >= 75" | bc -l) )); then
        color="yellow"
    elif (( $(echo "$line_cov >= 50" | bc -l) )); then
        color="orange"
    fi

    # Simple SVG badge (could be enhanced with shields.io API)
    cat > "$badge_file" << EOF
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="20">
  <linearGradient id="b" x2="0" y2="100%">
    <stop offset="0" stop-color="#bbb" stop-opacity=".1"/>
    <stop offset="1" stop-opacity=".1"/>
  </linearGradient>
  <mask id="a">
    <rect width="120" height="20" rx="3" fill="#fff"/>
  </mask>
  <g mask="url(#a)">
    <path fill="#555" d="M0 0h63v20H0z"/>
    <path fill="$color" d="M63 0h57v20H63z"/>
    <path fill="url(#b)" d="M0 0h120v20H0z"/>
  </g>
  <g fill="#fff" text-anchor="middle" font-family="DejaVu Sans,Verdana,Geneva,sans-serif" font-size="11">
    <text x="31.5" y="15" fill="#010101" fill-opacity=".3">coverage</text>
    <text x="31.5" y="14">coverage</text>
    <text x="90.5" y="15" fill="#010101" fill-opacity=".3">$line_cov%</text>
    <text x="90.5" y="14">$line_cov%</text>
  </g>
</svg>
EOF

    print_success "Generated coverage badge: $badge_file"
}

# Main execution
main() {
    print_header "libumem Coverage Report Generator"

    # Check dependencies
    check_dependencies

    # Clean if requested
    if [[ $CLEAN -eq 1 ]]; then
        clean_previous
    fi

    # Save current directory
    local start_dir
    start_dir=$(pwd)

    # Configure and build
    configure_with_coverage
    build_with_coverage

    # Run tests
    if ! run_tests; then
        print_warning "Some tests failed, but continuing with coverage analysis"
    fi

    # Generate coverage
    capture_coverage
    filter_coverage
    generate_html_report

    # Extract and display metrics
    local metrics
    metrics=$(extract_metrics)
    display_summary "$metrics"
    display_file_coverage
    identify_gaps

    # Generate badge
    generate_badge_data "$metrics"

    # Return to start directory
    cd "$start_dir"

    # Check threshold
    print_header "Final Status"
    if check_threshold "$metrics"; then
        print_success "Coverage meets threshold requirements"
        echo ""
        echo "View the full report:"
        echo "  xdg-open $COVERAGE_DIR/index.html"
        echo "  open $COVERAGE_DIR/index.html"
        echo "  firefox $COVERAGE_DIR/index.html"
        echo ""
        return 0
    else
        print_error "Coverage below threshold"
        echo ""
        echo "Next steps:"
        echo "  1. Review report: $COVERAGE_DIR/index.html"
        echo "  2. Identify untested code sections"
        echo "  3. Add tests to close coverage gaps"
        echo "  4. Re-run: ./scripts/generate-coverage-report.sh"
        echo ""
        echo "See TEST_COVERAGE_ANALYSIS.md for detailed gap analysis"
        echo ""
        return 1
    fi
}

# Run main function
main
exit $?
