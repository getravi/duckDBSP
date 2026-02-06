# CMake generated Testfile for 
# Source directory: /Users/ravi/Documents/Dev/duckDBSP/test
# Build directory: /Users/ravi/Documents/Dev/duckDBSP/test/build_test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(zset "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_zset")
set_tests_properties(zset PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;40;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;47;add_dbsp_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(cdc_manager "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_cdc_manager")
set_tests_properties(cdc_manager PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;40;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;48;add_dbsp_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(sql_parser "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_sql_parser")
set_tests_properties(sql_parser PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;40;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;49;add_dbsp_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(security_validation "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_security_validation")
set_tests_properties(security_validation PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;40;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;50;add_dbsp_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(error_system "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_error_system")
set_tests_properties(error_system PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;40;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;51;add_dbsp_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(extension_basic "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_extension_basic")
set_tests_properties(extension_basic PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;61;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(extension_cdc "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_extension_cdc")
set_tests_properties(extension_cdc PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;69;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(cascading_views "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_cascading_views")
set_tests_properties(cascading_views PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;77;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(persistence "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_persistence")
set_tests_properties(persistence PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;85;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(security "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/test_security")
set_tests_properties(security PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;93;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
add_test(benchmark_incremental "/Users/ravi/Documents/Dev/duckDBSP/test/build_test/bench_incremental")
set_tests_properties(benchmark_incremental PROPERTIES  _BACKTRACE_TRIPLES "/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;102;add_test;/Users/ravi/Documents/Dev/duckDBSP/test/CMakeLists.txt;0;")
subdirs("duckdb")
