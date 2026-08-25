cmake_minimum_required(VERSION 3.22)
project(memory_observatory_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(memory_observatory CONFIG REQUIRED)

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE MemoryObservatory::memory_observatory)
