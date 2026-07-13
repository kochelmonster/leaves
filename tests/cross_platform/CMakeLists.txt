cmake_minimum_required(VERSION 3.20)

project(leaves_cross_platform_smoke LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(LEAVES_LOG "Enable leaves logging in smoke binary" OFF)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../BLAKE3/c
                 ${CMAKE_CURRENT_BINARY_DIR}/blake3)

if(APPLE OR WIN32)
  find_package(Boost 1.80 REQUIRED COMPONENTS filesystem process)
else()
  find_package(Boost 1.80 REQUIRED)
endif()

add_executable(leaves_cross_platform_smoke
    main.cpp)

target_include_directories(leaves_cross_platform_smoke PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../include
    ${Boost_INCLUDE_DIRS})

target_link_libraries(leaves_cross_platform_smoke PRIVATE BLAKE3::blake3)

if(WIN32)
  # Disable Boost auto-link (#pragma comment(lib,...)) on Windows.
  # Chocolatey's boost-msvc-14.3 uses MSVC naming (no "lib" prefix) but
  # Boost's auto-link can generate names with "lib" prefix, causing LNK1104.
  target_compile_definitions(leaves_cross_platform_smoke PRIVATE BOOST_ALL_NO_LIB)
endif()

if (LEAVES_LOG)
  target_compile_definitions(leaves_cross_platform_smoke PRIVATE LEAVES_LOG)
endif()

if (TARGET Boost::process)
  target_link_libraries(leaves_cross_platform_smoke PRIVATE Boost::process)
endif()

if (TARGET Boost::filesystem)
  target_link_libraries(leaves_cross_platform_smoke PRIVATE Boost::filesystem)
endif()

enable_testing()
add_test(NAME leaves_cross_platform_smoke COMMAND leaves_cross_platform_smoke)