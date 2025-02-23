cmake_minimum_required(VERSION 3.10.0)
project(hello LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
include_directories(lib/include)

add_executable(hello main.cpp)

# Tests.
add_subdirectory(extern/googletest)
enable_testing()
include(GoogleTest)
add_subdirectory(test)

# Examples.
add_subdirectory(example)