cmake_minimum_required(VERSION 3.14)

project(dbsp_tests)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DuckDB source directory
set(DUCKDB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../duckdb" CACHE PATH "Path to DuckDB source")

if(NOT EXISTS "${DUCKDB_SOURCE_DIR}/src/include/duckdb.hpp")
    message(FATAL_ERROR "DuckDB source not found at ${DUCKDB_SOURCE_DIR}")
endif()

# Build DuckDB as a subdirectory (needed for integration tests)
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSIONS "" CACHE STRING "" FORCE)
add_subdirectory(${DUCKDB_SOURCE_DIR} duckdb EXCLUDE_FROM_ALL)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${DUCKDB_SOURCE_DIR}/src/include
    ${DUCKDB_SOURCE_DIR}/third_party/catch
)

# Copy catch.hpp to test directory for includes
file(COPY ${DUCKDB_SOURCE_DIR}/third_party/catch/catch.hpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})

# Catch2 main
add_library(catch2_main STATIC catch2_main.cpp)

# Helper for defining tests
function(add_dbsp_test TEST_NAME)
    set(TEST_TARGET test_${TEST_NAME})
    add_executable(${TEST_TARGET} ${ARGN})
    target_link_libraries(${TEST_TARGET} catch2_main duckdb_static)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_TARGET})
endfunction()

# Enable testing
enable_testing()

# Unit tests (no DuckDB integration needed)
add_dbsp_test(zset unit/test_zset.cpp)
add_dbsp_test(cdc_manager unit/test_cdc_manager.cpp)
add_dbsp_test(sql_parser unit/test_sql_parser.cpp)
add_dbsp_test(security_validation unit/test_security_validation.cpp)
add_dbsp_test(error_system unit/test_error_system.cpp)
add_dbsp_test(parser_errors unit/test_parser_errors.cpp)
add_dbsp_test(view_validation unit/test_view_validation.cpp)
add_dbsp_test(validation_errors unit/test_validation_errors.cpp)

# Integration tests (require DuckDB)
# Note: These tests need the extension functions, so we compile them with dbsp_extension.cpp
add_executable(test_extension_basic
    integration/test_extension_basic.cpp
    ../dbsp_extension.cpp
)
target_link_libraries(test_extension_basic catch2_main duckdb_static)
target_compile_definitions(test_extension_basic PRIVATE DUCKDB_EXTENSION_MAIN)
add_test(NAME extension_basic COMMAND test_extension_basic)

add_executable(test_extension_cdc
    integration/test_extension_cdc.cpp
    ../dbsp_extension.cpp
)
target_link_libraries(test_extension_cdc catch2_main duckdb_static)
target_compile_definitions(test_extension_cdc PRIVATE DUCKDB_EXTENSION_MAIN)
add_test(NAME extension_cdc COMMAND test_extension_cdc)

add_executable(test_cascading_views
    integration/test_cascading_views.cpp
    ../dbsp_extension.cpp
)
target_link_libraries(test_cascading_views catch2_main duckdb_static)
target_compile_definitions(test_cascading_views PRIVATE DUCKDB_EXTENSION_MAIN)
add_test(NAME cascading_views COMMAND test_cascading_views)

add_executable(test_persistence
    integration/test_persistence.cpp
    ../dbsp_extension.cpp
)
target_link_libraries(test_persistence catch2_main duckdb_static)
target_compile_definitions(test_persistence PRIVATE DUCKDB_EXTENSION_MAIN)
add_test(NAME persistence COMMAND test_persistence)

add_executable(test_security
    integration/test_security.cpp
    ../dbsp_extension.cpp
)
target_link_libraries(test_security catch2_main duckdb_static)
target_compile_definitions(test_security PRIVATE DUCKDB_EXTENSION_MAIN)
add_test(NAME security COMMAND test_security)

add_executable(test_acid_compliance
    integration/test_acid_compliance.cpp
    ../dbsp_extension.cpp
)
target_link_libraries(test_acid_compliance catch2_main duckdb_static)
target_compile_definitions(test_acid_compliance PRIVATE DUCKDB_EXTENSION_MAIN)
add_test(NAME acid_compliance COMMAND test_acid_compliance)

# Benchmarks
add_executable(bench_incremental
    benchmarks/bench_incremental.cpp
    ../dbsp_extension.cpp
)
target_link_libraries(bench_incremental catch2_main duckdb_static)
target_compile_definitions(bench_incremental PRIVATE DUCKDB_EXTENSION_MAIN)
add_test(NAME benchmark_incremental COMMAND bench_incremental)

message(STATUS "")
message(STATUS "DBSP Test Suite Configuration Complete")
message(STATUS "  Unit tests: 8")
message(STATUS "  Integration tests: 6")
message(STATUS "  Benchmarks: 1")
message(STATUS "")
message(STATUS "Run tests with: cd build_test && ctest --output-on-failure")
message(STATUS "")
