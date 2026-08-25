# Memory Observatory 1.0.0
cmake_minimum_required(VERSION 3.22)

project(MemoryObservatory
        VERSION 1.0.0
        DESCRIPTION "Observability runtime for memory behavior across heterogeneous AI infrastructure"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(MEMORY_OBSERVATORY_BUILD_TESTS "Build Memory Observatory tests" ON)
option(MEMORY_OBSERVATORY_BUILD_CLI "Build the memory-observatory CLI" ON)
option(MEMORY_OBSERVATORY_BUILD_EXAMPLES "Build Memory Observatory examples" ON)
option(MEMORY_OBSERVATORY_BUILD_BENCHMARKS "Build Memory Observatory benchmarks" ON)
option(MEMORY_OBSERVATORY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
    if(MEMORY_OBSERVATORY_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(MEMORY_OBSERVATORY_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# ---- Library target -------------------------------------------------------
set(MEMORY_OBSERVATORY_SOURCES
    src/error.cpp
    src/observatory.cpp
    src/trace.cpp
    src/report.cpp
    src/providers.cpp
    src/sampler.cpp)

add_library(memory_observatory ${MEMORY_OBSERVATORY_SOURCES})
add_library(MemoryObservatory::memory_observatory ALIAS memory_observatory)

if(WIN32)
    target_link_libraries(memory_observatory PRIVATE psapi)
endif()

target_include_directories(memory_observatory
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(memory_observatory PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)


# ---- Install --------------------------------------------------------------
install(TARGETS memory_observatory
    EXPORT MemoryObservatoryTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT MemoryObservatoryTargets
    FILE MemoryObservatoryTargets.cmake
    NAMESPACE MemoryObservatory::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/memory_observatory)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/memory_observatoryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/memory_observatoryConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/memory_observatory)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/memory_observatoryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/memory_observatoryConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/memory_observatoryConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/memory_observatory)

install(FILES LICENSE NOTICE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/memory_observatory)

# ---- Subdirectories -------------------------------------------------------
if(MEMORY_OBSERVATORY_BUILD_CLI)
    add_subdirectory(cli)
endif()
if(MEMORY_OBSERVATORY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(MEMORY_OBSERVATORY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(MEMORY_OBSERVATORY_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
