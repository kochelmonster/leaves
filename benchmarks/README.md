# Leaves Benchmarking Tools

This directory contains benchmarking tools for the Leaves database.

## Set Operations

`bench_set_operations` compares exact-key union and intersection for
`leaves::_MemoryDB`, `std::set<std::string>` and
`std::unordered_set<std::string>`. All backends receive identical unique binary
keys, identical insertion order and empty values. Every result is verified
against the expected keys outside the timed region; Leaves' input sets are
also checked for unintended mutation.

Build an optimized benchmark in a separate build directory:

```bash
cmake -S . -B build/bench-set-operations -DCMAKE_BUILD_TYPE=Release \
  -DLEAVES_BUILD_TESTS=OFF -DLEAVES_BUILD_BENCHMARKS=ON -DLEAVES_GCOV=OFF
cmake --build build/bench-set-operations --target bench_set_operations --config Release -j4
./build/bench-set-operations/bench_set_operations --num=100000 --rounds=5 --overlap=50
```

For an existing Release build with benchmarks enabled, use
`cmake --build build --target bench_set_operations -j4` and
`./build/bench_set_operations`. The target is also included in
`leaves_benchmarks`, including the automatic Release subbuild. Multi-config
generators may place the executable in a `Release/` subdirectory.

Flags:

- `--num=100000`: number of unique keys in **each** input; zero is allowed.
- `--key_size=16`: complete key width in bytes, at least eight bytes and at most
  `_MemoryTraits::MAX_KEY_SIZE`. This is not a prefix-intersection width.
- `--overlap=50`: percentage of each input shared with the other, from 0 to 100;
  shared count is rounded down. Union size is `2 * num - shared`; intersection
  size is `shared`.
- `--layout=random`: randomized binary IDs with a collision-free 64-bit
  permutation, or `sequential` for big-endian IDs with more shared trie paths.
  Insertion order is shuffled in both layouts.
- `--seed=301`: deterministic key-layout and insertion-order seed.
- `--rounds=3`: positive number of independently rebuilt repetitions.
- `--iterative=1`: also measure an iterative Leaves union (`union_iter`) using
  source cursor traversal and destination `find()`/`value()` calls. Its result
  and timing boundaries are identical to merger union; disabled by default.
- `--help`: print usage.

Example contrasting workloads:

```bash
./build/bench_set_operations --num=100000 --rounds=5 --overlap=0 --layout=sequential
./build/bench_set_operations --num=100000 --rounds=5 --overlap=50 --layout=random
./build/bench_set_operations --num=100000 --rounds=5 --overlap=100 --key_size=32
```

The output reports median and best milliseconds plus verified result cardinality
for each backend and operation. Timings include operation work, output node/key
allocations and hash-table growth. They exclude key generation, input filling,
destination preparation, verification and destruction. Allocator/cache history
and machine configuration still affect results; use several rounds and sizes.

Union adds the right input into a destination prefilled with the left input,
using `_Merger` for Leaves and range insertion for the standard containers.
Intersection materializes a fresh result using `_Intersector`, sorted
`std::set_intersection`, or hash membership probes followed by insertion,
respectively. Hash results are not sorted as part of timing. No capacity is
reserved for standard-container outputs ahead of the timed operation.

The public set-operation functions require transactional DB cursors, so this
benchmark invokes the same internal algorithms on nontransactional memory
cursors. Its merger adapter supplies only the initialization interface expected
by `_Merger` and rejects big-value migration, which cannot occur with these
empty-valued sets. It adds no transaction, mmap or disk-I/O cost. This benchmark
measures exact keys only, not foreign-key prefix matching or joined payloads.

Performance rankings depend on overlap, key width, key distribution and output
allocation. A result from a map benchmark with 32-byte keys and 100-byte values
does not establish a ranking for empty-valued sets with 16-byte keys. Compare
both layouts and multiple overlaps rather than infer one universal ordering.
Intersection need not always beat in-place union: with identical inputs, union
can retain equal leaves and writable branch layouts, whereas intersection must
allocate an independently owned result for every key.

