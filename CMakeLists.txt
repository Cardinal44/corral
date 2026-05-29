
cmake_minimum_required(VERSION 3.22)

project(corral LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(corral src/main.cpp)

enable_testing()

add_test(NAME smoke COMMAND corral)

