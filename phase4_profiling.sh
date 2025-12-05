#!/bin/bash

# Phase 4 Stage 3.1: Profiling Script to Identify Real Bottlenecks
# Uses perf to profile Urho3D samples and identify CPU hotspots

set -e

BIN_DIR="/home/leith/Desktop/URHO/New/urho3d-2.0.1/build/bin"
PROFILE_DIR="/home/leith/Desktop/URHO/New/urho3d-2.0.1/build/phase4_profiling"
DURATION=10  # Profile for 10 seconds per sample

# Create profiling directory
mkdir -p "$PROFILE_DIR"

echo "=== Phase 4 Stage 3.1: Bottleneck Profiling ==="
echo ""
echo "Profiling Urho3D samples to identify real CPU bottlenecks"
echo "Duration: $DURATION seconds per sample"
echo "Output directory: $PROFILE_DIR"
echo ""

# Function to profile a single sample
profile_sample() {
    local sample_name=$1
    local sample_bin=$2
    local output_file="$PROFILE_DIR/${sample_name}.perf.data"
    local report_file="$PROFILE_DIR/${sample_name}.perf.txt"

    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Profiling: $sample_name"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""

    # Run perf record
    echo "[1/3] Recording performance data (${DURATION}s)..."
    if timeout $((DURATION + 5)) perf record \
        -F 99 \
        -g \
        --call-graph=dwarf \
        -o "$output_file" \
        "$sample_bin" &>/dev/null || true; then
        echo "✓ Performance data recorded"
    else
        echo "✗ Failed to record performance data (may require elevated permissions)"
        echo "  Tip: Run 'sudo sysctl -w kernel.perf_event_paranoid=-1' for full access"
        return 1
    fi

    echo ""
    echo "[2/3] Generating report..."

    # Generate text report
    if perf report \
        -i "$output_file" \
        --no-children \
        --sort=overhead \
        > "$report_file" 2>&1; then
        echo "✓ Report generated: $report_file"
    else
        echo "✗ Failed to generate report"
        return 1
    fi

    echo ""
    echo "[3/3] Top 20 hotspots:"
    echo ""

    # Show top 20 functions
    perf report \
        -i "$output_file" \
        --no-children \
        --sort=overhead \
        --stdio 2>/dev/null | grep -E "^\s+[0-9]+\.[0-9]+%" | head -20 || true

    echo ""
    echo "Full report saved to: $report_file"
    echo ""
}

# Profile samples
echo "PROFILING SAMPLES"
echo ""

# Check if samples exist
if [ ! -f "$BIN_DIR/01_HelloWorld" ]; then
    echo "Error: Sample binaries not found in $BIN_DIR"
    exit 1
fi

echo "NOTE: Profiling may require elevated permissions."
echo "If you see 'Permission denied' errors, run:"
echo "  sudo sysctl -w kernel.perf_event_paranoid=-1"
echo ""

# Profile each sample
profile_sample "01_HelloWorld" "$BIN_DIR/01_HelloWorld" || true
profile_sample "14_SoundEffects" "$BIN_DIR/14_SoundEffects" || true
profile_sample "25_Urho2DParticle" "$BIN_DIR/25_Urho2DParticle" || true

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "PROFILING COMPLETE"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Results saved to: $PROFILE_DIR/"
echo ""
echo "To view full report for a sample:"
echo "  perf report -i $PROFILE_DIR/01_HelloWorld.perf.data"
echo ""
echo "To generate flame graph (if flamegraph-py installed):"
echo "  perf script -i $PROFILE_DIR/01_HelloWorld.perf.data | flamegraph.py > flame.svg"
echo ""
