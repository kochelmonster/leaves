cmake_minimum_required(VERSION 3.9)
cmake_policy(SET CMP0076 NEW)

project(leaves VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# ── Emscripten (WebAssembly) ───────────────────────────────────
if(EMSCRIPTEN)
    message(STATUS "Building for WebAssembly — only _BrowserStore")

    set(BLAKE3_SIMD_TYPE "none" CACHE STRING "" FORCE)
    add_subdirectory("BLAKE3/c")

    add_library(wasm_core INTERFACE)
    target_sources(wasm_core INTERFACE
        include/leaves/intern/core/_util.hpp
        include/leaves/intern/core/_exception.hpp
        include/leaves/intern/core/_bits.hpp
        include/leaves/intern/memory/_memory.hpp
        include/leaves/intern/memory/_twoquecache.hpp
        include/leaves/intern/core/_node.hpp
        include/leaves/intern/db/_cursor.hpp
        include/leaves/intern/db/_inserter.hpp
        include/leaves/intern/db/_db.hpp
        include/leaves/intern/storage/_cachestore.hpp
        include/leaves/intern/storage/_browserstore.hpp
    )
    target_compile_definitions(wasm_core INTERFACE LEAVES_SINGLE_PROCESS)
    target_include_directories(wasm_core INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/BLAKE3/c
    )
    target_compile_options(wasm_core INTERFACE -fwasm-exceptions)
    target_link_options(wasm_core INTERFACE
        -fwasm-exceptions
        -sASYNCIFY
        "-sASYNCIFY_IMPORTS=['emscripten_idb_load','emscripten_idb_store','emscripten_idb_delete','emscripten_idb_exists','emscripten_sleep']"
        -sALLOW_MEMORY_GROWTH=1
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','callMain']"
        -sENVIRONMENT=web,node
    )

    add_executable(test_browserstore tests/test_browserstore.cpp)
    target_link_libraries(test_browserstore PRIVATE wasm_core blake3)
    target_compile_definitions(test_browserstore PRIVATE TESTING)

    add_executable(db_bench_browser benchmarks/db_bench_browser.cpp)
    target_link_libraries(db_bench_browser PRIVATE wasm_core blake3)

    # Copy WASM artifacts to benchmarks/ for browser serving
    add_custom_command(TARGET db_bench_browser POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:db_bench_browser>
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/db_bench_browser.js
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE_DIR:db_bench_browser>/db_bench_browser.wasm
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/db_bench_browser.wasm
        COMMENT "Copying benchmark WASM to benchmarks/"
    )

    return()   # stop here; nothing below applies to WASM
endif()

# ── Native build ───────────────────────────────────────────────
find_package(Boost 1.80 REQUIRED)
if (TARGET Boost::process)
    set(HAVE_BOOST_PROCESS TRUE)
else()
    set(HAVE_BOOST_PROCESS FALSE)
endif()

if(Boost_FOUND)
    message(STATUS "Found Boost version ${Boost_VERSION}")
    include_directories(${Boost_INCLUDE_DIRS})
else()
    message(FATAL_ERROR "Boost >= 1.80 not found")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

option(LEAVES_GCOV "Add gconv support for tests" ON)
option(LEAVES_ASAN "Build tests with AddressSanitizer" ON)
option(LEAVES_BUILD_TESTS "Build leaves' unit tests" ON)
option(LEAVES_BUILD_BENCHMARKS "Build leaves' benchmarks" ON)
option(LEAVES_SINGLE_PROCESS "Disable multi-process support (for mobile/embedded)" OFF)


include(CheckCXXCompilerFlag)
include(CheckCXXSourceRuns)

# Helper: check that the compiler supports a flag AND the CPU can execute the instructions.
# check_cxx_compiler_flag only tests compilation, not runtime — using it alone causes
# SIGILL ("Illegal instruction") on CPUs that lack the feature (e.g. AVX-512 on Alder Lake).
macro(check_cpu_feature FLAG TEST_SOURCE RESULT_VAR)
    check_cxx_compiler_flag("${FLAG}" _compiler_has_${RESULT_VAR})
    if (_compiler_has_${RESULT_VAR})
        set(CMAKE_REQUIRED_FLAGS "${FLAG}")
        check_cxx_source_runs("${TEST_SOURCE}" ${RESULT_VAR})
        unset(CMAKE_REQUIRED_FLAGS)
    else()
        set(${RESULT_VAR} FALSE)
    endif()
    if (${RESULT_VAR})
        message(STATUS "CPU feature ${FLAG}: supported")
    else()
        message(STATUS "CPU feature ${FLAG}: NOT supported")
    endif()
endmacro()

check_cpu_feature("-mpopcnt"
    "#include <nmmintrin.h>\nint main() { return _mm_popcnt_u64(0x1234) != 5; }"
    CPU_SUPPORTS_POPCNT)
if (CPU_SUPPORTS_POPCNT)
    add_compile_options(-mpopcnt)
endif()

check_cpu_feature("-mbmi"
    "#include <immintrin.h>\nint main() { return _tzcnt_u64(8) != 3; }"
    CPU_SUPPORTS_BMI)
if (CPU_SUPPORTS_BMI)
    add_compile_options(-mbmi)
endif()

check_cpu_feature("-mlzcnt"
    "#include <immintrin.h>\nint main() { return _lzcnt_u64(1) != 63; }"
    CPU_SUPPORTS_LZCNT)
if (CPU_SUPPORTS_LZCNT)
    add_compile_options(-mlzcnt)
endif()

# Check for AVX2 support (32-byte SIMD operations)
check_cpu_feature("-mavx2"
    "#include <immintrin.h>\nint main() { __m256i v = _mm256_set1_epi32(1); return _mm256_extract_epi32(v,0) != 1; }"
    CPU_SUPPORTS_AVX2)
if (CPU_SUPPORTS_AVX2)
    add_compile_options(-mavx2)
endif()

# Check for AVX-512 support (64-byte SIMD operations)
check_cpu_feature("-mavx512bw"
    "#include <immintrin.h>\nint main() { __m512i v = _mm512_set1_epi8(1); return (int)_mm512_reduce_add_epi32(_mm512_setzero_si512()); }"
    CPU_SUPPORTS_AVX512BW)
if (CPU_SUPPORTS_AVX512BW)
    add_compile_options(-mavx512bw)
endif()


add_subdirectory("BLAKE3/c")

add_library(core INTERFACE)
target_sources(core INTERFACE 
    # include/leaves.hpp
    include/leaves/intern/core/_util.hpp
    include/leaves/intern/core/_exception.hpp
    include/leaves/intern/core/_bits.hpp
    include/leaves/intern/memory/_memory.hpp
    include/leaves/intern/storage/_mmap.hpp
    include/leaves/intern/db/_cursor.hpp
    include/leaves/intern/core/_node.hpp
    include/leaves/intern/db/_inserter.hpp
    include/leaves/intern/storage/_fstore.hpp
    include/leaves/intern/memory/_twoquecache.hpp
)

if (LEAVES_SINGLE_PROCESS)
    target_compile_definitions(core INTERFACE LEAVES_SINGLE_PROCESS)
elseif (HAVE_BOOST_PROCESS)
    target_link_libraries(core INTERFACE Boost::process)
endif()

target_include_directories(core INTERFACE ${Boost_INCLUDE_DIRS})
target_include_directories(core
                           INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           )
target_include_directories(core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/BLAKE3/c)

if (LEAVES_BUILD_TESTS)

add_library(test INTERFACE)

target_include_directories(test
                           INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           )
target_include_directories(test INTERFACE src)
target_link_libraries(test INTERFACE core)
target_compile_options(test INTERFACE -g)
target_compile_definitions(test INTERFACE 
    DEBUG 
    TESTING
    CMPFILES="${CMAKE_CURRENT_SOURCE_DIR}/tests/cmpfiles/") 
target_sources(test INTERFACE
    tests/test.hpp)

if (LEAVES_GCOV)
target_compile_options(test INTERFACE -g --coverage)
target_compile_definitions(test INTERFACE DEBUG GCOV_ENABLED)
target_link_options(test INTERFACE --coverage)
else (LEAVES_GCOV)
target_compile_options(test INTERFACE -g)
target_compile_definitions(test INTERFACE DEBUG)
endif (LEAVES_GCOV)

if (LEAVES_ASAN AND NOT LEAVES_GCOV)
target_compile_options(test INTERFACE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(test INTERFACE -fsanitize=address)
endif (LEAVES_ASAN AND NOT LEAVES_GCOV)

add_executable(test_mmap tests/test_mmap.cpp)
target_sources(test_mmap INTERFACE
    include/intern/_mmap.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_mmap PUBLIC test)

add_executable(test_transfer tests/test_transfer.cpp)
target_link_libraries(test_transfer PUBLIC core test blake3)

add_executable(test_replication_fsm tests/test_replication_fsm.cpp)
target_link_libraries(test_replication_fsm PUBLIC core test blake3)

add_executable(test_fstore tests/test_fstore.cpp)
target_sources(test_fstore INTERFACE
    include/intern/_fstore.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_fstore PUBLIC test)

add_executable(test_memory tests/test_memory.cpp)
target_sources(test_memory INTERFACE
    include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_memory PUBLIC test)

add_executable(test_memstore tests/test_memstore.cpp)
target_sources(test_memstore INTERFACE
    include/leaves/intern/_memstore.hpp include/leaves/intern/_memory.hpp)
target_link_libraries(test_memstore PUBLIC core test)

add_executable(test_bits tests/test_bits.cpp)
target_sources(test_memory INTERFACE
    include/intern/_bits.hpp)
target_link_libraries(test_bits PUBLIC test)

add_executable(test_cursor tests/test_cursor.cpp)
target_link_libraries(test_cursor PUBLIC core test)

add_executable(test_node tests/test_node.cpp)
target_link_libraries(test_node PUBLIC core test)

add_executable(test_db tests/test_db.cpp)
target_link_libraries(test_db PUBLIC core test)

add_executable(test_bigmemory tests/test_bigmemory.cpp)
target_link_libraries(test_bigmemory PUBLIC core test)

add_executable(test_map_storage tests/test_map_storage.cpp)
target_sources(test_map_storage INTERFACE
    include/mmap.hpp include/intern/_mmap.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_map_storage PUBLIC core test)

add_executable(test_file_storage tests/test_file_storage.cpp)
target_sources(test_file_storage INTERFACE
    include/fstore.hpp include/intern/_fstore.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_file_storage PUBLIC core test)

add_executable(test_string_insert tests/test_string_insert.cpp)

add_executable(test_merger tests/test_merger.cpp)
target_link_libraries(test_merger PUBLIC core test)

add_executable(test_hash_updater tests/test_hash_updater.cpp)
target_link_libraries(test_hash_updater PUBLIC core test blake3)

add_executable(test_hash_lookup tests/test_hash_lookup.cpp)
target_link_libraries(test_hash_lookup PUBLIC core test blake3)

add_executable(test_serial tests/test_serial.cpp)
target_link_libraries(test_serial PUBLIC test)

add_executable(test_twoquecache tests/test_twoquecache.cpp)
target_link_libraries(test_twoquecache PUBLIC test)

target_link_libraries(test_file_storage PUBLIC core test)

add_executable(test_multithread tests/test_multithread.cpp)
target_link_libraries(test_multithread PUBLIC core test pthread)

add_executable(test_executor tests/test_executor.cpp)
target_link_libraries(test_executor PUBLIC core test pthread)

add_executable(test_aspect tests/test_aspect.cpp)
target_link_libraries(test_aspect PUBLIC core test blake3)

add_executable(profile tests/profile.cpp)

# Dumper tool
add_executable(dump-leaves tools/dump-leaves.cpp)
target_link_libraries(dump-leaves PUBLIC core)

# YCSB debug tool
add_executable(ycsb-debug tests/ycsb-bug/ycsb-debug.cpp)
target_link_libraries(ycsb-debug PUBLIC core)

add_custom_target(cov
COMMAND lcov -d . --zerocounters
COMMAND rm -rf lcov-report
COMMAND mkdir lcov-report
COMMAND ./test_twoquecache
COMMAND ./test_cursor
COMMAND ./test_node
COMMAND ./test_memory
COMMAND ./test_bits
COMMAND ./test_mmap
COMMAND ./test_db
COMMAND ./test_bigmemory
COMMAND ./test_string_insert
COMMAND ./test_multithread
COMMAND ./test_merger
COMMAND ./test_serial
COMMAND ./test_transfer
COMMAND ./test_replication_fsm
COMMAND ./test_hash_lookup
COMMAND ./test_hash_updater
COMMAND ./test_fstore
COMMAND ./test_memstore
COMMAND ./test_map_storage
COMMAND ./test_file_storage
COMMAND ./test_aspect
COMMAND lcov -c -d . -o lcov-report/report.info -b . --ignore-errors mismatch,negative --include '${CMAKE_CURRENT_SOURCE_DIR}/include/*' --exclude '*/third_party/*'
COMMAND genhtml -o lcov-report lcov-report/report.info
COMMENT &quot;Generated Coverage Report Successfully!&quot;
DEPENDS test_cursor test_memory test_bits test_mmap test_db test_node test_bigmemory test_string_insert test_multithread test_merger test_serial test_twoquecache test_transfer test_replication_fsm test_hash_lookup test_hash_updater test_fstore test_memstore test_map_storage test_file_storage test_aspect
)

endif(LEAVES_BUILD_TESTS)

if (LEAVES_BUILD_BENCHMARKS) 
add_subdirectory("leveldb")

set(DEBUG_BENCHMARKS OFF CACHE BOOL "Build benchmarks with debug info")

add_library(lbench INTERFACE)
target_include_directories(lbench INTERFACE leveldb)

target_sources(lbench INTERFACE 
    "leveldb/util/histogram.cc"
    "leveldb/util/histogram.h"
    "leveldb/util/testutil.cc"
    "leveldb/util/testutil.h")

add_executable(db_bench_leaves benchmarks/db_bench_leaves.cpp)
target_link_libraries(db_bench_leaves PUBLIC core lbench gmock gtest    
    benchmark leveldb pthread core blake3)

add_executable(db_bench_mdb benchmarks/db_bench_mdb.cpp)
target_link_libraries(db_bench_mdb PUBLIC core lbench gmock gtest 
    benchmark leveldb lmdb pthread)
target_compile_options(db_bench_mdb PRIVATE -O3 -march=native -funroll-loops -flto=auto)
target_compile_definitions(db_bench_mdb PRIVATE NDEBUG)

add_executable(db_bench_memory benchmarks/db_bench_memory.cpp)
target_link_libraries(db_bench_memory PUBLIC core lbench gmock gtest 
    benchmark leveldb pthread)

add_executable(bench_hash_updater benchmarks/bench_hash_updater.cpp)
target_link_libraries(bench_hash_updater PUBLIC core pthread blake3)

add_executable(bench_merger benchmarks/bench_merger.cpp)
target_link_libraries(bench_merger PUBLIC core pthread)

if (DEBUG_BENCHMARKS)
    target_compile_options(db_bench_leaves PRIVATE -g -O0)
    target_compile_definitions(db_bench_leaves PRIVATE DEBUG)
    target_compile_options(db_bench_memory PUBLIC  -g -O0)
    target_compile_definitions(db_bench_memory PRIVATE DEBUG)
    target_compile_options(bench_hash_updater PRIVATE -g -O0)
    target_compile_definitions(bench_hash_updater PRIVATE DEBUG)
    target_compile_options(bench_merger PRIVATE -g -O0)
    target_compile_definitions(bench_merger PRIVATE DEBUG)
else()
    target_compile_options(db_bench_leaves PRIVATE -O3 -march=native -funroll-loops -flto=auto)
    target_compile_definitions(db_bench_leaves PRIVATE NDEBUG)
    target_compile_options(db_bench_memory PUBLIC -march=native -O3 -g)
    target_compile_definitions(db_bench_memory PRIVATE NDEBUG)
    target_compile_options(bench_hash_updater PRIVATE -O3 -march=native -funroll-loops -flto=auto)
    target_compile_definitions(bench_hash_updater PRIVATE NDEBUG)
    target_compile_options(bench_merger PRIVATE -O3 -march=native -funroll-loops -flto=auto)
    target_compile_definitions(bench_merger PRIVATE NDEBUG)
endif()


# Check for RocksDB and add benchmark if available
find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h)
if(ROCKSDB_INCLUDE_DIR)
    find_library(ROCKSDB_LIBRARY rocksdb)
    if(ROCKSDB_LIBRARY)
        message(STATUS "Found RocksDB: ${ROCKSDB_LIBRARY}")
        
        # Build histogram utility separately for db_bench_rocksdb
        add_library(rocksdb_histogram OBJECT leveldb/util/histogram.cc)
        target_include_directories(rocksdb_histogram PRIVATE 
            ${CMAKE_CURRENT_SOURCE_DIR}/leveldb/include 
            ${CMAKE_CURRENT_SOURCE_DIR}/leveldb)
        
        add_executable(db_bench_rocksdb benchmarks/db_bench_rocksdb.cpp)
        target_compile_definitions(db_bench_rocksdb PUBLIC NDEBUG)
        target_link_libraries(db_bench_rocksdb PUBLIC ${ROCKSDB_LIBRARY} rocksdb_histogram pthread)
        target_compile_options(db_bench_rocksdb PUBLIC -march=native -O3 -g -flto=auto -funroll-loops -ffast-math)
        target_include_directories(db_bench_rocksdb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/leveldb)
    else()
        message(STATUS "RocksDB headers found but library not found")
    endif()
else()
    message(STATUS "RocksDB not found - skipping db_bench_rocksdb")
endif()


endif (LEAVES_BUILD_BENCHMARKS) 