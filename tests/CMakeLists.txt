add_executable(memory_observatory_tests
    test_main.cpp
    test_core.cpp
    test_trace.cpp
    test_providers.cpp
    test_stress.cpp)

target_link_libraries(memory_observatory_tests PRIVATE MemoryObservatory::memory_observatory)

add_test(NAME memory_observatory_tests COMMAND memory_observatory_tests)
