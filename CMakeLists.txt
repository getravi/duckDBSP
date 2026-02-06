cmake_minimum_required(VERSION 3.14)

project(dbsp_extension)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TARGET_NAME dbsp)
set(EXT_VERSION_DBSP "0.1.0")

# DuckDB source directory (set via command line or default)
set(DUCKDB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb" CACHE PATH "Path to DuckDB source")

if(NOT EXISTS "${DUCKDB_SOURCE_DIR}/src/include/duckdb.hpp")
    message(FATAL_ERROR "DuckDB source not found at ${DUCKDB_SOURCE_DIR}. Run build.sh to fetch it.")
endif()

# Build DuckDB as a subdirectory
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSIONS "" CACHE STRING "" FORCE)
add_subdirectory(${DUCKDB_SOURCE_DIR} duckdb EXCLUDE_FROM_ALL)

# Extension source
add_library(${TARGET_NAME}_loadable_extension SHARED
    dbsp_extension.cpp
)

target_compile_definitions(${TARGET_NAME}_loadable_extension PRIVATE
    DUCKDB_EXTENSION_MAIN
    EXT_VERSION_DBSP="${EXT_VERSION_DBSP}"
)

target_include_directories(${TARGET_NAME}_loadable_extension PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${DUCKDB_SOURCE_DIR}/src/include
)

target_link_libraries(${TARGET_NAME}_loadable_extension duckdb_static)

set_target_properties(${TARGET_NAME}_loadable_extension PROPERTIES
    OUTPUT_NAME ${TARGET_NAME}
    PREFIX ""
    SUFFIX ".duckdb_extension"
)

message(STATUS "")
message(STATUS "DBSP DuckDB Extension v${EXT_VERSION_DBSP}")
message(STATUS "  DuckDB: ${DUCKDB_SOURCE_DIR}")
message(STATUS "")
