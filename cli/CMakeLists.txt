add_executable(memory-observatory main.cpp)
target_link_libraries(memory-observatory PRIVATE MemoryObservatory::memory_observatory)

install(TARGETS memory-observatory
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
