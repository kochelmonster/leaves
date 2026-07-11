#!/bin/bash

# This script finds the merge threshold sweet spot in leaves for the
# workload_kv_concurrent_write workload.

set -e

YCSB_DIR="../YCSB-cpp"
YCSB_BIN="$YCSB_DIR/build/ycsb"
WORKLOAD="$YCSB_DIR/workloads/workload_kv_session"
RESULTS_DIR="$YCSB_DIR/benchmark_results"
DB_PROPERTIES="$YCSB_DIR/leaves/leaves.properties"


# Ensure the results directory exists
mkdir -p "$RESULTS_DIR"
# Clean the database directory
rm -rf /tmp/ycsb-leaves

# Load phase
"$YCSB_BIN" -load -db leaves -P "$DB_PROPERTIES" -P "$WORKLOAD" \
   leaves.destroy=true -s > "$RESULTS_DIR/load.log" 2>&1

# Run phase
output_file="$RESULTS_DIR/run_leaves_concurrent_write_t${threshold}.log"
perf_data_file="perf.data.t1024000"
perf_report_file="perf.report.t${threshold}.txt"

# Profile the benchmark
perf record -g --call-graph dwarf -o "$perf_data_file" -- \
  "$YCSB_BIN" -run -db leaves -P "$DB_PROPERTIES" -P "$WORKLOAD" -p batch_size=1 -s > "$output_file" 2>&1

# Generate perf report
perf report --call-graph -i "$perf_data_file" > "$perf_report_file"


