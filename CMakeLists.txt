cmake_minimum_required(VERSION 3.9)
cmake_policy(SET CMP0076 NEW)

project(leaves VERSION 3.0.0 LANGUAGES C CXX)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/leaves/version.hpp"
    @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Emscripten (WebAssembly) ───────────────────────────────────
option(LEAVES_BROWSER_DEBUG "Build browser WASM targets in debug mode (with diagnostic output)" OFF)

# ── Async backend: JSPI (default) or ASYNCIFY ─────────────────
set(LEAVES_ASYNC_BACKEND "JSPI" CACHE STRING "Async WASM backend: JSPI or ASYNCIFY")
set_property(CACHE LEAVES_ASYNC_BACKEND PROPERTY STRINGS "JSPI" "ASYNCIFY")
set(LEAVES_WASM_RUNTIME_EXPORTS "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','setValue','getValue','HEAPU8']")

if(LEAVES_ASYNC_BACKEND STREQUAL "JSPI")
    set(LEAVES_ASYNC_FLAG "-sJSPI")
    set(LEAVES_ASYNC_IMPORTS "-sJSPI_IMPORTS=['emscripten_idb_load','emscripten_idb_store','emscripten_idb_delete','emscripten_idb_exists','emscripten_sleep']")
    set(LEAVES_WASM_EXCEPTIONS "-fwasm-exceptions")
else()
    set(LEAVES_ASYNC_FLAG "-sASYNCIFY")
    set(LEAVES_ASYNC_IMPORTS "-sASYNCIFY_IMPORTS=['emscripten_idb_load','emscripten_idb_store','emscripten_idb_delete','emscripten_idb_exists','emscripten_sleep']")
    set(LEAVES_WASM_EXCEPTIONS "-fexceptions")
endif()

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
        include/leaves/intern/replication/_replication_protocol.hpp
        include/leaves/intern/replication/_replication_slot.hpp
        include/leaves/intern/replication/_big_value_receiver.hpp
        include/leaves/intern/replication/_hash.hpp
        include/leaves/intern/replication/_replication_db.hpp
        include/leaves/intern/replication/_replication_fsm.hpp
        include/leaves/intern/replication/_transfer.hpp
        include/leaves/intern/util/_threadpool.hpp
        include/leaves/intern/util/_transfer_trie.hpp
        include/leaves/replication.hpp
        include/leaves/db.hpp
        include/leaves/cursor.hpp
    )
    target_compile_definitions(wasm_core INTERFACE LEAVES_SINGLE_PROCESS)
    if(LEAVES_ASYNC_BACKEND STREQUAL "ASYNCIFY")
        target_compile_definitions(wasm_core INTERFACE LEAVES_ASYNCIFY)
    endif()
    target_include_directories(wasm_core INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/BLAKE3/c
    )

    if(LEAVES_BROWSER_DEBUG)
        message(STATUS "Browser WASM: debug mode (diagnostic output enabled)")
        target_compile_definitions(wasm_core INTERFACE DEBUG)
        target_compile_options(wasm_core INTERFACE ${LEAVES_WASM_EXCEPTIONS} -g -O0)
        target_link_options(wasm_core INTERFACE
            ${LEAVES_WASM_EXCEPTIONS}
            --profiling-funcs
            ${LEAVES_ASYNC_FLAG}
            ${LEAVES_ASYNC_IMPORTS}
            -sALLOW_MEMORY_GROWTH=1
            ${LEAVES_WASM_RUNTIME_EXPORTS}
            -sENVIRONMENT=web,node
        )
    else()
        message(STATUS "Browser WASM: release mode (diagnostic output suppressed)")
        target_compile_definitions(wasm_core INTERFACE NDEBUG)
        target_compile_options(wasm_core INTERFACE ${LEAVES_WASM_EXCEPTIONS} -O3)
        target_link_options(wasm_core INTERFACE
            ${LEAVES_WASM_EXCEPTIONS}
            ${LEAVES_ASYNC_FLAG}
            ${LEAVES_ASYNC_IMPORTS}
            -sALLOW_MEMORY_GROWTH=1
            ${LEAVES_WASM_RUNTIME_EXPORTS}
            -sENVIRONMENT=web,node
        )
    endif()
    # ── Leaves JS library (embind) ──────────────────────────────
    add_executable(leaves js/leaves_embind.cpp)
    target_link_libraries(leaves PRIVATE wasm_core blake3)
    target_link_options(leaves PRIVATE
        -lembind
        --no-entry
        ${LEAVES_ASYNC_FLAG}
        ${LEAVES_ASYNC_IMPORTS}
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        ${LEAVES_WASM_RUNTIME_EXPORTS}
    )
    set_target_properties(leaves PROPERTIES OUTPUT_NAME "leaves")

    add_custom_command(
        OUTPUT
            ${CMAKE_CURRENT_SOURCE_DIR}/js/leaves.js
            ${CMAKE_CURRENT_SOURCE_DIR}/js/leaves.wasm
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:leaves>
            ${CMAKE_CURRENT_SOURCE_DIR}/js/leaves.js
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE_DIR:leaves>/leaves.wasm
            ${CMAKE_CURRENT_SOURCE_DIR}/js/leaves.wasm
        DEPENDS leaves
        COMMENT "Copying leaves library WASM to js/"
    )

    add_custom_target(leaves_js_output ALL
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/js/leaves.js
            ${CMAKE_CURRENT_SOURCE_DIR}/js/leaves.wasm
    )

    # ── Browser test target ────────────────────────────────────
    # Build all browser WASM targets in debug mode:
    #   cmake -B build-wasm-debug -G Ninja -DLEAVES_BROWSER_DEBUG=ON
    #   cmake --build build-wasm-debug --target browser_test
    if(LEAVES_BROWSER_DEBUG)
        add_custom_target(browser_test ALL
            DEPENDS
                leaves_js_output
            COMMENT "Building all browser WASM targets in debug mode"
        )
    else()
        add_custom_target(browser_test
            COMMAND ${CMAKE_COMMAND} -E echo
                "To build browser WASM in debug mode, reconfigure with -DLEAVES_BROWSER_DEBUG=ON"
            COMMENT "browser_test requires LEAVES_BROWSER_DEBUG=ON"
        )
    endif()

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

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(LEAVES_REPOSITORY_OPTION_DEFAULT ON)
else()
    set(LEAVES_REPOSITORY_OPTION_DEFAULT OFF)
