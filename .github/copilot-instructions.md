# Copilot Instructions

## Cross-Platform Requirements
- All code must be cross-platform and cross-compiler compatible (GCC, Clang, MSVC)
- Use the `LEAVES_X86_64`, `LEAVES_ARM64` macros defined in `_util.hpp` for platform detection
- Use `_MSC_VER` intrinsics alongside GCC/Clang builtins where needed
- Never use compiler-specific builtins without providing alternatives for other compilers

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

# Finishing Signal
if an agent finishes a task, it should call the bash command: spd-say "I've finished with leaves"