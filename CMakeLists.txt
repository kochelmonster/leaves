cmake_minimum_required(VERSION 3.9)
cmake_policy(SET CMP0076 NEW)

project(leaves VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

option(LEAVES_GCOV "Add gconv support for tests" ON)
option(LEAVES_BUILD_TESTS "Build leaves' unit tests" ON)
option(LEAVES_BUILD_BENCHMARKS "Build leaves' benchmarks" ON)

file(GLOB LEAVE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/.cpp)

add_library(core INTERFACE)
target_sources(core INTERFACE 
    include/leaves.hpp
    src/port.hpp
    src/storage.cpp
    src/storage.hpp
    src/page.cpp
    src/page.hpp
    src/node.cpp
    src/node.hpp
    src/trace.cpp
    src/trace.hpp)

target_include_directories(core
                           INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           )
target_include_directories(core INTERFACE src)



if (LEAVES_BUILD_TESTS)

add_library(test INTERFACE)
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


add_executable(test_storage tests/test_storage.cpp)
target_link_libraries(test_storage PUBLIC core test)

add_executable(test_trie tests/test_trie.cpp)
target_link_libraries(test_trie PUBLIC core test)

add_executable(test_db tests/test_db.cpp src/leaves.cpp)
target_link_libraries(test_db PUBLIC core test)

add_executable(profile tests/profile.cpp src/leaves.cpp)
target_link_libraries(profile PUBLIC core test)

add_custom_target( cov
COMMAND lcov -d . --zerocounters
COMMAND rm -rf lcov-report
COMMAND mkdir lcov-report
COMMAND ./test_storage
COMMAND ./test_trie
COMMAND lcov -c -d . -o lcov-report/report.info -b .
COMMAND genhtml -o lcov-report lcov-report/report.info
COMMENT &quot;Generated Coverage Report Successfully!&quot;
DEPENDS test_storage
)

endif(LEAVES_BUILD_TESTS)

if (LEAVES_BUILD_BENCHMARKS) 

add_subdirectory("leveldb/third_party/googletest")
add_subdirectory("leveldb/third_party/benchmark")

add_library(lbench INTERFACE)
target_include_directories(lbench INTERFACE leveldb)
target_compile_definitions(lbench INTERFACE NDEBUG)
#target_compile_definitions(lbench INTERFACE DEBUG)
target_sources(lbench INTERFACE 
    "leveldb/util/histogram.cc"
    "leveldb/util/histogram.h"
    "leveldb/util/testutil.cc"
    "leveldb/util/testutil.h")

add_executable(db_bench_leaves benchmarks/db_bench_leaves.cpp src/leaves.cpp)
target_link_libraries(db_bench_leaves PUBLIC core lbench gmock gtest 
    benchmark leveldb pthread core)
# target_compile_options(db_bench_leaves PUBLIC -g)
target_compile_options(db_bench_leaves PUBLIC -O3)

add_executable(db_bench_mdb benchmarks/db_bench_mdb.cpp)
target_link_libraries(db_bench_mdb PUBLIC core lbench gmock gtest 
    benchmark leveldb lmdb pthread)
target_compile_options(db_bench_mdb PUBLIC -O3)

endif (LEAVES_BUILD_BENCHMARKS) 