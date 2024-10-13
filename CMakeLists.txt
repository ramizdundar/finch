cmake_minimum_required(VERSION 3.10)

# Project Name
project(Finch)

# Set C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)

# Add the source files
add_executable(server server.cpp)
add_executable(client client.cpp)
add_executable(test test.cpp)
