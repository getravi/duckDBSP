file(REMOVE_RECURSE
  ".1.4"
  "libduckdb.1.4.0.dylib"
  "libduckdb.1.4.dylib"
  "libduckdb.dylib"
  "libduckdb.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CXX)
  include(CMakeFiles/duckdb.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
