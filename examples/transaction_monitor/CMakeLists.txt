cmake_minimum_required(VERSION 3.22)
project(leaves-transaction-monitor-example VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.80 REQUIRED)
find_package(Threads REQUIRED)

add_library(leaves_headers INTERFACE)
target_include_directories(leaves_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${Boost_INCLUDE_DIRS})

add_executable(transaction_monitor main.cpp)
target_sources(transaction_monitor PRIVATE transaction_monitor.hpp)
target_link_libraries(transaction_monitor PRIVATE leaves_headers Threads::Threads)