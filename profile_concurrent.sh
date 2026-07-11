#!/bin/bash

# This script finds the merge threshold sweet spot in leaves for the
# workload_kv_concurrent_write workload.

set -e

YCSB_DIR="../YCSB-cpp"
YCSB_BIN="$YCSB_DIR/build/ycsb"
WORKLOAD="$YCSB_DIR/workloads/workload_kv_concurrent_write"
RESULTS_DIR="$YCSB_DIR/benchmark_results"
DB_PROPERTIES="$YCSB_DIR/leaves/leaves.properties"

# Merge thresholds to test
THRESHOLDS=(512000 1024000 2048000)

# File to store the results
RESULTS_FILE="merge_threshold_results.csv"
echo "threshold,throughput" > "$RESULTS_FILE"

# Ensure the results directory exists
mkdir -p "$RESULTS_DIR"

for threshold in "${THRESHOLDS[@]}"; do
  echo "Testing merge threshold: $threshold"

  # Clean the database directory
  rm -rf /tmp/ycsb-leaves

  # Load phase
  "$YCSB_BIN" -load -db leaves -P "$DB_PROPERTIES" -P "$WORKLOAD" \
    -p leaves.format=confluence -p leaves.destroy=true -s > "$RESULTS_DIR/load.log" 2>&1

  # Run phase
  output_file="$RESULTS_DIR/run_leaves_concurrent_write_t${threshold}.log"
  perf_data_file="perf.data.t1024000"
  perf_report_file="perf.report.t${threshold}.txt"

  # Profile the benchmark
  perf record -g --call-graph dwarf -o "$perf_data_file" -- \
    "$YCSB_BIN" -run -db leaves -P "$DB_PROPERTIES" -P "$WORKLOAD" \
      -p leaves.format=confluence -p leaves.merge_threshold="$threshold" -p leaves.merge_at_end=false -s > "$output_file" 2>&1

  # Generate perf report
  perf report --call-graph -i "$perf_data_file" > "$perf_report_file"

  # Extract the throughput
  THROUGHPUT=$(grep "Run throughput(ops/sec):" "$output_file" | awk '{print $3}')

  # Save the result
  echo "$threshold,$THROUGHPUT" >> "$RESULTS_FILE"
done

echo "Results saved to $RESULTS_FILE"
echo ""
echo "Results:"
column -s, -t < "$RESULTS_FILE"

# Find the best threshold
BEST_THRESHOLD=$(sort -t, -k2,2nr "$RESULTS_FILE" | head -n1 | cut -d, -f1)
BEST_THROUGHPUT=$(sort -t, -k2,2nr "$RESULTS_FILE" | head -n1 | cut -d, -f2 | sed 's/,/./')
echo ""
printf "Best threshold: %s, Throughput: %.2f\n" "$BEST_THRESHOLD" "$BEST_THROUGHPUT"