endif()

option(LEAVES_GCOV "Add gconv support for tests" ${LEAVES_REPOSITORY_OPTION_DEFAULT})
option(LEAVES_ASAN "Build tests with AddressSanitizer" ${LEAVES_REPOSITORY_OPTION_DEFAULT})
option(LEAVES_BUILD_TESTS "Build leaves' unit tests" ${LEAVES_REPOSITORY_OPTION_DEFAULT})
option(LEAVES_BUILD_BENCHMARKS "Build leaves' benchmarks" ${LEAVES_REPOSITORY_OPTION_DEFAULT})
option(LEAVES_SINGLE_PROCESS "Disable multi-process support (for mobile/embedded)" OFF)
option(LEAVES_LOG "Enable leaves logging macros for consumers" OFF)


include(CheckCXXCompilerFlag)
include(CheckCXXSourceRuns)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)

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

if (NOT MSVC)
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
endif()


add_subdirectory("BLAKE3/c")

add_library(leaves INTERFACE)
add_library(leaves::leaves ALIAS leaves)
add_library(core ALIAS leaves)
add_library(leaves_replication INTERFACE)
add_library(leaves::replication ALIAS leaves_replication)
set_target_properties(leaves_replication PROPERTIES EXPORT_NAME replication)

target_sources(leaves INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/mmap.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/fstore.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/db.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/cursor.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/confluence.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/core/_util.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/core/_exception.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/core/_bits.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/memory/_memory.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/storage/_mmap.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/db/_cursor.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/core/_node.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/db/_inserter.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/storage/_fstore.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/intern/memory/_twoquecache.hpp>
)

if (TARGET Boost::headers)
    target_link_libraries(leaves INTERFACE Boost::headers)
elseif (TARGET Boost::boost)
    target_link_libraries(leaves INTERFACE Boost::boost)
else()
    target_include_directories(leaves INTERFACE ${Boost_INCLUDE_DIRS})
endif()

if (TARGET Boost::process)
    target_link_libraries(leaves INTERFACE Boost::process)
endif()

if (LEAVES_SINGLE_PROCESS)
    target_compile_definitions(leaves INTERFACE LEAVES_SINGLE_PROCESS)
endif()

if (LEAVES_LOG)
    target_compile_definitions(leaves INTERFACE LEAVES_LOG)
endif()

