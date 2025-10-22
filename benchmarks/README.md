# Leaves Benchmarking Tools

This directory contains benchmarking tools for the Leaves database.

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