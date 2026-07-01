cmake_minimum_required(VERSION 3.9)
project(leaves-p2p-example VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BLAKE3_SIMD_TYPE "none" CACHE STRING "" FORCE)
add_subdirectory(../../BLAKE3/c ${CMAKE_BINARY_DIR}/blake3)

find_package(Boost 1.80 REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

include(CheckCXXCompilerFlag)
include(CheckCXXSourceRuns)

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

check_cpu_feature("-mavx2"
    "#include <immintrin.h>\nint main() { __m256i v = _mm256_set1_epi32(1); return _mm256_extract_epi32(v,0) != 1; }"
    CPU_SUPPORTS_AVX2)
if (CPU_SUPPORTS_AVX2)
    add_compile_options(-mavx2)
endif()

check_cpu_feature("-mavx512bw"
    "#include <immintrin.h>\nint main() { __m512i v = _mm512_set1_epi8(1); return (int)_mm512_reduce_add_epi32(_mm512_setzero_si512()); }"
    CPU_SUPPORTS_AVX512BW)
if (CPU_SUPPORTS_AVX512BW)
    add_compile_options(-mavx512bw)
endif()

add_library(core INTERFACE)
target_include_directories(core INTERFACE ../../include)

add_library(leaves_replication INTERFACE)
target_include_directories(leaves_replication INTERFACE ../../include)
target_link_libraries(leaves_replication INTERFACE core)

add_executable(p2p_kv main.cpp)
target_link_libraries(p2p_kv PUBLIC core leaves_replication blake3 pthread)