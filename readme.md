# Leaves

Leaves is a cursor-first, header-only C++20 embedded key-value store built around a persistent trie. It is designed for ordered access, copy-on-write snapshots, multi-database files, and built-in replication across native and browser-oriented storage backends.

Leaves is not a drop-in LevelDB API clone. The public API is centered around named databases and cursors rather than `Get`/`Put` calls, so projects that need LevelDB compatibility should plan a thin adapter layer.

## Highlights

- Ordered trie storage with efficient scans and prefix-friendly access patterns
- Cursor-based read, write, delete, and iteration workflow
- Multiple storage backends: `MapStorage`, `FileStorage`, and `BrowserStorage`
- ACID transactions with rollback, optional sync, and crash-recovery support
- Explicit `prepare_commit()` support for two-phase commit style workflows
- Copy-on-write snapshots with lock-free readers
- Built-in replication protocol backed by BLAKE3 content addressing
- Multiple named databases inside a single storage file
- Cross-platform native support plus WebAssembly/browser targets

## Quick Start

```cpp
#include <leaves/mmap.hpp>

int main() {
    auto storage = leaves::MapStorage::create("mydata.lvs");
    auto db = storage->open("main");
    auto cursor = db.cursor();

    cursor.find(leaves::Slice("hello"));
    cursor.value(leaves::Slice("world"));
    cursor.commit();

    cursor.find(leaves::Slice("hello"));
    if (!cursor.is_valid()) {
        return 1;
    }

    for (cursor.first(); cursor.is_valid(); cursor.next()) {
        leaves::Slice key = cursor.key();
        leaves::Slice value = cursor.value();
        (void)key;
        (void)value;
    }

    return 0;
}
```

## Storage Backends

| Backend | Header | Best Fit |
|---|---|---|
| `MapStorage` | `leaves/mmap.hpp` | Default native backend. Uses memory mapping and is typically the fastest for random access. |
| `FileStorage` | `leaves/fstore.hpp` | Page-oriented file I/O with an in-memory cache. Better for tighter memory budgets or environments where large virtual mappings are undesirable. |
| `BrowserStorage` | `leaves/browserstore.hpp` | IndexedDB-backed storage for Emscripten/WebAssembly builds. |

`MapStorage::create(path, map_size)` reserves virtual address space. On mobile targets, a smaller `map_size` is often the right tradeoff. `FileStorage::create(path, cache_capacity)` lets you tune cache size instead.

## Build

### Prerequisites

- A C++20 compiler
- Boost 1.80 or newer for the native project build and test tooling
- CMake 3.23 or newer to use the repository presets
- Ninja is recommended

### Recommended local build

```bash
cmake --preset default
cmake --build --preset default -j
```

The `default` preset configures a debug-oriented native build in `build/` with tests and benchmarks enabled.

### Manual configure fallback

If you prefer not to use presets:

```bash
cmake -B build -G Ninja \
  -DLEAVES_BUILD_TESTS=ON \
  -DLEAVES_BUILD_BENCHMARKS=ON
cmake --build build -j
```

### Important CMake options

| Option | Default | Description |
|---|---|---|
| `LEAVES_BUILD_TESTS` | `ON` | Build the native Leaves test executables and register them with CTest. |
| `LEAVES_BUILD_BENCHMARKS` | `ON` | Build the benchmark executables. |
| `LEAVES_GCOV` | `ON` | Enable coverage instrumentation for test-oriented builds. |
| `LEAVES_ASAN` | `ON` | Enable AddressSanitizer when coverage is off. |
| `LEAVES_SINGLE_PROCESS` | `OFF` | Disable multi-process support for constrained targets such as mobile or embedded environments. |

For timing-sensitive benchmarks, use a separate release-style build with coverage and sanitizers disabled.

```bash
cmake -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLEAVES_BUILD_TESTS=OFF \
  -DLEAVES_BUILD_BENCHMARKS=ON \
  -DLEAVES_GCOV=OFF \
  -DLEAVES_ASAN=OFF
cmake --build build-release -j
```

## Using Leaves in Another Project

Leaves is header-only at the database layer. To consume it directly, add `include/` to your include path.

```cmake
target_include_directories(mytarget PRIVATE /path/to/leaves/include)
```

If you use replication, also compile the bundled BLAKE3 sources from `BLAKE3/c/` and add that directory to your include path.

### Install and package

The native build now exports an installable CMake package for downstream consumers.

```bash
cmake --preset default
cmake --build --preset default -j
cmake --install build --prefix "$PWD/install"
```

That install tree always contains the exported `leaves::leaves` target. Replication users can additionally consume `leaves::replication`, which brings in the bundled `blake3` package. Vendored LevelDB and Google Benchmark artifacts are not installed as part of the Leaves package.

To create a redistributable archive from the current build tree:

```bash
cmake --build build --target package
```

### Consuming the installed package

```cmake
find_package(leaves CONFIG REQUIRED)
target_link_libraries(mytarget PRIVATE leaves::leaves)
```

If you use replication, request the optional replication component and link the replication target instead:

```cmake
find_package(leaves CONFIG REQUIRED COMPONENTS replication)
target_link_libraries(mytarget PRIVATE leaves::replication)
```

