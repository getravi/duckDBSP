#pragma once

// Qualified table-name utilities (Phase D2). Standalone so both the plan
// translator (source extraction) and the CDC manager (tracking, sync SQL)
// can use one canonical naming scheme: keys are dotted
// catalog.schema.table derived from resolved TableCatalogEntry objects.

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/qualified_name.hpp"

#include <string>

namespace dbsp_native {

// Plain-identifier check for one dotted part (mirrors validate_identifier's
// character rules; kept dependency-free of dbsp_errors).
inline bool qualified_part_ok(const std::string &part) {
  if (part.empty() || part.length() > 255) {
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(part[0])) && part[0] != '_') {
    return false;
  }
  for (char c : part) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
      return false;
    }
  }
  return true;
}

// ---- Qualified table names (Phase D2) -------------------------------------
//
// Tracked-table keys are canonical dotted names: catalog.schema.table,
// derived from the resolved TableCatalogEntry — never from SQL text — so
// `li_1` and `m.li_1` map to the same key regardless of how the user wrote
// them. Attached catalogs work because resolution goes through DuckDB's
// catalog, not a hardcoded main-catalog lookup.

// Each dot-separated part must be a plain identifier (the same rule the
// bare-name validator enforces). 1–3 parts.
inline bool is_valid_table_reference(const std::string &name) {
  size_t start = 0, parts = 0;
  while (start <= name.size()) {
    size_t dot = name.find('.', start);
    std::string part = name.substr(
        start, dot == std::string::npos ? std::string::npos : dot - start);
    if (!qualified_part_ok(part)) {
      return false;
    }
    parts++;
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  return parts >= 1 && parts <= 3;
}

// Canonical dotted key for a resolved table.
inline std::string canonical_table_key(const duckdb::TableCatalogEntry &entry) {
  return entry.ParentCatalog().GetName() + "." + entry.ParentSchema().name +
         "." + entry.name;
}

// Resolve a user-supplied table reference (bare or dotted) through the
// catalog. Returns nullptr when it does not name a table.
inline duckdb::optional_ptr<duckdb::TableCatalogEntry>
resolve_table_entry(duckdb::ClientContext &context, const std::string &ref) {
  try {
    auto qn = duckdb::QualifiedName::Parse(ref);
    if (qn.catalog == INVALID_CATALOG) {
      qn.catalog = "";
    }
    if (qn.schema == INVALID_SCHEMA) {
      qn.schema = "";
    }
    auto entry = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(
        context, qn.catalog, qn.schema, qn.name,
        duckdb::OnEntryNotFound::RETURN_NULL);
    if (!entry && qn.catalog.empty() && !qn.schema.empty()) {
      // Two-part refs are ambiguous: Parse() reads "a.li" as schema.table,
      // but "a" may be a catalog (ATTACH ... AS a). Mirror the binder and
      // retry with the first part as the catalog.
      entry = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(
          context, qn.schema, "", qn.name,
          duckdb::OnEntryNotFound::RETURN_NULL);
    }
    return entry;
  } catch (...) {
    return nullptr;
  }
}

// SQL-quotable form of a canonical dotted key: "cat"."sch"."tbl".
// Keys are canonical (produced by canonical_table_key) or validated user
// references, so each part is a plain identifier.
inline std::string quote_table_key(const std::string &key) {
  std::string out;
  size_t start = 0;
  while (start <= key.size()) {
    size_t dot = key.find('.', start);
    if (!out.empty()) {
      out += ".";
    }
    out += "\"" +
           key.substr(start, dot == std::string::npos ? std::string::npos
                                                      : dot - start) +
           "\"";
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  return out;
}

} // namespace dbsp_native
