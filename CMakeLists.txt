cmake_minimum_required(VERSION 3.9)
cmake_policy(SET CMP0076 NEW)

project(leaves VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

option(LEAVES_GCOV "Add gconv support for tests" ON)
option(LEAVES_BUILD_TESTS "Build leaves' unit tests" ON)
option(LEAVES_BUILD_BENCHMARKS "Build leaves' benchmarks" ON)

file(GLOB LEAVE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/.cpp)

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
    include/leaves/intern/_util.hpp
    include/leaves/intern/_exception.hpp
    include/leaves/intern/_bits.hpp
    include/leaves/intern/_memory.hpp
    include/leaves/intern/_mmap.hpp
    include/leaves/intern/_cursor.hpp
    include/leaves/intern/_node.hpp
    include/leaves/intern/_inserter.hpp
)

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

add_executable(test_memory tests/test_memory.cpp)
target_sources(test_memory INTERFACE
    include/intern/_memory.hpp include/intern/_bits.hpp)
target_link_libraries(test_memory PUBLIC test)

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

add_executable(test_string_insert tests/test_string_insert.cpp)
target_link_libraries(test_string_insert PUBLIC core test)

add_executable(profile tests/profile.cpp)
target_compile_options(profile PUBLIC -O2)
target_link_libraries(profile PUBLIC core test)

add_custom_target( cov
COMMAND lcov -d . --zerocounters
COMMAND rm -rf lcov-report
COMMAND mkdir lcov-report
COMMAND ./test_cursor
COMMAND ./test_node
COMMAND ./test_memory
COMMAND ./test_bits
COMMAND ./test_mmap
COMMAND ./test_db
COMMAND ./test_string_insert
COMMAND lcov -c -d . -o lcov-report/report.info -b . --ignore-errors mismatch --include '${CMAKE_CURRENT_SOURCE_DIR}/include/*'
COMMAND genhtml -o lcov-report lcov-report/report.info
COMMENT &quot;Generated Coverage Report Successfully!&quot;
DEPENDS test_cursor test_memory test_bits test_mmap test_db test_node test_string_insert
)

endif(LEAVES_BUILD_TESTS)

if (LEAVES_BUILD_BENCHMARKS) 

add_subdirectory("leveldb/third_party/googletest")
add_subdirectory("leveldb/third_party/benchmark")

add_library(lbench INTERFACE)
target_include_directories(lbench INTERFACE leveldb)
target_compile_definitions(lbench INTERFACE NDEBUG)
# target_compile_definitions(lbench INTERFACE DEBUG)
target_sources(lbench INTERFACE 
    "leveldb/util/histogram.cc"
    "leveldb/util/histogram.h"
    "leveldb/util/testutil.cc"
    "leveldb/util/testutil.h")

add_executable(db_bench_leaves benchmarks/db_bench_leaves.cpp)
target_link_libraries(db_bench_leaves PUBLIC core lbench gmock gtest    
    benchmark leveldb pthread core)
target_compile_options(db_bench_leaves PUBLIC -g -march=native -O2)

add_executable(db_bench_mdb benchmarks/db_bench_mdb.cpp)
target_link_libraries(db_bench_mdb PUBLIC core lbench gmock gtest 
    benchmark leveldb lmdb pthread)
target_compile_options(db_bench_mdb PUBLIC -march=native -O2)
target_compile_options(db_bench_mdb PUBLIC -g)

endif (LEAVES_BUILD_BENCHMARKS) 