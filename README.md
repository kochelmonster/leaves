# leaves

leaves is a trie-based embedded key-value database. It uses a sparse bitmap trie structure that brings the access advantages of a radix trie while saving memory by storing only necessary link pointers.

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

## Why Version 3.0.0?

Leaves began development in 2008 as an internal project and has gone through significant architectural evolution:

- **Version 1.x**: Classical C++ interface with virtual functions and classes
- **Version 2.x**: Complete rewrite as a template library to eliminate performance problems caused by double indirection on modern processors
- **Version 3.0**: A **complete reimplementation** that meets higher standards for a published library:
  - **Extensibility**: An extensible architecture enables creating extensions (e.g., ReplicationDB, ConfluenceDB) without modifying the core
  - **User Control**: Aspects give users precise influence over processing and behavior
  - **Easy Integration**: Header-only design with minimal external dependencies enables straightforward integration into projects
  - **Cross-Platform**: Native support for Windows, Linux, macOS, WebAssembly, and browser targets

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

## Which database type?

- `DB`
    The default embedded database. Use this when you want ACID transactions, a single-writer model, lock-free readers, and the smallest API surface.

- `ReplicationDB`
    Use this when you need the regular `DB` API plus deterministic replication. It tracks replicated deletions and is the database type used with `ReplicationSender` and `ReplicationReceiver`.

- `ConfluenceDB`
    Use this when multiple threads need to write concurrently to the same logical database. Each writer commits to its own tributary and Confluence merges those tributaries into the main database.

- `ConfluenceReplicationDB`
    Combines Confluence multi-writer behavior with replication support. Use it when you need both concurrent writers and replication in one database.

## Using leaves in your own project

To use Leaves, you need:

- A C++20 compiler
- Boost 1.80 or newer
- CMake 3.25 or newer for preset workflow builds (`cmake --workflow --preset ...`)
- CMake 3.22 or newer for manual configure/build invocations

Simply integrate it into your project using one of the following methods.

### Include the headers directly

Add the `include/` directory to your project's include path.

```cmake
target_include_directories(mytarget PRIVATE /path/to/leaves/include)
```

If you use the optional replication API, also compile the bundled BLAKE3 sources from `BLAKE3/c/` and add that directory to your include path.

On Windows with MSVC, replication builds may require the `/bigobj` compiler flag for some targets if you hit object-file size errors. In CMake, this can be enabled for an affected target with:

```cmake
target_compile_options(mytarget PRIVATE /bigobj)
```

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

### Optimization flags

Leaves does not require architecture-specific optimization flags for
correctness. It is header-only, so its performance depends on the optimization
settings of the target that includes it. For portable production binaries, use
your build system's normal Release configuration and target the oldest CPU that
the binary must support. CMake Release builds normally enable `-O3` for GCC and
Clang or `/O2` for MSVC.

For machine-local GCC or Clang builds, `-march=native` enables all instruction
sets supported by the build host. With MSVC, `/arch:AVX2` or `/arch:AVX512` can
be used when every deployment machine supports the selected instruction set.
These flags are optional and should not be used for redistributable binaries
unless the deployment CPU baseline guarantees them.

When Leaves is configured from its source tree, CMake probes compiler and host
support for POPCNT, BMI, LZCNT, AVX2, and AVX-512 on GCC and Clang, and for AVX2
and AVX-512 on MSVC. Supported flags apply within the Leaves CMake directory;
they are not exported as usage requirements by the `leaves::leaves` target.
Consumers must therefore choose the appropriate optimization and CPU target
flags for their own targets.

Leaves' header SIMD paths are selected at compile time and do not perform
runtime CPU dispatch. A binary compiled with `-march=native` or explicit AVX
flags can fail with an illegal-instruction error on an older CPU. Bundled BLAKE3
code performs its own runtime dispatch independently of Leaves' header code.

## Configuration options

The following CMake options configure either the repository build or library behavior for consumers.

| Option | Top-level default | Scope | Description |
|---|---|---|---|
| `LEAVES_BROWSER_DEBUG` | `OFF` | Repository build only | Build browser WASM targets in debug mode with diagnostic output. |
| `LEAVES_ASYNC_BACKEND` | `JSPI` | Repository build only | Select the async WASM backend: `JSPI` or `ASYNCIFY`. |
| `LEAVES_BUILD_TESTS` | `ON` | Repository build only | Build the repository test executables. |
| `LEAVES_BUILD_BENCHMARKS` | `ON` | Repository build only | Build the repository benchmark applications. |
| `LEAVES_GCOV` | `ON` | Repository build only | Enable coverage instrumentation for repository builds. |
| `LEAVES_ASAN` | `ON` | Repository build only | Enable AddressSanitizer when coverage is disabled. |
| `LEAVES_LOG` | `OFF` | Repository build and library consumers | Enable Leaves logging macros. |

## Installing as CMake Package

The repository can export an installable CMake package for downstream consumers. This step is optional and only required if you want to consume Leaves through `find_package()` in other projects.

```bash
cmake -B build -G Ninja 
cmake --build build -j4
cmake --install build --prefix "$PWD/install"
```

The install tree always contains the exported `leaves::leaves` target. Replication users can additionally consume `leaves::replication`, which brings in the bundled `blake3` package. Vendored LevelDB and Google Benchmark artifacts are not installed as part of the Leaves package.

To create a redistributable archive from the current build tree:

```bash
cmake --build build --target package
```

## Building the Tests and Benchmarks

Building the repository is only required to run the included tests and benchmarks or to contribute to Leaves.

### Default profile: tests in Debug, benchmarks in Release

Use the default workflow preset to configure once and build tests in Debug mode followed by benchmarks in Release mode.

Linux:

```bash
cmake --workflow --preset default
ctest --test-dir build-default -C Debug --output-on-failure
```

Windows:

```powershell
cmake --workflow --preset default-windows
ctest --test-dir build -C Debug --output-on-failure
```

The Linux default profile uses the `Ninja Multi-Config` generator to support Debug and Release builds from one configure step.

### Windows (out-of-box)

On Windows, the default configure path uses repository-local dependencies from `vcpkg_installed/x64-windows`.

```powershell
cmake --preset windows-vs18-x64 --fresh
cmake --build --preset windows-vs18-x64-debug -j
ctest --test-dir build -C Debug --output-on-failure
```

If your clone does not include populated local dependencies, you can still use external vcpkg manifest mode:

```powershell
git clone https://github.com/microsoft/vcpkg "$env:USERPROFILE\\vcpkg"
& "$env:USERPROFILE\\vcpkg\\bootstrap-vcpkg.bat"

cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\\vcpkg\\scripts\\buildsystems\\vcpkg.cmake" `
    -DVCPKG_MANIFEST_MODE=ON `
    -DVCPKG_TARGET_TRIPLET=x64-windows

cmake --build build --config Debug -j
ctest --test-dir build -C Debug --output-on-failure
```

To build examples with the same dependency setup, pass the same dependency arguments when configuring an example directory.

### Recommended local build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

For compiling in debug mode use:

```bash
cmake -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug

cmake --build build-debug -j4
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

See [LICENSE.md](LICENSE.md) for the Leaves Community License 1.0.

Third-party components include their own license files (for example in BLAKE3). If you plan to redistribute Leaves, add or confirm project-level licensing metadata for your distribution workflow.
