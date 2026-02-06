#include "duckdb/main/extension/generated_extension_loader.hpp"

namespace duckdb{

//! Looks through the CMake-generated list of extensions that are linked into DuckDB currently to try load <extension>
bool TryLoadLinkedExtension(DuckDB &db, const string &extension) {

  if (extension=="core_functions") {
      db.LoadStaticExtension<CoreFunctionsExtension>();
      return true;
  }
  if (extension=="parquet") {
      db.LoadStaticExtension<ParquetExtension>();
      return true;
  }

    return false;
}

vector<string> LinkedExtensions(){
    vector<string> VEC = {
	"core_functions",
	"parquet"
    };
    return VEC;
}


vector<string> LoadedExtensionTestPaths(){
    vector<string> VEC = {
    };
    return VEC;
}

}
