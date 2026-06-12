#!/bin/bash
# Sweet spot finder for leaves.merge_threshold
# Workload: workload_kv_concurrent_session (50/50 read/update, 8 threads, zipfian)
#
# Tests a range of merge_threshold values and reports load + run throughput
# to identify the optimal setting.

set -euo pipefail

YCSB_DIR="/home/michael/src/YCSB-cpp"
YCSB_BIN="$YCSB_DIR/build/ycsb"
PROPS="$YCSB_DIR/leaves/leaves.properties"
WORKLOAD="$YCSB_DIR/workloads/workload_kv_concurrent_session"
DBPATH="/tmp/ycsb-leaves"

# Threshold values to test (sorted)
THRESHOLDS=(
  10000
  25000
  40000
  50000
  60000
  75000
  100000
  150000
  200000
  300000
  500000
  1000000
)

# Results storage
declare -A LOAD_RESULTS
declare -A RUN_RESULTS

echo "=============================================="
echo " Sweet Spot Finder: leaves.merge_threshold"
echo " Workload: workload_kv_concurrent_session"
echo "=============================================="
echo ""
echo "Testing ${#THRESHOLDS[@]} threshold values..."
echo ""

for threshold in "${THRESHOLDS[@]}"; do
  echo "--- threshold=$threshold ---"

  # Clean up previous database
  rm -rf "$DBPATH" "$DBPATH".*

  # Load phase
  echo "  Load phase..."
  LOAD_OUTPUT=$("$YCSB_BIN" -load -threads 8 -db leaves \
    -P "$PROPS" \
    -P "$WORKLOAD" \
    -p leaves.merge_threshold="$threshold" \
    -p leaves.destroy=true \
    -s 2>&1)

  LOAD_OPS=$(echo "$LOAD_OUTPUT" | grep -oP 'Load throughput\(ops/sec\):\s*\K[0-9]+(\.[0-9]+)?')
  if [ -z "$LOAD_OPS" ]; then
    echo "  ERROR: Failed to parse load throughput. Output:"
    echo "$LOAD_OUTPUT" | tail -20
    LOAD_OPS="N/A"
  fi
  LOAD_RESULTS[$threshold]="$LOAD_OPS"
  echo "  Load throughput: $LOAD_OPS ops/sec"

  # Run (transaction) phase
  echo "  Run phase..."
  RUN_OUTPUT=$("$YCSB_BIN" -run -threads 8 -db leaves \
    -P "$PROPS" \
    -P "$WORKLOAD" \
    -p leaves.merge_threshold="$threshold" \
    -s 2>&1)

  RUN_OPS=$(echo "$RUN_OUTPUT" | grep -oP 'Run throughput\(ops/sec\):\s*\K[0-9]+(\.[0-9]+)?')
  if [ -z "$RUN_OPS" ]; then
    echo "  ERROR: Failed to parse run throughput. Output:"
    echo "$RUN_OUTPUT" | tail -20
    RUN_OPS="N/A"
  fi
  RUN_RESULTS[$threshold]="$RUN_OPS"
  echo "  Run throughput: $RUN_OPS ops/sec"
  echo ""
done

# Print summary table
echo ""
echo "=============================================="
echo " RESULTS SUMMARY"
echo "=============================================="
printf "%-12s %-18s %-18s\n" "threshold" "load_ops/sec" "run_ops/sec"
printf "%-12s %-18s %-18s\n" "----------" "------------------" "------------------"

BEST_THRESHOLD=""
BEST_RUN=0

for threshold in "${THRESHOLDS[@]}"; do
  load="${LOAD_RESULTS[$threshold]}"
  run="${RUN_RESULTS[$threshold]}"
  printf "%-12s %-18s %-18s\n" "$threshold" "$load" "$run"

  # Track best run throughput
  if [ "$run" != "N/A" ]; then
    if awk "BEGIN {exit !($run > $BEST_RUN)}"; then
      BEST_RUN=$run
      BEST_THRESHOLD=$threshold
    fi
  fi
done

echo ""
if [ -n "$BEST_THRESHOLD" ]; then
  echo "Best merge_threshold: $BEST_THRESHOLD ($BEST_RUN ops/sec)"
else
  echo "Could not determine best threshold (all runs failed)."
fi
echo ""
echo "Done."