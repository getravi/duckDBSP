cmake_minimum_required(VERSION 3.14)
project(dbsp_duckdb_extension VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_STANDALONE_DEMO "Build standalone demo" ON)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Header-only library (the DBSP implementation)
add_library(dbsp_headers INTERFACE)
target_include_directories(dbsp_headers INTERFACE ${CMAKE_SOURCE_DIR}/include)

# Standalone demo executable
if(BUILD_STANDALONE_DEMO)
    add_executable(dbsp_demo src/dbsp_extension.cpp)
    target_compile_definitions(dbsp_demo PRIVATE DBSP_STANDALONE_TEST)
    target_link_libraries(dbsp_demo PRIVATE dbsp_headers)
endif()

# Interactive CLI (no external dependencies)
add_executable(dbsp src/dbsp_cli.cpp)
target_link_libraries(dbsp PRIVATE dbsp_headers)

# Tests
if(BUILD_TESTS)
    add_executable(dbsp_tests test/test_zset.cpp)
    target_link_libraries(dbsp_tests PRIVATE dbsp_headers)
endif()

# Install headers
install(DIRECTORY include/ DESTINATION include/dbsp)

# Print configuration summary
message(STATUS "")
message(STATUS "DBSP DuckDB Extension Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
message(STATUS "  Build Standalone Demo: ${BUILD_STANDALONE_DEMO}")
message(STATUS "")
