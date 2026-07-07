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

### Emscripten/WebAssembly Build

To build the WebAssembly targets, you first need to install and activate the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html).

Once your environment is configured, you can generate the build files using the Emscripten toolchain. The recommended way is to use the `emcmake` wrapper. The default configuration builds in **release mode** (`-O3`, `NDEBUG`, no diagnostic output):

```bash
emcmake cmake -B build-wasm
```

Alternatively, you can manually specify the path to the Emscripten toolchain file:

```bash
cmake -B build-wasm -DCMAKE_TOOLCHAIN_FILE=<path-to-emscripten-sdk>/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake
```

Then, run the build:

```bash
cmake --build build-wasm -j
```

#### Debug Mode for Browser Testing

To enable diagnostic output (such as `[flush]` messages from the browser store) and include debug symbols, configure with `-DLEAVES_BROWSER_DEBUG=ON`:

```bash
emcmake cmake -B build-wasm-debug -G Ninja -DLEAVES_BROWSER_DEBUG=ON
cmake --build build-wasm-debug -j --target browser_test
```

This builds the browser WASM runtime targets used by the web UI, including `leaves_js_output` and `ws_replication_client`, with `DEBUG` defined, `-g -O0` flags, and profiling symbols.

The build will place the generated Javascript and WebAssembly files in the `js/` and `examples/` directories. For example, you will find `leaves.js` and `leaves.wasm` in the `js/` directory, and `benchmarks/bench.html` uses that module together with a pure JavaScript benchmark runner.

#### Switching the Async Backend

By default the WebAssembly build uses **JSPI** (JavaScript Promise Integration) to suspend and resume WASM execution across IndexedDB I/O calls. To switch to the older **ASYNCIFY** backend instead, configure with `-DLEAVES_ASYNC_BACKEND=ASYNCIFY`:

```bash
emcmake cmake -B build-wasm -G Ninja -DLEAVES_ASYNC_BACKEND=ASYNCIFY
cmake --build build-wasm -j
```

Differences between the two backends:

| | JSPI (default) | ASYNCIFY |
|---|---|---|
| Exception handling | `-fwasm-exceptions` (native WASM) | `-fexceptions` (JS-based) |
| JS API | `async()` → returns Promise | synchronous (blocking) |
| Main thread | non-blocking during I/O | blocks during I/O |
| `.function("foo", &foo, async())` | `&foo, async()` | `&foo` (no-op, via `LEAVES_ASYNC` macro) |

Both backends present the same JS API — callers always use `await`, which is a no-op on non-Promise values. The `LEAVES_ASYNCIFY` compile definition is set automatically when using ASYNCIFY, so the embind bindings compile correctly in either mode.

Switch back by omitting the flag (defaults to `JSPI`) or passing `-DLEAVES_ASYNC_BACKEND=JSPI`.

## Using Leaves in Another Project

Leaves is header-only at the database layer. To consume it directly, add `include/` to your include path.

```cmake
target_include_directories(mytarget PRIVATE /path/to/leaves/include)
```

If you use replication, also compile the bundled BLAKE3 sources from `BLAKE3/c/` and add that directory to your include path.

### Using `add_subdirectory`

As an alternative to installing, you can add Leaves as a Git submodule and include it directly in your build with `add_subdirectory`.

1.  **Add the submodule:**
    ```bash
    git submodule add https://github.com/kochelmonster/leaves.git extern/leaves
    ```

2.  **Call `add_subdirectory`:** In your `CMakeLists.txt`, disable tests and benchmarks and then add the directory.
    ```cmake
    set(LEAVES_BUILD_TESTS OFF)
    set(LEAVES_BUILD_BENCHMARKS OFF)
    add_subdirectory(extern/leaves)
    ```

3.  **Link the target:**
    ```cmake
    target_link_libraries(mytarget PRIVATE leaves::leaves)
    ```

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

When configured with Emscripten, the native build path is replaced with browser-oriented targets such as `test_browserstore`, `leaves`, `kv_demo_client`, and `ws_replication_client`. The build copies generated JS and WASM artifacts into `js/` for local use, while the browser benchmark page loads `js/leaves.js` directly and runs its benchmark logic in JavaScript.

### Example: Using `BrowserStorage`

After building the project with Emscripten, you will have `leaves.js` and `leaves.wasm` in the `js/` directory. You can then use them in your web application.

Most of the API functions are asynchronous and return Javascript `Promise`s, so using `async/await` is recommended.

Here is a basic example:

**index.html**
```html
<!DOCTYPE html>
<html>
<head>
    <title>Leaves-JS Example</title>
</head>
<body>
    <h1>Leaves-JS Example</h1>
    <script src="leaves.js"></script>
    <script src="example.js"></script>
</body>
</html>
```

**example.js**
```javascript
async function run() {
    // The 'leaves' module is loaded automatically by leaves.js
    const Module = await leaves();

    console.log("Leaves module loaded.");

    // Create or open a store
    const store = await Module.LeavesStore.create("my-db", 1024 * 1024);
    console.log("Store created.");

    // Open a database within the store
    const db = await store.open("my-first-db");
    console.log("Database opened.");

    // Create a cursor
    const cursor = await db.createCursor();
    console.log("Cursor created.");

    // Write a key-value pair
    await cursor.find("hello");
    await cursor.setValue("world from Javascript!");
    await cursor.commit(true); // true for sync
    console.log("Wrote 'hello' -> 'world from Javascript!'");

    // Read the value back
    await cursor.find("hello");
    if (cursor.isValid()) {
        const value = await cursor.getValue();
        console.log("Read value:", value);
    } else {
        console.log("Key 'hello' not found.");
    }

    // Close the store
    await store.close();
    console.log("Store closed.");
}

run().catch(console.error);
```

### Running the Browser Test

After building with Emscripten, you can run the test suite in a browser:

1.  **Start a web server:**
    ```bash
    python3 -m http.server -d js 8000
    ```

2.  **Open the test page:**
    Open `http://localhost:8000/test.html` in your web browser. The test results will be displayed on the page.

### Running the Browser Benchmark

From the repository root, build the browser module outputs and start a local server:

```bash
emcmake cmake -B build-wasm -G Ninja
cmake --build build-wasm -j --target leaves_js_output
python3 -m http.server 8000
```

Then open `http://localhost:8000/benchmarks/bench.html`.

For hang diagnosis, use the debug browser build so diagnostics are emitted from both the JS runner and BrowserStore wait loops:

```bash
emcmake cmake -B build-wasm-debug -G Ninja -DLEAVES_BROWSER_DEBUG=ON
cmake --build build-wasm-debug -j --target browser_test
python3 -m http.server 8000
```

In `bench.html`, debug builds now print `[diag]` heartbeats with current phase, progress counters, pending IndexedDB writes, and the current await point. If a benchmark stalls, these lines show exactly where it is waiting (for example `cursor.commit`, `reqDone(get)`, or `wait_for_writes`).

## Additional Documentation

- [docs/BROWSER_STORAGE.md](docs/BROWSER_STORAGE.md)
- [docs/performance.md](docs/performance.md)
- [docs/TRANSFER_TRIE_SYNC.md](docs/TRANSFER_TRIE_SYNC.md)
- [docs/SERIAL_NUMBER_ARITHMETIC.md](docs/SERIAL_NUMBER_ARITHMETIC.md)
- [benchmarks/README.md](benchmarks/README.md)