target_include_directories(leaves
                           INTERFACE
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
                           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_sources(leaves_replication INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/leaves/replication.hpp>)
target_link_libraries(leaves_replication INTERFACE leaves BLAKE3::blake3)

if (LEAVES_BUILD_TESTS)

function(leaves_add_test target)
    set(options)
    set(oneValueArgs TIMEOUT)
    set(multiValueArgs LABELS)
    cmake_parse_arguments(LEAVES_TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT LEAVES_TEST_TIMEOUT)
        set(LEAVES_TEST_TIMEOUT 240)
    endif()

    if (NOT LEAVES_TEST_LABELS)
        set(LEAVES_TEST_LABELS native unit)
    endif()

    add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
    set_tests_properties(${target} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        TIMEOUT ${LEAVES_TEST_TIMEOUT}
        LABELS "${LEAVES_TEST_LABELS}")
endfunction()

add_library(leaves_test_support INTERFACE)

target_include_directories(leaves_test_support
                           INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           )
target_include_directories(leaves_test_support INTERFACE src)
target_link_libraries(leaves_test_support INTERFACE core)
if (MSVC)
    target_compile_options(leaves_test_support INTERFACE /Zi)
else()
    target_compile_options(leaves_test_support INTERFACE -g -O0)
endif()
target_compile_definitions(leaves_test_support INTERFACE 
    DEBUG 
    TESTING
    CMPFILES="${CMAKE_CURRENT_SOURCE_DIR}/tests/cmpfiles/") 
if (LEAVES_LOG) 
    target_compile_definitions(leaves_test_support INTERFACE LEAVES_LOG)
endif()

target_sources(leaves_test_support INTERFACE tests/test.hpp)

if (LEAVES_GCOV)
if (NOT MSVC)
target_compile_options(leaves_test_support INTERFACE -g --coverage)
target_compile_definitions(leaves_test_support INTERFACE DEBUG GCOV_ENABLED)
target_link_options(leaves_test_support INTERFACE --coverage)
endif()
else (LEAVES_GCOV)
if (MSVC)
target_compile_options(leaves_test_support INTERFACE /Zi)
else()
target_compile_options(leaves_test_support INTERFACE -g)
endif()
target_compile_definitions(leaves_test_support INTERFACE DEBUG)
endif (LEAVES_GCOV)

if (LEAVES_ASAN AND NOT LEAVES_GCOV)
if (NOT MSVC)
target_compile_options(leaves_test_support INTERFACE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(leaves_test_support INTERFACE -fsanitize=address)
endif()
endif (LEAVES_ASAN AND NOT LEAVES_GCOV)

add_executable(test_mmap tests/test_mmap.cpp)
target_sources(test_mmap INTERFACE
    include/intern/_mmap.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_mmap PUBLIC leaves_test_support blake3)

add_executable(test_transfer tests/test_transfer.cpp)
target_link_libraries(test_transfer PUBLIC core leaves_test_support blake3)

add_executable(test_replication_fsm tests/test_replication_fsm.cpp)
target_link_libraries(test_replication_fsm PUBLIC core leaves_test_support blake3)

add_executable(test_replication_api tests/test_replication_api.cpp)
target_link_libraries(test_replication_api PUBLIC core leaves_test_support blake3)

add_executable(test_replication_multiclient tests/test_replication_multiclient.cpp)
target_link_libraries(test_replication_multiclient PUBLIC core leaves_test_support blake3)

add_executable(ws_replication_server tests/ws_replication/server.cpp)
target_link_libraries(ws_replication_server PUBLIC core blake3 Threads::Threads)

add_executable(test_fstore tests/test_fstore.cpp)
target_sources(test_fstore INTERFACE
    include/intern/_fstore.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_fstore PUBLIC leaves_test_support blake3)

add_executable(test_memory tests/test_memory.cpp)
target_sources(test_memory INTERFACE
    include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_memory PUBLIC leaves_test_support)

add_executable(test_memstore tests/test_memstore.cpp)
target_sources(test_memstore INTERFACE
    include/leaves/intern/_memstore.hpp include/leaves/intern/_memory.hpp)
target_link_libraries(test_memstore PUBLIC core leaves_test_support)

add_executable(test_bits tests/test_bits.cpp)
target_sources(test_memory INTERFACE
    include/intern/_bits.hpp)
target_link_libraries(test_bits PUBLIC leaves_test_support)

add_executable(test_cursor tests/test_cursor.cpp)
target_link_libraries(test_cursor PUBLIC core leaves_test_support)

add_executable(test_node tests/test_node.cpp)
target_link_libraries(test_node PUBLIC core leaves_test_support)

add_executable(test_db tests/test_db.cpp)
target_link_libraries(test_db PUBLIC core leaves_test_support blake3)

add_executable(test_wal tests/test_wal.cpp)
target_link_libraries(test_wal PUBLIC core leaves_test_support blake3)

add_executable(test_bigmemory tests/test_bigmemory.cpp)
target_link_libraries(test_bigmemory PUBLIC core leaves_test_support)

add_executable(test_map_storage tests/test_map_storage.cpp)
target_sources(test_map_storage INTERFACE
    include/mmap.hpp include/intern/_mmap.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_map_storage PUBLIC core leaves_test_support)

add_executable(test_file_storage tests/test_file_storage.cpp)
target_sources(test_file_storage INTERFACE
    include/fstore.hpp include/intern/_fstore.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_file_storage PUBLIC core leaves_test_support blake3)

add_executable(test_string_insert tests/test_string_insert.cpp)

add_executable(test_merger tests/test_merger.cpp)
target_link_libraries(test_merger PUBLIC core leaves_test_support)

add_executable(test_tributary tests/test_tributary.cpp)
target_link_libraries(test_tributary PUBLIC core leaves_test_support)

add_executable(test_multiprocess tests/test_multiprocess.cpp)
target_link_libraries(test_multiprocess PUBLIC core leaves_test_support Threads::Threads)


add_executable(test_hash_updater tests/test_hash_updater.cpp)
target_link_libraries(test_hash_updater PUBLIC core leaves_test_support blake3)

add_executable(test_hash_lookup tests/test_hash_lookup.cpp)
target_link_libraries(test_hash_lookup PUBLIC core leaves_test_support blake3)

add_executable(test_serial tests/test_serial.cpp)
target_link_libraries(test_serial PUBLIC leaves_test_support)

add_executable(test_twoquecache tests/test_twoquecache.cpp)
target_link_libraries(test_twoquecache PUBLIC leaves_test_support)

add_executable(test_multithread tests/test_multithread.cpp)
target_link_libraries(test_multithread PUBLIC core leaves_test_support Threads::Threads)

add_executable(test_executor tests/test_executor.cpp)
target_link_libraries(test_executor PUBLIC core leaves_test_support Threads::Threads)

add_executable(test_aspect tests/test_aspect.cpp)
target_link_libraries(test_aspect PUBLIC core leaves_test_support blake3)

add_executable(test_metrics_aspects tests/test_metrics_aspects.cpp)
target_link_libraries(test_metrics_aspects PUBLIC core leaves_test_support)

foreach(target IN ITEMS
    test_mmap
    test_fstore
    test_memory
    test_memstore
    test_bits
    test_cursor
    test_node
    test_db
    test_wal
    test_bigmemory
    test_map_storage
    test_file_storage
    test_string_insert
    test_merger
    test_hash_updater
    test_hash_lookup
    test_serial
    test_twoquecache
    test_aspect
    test_metrics_aspects)
    leaves_add_test(${target} LABELS "native;unit")
endforeach()

foreach(target IN ITEMS
    test_transfer
    test_replication_fsm
    test_replication_api
    test_tributary
    test_executor)
    leaves_add_test(${target} TIMEOUT 60 LABELS "native;integration")
endforeach()
leaves_add_test(test_multithread TIMEOUT 120 LABELS "native;stress;concurrency")
leaves_add_test(test_multiprocess TIMEOUT 120 LABELS "native;stress;multiprocess")
leaves_add_test(test_replication_multiclient TIMEOUT 120 LABELS "native;integration;network")

add_executable(profile tests/profile.cpp)

# Dumper tool
add_executable(dump-leaves tools/dump-leaves.cpp)
target_link_libraries(dump-leaves PUBLIC core)

# Merger test tool
add_executable(merge-dbs tools/merge-dbs.cpp)
target_link_libraries(merge-dbs PUBLIC core)

# YCSB debug tool
add_executable(ycsb-debug tests/ycsb-bug/ycsb-debug.cpp)
target_link_libraries(ycsb-debug PUBLIC core)

set(LEAVES_GCOV_TOOL "")
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_leaves_gcov_candidates "")
    if (CMAKE_CXX_COMPILER_VERSION)
        list(APPEND _leaves_gcov_candidates
            "gcov-${CMAKE_CXX_COMPILER_VERSION}"
            "gcov-${CMAKE_CXX_COMPILER_VERSION_MAJOR}")
    endif()

    get_filename_component(_leaves_cxx_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_leaves_gcov_candidate IN LISTS _leaves_gcov_candidates)
        if (EXISTS "${_leaves_cxx_compiler_dir}/${_leaves_gcov_candidate}")
            set(LEAVES_GCOV_TOOL "${_leaves_cxx_compiler_dir}/${_leaves_gcov_candidate}")
            break()
        endif()
    endforeach()

    if (NOT LEAVES_GCOV_TOOL)
        find_program(LEAVES_GCOV_TOOL NAMES ${_leaves_gcov_candidates})
    endif()

    if (NOT LEAVES_GCOV_TOOL)
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-prog-name=gcov
            OUTPUT_VARIABLE LEAVES_GCOV_TOOL
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if (LEAVES_GCOV_TOOL AND NOT IS_ABSOLUTE "${LEAVES_GCOV_TOOL}")
            find_program(LEAVES_GCOV_TOOL NAMES "${LEAVES_GCOV_TOOL}")
        endif()
    endif()
endif()
if (NOT LEAVES_GCOV_TOOL)
    find_program(LEAVES_GCOV_TOOL NAMES gcov)
endif()
if (NOT LEAVES_GCOV_TOOL)
    message(FATAL_ERROR "LEAVES_GCOV is enabled, but no gcov tool was found for coverage capture")
endif()

add_custom_target(cov
COMMAND lcov --gcov-tool "${LEAVES_GCOV_TOOL}" -d . --zerocounters
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
COMMAND ./test_tributary
COMMAND ./test_serial
COMMAND ./test_transfer
COMMAND ./test_replication_fsm
COMMAND ./test_replication_api
COMMAND ./test_replication_multiclient
COMMAND ./test_hash_lookup
COMMAND ./test_hash_updater
COMMAND ./test_fstore
COMMAND ./test_wal
COMMAND ./test_memstore
COMMAND ./test_map_storage
COMMAND ./test_file_storage
COMMAND ./test_aspect
COMMAND ./test_executor
COMMAND lcov --gcov-tool "${LEAVES_GCOV_TOOL}" -c -d . -o lcov-report/report.info -b . --ignore-errors mismatch,negative --include '${CMAKE_CURRENT_SOURCE_DIR}/include/*' --exclude '*/third_party/*'
COMMAND genhtml -o lcov-report lcov-report/report.info
COMMENT &quot;Generated Coverage Report Successfully!&quot;
DEPENDS test_cursor test_memory test_bits test_mmap test_db test_node test_bigmemory test_string_insert test_multithread test_merger test_tributary test_serial test_twoquecache test_transfer test_replication_fsm test_replication_api test_replication_multiclient test_hash_lookup test_hash_updater test_fstore test_memstore test_map_storage test_file_storage test_aspect test_executor
)

endif(LEAVES_BUILD_TESTS)

set(LEAVES_GTEST_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/leveldb/third_party/googletest/googlemock/include
    ${CMAKE_CURRENT_SOURCE_DIR}/leveldb/third_party/googletest/googlemock
    ${CMAKE_CURRENT_SOURCE_DIR}/leveldb/third_party/googletest/googletest/include
    ${CMAKE_CURRENT_SOURCE_DIR}/leveldb/third_party/googletest/googletest)

if (LEAVES_BUILD_BENCHMARKS) 
set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "Build LevelDB's unit tests" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "Build LevelDB's benchmarks" FORCE)
set(LEVELDB_INSTALL OFF CACHE BOOL "Install LevelDB's header and library" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "Enable installation of benchmark." FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Enable testing of the benchmark library." FORCE)
include_directories(SYSTEM ${LEAVES_GTEST_INCLUDE_DIRS})
add_subdirectory("leveldb/third_party/benchmark")
add_subdirectory("leveldb")

set(DEBUG_BENCHMARKS OFF CACHE BOOL "Build benchmarks with debug info")
set(LEAVES_BENCHMARK_NATIVE_ARCH ON CACHE BOOL "Enable -march=native for benchmark targets")

set(LEAVES_BENCH_NATIVE_FLAG "")
if (LEAVES_BENCHMARK_NATIVE_ARCH AND NOT MSVC)
    set(LEAVES_BENCH_NATIVE_FLAG "-march=native")
endif()

add_library(lbench INTERFACE)
target_include_directories(lbench SYSTEM INTERFACE leveldb ${LEAVES_GTEST_INCLUDE_DIRS})

target_sources(lbench INTERFACE 
    "leveldb/util/histogram.cc"
    "leveldb/util/histogram.h"
    "leveldb/util/testutil.cc"
    "leveldb/util/testutil.h")

add_executable(db_bench_leaves benchmarks/db_bench_leaves.cpp)
target_link_libraries(db_bench_leaves PUBLIC core lbench benchmark leveldb Threads::Threads blake3)

add_executable(db_bench_mdb benchmarks/db_bench_mdb.cpp)
target_link_libraries(db_bench_mdb PUBLIC core lbench benchmark leveldb lmdb Threads::Threads)
if (NOT MSVC)
target_compile_options(db_bench_mdb PRIVATE -O3 ${LEAVES_BENCH_NATIVE_FLAG} -funroll-loops -flto=auto)
endif()
target_compile_definitions(db_bench_mdb PRIVATE NDEBUG)

add_executable(db_bench_memory benchmarks/db_bench_memory.cpp)
target_link_libraries(db_bench_memory PUBLIC core lbench benchmark leveldb Threads::Threads)

add_executable(bench_page_sizes benchmarks/bench_page_sizes.cpp)
target_link_libraries(bench_page_sizes PUBLIC core Threads::Threads)

add_executable(bench_merge benchmarks/bench_merge.cpp)
target_link_libraries(bench_merge PUBLIC core Threads::Threads blake3)
if (NOT MSVC)
target_compile_options(bench_merge PRIVATE -O3 ${LEAVES_BENCH_NATIVE_FLAG} -funroll-loops -flto=auto)
endif()
target_compile_definitions(bench_merge PRIVATE NDEBUG LEAVES_SINGLE_PROCESS)

add_executable(bench_overwrite_vs_merge benchmarks/bench_overwrite_vs_merge.cpp)
target_link_libraries(bench_overwrite_vs_merge PUBLIC core Threads::Threads blake3)
if (NOT MSVC)
target_compile_options(bench_overwrite_vs_merge PRIVATE -O3 ${LEAVES_BENCH_NATIVE_FLAG} -funroll-loops -flto=auto)
endif()
target_compile_definitions(bench_overwrite_vs_merge PRIVATE NDEBUG LEAVES_SINGLE_PROCESS)

if (DEBUG_BENCHMARKS)
    if (MSVC)
    target_compile_options(db_bench_leaves PRIVATE /Zi /Od)
    target_compile_options(db_bench_memory PUBLIC /Zi /Od)
    else()
    target_compile_options(db_bench_leaves PRIVATE -g -O0)
    target_compile_definitions(db_bench_leaves PRIVATE DEBUG)
    target_compile_options(db_bench_memory PUBLIC  -g -O0)
    target_compile_definitions(db_bench_memory PRIVATE DEBUG)
    endif()
else()
    if (NOT MSVC)
    target_compile_options(db_bench_leaves PRIVATE -O3 ${LEAVES_BENCH_NATIVE_FLAG} -funroll-loops -flto=auto)
    target_compile_definitions(db_bench_leaves PRIVATE NDEBUG LEAVES_SINGLE_PROCESS)
    target_compile_options(db_bench_memory PUBLIC ${LEAVES_BENCH_NATIVE_FLAG} -O3 -g)
    target_compile_definitions(db_bench_memory PRIVATE NDEBUG)
    endif()
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
        target_link_libraries(db_bench_rocksdb PUBLIC ${ROCKSDB_LIBRARY} rocksdb_histogram Threads::Threads)
        if (NOT MSVC)
        target_compile_options(db_bench_rocksdb PUBLIC ${LEAVES_BENCH_NATIVE_FLAG} -O3 -g -flto=auto -funroll-loops -ffast-math)
        endif()
        target_include_directories(db_bench_rocksdb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/leveldb)
    else()
        message(STATUS "RocksDB headers found but library not found")
    endif()
else()
    message(STATUS "RocksDB not found - skipping db_bench_rocksdb")
endif()


endif (LEAVES_BUILD_BENCHMARKS) 

install(TARGETS leaves EXPORT leavesTargets
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS leaves_replication EXPORT leavesReplicationTargets
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/leaves"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/leaves/version.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/leaves")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/leaves-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/leaves-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/leaves")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/leaves-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(EXPORT leavesTargets
    FILE leaves-targets.cmake
    NAMESPACE leaves::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/leaves")
install(EXPORT leavesReplicationTargets
    FILE leaves-replication-targets.cmake
    NAMESPACE leaves::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/leaves")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/leaves-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/leaves-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/leaves")

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Header-only trie-based embedded key-value store")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "TGZ")
include(CPack)