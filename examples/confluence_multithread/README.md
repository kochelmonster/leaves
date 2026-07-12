# Confluence Multithread Example

This example demonstrates concurrent writes to a `ConfluenceDB` with lean output:

- Multiple writer threads perform short write transactions in parallel.
- No merge worker thread is used; Confluence handles merge progression internally.
- The run now requires that at least one merge drain generation completes while
  writers are still active.
- Internal processing visibility comes from aspects (`MetricsMapTraits`) via
  snapshot counters (user commits, merge commits, merge adds/overwrites/deletes).
- The demo issues one in-run merge probe plus a final `merge_all_now()` so
  merge activity is visible both during writes and after final drain.
- Final validation checks key count and sampled key/value integrity.

## Build

```bash
cd examples/confluence_multithread
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```bash
./build/confluence_multithread
```

Output includes:

- startup/mid-run/final aspect metric snapshots
- explicit confirmation when merge is observed while writers are active
- writer completion lines
- commit throughput summary
- final consistency result

## Configuration

The demo uses constants in
`examples/confluence_multithread/main.cpp` (`DemoConfig`).
Adjust these values directly in code:

- `writers`
- `ops_per_writer`
- `writer_pause_every_ops`, `writer_pause_us`
- `merge_probe_min_commits`, `merge_probe_timeout_ms`
- `merge_write_threshold`
- `max_attached_age_ms`
- `db_path`, `db_name`, `clean_start`

For a heavier run, increase `writers` and `ops_per_writer`, rebuild, and run again.
