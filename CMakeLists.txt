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
    # Fetch Catch2 v3
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
    )
    FetchContent_MakeAvailable(Catch2)

    # Unit tests
    add_executable(unit_tests
        test/catch2_main.cpp
        test/unit/test_zset.cpp
    )
    target_link_libraries(unit_tests PRIVATE dbsp_headers Catch2::Catch2)
    target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

    enable_testing()
    add_test(NAME unit_tests COMMAND unit_tests)

    # Integration tests (requires DuckDB)
    add_executable(integration_tests
        test/catch2_main.cpp
        test/integration/test_extension_basic.cpp
    )
    target_link_libraries(integration_tests PRIVATE
        dbsp_headers
        Catch2::Catch2
    )
    target_include_directories(integration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/duckdb_extension
        ${CMAKE_SOURCE_DIR}/duckdb_extension/duckdb/src/include
    )

    add_test(NAME integration_tests COMMAND integration_tests)

    # Existing unit tests (will port to Catch2 later)
    add_executable(dbsp_tests_old test/test_zset.cpp)
    target_link_libraries(dbsp_tests_old PRIVATE dbsp_headers)
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
