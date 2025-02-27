cmake_minimum_required(VERSION 3.10.0)
project(coro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

# Library (coro).
add_subdirectory(lib)

# Tests.
add_subdirectory(extern/googletest)
enable_testing()
include(GoogleTest)
add_subdirectory(test)

# Examples.
add_subdirectory(example)
