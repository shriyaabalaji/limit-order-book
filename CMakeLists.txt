cmake_minimum_required(VERSION 3.20)
project(limit_order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE include)

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE lob)

add_executable(test_matching tests/test_matching.cpp)
target_link_libraries(test_matching PRIVATE lob)

add_executable(bench_insert bench/bench_insert.cpp)
target_link_libraries(bench_insert PRIVATE lob)

enable_testing()
add_test(NAME test_order_book COMMAND test_order_book)
add_test(NAME test_matching COMMAND test_matching)
