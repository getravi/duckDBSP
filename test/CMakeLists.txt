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
set(ENABLE_UBSAN OFF CACHE BOOL "" FORCE)
# DuckDB is already built by parent project
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
    target_link_libraries(${TEST_TARGET} catch2_main duckdb_static
                          duckdb_generated_extension_loader
                          core_functions_extension parquet_extension)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_TARGET})
endfunction()

# Enable testing
enable_testing()

# Unit tests (no DuckDB integration needed)
add_dbsp_test(zset unit/test_zset.cpp)
add_dbsp_test(cdc_manager unit/test_cdc_manager.cpp)
add_dbsp_test(thread_safety unit/test_thread_safety.cpp)
add_dbsp_test(security_validation unit/test_security_validation.cpp)
# add_dbsp_test(error_system unit/test_error_system.cpp)
# add_dbsp_test(view_validation unit/test_view_validation.cpp)
add_dbsp_test(validation_errors unit/test_validation_errors.cpp)
add_dbsp_test(null_handling unit/test_null_handling.cpp)
add_dbsp_test(minmax_correctness unit/test_minmax_correctness.cpp)
add_dbsp_test(native_sort_limit unit/test_native_sort_limit.cpp)
add_dbsp_test(window_functions unit/test_window_functions.cpp)
add_dbsp_test(set_ops unit/test_set_ops.cpp)
add_dbsp_test(distinct_on unit/test_distinct_on.cpp)
add_dbsp_test(recursive unit/test_recursive.cpp)
add_dbsp_test(auto_cdc unit/test_auto_cdc.cpp)

# Integration tests (require DuckDB)
function(add_dbsp_integration_test TEST_NAME SOURCE)
    set(TEST_TARGET test_${TEST_NAME})
    add_executable(${TEST_TARGET}
        ${SOURCE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbsp_extension.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbsp_recovery.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbsp_crash_marker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbsp_checkpoint_format.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbsp_wal_manager.cpp
    )
    target_link_libraries(${TEST_TARGET} catch2_main duckdb_static
                          duckdb_generated_extension_loader
                          core_functions_extension parquet_extension)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_TARGET})
endfunction()

add_dbsp_integration_test(extension_basic integration/test_extension_basic.cpp)
add_dbsp_integration_test(planner_frontend integration/test_planner_frontend.cpp)
add_dbsp_integration_test(extension_cdc integration/test_extension_cdc.cpp)
add_dbsp_integration_test(cascading_views integration/test_cascading_views.cpp)
add_dbsp_integration_test(join_complex integration/test_join_complex.cpp)
add_dbsp_integration_test(recursive_integration integration/test_recursive.cpp)
add_dbsp_integration_test(optimization_integration integration/test_optimization_integration.cpp)
add_dbsp_integration_test(error_handling_e2e integration/test_error_handling_e2e.cpp)
add_dbsp_integration_test(acid_compliance integration/test_acid_compliance.cpp)
add_dbsp_integration_test(persistence integration/test_persistence.cpp)
add_dbsp_integration_test(advanced_sql integration/test_advanced_sql.cpp)
add_dbsp_integration_test(advanced_window integration/test_advanced_window.cpp)
add_dbsp_integration_test(auto_cdc_integration integration/test_auto_cdc.cpp)
add_dbsp_integration_test(security_integration integration/test_security.cpp)
add_dbsp_integration_test(distinct_ddl integration/test_distinct_ddl.cpp)
add_dbsp_integration_test(set_ops_integration integration/test_set_ops.cpp)
add_dbsp_integration_test(crash_recovery_phase1 integration/test_crash_recovery_phase1.cpp)
add_dbsp_integration_test(crash_recovery_phase2 integration/test_crash_recovery_phase2.cpp)
add_dbsp_integration_test(crash_recovery_phase3 integration/test_crash_recovery_phase3.cpp)
