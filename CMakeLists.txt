cmake_minimum_required(VERSION 3.9)
cmake_policy(SET CMP0076 NEW)

project(leaves VERSION 3.0.0 LANGUAGES C CXX)

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
option(LEAVES_BUILD_TESTS "Build leaves' unit tests" ON)
option(LEAVES_BUILD_BENCHMARKS "Build leaves' benchmarks" ON)


include(CheckCXXCompilerFlag)

check_cxx_compiler_flag("-mpopcnt" COMPILER_SUPPORTS_POPCNT)
if (COMPILER_SUPPORTS_POPCNT)
    add_compile_options(-mpopcnt)
endif()

check_cxx_compiler_flag("-mbmi" COMPILER_SUPPORTS_BMI)
if (COMPILER_SUPPORTS_BMI)
    add_compile_options(-mbmi)
endif()


check_cxx_compiler_flag("-mlzcnt" COMPILER_SUPPORTS_LZCNT)
if (COMPILER_SUPPORTS_LZCNT)
    add_compile_options(-mlzcnt)
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

if (HAVE_BOOST_PROCESS)
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
target_compile_options(test INTERFACE -g -fprofile-arcs -ftest-coverage)
target_compile_definitions(test INTERFACE DEBUG GCOV_ENABLED)
target_link_options(test INTERFACE --coverage)
target_link_libraries(test INTERFACE gcov)
else (LEAVES_GCOV)
target_compile_options(test INTERFACE -g)
target_compile_definitions(test INTERFACE DEBUG)
endif (LEAVES_GCOV)

add_executable(test_mmap tests/test_mmap.cpp)
target_sources(test_mmap INTERFACE
    include/intern/_mmap.hpp include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_mmap PUBLIC test)

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

add_executable(test_lsm tests/test_lsm.cpp)
target_link_libraries(test_lsm PUBLIC core test)

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

add_executable(test_serial tests/test_serial.cpp)
target_link_libraries(test_serial PUBLIC test)

add_executable(test_twoquecache tests/test_twoquecache.cpp)
target_link_libraries(test_file_storage PUBLIC core test)

add_executable(test_multithread tests/test_multithread.cpp)
target_link_libraries(test_multithread PUBLIC core test pthread)

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
COMMAND lcov -c -d . -o lcov-report/report.info -b . --ignore-errors mismatch --include '${CMAKE_CURRENT_SOURCE_DIR}/include/*'
COMMAND genhtml -o lcov-report lcov-report/report.info
COMMENT &quot;Generated Coverage Report Successfully!&quot;
DEPENDS test_cursor test_memory test_bits test_mmap test_db test_node test_bigmemory test_string_insert test_multithread test_merger test_serial test_twoquecache
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
    benchmark leveldb pthread core)

if (DEBUG_BENCHMARKS)
    target_compile_options(db_bench_leaves PRIVATE -g -O0)
    target_compile_definitions(db_bench_leaves PRIVATE DEBUG)
else()
    target_compile_options(db_bench_leaves PRIVATE -O3 -march=native -funroll-loops -flto=auto)
    target_compile_definitions(db_bench_leaves PRIVATE NDEBUG)
endif()

add_executable(db_bench_mdb benchmarks/db_bench_mdb.cpp)
target_link_libraries(db_bench_mdb PUBLIC core lbench gmock gtest 
    benchmark leveldb lmdb pthread)
target_compile_options(db_bench_mdb PRIVATE -O3 -march=native -funroll-loops -flto=auto)
target_compile_definitions(db_bench_mdb PRIVATE NDEBUG)

add_executable(db_bench_memory benchmarks/db_bench_memory.cpp)
target_link_libraries(db_bench_memory PUBLIC core lbench gmock gtest 
    benchmark leveldb pthread)
target_compile_options(db_bench_memory PUBLIC -march=native -O3 -g)
target_compile_definitions(db_bench_memory PRIVATE NDEBUG)
#target_compile_options(db_bench_memory PUBLIC -g -march=native)

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