## db_bench_leaves

The `db_bench_leaves` program benchmarks various operations with the Leaves database. It's based on the LevelDB benchmark tool.

### Available Flags

- `--benchmarks=<list>`: Comma-separated list of benchmarks to run (default: all)
  - `fillseq`: Write values in sequential key order in async mode
  - `fillrandom`: Write values in random key order in async mode
  - `overwrite`: Overwrite values in random key order in async mode
  - `fillseqsync`: Write values in sequential key order in sync mode
  - `fillrandsync`: Write values in random key order in sync mode
  - `fillrand100K`: Write 100K values in random order in async mode
  - `fillseq100K`: Write 100K values in sequential order in async mode
  - `readseq`: Read values sequentially
  - `readrandom`: Read values in random order
  - `readseq100K`: Read 100K values in sequential order
  - `readrand100K`: Read 100K values in random order
  
- `--num=<n>`: Number of key/values to place in database (default: 1,000,000)
- `--reads=<n>`: Number of read operations to do (default: same as --num)
- `--value_size=<n>`: Size of each value in bytes (default: 100)
- `--use_file_storage=<0|1>`: Use FileStorage instead of MapStorage (default: 0)
- `--move_after_fill=<0|1>`: After each fresh fill, merge into a new database and remove the original (default: 0). The merge is not included in fill timing. Supported with MapStorage and FileStorage; not supported with replication or ConfluenceDB.
- `--compression=<0|1>`: Enable compression (default: 1)
- `--compression_ratio=<n>`: Target compression ratio (default: 0.5)
- `--page_size=<n>`: Page size in bytes (default: 1024)
- `--histogram=<0|1>`: Print operation timing histogram (default: 0)
- `--use_existing_db=<0|1>`: Use existing database instead of creating a new one (default: 0)
- `--db=<path>`: Path for database files (default: temp directory)

### Example Usage

```bash
# Run fill sequential and read sequential benchmarks with MapStorage
./build/db_bench_leaves --benchmarks=fillseq,readseq --num=10000

# Run benchmarks with FileStorage
./build/db_bench_leaves --benchmarks=fillseq,readseq --num=10000 --use_file_storage=1

# Merge the filled database into a new database before running reads
./build/db_bench_leaves --benchmarks=fillseq,readseq --num=10000 --move_after_fill=1

# Run all benchmarks with a larger value size
./build/db_bench_leaves --value_size=4096
```

### Comparing Storage Types

The benchmark tool allows comparing performance between `MapStorage` (memory-mapped files) and `FileStorage` (direct file operations). Use the `--use_file_storage=1` flag to switch to FileStorage for testing.

Example performance comparison:

```
# MapStorage
fillseq      :     0.282 micros/op;  393.0 MB/s   
readseq      :     0.033 micros/op; 3384.4 MB/s

# FileStorage
fillseq      :     0.350 micros/op;  315.9 MB/s   
readseq      :     0.032 micros/op; 3437.0 MB/s
```

Different storage types may perform better for different workloads and environments. Use this benchmarking tool to determine the optimal configuration for your use case.

## Browser Benchmark

The browser benchmark lives in [bench.html](bench.html) and runs entirely in JavaScript on top of [js/leaves.js](../js/leaves.js) via the embind API. It exercises `LeavesStore`, `LeavesDB`, and `LeavesCursor` directly from the browser rather than through a generated benchmark wasm entrypoint.

### Run It Locally

From the repository root:

```bash
# Build leaves.js + leaves.wasm (required once per rebuild)
emcmake cmake -B build-wasm -G Ninja
cmake --build build-wasm -j --target leaves_js_output

# Start a local HTTP server from the repo root
python3 -m http.server 8000
```

Then open:

```text
http://localhost:8000/benchmarks/bench.html
```

Notes:
- Serve from the repository root so both `benchmarks/` and `js/` are reachable.
- Stop the server with `Ctrl+C` when done.