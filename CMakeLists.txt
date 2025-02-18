cmake_minimum_required(VERSION 3.10.0)
project(hello LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
include_directories(lib/include)

add_subdirectory(test)
add_executable(hello main.cpp)