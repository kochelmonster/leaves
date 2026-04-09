# Leaves

A high-performance, header-only C++20 embedded key-value database with built-in replication, built on a trie data structure. Leaves supports multiple storage backends including memory-mapped files, file-based storage with LRU caching, and WebAssembly/IndexedDB for browser environments.

## Features

- **Trie-based storage** — Ordered key-value store with efficient prefix operations
- **Cursor API** — Navigate, read, write, and scan with a single cursor object
- **Multiple storage backends** — Memory-mapped files (`MapStorage`), file-based with LRU cache (`FileStorage`), in-memory, and IndexedDB (WASM)
- **Transactions** — ACID commits with optional sync, rollback, and crash recovery
- **Two-phase commit** — Explicit prepare/commit flow via `prepare_commit()` for coordinated durability
- **Copy-on-Write** — Lock-free readers, consistent snapshots
- **Replication** — Streaming replication protocol (LVRP) with BLAKE3-based content addressing
- **Multiple named databases** — A single storage file can host many independent databases
- **Cross-platform** — GCC, Clang, MSVC; Linux, macOS, Windows, iOS, Android, WebAssembly
- **Header-only** — Just add `include/` to your include path

## Performance

1M entries, 100-byte values, Intel i7-12700KF:

### MapStorage (memory-mapped)

| Benchmark | 8-byte binary keys | 16-byte string keys |
|---|---|---|
| fillseq | 0.10 µs/op (1010 MB/s) | 0.14 µs/op (767 MB/s) |
| fillrandom | 0.29 µs/op (358 MB/s) | 0.50 µs/op (222 MB/s) |
| overwrite | 0.37 µs/op (277 MB/s) | 0.54 µs/op (205 MB/s) |
| readrandom | 0.16 µs/op (554 MB/s) | 0.29 µs/op (331 MB/s) |
| readseq | 0.03 µs/op (3233 MB/s) | 0.04 µs/op (2572 MB/s) |

### FileStorage (file-based with LRU cache)

| Benchmark | 8-byte binary keys |
|---|---|
| fillseq | 0.32 µs/op (318 MB/s) |
| fillrandom | 1.25 µs/op (83 MB/s) |
| readrandom | 0.23 µs/op (394 MB/s) |
| readseq | 0.07 µs/op (1594 MB/s) |

## Quick Start

```cpp
#include <leaves/mmap.hpp>

int main() {
    // Create or open a database file
    auto storage = leaves::MapStorage::create("mydata.lvs");

    // Open a named database
    auto db = storage->open("mydb");

    // Get a cursor for reading and writing
    auto cursor = db.cursor();

    // Insert a key-value pair
    cursor.find(leaves::Slice("hello"));
    cursor.value(leaves::Slice("world"));
    cursor.commit();

    // Read it back
    cursor.find(leaves::Slice("hello"));
    if (cursor.is_valid()) {
        leaves::Slice val = cursor.value();
        // val.data() == "world", val.size() == 5
    }

    // Iterate all keys in order
    for (cursor.first(); cursor.is_valid(); cursor.next()) {
        leaves::Slice key = cursor.key();
        leaves::Slice val = cursor.value();
    }
}
```

## Building

Leaves requires C++20 and Boost (for the test framework).

```bash
cmake -B build -G Ninja
cmake --build build -j
```

### CMake Options

| Option | Description |
|---|---|
| `LEAVES_BUILD_TESTS` | Build the test suite |
| `LEAVES_BUILD_BENCHMARKS` | Build benchmark executables |
| `LEAVES_GCOV` | Enable code coverage |
| `LEAVES_ASAN` | Enable AddressSanitizer |
| `LEAVES_SINGLE_PROCESS` | Disable multi-process support (mobile/embedded) |

### Using as a dependency

Leaves is header-only. Add the `include/` directory to your include path:

```cmake
target_include_directories(mytarget PRIVATE /path/to/leaves/include)
```

BLAKE3 sources are bundled under `BLAKE3/c/` and need to be compiled if replication is used.

## API Reference

### Storage Backends

```cpp
#include <leaves/mmap.hpp>    // MapStorage — memory-mapped, fastest for random access
#include <leaves/fstore.hpp>  // FileStorage — file-based with configurable LRU cache

// MapStorage: reserves virtual address space, uses mmap
auto storage = leaves::MapStorage::create("data.lvs", map_size, db_count);

// FileStorage: page-based I/O with in-memory LRU cache
auto storage = leaves::FileStorage::create("data.lvs", cache_capacity, db_count);
```

### Multiple Databases

A single storage file can contain many independent databases:

```cpp
auto storage = leaves::MapStorage::create("data.lvs");

auto users = storage->open("users");
auto logs  = storage->open("logs");

// List all databases
std::vector<std::string> names;
storage->list_dbs(names);

// Remove a database
storage->remove("logs");
```

### Cursor Operations

```cpp
auto cursor = db.cursor();

// Positioning
cursor.find(key);    // Seek to key (exact match or insertion point)
cursor.first();      // Move to first key
cursor.last();       // Move to last key
cursor.next();       // Move to next key
cursor.prev();       // Move to previous key

// Reading
bool valid = cursor.is_valid();  // Check if cursor points to a valid entry
Slice key = cursor.key();        // Get current key
Slice val = cursor.value();      // Get current value

// Writing
cursor.find(key);
cursor.value(new_value);         // Insert or update
cursor.remove();                 // Delete current entry
cursor.commit(sync);             // Commit changes (sync=true for durability)
cursor.rollback();               // Discard uncommitted changes
```

### Transactions

```cpp
auto cursor = db.cursor();

if (!cursor.start_transaction()) {
    return 1;
}

cursor.find("key1");
cursor.value("value1");

cursor.find("key2");
cursor.value("value2");

// Atomic commit of all changes
cursor.commit(/* sync */ true);

// Or discard
cursor.rollback();
```

### Replication

```cpp
#include <leaves/mmap.hpp>
#include <leaves/replication.hpp>

auto storage = leaves::MapStorage::create("data.lvs");
auto db = storage->open<leaves::_ReplicationDB>("mydb");

// Sender side
leaves::ReplicationSender<leaves::MapStorage> sender(db);
sender.begin(&transport, &events);

// Receiver side
leaves::ReplicationReceiver<leaves::MapStorage> receiver(db);
receiver.begin(&transport, &events);
```

## Storage Backends

| Backend | Header | Use Case |
|---|---|---|
| `MapStorage` | `mmap.hpp` | Default. Memory-mapped files. Fastest random access. |
| `FileStorage` | `fstore.hpp` | File-based with LRU cache. Better for large-data or memory-constrained environments. |
| `BrowserStorage` | `browserstore.hpp` | IndexedDB backend for WebAssembly (Emscripten). |

## Tests

```bash
cmake -B build -G Ninja -DLEAVES_BUILD_TESTS=ON
cmake --build build -j
cd build && ctest
```

## Benchmarks

```bash
cmake -B build -G Ninja -DLEAVES_BUILD_BENCHMARKS=ON
cmake --build build -j

# Run with binary keys (8-byte, big-endian)
./build/db_bench_leaves --binary_key=1

# Run with string keys (16-byte decimal)
./build/db_bench_leaves --binary_key=0

# Use FileStorage instead of MapStorage
./build/db_bench_leaves --use_file_storage=1
```

    
- Convertsion tools value->binary sortable
- TransferTries (Result und Joins)
- Joiner
- Subtrie Replication
- Subtrie remove 

