# Copilot Instructions

## Cross-Platform Requirements
- All code must be cross-platform and cross-compiler compatible (GCC, Clang, MSVC)
- Use the `LEAVES_X86_64`, `LEAVES_ARM64` macros defined in `_util.hpp` for platform detection
- Use `_MSC_VER` intrinsics alongside GCC/Clang builtins where needed
- Never use compiler-specific builtins without providing alternatives for other compilers
- cross-platform code shall be separated into functions in _port.hpp.
- _BrowserStore will only compile with emscripten

## Project Conventions
- C++ header-only trie database with CMake + Ninja build system
- Build directory: `build/`
- Test framework: Boost.Test with `BOOST_TEST_DYN_LINK`
- Storage backends must work with both in-process memory and mmap (shared memory)
- Synchronization primitives in shared memory must use only hardware atomics (no kernel state)
- the project is new you don't have to worry about backwards compatibility.

## Coding Conventions
inside include/leaves/intern folder:
  - use structs not classes the struct names shall begin with an underscore _
  - private members begin with an underscore _ and public members shall not begin with an underscore
  - no public, private, proteced access modifiers, use naming conventions to indicate visibility

## Windows Specifics

leaves shall be usable as library on windows ands compilable with msvc.
On the windows2 branch the source of truth is origin/develop. The behaviour of the windows2 branch shall be equivalent to the behaviour of the develop branch on linux. The windows2 branch is a temporary branch to get the project to compile on windows. Once it compiles and passes all tests, the changes will be merged back into develop. Leaves shall work with Visual Studio 2022 (VS 18 / MSVC 19.51) or higher.

Leaves shall be compilable by its own like described in the README.md file. It shall also be compilable as a submodule of another project. The CMakeLists.txt file shall be compatible with CMake 3.23 or higher. 

When compiled by its own all tests, benchmarks and examples shall work. 
In summary it shall be equally functional on windows as it is on linux but in a windows specific way. 

The files created in windows shall be stored in a build directory.

It is intentional that in _MemoryMapFile the mmap region is bigger than the file size. Do not alter the code to make the file size equal to the region size
