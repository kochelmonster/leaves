cmake_minimum_required(VERSION 3.9)
project(leaves-confluence-multithread-example VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(core INTERFACE)
target_include_directories(core INTERFACE ../../include)

add_executable(confluence_multithread main.cpp)
target_link_libraries(confluence_multithread PUBLIC core pthread)
