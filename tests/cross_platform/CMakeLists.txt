cmake_minimum_required(VERSION 3.20)

project(leaves_cross_platform_smoke LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../BLAKE3/c
                 ${CMAKE_CURRENT_BINARY_DIR}/blake3)

add_executable(leaves_cross_platform_smoke
    main.cpp)

target_include_directories(leaves_cross_platform_smoke PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../include)

target_link_libraries(leaves_cross_platform_smoke PRIVATE BLAKE3::blake3)

enable_testing()
add_test(NAME leaves_cross_platform_smoke COMMAND leaves_cross_platform_smoke)