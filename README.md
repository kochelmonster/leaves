# leaves

leaves is a trie-based embedded key-value database.

## Key features

- Extremely fast, see [benchmarks](docs/performance/performance.md)
- Header-only C++20 core with a cursor-first API
- Cursor-based workflow for reading, writing, deleting, and iterating
- ACID transactions with Two-Phase Commit support
- Copy-on-write snapshots with lock-free readers
- Deterministic replication framework with a transport abstraction
- Extensible through Aspects
- Multi-database and multi-writer support
- Native cross-platform support, including WebAssembly and browser targets

## Getting started
```cpp
#include <leaves/mmap.hpp>

int main() {
    auto storage = leaves::MapStorage::create("mydata.lvs");
    auto db = storage->open("main");
    auto cursor = db.cursor();

    // write
    cursor.find(leaves::Slice("hello"));
    cursor.value(leaves::Slice("world"));
    cursor.commit();

    // read
    cursor.find(leaves::Slice("hello"));
    if (!cursor.is_valid()) {
        return 1;
    }
    leaves::Slice value = cursor.value();

    // scan
    for (cursor.first(); cursor.is_valid(); cursor.next()) {
        leaves::Slice key = cursor.key();
        leaves::Slice value = cursor.value();
    }

    return 0;
}
```

## Using leaves in your own project

To use Leaves, you need:

- A C++20 compiler
- Boost 1.80 or newer
- CMake 3.23 or newer when using the CMake integration or building the repository

Simply integrate it into your project using one of the following methods.

### Windows with vcpkg manifest mode

On Windows, Leaves supports vcpkg manifest mode out of the box through the repository `vcpkg.json`.

```powershell
git clone https://github.com/microsoft/vcpkg "$env:USERPROFILE\\vcpkg"
& "$env:USERPROFILE\\vcpkg\\bootstrap-vcpkg.bat"

cmake -S . -B build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\\vcpkg\\scripts\\buildsystems\\vcpkg.cmake" `
    -DVCPKG_MANIFEST_MODE=ON `
    -DVCPKG_TARGET_TRIPLET=x64-windows

cmake --build build -j4
ctest --test-dir build --output-on-failure
```

To build examples with the same dependency setup, pass the same toolchain and manifest flags when configuring an example directory.

### Include the headers directly

Add the `include/` directory to your project's include path.

```cmake
target_include_directories(mytarget PRIVATE /path/to/leaves/include)
```

If you use the optional replication API, also compile the bundled BLAKE3 sources from `BLAKE3/c/` and add that directory to your include path.

### Using `add_subdirectory`

Alternatively, add Leaves as a Git submodule and integrate it directly into your CMake project.

1. **Add the submodule**

   ```bash
   git submodule add https://github.com/kochelmonster/leaves.git extern/leaves
   ```

2. **Configure the repository targets**

   When embedding Leaves into another project, the repository's tests and benchmarks are typically not needed.

   ```cmake
   add_subdirectory(extern/leaves)
   ```

3. **Link the target**

   ```cmake
   target_link_libraries(mytarget PRIVATE leaves::leaves)
   ```


### Consuming as installed package

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


## Configuration options

The following CMake options configure either the repository build or library behavior for consumers.

| Option | Default | Scope | Description |
|---|---|---|---|
| `LEAVES_BROWSER_DEBUG` | `OFF` | Repository build only | Build browser WASM targets in debug mode with diagnostic output. |
| `LEAVES_ASYNC_BACKEND` | `JSPI` | Repository build only | Select the async WASM backend: `JSPI` or `ASYNCIFY`. |
| `LEAVES_BUILD_TESTS` | `ON` | Repository build only | Build the repository test executables. |
| `LEAVES_BUILD_BENCHMARKS` | `ON` | Repository build only | Build the repository benchmark applications. |
| `LEAVES_GCOV` | `ON` | Repository build only | Enable coverage instrumentation for repository builds. |
| `LEAVES_ASAN` | `ON` | Repository build only | Enable AddressSanitizer when coverage is disabled. |
| `LEAVES_SINGLE_PROCESS` | `OFF` | Repository build and library consumers | Disable multi-process support for constrained targets such as embedded or mobile environments. |
| `LEAVES_LOG` | `OFF` | Repository build and library consumers | Enable Leaves logging macros. |


## Installing as CMake Package

The repository can export an installable CMake package for downstream consumers. This step is optional and only required if you want to consume Leaves through `find_package()` in other projects.

```bash
cmake -B build -G Ninja 
cmake --build build -j
cmake --install build --prefix "$PWD/install"
```

The install tree always contains the exported `leaves::leaves` target. Replication users can additionally consume `leaves::replication`, which brings in the bundled `blake3` package. Vendored LevelDB and Google Benchmark artifacts are not installed as part of the Leaves package.

To create a redistributable archive from the current build tree:

```bash
cmake --build build --target package
```


## Building the Tests and Benchmarks

Building the repository is only required to run the included tests and benchmarks or to contribute to Leaves.

### Recommended local build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

For compiling in debug mode use:

```bash
cmake -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug

cmake --build build-debug -j
```

## Documentation index
- C++ API: [docs/cpp-api.md](docs/cpp-api.md)
- JavaScript/Browser API: [docs/js-api.md](docs/js-api.md)
- Architecture: [docs/architecture/architecture.md](docs/architecture/architecture.md)
- Replication: [docs/replication/replication.md](docs/replication/replication.md)
- Lessons learned: [docs/lessons-learned/lessons-learned.md](docs/lessons-learned/lessons-learned.md)
- Performance: [docs/performance/performance.md](docs/performance/performance.md)


## Future Extensions

### Set Findings
- Find all keys in a given range, returns a TransferTrie
- Find all keys suiting a FSM (e.g. regular expression), returns a TransferTrie

### Set Operations
- intersection of tries


## License
See [license.md](license.md) for the Leaves Community License 1.0.

Third-party components include their own license files (for example in BLAKE3). If you plan to redistribute Leaves, add or confirm project-level licensing metadata for your distribution workflow.
