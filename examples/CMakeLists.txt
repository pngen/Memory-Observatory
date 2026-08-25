add_executable(example-basic basic.cpp)
target_link_libraries(example-basic PRIVATE MemoryObservatory::memory_observatory)

add_executable(example-custom-provider custom_provider.cpp)
target_link_libraries(example-custom-provider PRIVATE MemoryObservatory::memory_observatory)

add_executable(example-trace-replay trace_replay.cpp)
target_link_libraries(example-trace-replay PRIVATE MemoryObservatory::memory_observatory)
