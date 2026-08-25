add_executable(memory-observatory-benchmark benchmark.cpp)
target_link_libraries(memory-observatory-benchmark PRIVATE MemoryObservatory::memory_observatory)