The core `leaves::leaves` target carries the public include paths and the required Boost header dependency. `leaves::replication` is the optional target that adds the BLAKE3 dependency needed by `leaves/replication.hpp`.

## API Overview

### Named databases

One storage file can host multiple logical databases.

```cpp
auto storage = leaves::MapStorage::create("data.lvs");

auto users = storage->open("users");
auto logs = storage->open("logs");

std::vector<std::string> names;
storage->list_dbs(names);

storage->remove("logs");
```

### Cursor operations

```cpp
auto cursor = db.cursor();

cursor.find(key);
cursor.first();
cursor.last();
cursor.next();
cursor.prev();

bool valid = cursor.is_valid();
leaves::Slice current_key = cursor.key();
leaves::Slice current_value = cursor.value();

cursor.find(key);
cursor.value(new_value);
cursor.remove();
cursor.commit(false);
cursor.rollback();
```

### Transactions and durability

- `cursor.start_transaction(non_blocking)` starts an explicit write transaction.
- `cursor.commit(sync)` commits the current transaction. Pass `true` when you need durability before returning.
- `cursor.rollback()` discards uncommitted changes.
- `cursor.prepare_commit(sync)` is available when you need a separate prepare phase before the final commit.
- `db.commit(sync)` and `db.rollback()` expose database-level transaction recovery hooks.

### Replication

```cpp
#include <leaves/mmap.hpp>
#include <leaves/replication.hpp>

auto storage = leaves::MapStorage::create("data.lvs");
auto db = storage->open<leaves::_ReplicationDB>("main");

leaves::ReplicationSender<leaves::MapStorage> sender(db);
sender.begin(&transport, &events);

leaves::ReplicationReceiver<leaves::MapStorage> receiver(db);
receiver.begin(&transport, &events);
```

For in-process testing or local synchronization loops, the public API also exposes `run_replication(sender, receiver, sender_transport, receiver_transport)`.

## Testing

Leaves registers only its own native tests with CTest. Vendored LevelDB tests are intentionally excluded from the default test surface.

```bash
cmake --preset default
cmake --build --preset default -j
ctest --preset test-all
```

The test suite is labeled into slices so you can run the smallest relevant scope:

| Preset | Scope |
|---|---|
| `test-all` | All native Leaves tests in the default build tree |
| `test-unit` | Small, focused unit-style tests |
| `test-integration` | Transfer, replication, executor, and similar integration flows |
| `test-stress` | Concurrency and longer-running stress tests |
| `ci-all`, `ci-unit`, `ci-integration`, `ci-stress` | The same filters in the isolated `build/ci-tests` tree used by CI |

Examples:

```bash
ctest --preset test-unit
ctest --preset test-integration
ctest --preset test-stress
```

## Benchmarks

The main native benchmark driver is `db_bench_leaves`, modeled after the familiar LevelDB benchmark workflow. Additional executables cover in-memory comparisons and lower-level microbenchmarks.

```bash
cmake --preset default
cmake --build --preset default -j --target db_bench_leaves

# Sequential write + sequential read with the default MapStorage backend
./build/db_bench_leaves --benchmarks=fillseq,readseq --num=10000

# Compare against FileStorage
./build/db_bench_leaves --benchmarks=fillseq,readseq --num=10000 --use_file_storage=1
```

Representative numbers for 1M entries with 100-byte values on an Intel i7-12700KF:

### MapStorage

| Benchmark | 8-byte binary keys | 16-byte string keys |
|---|---|---|
| fillseq | 0.10 us/op (1010 MB/s) | 0.14 us/op (767 MB/s) |
| fillrandom | 0.29 us/op (358 MB/s) | 0.50 us/op (222 MB/s) |
| overwrite | 0.37 us/op (277 MB/s) | 0.54 us/op (205 MB/s) |
| readrandom | 0.16 us/op (554 MB/s) | 0.29 us/op (331 MB/s) |
| readseq | 0.03 us/op (3233 MB/s) | 0.04 us/op (2572 MB/s) |

### FileStorage

| Benchmark | 8-byte binary keys |
|---|---|
| fillseq | 0.32 us/op (318 MB/s) |
| fillrandom | 1.25 us/op (83 MB/s) |
| readrandom | 0.23 us/op (394 MB/s) |
| readseq | 0.07 us/op (1594 MB/s) |

For benchmark flags and additional examples, see [benchmarks/README.md](benchmarks/README.md).

## Browser and WebAssembly

When configured with Emscripten, the native build path is replaced with browser-oriented targets such as `test_browserstore`, `db_bench_browser`, `leaves`, `kv_demo_client`, and `ws_replication_client`. The build copies generated JS and WASM artifacts into `js/` and `benchmarks/` for local use.

## Additional Documentation

- [docs/BROWSER_STORAGE.md](docs/BROWSER_STORAGE.md)
- [docs/performance.md](docs/performance.md)
- [docs/TRANSFER_TRIE_SYNC.md](docs/TRANSFER_TRIE_SYNC.md)
- [docs/SERIAL_NUMBER_ARITHMETIC.md](docs/SERIAL_NUMBER_ARITHMETIC.md)
- [benchmarks/README.md](benchmarks/README.md)

## Roadmap

- Conversion tools for value to binary-sortable representations
- TransferTrie result and join support
- Joiner support
- Subtrie replication
- Subtrie removal


