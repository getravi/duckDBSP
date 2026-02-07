// DBSP DuckDB Extension - Real-Time Incremental Materialized Views
// Version 3.0 - With cascading views and persistence
//
// Usage:
//   LOAD 'dbsp';
//
//   -- Track a table for CDC (auto-detects schema)
//   SELECT * FROM dbsp_track('orders');
//
//   -- Create view with SQL syntax
//   SELECT * FROM dbsp_create_view('high_value', 'SELECT * FROM orders WHERE
//   amount > 100'); SELECT * FROM dbsp_create_view('totals', 'SELECT
//   customer_id, SUM(amount) FROM orders GROUP BY customer_id');
//
//   -- Cascading views (views on views)
//   SELECT * FROM dbsp_create_view('vip_totals', 'SELECT * FROM totals WHERE
//   SUM > 1000');
//
//   -- Notify changes (or use dbsp_sync to auto-detect)
//   SELECT * FROM dbsp_notify_insert('orders', 1, 'Alice', 250.00);
//   SELECT * FROM dbsp_sync('orders');  -- Sync with actual table
//
//   -- Query views
//   SELECT * FROM dbsp_query('high_value');
//
//   -- Persistence
//   SELECT * FROM dbsp_save();           -- Save to DuckDB table
//   SELECT * FROM dbsp_save('file.json'); -- Save to JSON file
//   SELECT * FROM dbsp_load();           -- Load from DuckDB table
//   SELECT * FROM dbsp_load('file.json'); -- Load from JSON file
//
//   -- View dependencies
//   SELECT * FROM dbsp_deps('view_name');
//   SELECT dbsp_drop_cascade('view_name'); -- Drop view and dependents

#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include "dbsp_cdc.hpp"
#include "dbsp_parser_extension.hpp"

namespace duckdb {

// ============================================================================
// dbsp_track - Track a table for automatic CDC
// Usage: SELECT * FROM dbsp_track('table_name');
// ============================================================================

struct TrackBindData : public TableFunctionData {
  string table_name;
  string result;
  bool done = false;
};

unique_ptr<FunctionData> TrackBind(ClientContext &context,
                                   TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types,
                                   vector<string> &names) {
  auto data = make_uniq<TrackBindData>();

  if (input.inputs.empty()) {
    throw InvalidInputException("dbsp_track(table_name)");
  }

  data->table_name = input.inputs[0].GetValue<string>();

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void TrackFunc(ClientContext &context, TableFunctionInput &input,
               DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<TrackBindData>();
  if (data.done)
    return;

  auto &manager = dbsp_native::get_cdc_manager();
  bool ok = manager.track_table(context, data.table_name);

  if (!ok) {
    std::string formatted_error = manager.last_error();
    // If last_error doesn't contain DBSP-E code, it's an old-style error
    if (formatted_error.find("DBSP-E") == std::string::npos) {
      // Wrap in generic format for consistency
      formatted_error = "Failed to track table '" + data.table_name +
                       "': " + formatted_error;
    }
    throw InvalidInputException(formatted_error);
  }

  output.SetCardinality(1);
  auto schema = manager.get_table_schema(data.table_name);
  string cols =
      schema ? std::to_string(schema->columns.size()) + " columns" : "";
  output.SetValue(
      0, 0, Value("Tracking table: " + data.table_name + " (" + cols + ")"));
  data.done = true;
}

// ============================================================================
// dbsp_create_view - Create a materialized view
// Usage (SQL): SELECT * FROM dbsp_create_view('name', 'SELECT ... FROM ...');
// Usage (simple): SELECT * FROM dbsp_create_view('name', 'table', 'type',
// 'spec');
// ============================================================================

struct CreateViewBindData : public TableFunctionData {
  string view_name;
  string sql_or_table;
  string view_type;
  string spec;
  bool is_sql_mode = false;
  bool done = false;
};

unique_ptr<FunctionData> CreateViewBind(ClientContext &context,
                                        TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types,
                                        vector<string> &names) {
  auto data = make_uniq<CreateViewBindData>();

  if (input.inputs.size() < 2) {
    throw InvalidInputException("dbsp_create_view(name, sql) or "
                                "dbsp_create_view(name, table, type, spec)");
  }

  data->view_name = input.inputs[0].GetValue<string>();
  data->sql_or_table = input.inputs[1].GetValue<string>();

  // Detect mode: if second arg starts with SELECT, it's SQL mode
  string trimmed = data->sql_or_table;
  string upper = StringUtil::Upper(trimmed);
  if (upper.rfind("SELECT", 0) == 0) {
    data->is_sql_mode = true;
  } else if (input.inputs.size() >= 4) {
    data->view_type = input.inputs[2].GetValue<string>();
    data->spec = input.inputs[3].GetValue<string>();
    data->is_sql_mode = false;
  } else {
    throw InvalidInputException(
        "Use SQL syntax: dbsp_create_view('name', 'SELECT ...')");
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void CreateViewFunc(ClientContext &context, TableFunctionInput &input,
                    DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<CreateViewBindData>();
  if (data.done)
    return;

  auto &manager = dbsp_native::get_cdc_manager();
  string result;

  if (data.is_sql_mode) {
    // SQL mode - use the parser
    bool ok = manager.create_view(context, data.view_name, data.sql_or_table);
    if (!ok) {
      std::string formatted_error = manager.last_error();
      // If last_error doesn't contain DBSP-E code, it's an old-style error
      if (formatted_error.find("DBSP-E") == std::string::npos) {
        // Wrap in generic format for consistency
        formatted_error = "Failed to create view '" + data.view_name +
                         "': " + formatted_error;
      }
      throw InvalidInputException(formatted_error);
    }
    auto info = manager.get_view_info(data.view_name);
    result = "Created view: " + data.view_name + " (sources: ";
    for (size_t i = 0; i < info.source_tables.size(); i++) {
      if (i > 0)
        result += ", ";
      result += info.source_tables[i];
    }
    result += ")";
  } else {
    // Simple mode - construct SQL from parameters
    string sql;
    string table = data.sql_or_table;
    string type = StringUtil::Lower(data.view_type);

    if (type == "filter") {
      // Parse spec: "column op value"
      sql = "SELECT * FROM " + table + " WHERE " + data.spec;
    } else if (type == "aggregate") {
      // Parse spec: "group_col AGG value_col"
      auto parts = StringUtil::Split(data.spec, ' ');
      if (parts.size() >= 3) {
        sql = "SELECT " + parts[0] + ", " + parts[1] + "(" + parts[2] +
              ") FROM " + table + " GROUP BY " + parts[0];
      }
    } else if (type == "distinct") {
      sql = "SELECT DISTINCT * FROM " + table;
    } else {
      sql = "SELECT * FROM " + table;
    }

    bool ok = manager.create_view(context, data.view_name, sql);
    if (!ok) {
      std::string formatted_error = manager.last_error();
      // If last_error doesn't contain DBSP-E code, it's an old-style error
      if (formatted_error.find("DBSP-E") == std::string::npos) {
        // Wrap in generic format for consistency
        formatted_error = "Failed to create view '" + data.view_name +
                         "': " + formatted_error;
      }
      throw InvalidInputException(formatted_error);
    }
    result = "Created " + type + " view: " + data.view_name;
  }

  output.SetCardinality(1);
  output.SetValue(0, 0, Value(result));
  data.done = true;
}

// ============================================================================
// dbsp_notify_insert / dbsp_notify_delete - Notify CDC of changes
// Usage: SELECT * FROM dbsp_notify_insert('table', val1, val2, ...);
// ============================================================================

struct NotifyBindData : public TableFunctionData {
  string table_name;
  dbsp_native::DuckDBRow row;
  bool is_delete = false;
  bool done = false;
};

unique_ptr<FunctionData> NotifyBind(ClientContext &context,
                                    TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types,
                                    vector<string> &names) {
  auto data = make_uniq<NotifyBindData>();

  if (input.inputs.size() < 2) {
    throw InvalidInputException(
        "dbsp_notify_insert/delete(table, val1, val2, ...)");
  }

  data->table_name = input.inputs[0].GetValue<string>();

  // Convert values to DuckDBRow
  for (idx_t i = 1; i < input.inputs.size(); i++) {
    data->row.columns.push_back(input.inputs[i]);
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void NotifyInsertFunc(ClientContext &context, TableFunctionInput &input,
                      DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<NotifyBindData>();
  if (data.done)
    return;

  auto &manager = dbsp_native::get_cdc_manager();
  manager.on_insert(data.table_name, data.row);

  output.SetCardinality(1);
  output.SetValue(0, 0, Value("Notified insert into " + data.table_name));
  data.done = true;
}

void NotifyDeleteFunc(ClientContext &context, TableFunctionInput &input,
                      DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<NotifyBindData>();
  if (data.done)
    return;

  auto &manager = dbsp_native::get_cdc_manager();
  manager.on_delete(data.table_name, data.row);

  output.SetCardinality(1);
  output.SetValue(0, 0, Value("Notified delete from " + data.table_name));
  data.done = true;
}

// ============================================================================
// dbsp_sync - Sync tracked table with actual DuckDB table
// Usage: SELECT * FROM dbsp_sync('table_name');
//        SELECT * FROM dbsp_sync();  -- Sync all
// ============================================================================

struct SyncBindData : public TableFunctionData {
  string table_name;
  bool sync_all = false;
  bool done = false;
};

unique_ptr<FunctionData> SyncBind(ClientContext &context,
                                  TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types,
                                  vector<string> &names) {
  auto data = make_uniq<SyncBindData>();

  if (input.inputs.empty()) {
    data->sync_all = true;
  } else {
    data->table_name = input.inputs[0].GetValue<string>();
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void SyncFunc(ClientContext &context, TableFunctionInput &input,
              DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<SyncBindData>();
  if (data.done)
    return;

  auto &manager = dbsp_native::get_cdc_manager();

  if (data.sync_all) {
    manager.sync_all(context);
    output.SetCardinality(1);
    output.SetValue(0, 0, Value("Synced all tracked tables"));
  } else {
    bool ok = manager.sync_table(context, data.table_name);
    output.SetCardinality(1);
    output.SetValue(
        0, 0, Value(ok ? "Synced: " + data.table_name : "Failed to sync"));
  }
  data.done = true;
}

// ============================================================================
// dbsp_query - Query a materialized view
// Usage: SELECT * FROM dbsp_query('view_name');
// ============================================================================

struct QueryBindData : public TableFunctionData {
  string view_name;
  vector<dbsp_native::DuckDBRow> rows;
  vector<LogicalType> types;
  idx_t current = 0;
};

unique_ptr<FunctionData> QueryBind(ClientContext &context,
                                   TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types,
                                   vector<string> &names) {
  auto data = make_uniq<QueryBindData>();

  if (input.inputs.empty()) {
    throw InvalidInputException("dbsp_query(view_name)");
  }

  data->view_name = input.inputs[0].GetValue<string>();

  auto &manager = dbsp_native::get_cdc_manager();
  const auto *result = manager.query_view(data->view_name);
  const auto *schema = manager.get_view_schema(data->view_name);

  if (!result) {
    throw InvalidInputException("View not found: " + data->view_name);
  }

  // Collect rows with positive weight
  for (const auto &[row, weight] : *result) {
    if (weight > 0) {
      for (int64_t i = 0; i < weight; i++) {
        data->rows.push_back(row);
      }
    }
  }

  // Build return schema
  if (schema && !schema->columns.empty()) {
    for (const auto &col : schema->columns) {
      return_types.push_back(col.type);
      names.push_back(col.name);
    }
    data->types = return_types;
  } else if (!data->rows.empty()) {
    // Infer from first row
    const auto &first = data->rows[0];
    for (size_t i = 0; i < first.columns.size(); i++) {
      return_types.push_back(first.columns[i].type());
      names.push_back("col" + std::to_string(i));
    }
    data->types = return_types;
  } else {
    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("result");
  }

  return std::move(data);
}

void QueryFunc(ClientContext &context, TableFunctionInput &input,
               DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<QueryBindData>();

  idx_t count = 0;
  while (data.current < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
    const auto &row = data.rows[data.current];

    for (idx_t col = 0; col < output.ColumnCount() && col < row.columns.size();
         col++) {
      output.SetValue(col, count, row.columns[col]);
    }

    data.current++;
    count++;
  }
  output.SetCardinality(count);
}

// ============================================================================
// dbsp_views - List all views
// ============================================================================

struct ListViewsBindData : public TableFunctionData {
  vector<dbsp_native::CDCManager::ViewInfo> views;
  idx_t current = 0;
};

unique_ptr<FunctionData> ListViewsBind(ClientContext &context,
                                       TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types,
                                       vector<string> &names) {
  auto data = make_uniq<ListViewsBindData>();

  auto &manager = dbsp_native::get_cdc_manager();
  for (const auto &name : manager.list_views()) {
    data->views.push_back(manager.get_view_info(name));
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("view_name");
  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("sql");
  return_types.push_back(LogicalType::BIGINT);
  names.push_back("rows");
  return_types.push_back(LogicalType::BIGINT);
  names.push_back("version");

  return std::move(data);
}

void ListViewsFunc(ClientContext &context, TableFunctionInput &input,
                   DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<ListViewsBindData>();

  idx_t count = 0;
  while (data.current < data.views.size() && count < STANDARD_VECTOR_SIZE) {
    const auto &view = data.views[data.current];

    output.SetValue(0, count, Value(view.name));
    output.SetValue(1, count, Value(view.sql));
    output.SetValue(2, count, Value::BIGINT(view.row_count));
    output.SetValue(3, count, Value::BIGINT(view.version));

    data.current++;
    count++;
  }
  output.SetCardinality(count);
}

// ============================================================================
// dbsp_tables - List tracked tables
// ============================================================================

struct ListTablesBindData : public TableFunctionData {
  vector<string> tables;
  idx_t current = 0;
};

unique_ptr<FunctionData> ListTablesBind(ClientContext &context,
                                        TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types,
                                        vector<string> &names) {
  auto data = make_uniq<ListTablesBindData>();

  auto &manager = dbsp_native::get_cdc_manager();
  data->tables = manager.list_tracked_tables();

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("table_name");
  return_types.push_back(LogicalType::BIGINT);
  names.push_back("columns");

  return std::move(data);
}

void ListTablesFunc(ClientContext &context, TableFunctionInput &input,
                    DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<ListTablesBindData>();

  auto &manager = dbsp_native::get_cdc_manager();

  idx_t count = 0;
  while (data.current < data.tables.size() && count < STANDARD_VECTOR_SIZE) {
    const auto &table = data.tables[data.current];
    const auto *schema = manager.get_table_schema(table);

    output.SetValue(0, count, Value(table));
    output.SetValue(1, count,
                    Value::BIGINT(schema ? schema->columns.size() : 0));

    data.current++;
    count++;
  }
  output.SetCardinality(count);
}

// ============================================================================
// dbsp_drop - Drop a view
// ============================================================================

void DropScalar(DataChunk &args, ExpressionState &state, Vector &result) {
  UnaryExecutor::Execute<string_t, string_t>(
      args.data[0], result, args.size(), [&](string_t name) {
        auto &manager = dbsp_native::get_cdc_manager();
        bool ok = manager.drop_view(name.GetString());
        return StringVector::AddString(result,
                                       ok ? "Dropped" : manager.last_error());
      });
}

// ============================================================================
// dbsp_drop_cascade - Drop a view and all dependent views
// ============================================================================

void DropCascadeScalar(DataChunk &args, ExpressionState &state,
                       Vector &result) {
  UnaryExecutor::Execute<string_t, string_t>(
      args.data[0], result, args.size(), [&](string_t name) {
        auto &manager = dbsp_native::get_cdc_manager();
        auto deps = manager.get_dependents(name.GetString());
        bool ok = manager.drop_view_cascade(name.GetString());
        string msg = ok ? "Dropped " + name.GetString() : "Not found";
        if (ok && !deps.empty()) {
          msg += " (and " + std::to_string(deps.size()) + " dependent views)";
        }
        return StringVector::AddString(result, msg);
      });
}

// ============================================================================
// dbsp_save - Save views to storage
// Usage: SELECT * FROM dbsp_save();           -- Save to DuckDB table
//        SELECT * FROM dbsp_save('file.json'); -- Save to JSON file
// ============================================================================

struct SaveBindData : public TableFunctionData {
  string view_name;      // Optional: specific view to save
  string target;         // Table name or file path
  string format;         // "table" or "json"
  bool save_all = false; // Save all views
  bool done = false;
};

unique_ptr<FunctionData> SaveBind(ClientContext &context,
                                  TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types,
                                  vector<string> &names) {
  auto data = make_uniq<SaveBindData>();

  if (input.inputs.empty()) {
    // dbsp_save() - save all to default table
    data->save_all = true;
    data->target = "_dbsp_views";
    data->format = "table";
  } else if (input.inputs.size() == 1) {
    // dbsp_save('filepath') - save all to file
    data->save_all = true;
    data->target = input.inputs[0].GetValue<string>();
    data->format = "json";
  } else if (input.inputs.size() == 2) {
    // dbsp_save('view_name', 'table_name') - save specific view to table
    data->view_name = input.inputs[0].GetValue<string>();
    data->target = input.inputs[1].GetValue<string>();
    data->format = "table";
  } else if (input.inputs.size() >= 3) {
    // dbsp_save('view_name', 'filepath', 'json') - save to file
    data->view_name = input.inputs[0].GetValue<string>();
    data->target = input.inputs[1].GetValue<string>();
    data->format = input.inputs[2].GetValue<string>();
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void SaveFunc(ClientContext &context, TableFunctionInput &input,
              DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<SaveBindData>();
  if (data.done)
    return;

  auto &manager = dbsp_native::get_cdc_manager();
  bool ok;
  string msg;

  if (data.format == "json") {
    ok = manager.save_to_file(data.target);
    msg = ok ? "Saved to file: " + data.target
             : "Error: " + manager.last_error();
  } else {
    // Table mode - not supported from table functions due to DuckDB transaction restrictions
    ok = false;
    msg = "Error: DuckDB table persistence not supported (use JSON files instead). Try: dbsp_save('view_name', 'file.json', 'json')";
  }

  if (!ok) {
    throw InvalidInputException("%s", msg);
  }

  output.SetCardinality(1);
  output.SetValue(0, 0, Value(msg));
  data.done = true;
}

// ============================================================================
// dbsp_load - Load views from storage
// Usage: SELECT * FROM dbsp_load();           -- Load from DuckDB table
//        SELECT * FROM dbsp_load('file.json'); -- Load from JSON file
// ============================================================================

struct LoadBindData : public TableFunctionData {
  string source;      // Table name or file path
  string format;      // "table" or "json"
  bool done = false;
};

unique_ptr<FunctionData> LoadBind(ClientContext &context,
                                  TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types,
                                  vector<string> &names) {
  auto data = make_uniq<LoadBindData>();

  if (input.inputs.empty()) {
    // dbsp_load() - load from default table
    data->source = "_dbsp_views";
    data->format = "table";
  } else if (input.inputs.size() == 1) {
    // dbsp_load('table_name') - load from specific table
    data->source = input.inputs[0].GetValue<string>();
    data->format = "table";
  } else if (input.inputs.size() >= 2) {
    // dbsp_load('filepath', 'json') - load from file
    data->source = input.inputs[0].GetValue<string>();
    data->format = input.inputs[1].GetValue<string>();
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void LoadFunc(ClientContext &context, TableFunctionInput &input,
              DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<LoadBindData>();
  if (data.done)
    return;

  auto &manager = dbsp_native::get_cdc_manager();
  bool ok;
  string msg;

  if (data.format == "json") {
    ok = manager.load_from_file(context, data.source);
    msg = ok ? "Loaded from file: " + data.source
             : "Error: " + manager.last_error();
  } else {
    // Table mode - not supported from table functions due to DuckDB transaction restrictions
    ok = false;
    msg = "Error: DuckDB table persistence not supported (use JSON files instead). Try: dbsp_load('file.json', 'json')";
  }

  if (!ok) {
    throw InvalidInputException("%s", msg);
  }

  // Count loaded views
  auto views = manager.list_views();
  msg += " (" + std::to_string(views.size()) + " views)";

  output.SetCardinality(1);
  output.SetValue(0, 0, Value(msg));
  data.done = true;
}

// ============================================================================
// dbsp_deps - Show view dependencies
// Usage: SELECT * FROM dbsp_deps('view_name');
// ============================================================================

struct DepsBindData : public TableFunctionData {
  string view_name;
  vector<std::pair<string, string>>
      deps; // (name, type: "depends_on" or "depended_by")
  idx_t current = 0;
};

unique_ptr<FunctionData> DepsBind(ClientContext &context,
                                  TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types,
                                  vector<string> &names) {
  auto data = make_uniq<DepsBindData>();

  if (input.inputs.empty()) {
    throw InvalidInputException("dbsp_deps(view_name)");
  }

  data->view_name = input.inputs[0].GetValue<string>();

  auto &manager = dbsp_native::get_cdc_manager();

  // Get dependencies (what this view depends on)
  auto deps = manager.get_view_dependencies(data->view_name);
  for (const auto &dep : deps) {
    data->deps.push_back({dep, "depends_on"});
  }

  // Get dependents (what depends on this view)
  auto dependents = manager.get_dependents(data->view_name);
  for (const auto &dep : dependents) {
    data->deps.push_back({dep, "depended_by"});
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("name");
  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("relationship");

  return std::move(data);
}

void DepsFunc(ClientContext &context, TableFunctionInput &input,
              DataChunk &output) {
  auto &data = input.bind_data->CastNoConst<DepsBindData>();

  idx_t count = 0;
  while (data.current < data.deps.size() && count < STANDARD_VECTOR_SIZE) {
    const auto &[name, rel] = data.deps[data.current];

    output.SetValue(0, count, Value(name));
    output.SetValue(1, count, Value(rel));

    data.current++;
    count++;
  }
  output.SetCardinality(count);
}

// ============================================================================
// Materialized View DDL - CREATE MATERIALIZED VIEW
// ============================================================================

struct CreateMaterializedViewData : public TableFunctionData {
  string view_name;
  string select_query;
  bool done = false;
};

unique_ptr<FunctionData>
CreateMaterializedViewBind(ClientContext &context,
                           TableFunctionBindInput &input,
                           vector<LogicalType> &return_types,
                           vector<string> &names) {
  auto data = make_uniq<CreateMaterializedViewData>();
  data->view_name = input.inputs[0].GetValue<string>();
  data->select_query = input.inputs[1].GetValue<string>();
  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void CreateMaterializedViewExecute(ClientContext &context,
                                   TableFunctionInput &input,
                                   DataChunk &output) {
  auto &state = input.bind_data->CastNoConst<CreateMaterializedViewData>();

  if (state.done) {
    output.SetCardinality(0);
    return;
  }

  auto &manager = dbsp_native::get_cdc_manager();
  bool success = manager.create_view(context, state.view_name, state.select_query);

  if (!success) {
    string error = manager.last_error();
    if (error.find("DBSP-E") == string::npos) {
      error = "Failed to create materialized view '" + state.view_name + "': " + error;
    }
    throw InvalidInputException(error);
  }

  // Return success message
  output.SetCardinality(1);
  auto info = manager.get_view_info(state.view_name);
  string sources = "";
  for (size_t i = 0; i < info.source_tables.size(); i++) {
    if (i > 0) sources += ", ";
    sources += info.source_tables[i];
  }

  string message = "Created materialized view: " + state.view_name +
                   " (sources: " + sources + ")";
  output.SetValue(0, 0, Value(message));

  state.done = true;
}

// ============================================================================
// Materialized View DDL - DROP MATERIALIZED VIEW
// ============================================================================

struct DropMaterializedViewData : public TableFunctionData {
  string view_name;
  bool cascade = false;
  bool done = false;
};

unique_ptr<FunctionData>
DropMaterializedViewBind(ClientContext &context,
                         TableFunctionBindInput &input,
                         vector<LogicalType> &return_types,
                         vector<string> &names) {
  auto data = make_uniq<DropMaterializedViewData>();
  data->view_name = input.inputs[0].GetValue<string>();
  data->cascade = input.inputs[1].GetValue<bool>();
  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void DropMaterializedViewExecute(ClientContext &context,
                                 TableFunctionInput &input, DataChunk &output) {
  auto &state = input.bind_data->CastNoConst<DropMaterializedViewData>();

  if (state.done) {
    output.SetCardinality(0);
    return;
  }

  auto &manager = dbsp_native::get_cdc_manager();

  // Check if view exists
  if (!manager.view_exists(state.view_name)) {
    // IF EXISTS was handled by parser, so this is an error
    throw InvalidInputException("Materialized view does not exist: " + state.view_name);
  }

  // Check dependencies
  auto dependents = manager.get_dependent_views(state.view_name);

  if (!dependents.empty() && !state.cascade) {
    // Build error message
    string dep_list = "";
    for (size_t i = 0; i < dependents.size() && i < 5; i++) {
      if (i > 0) dep_list += ", ";
      dep_list += dependents[i];
    }
    if (dependents.size() > 5) {
      dep_list += "... and " + std::to_string(dependents.size() - 5) + " more";
    }

    throw InvalidInputException(
      "Cannot drop materialized view '" + state.view_name +
      "': other views depend on it (" + dep_list + ")\n" +
      "Use DROP MATERIALIZED VIEW " + state.view_name + " CASCADE to drop with dependents");
  }

  // Drop the view (and dependents if cascade)
  size_t dropped_count = 1;
  if (state.cascade && !dependents.empty()) {
    // Drop dependents first (in reverse topological order)
    auto drop_order = manager.get_drop_order(state.view_name);
    for (const auto &view : drop_order) {
      manager.drop_view(view);
      dropped_count++;
    }
  } else {
    manager.drop_view(state.view_name);
  }

  // Return success message
  output.SetCardinality(1);
  string message = "Dropped materialized view: " + state.view_name;
  if (dropped_count > 1) {
    message += " (and " + std::to_string(dropped_count - 1) + " dependent views)";
  }
  output.SetValue(0, 0, Value(message));

  state.done = true;
}

// ============================================================================
// Materialized View DDL - REFRESH MATERIALIZED VIEW (no-op)
// ============================================================================

struct RefreshMaterializedViewData : public TableFunctionData {
  string view_name;
  bool done = false;
};

unique_ptr<FunctionData>
RefreshMaterializedViewBind(ClientContext &context,
                            TableFunctionBindInput &input,
                            vector<LogicalType> &return_types,
                            vector<string> &names) {
  auto data = make_uniq<RefreshMaterializedViewData>();
  data->view_name = input.inputs[0].GetValue<string>();
  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void RefreshMaterializedViewExecute(ClientContext &context,
                                    TableFunctionInput &input,
                                    DataChunk &output) {
  auto &state = input.bind_data->CastNoConst<RefreshMaterializedViewData>();

  if (state.done) {
    output.SetCardinality(0);
    return;
  }

  // REFRESH is a no-op since views are automatically incremental
  output.SetCardinality(1);
  output.SetValue(0, 0,
    Value("Materialized view '" + state.view_name +
          "' is always up-to-date (automatic incremental refresh)"));

  state.done = true;
}

} // namespace duckdb

// ============================================================================
// Parser Extension Plan Function
// ============================================================================

namespace dbsp_native {

ParserExtensionPlanResult MaterializedViewPlan(
    ParserExtensionInfo *info, ClientContext &context,
    unique_ptr<ParserExtensionParseData> parse_data_p) {

  ParserExtensionPlanResult result;

  // Handle CREATE MATERIALIZED VIEW
  if (auto *create_data = dynamic_cast<::dbsp_native::CreateMaterializedViewParseData *>(parse_data_p.get())) {
    TableFunction func("create_materialized_view",
                      {LogicalType::VARCHAR, LogicalType::VARCHAR},
                      CreateMaterializedViewExecute,
                      CreateMaterializedViewBind);

    result.function = func;
    result.parameters.push_back(Value(create_data->view_name));
    result.parameters.push_back(Value(create_data->select_query));
    result.return_type = StatementReturnType::QUERY_RESULT;

    return result;
  }

  // Handle DROP MATERIALIZED VIEW
  if (auto *drop_data = dynamic_cast<::dbsp_native::DropMaterializedViewParseData *>(parse_data_p.get())) {
    TableFunction func("drop_materialized_view",
                      {LogicalType::VARCHAR, LogicalType::BOOLEAN},
                      DropMaterializedViewExecute,
                      DropMaterializedViewBind);

    result.function = func;
    result.parameters.push_back(Value(drop_data->view_name));
    result.parameters.push_back(Value(drop_data->cascade));
    result.return_type = StatementReturnType::QUERY_RESULT;

    return result;
  }

  // Handle REFRESH MATERIALIZED VIEW
  if (auto *refresh_data = dynamic_cast<::dbsp_native::RefreshMaterializedViewParseData *>(parse_data_p.get())) {
    TableFunction func("refresh_materialized_view",
                      {LogicalType::VARCHAR},
                      RefreshMaterializedViewExecute,
                      RefreshMaterializedViewBind);

    result.function = func;
    result.parameters.push_back(Value(refresh_data->view_name));
    result.return_type = StatementReturnType::QUERY_RESULT;

    return result;
  }

  throw InternalException("Unknown materialized view statement type");
}

} // namespace dbsp_native

// ============================================================================
// Extension Entry Point
// ============================================================================

namespace duckdb {

void LoadInternal(ExtensionLoader &loader) {

  // Register parser extension for CREATE/DROP/REFRESH MATERIALIZED VIEW syntax
  auto &db = loader.GetDatabaseInstance();
  auto &config = DBConfig::GetConfig(db);
  config.parser_extensions.push_back(::dbsp_native::CreateMaterializedViewParserExtension());

  // dbsp_track
  TableFunction track_fn("dbsp_track", {LogicalType::VARCHAR}, TrackFunc,
                         TrackBind);

  loader.RegisterFunction(track_fn);

  // dbsp_create_view (varargs for both SQL and simple mode)
  TableFunction create_fn("dbsp_create_view", {}, CreateViewFunc,
                          CreateViewBind);
  create_fn.varargs = LogicalType::VARCHAR;
  loader.RegisterFunction(create_fn);

  // dbsp_notify_insert
  TableFunction notify_insert_fn("dbsp_notify_insert", {}, NotifyInsertFunc,
                                 NotifyBind);
  notify_insert_fn.varargs = LogicalType::ANY;
  loader.RegisterFunction(notify_insert_fn);

  // dbsp_notify_delete
  TableFunction notify_delete_fn("dbsp_notify_delete", {}, NotifyDeleteFunc,
                                 NotifyBind);
  notify_delete_fn.varargs = LogicalType::ANY;
  loader.RegisterFunction(notify_delete_fn);

  // dbsp_sync
  TableFunction sync_fn("dbsp_sync", {}, SyncFunc, SyncBind);
  sync_fn.varargs = LogicalType::VARCHAR;
  loader.RegisterFunction(sync_fn);

  // dbsp_query
  TableFunction query_fn("dbsp_query", {LogicalType::VARCHAR}, QueryFunc,
                         QueryBind);
  loader.RegisterFunction(query_fn);

  // dbsp_views
  TableFunction list_views_fn("dbsp_views", {}, ListViewsFunc, ListViewsBind);
  loader.RegisterFunction(list_views_fn);

  // dbsp_tables
  TableFunction list_tables_fn("dbsp_tables", {}, ListTablesFunc,
                               ListTablesBind);
  loader.RegisterFunction(list_tables_fn);

  // dbsp_drop
  ScalarFunction drop_fn("dbsp_drop", {LogicalType::VARCHAR},
                         LogicalType::VARCHAR, DropScalar);
  loader.RegisterFunction(drop_fn);

  // dbsp_drop_cascade
  ScalarFunction drop_cascade_fn("dbsp_drop_cascade", {LogicalType::VARCHAR},
                                 LogicalType::VARCHAR, DropCascadeScalar);
  loader.RegisterFunction(drop_cascade_fn);

  // dbsp_save
  TableFunction save_fn("dbsp_save", {}, SaveFunc, SaveBind);
  save_fn.varargs = LogicalType::VARCHAR;
  loader.RegisterFunction(save_fn);

  // dbsp_load
  TableFunction load_fn("dbsp_load", {}, LoadFunc, LoadBind);
  load_fn.varargs = LogicalType::VARCHAR;
  loader.RegisterFunction(load_fn);

  // dbsp_deps
  TableFunction deps_fn("dbsp_deps", {LogicalType::VARCHAR}, DepsFunc,
                        DepsBind);
  loader.RegisterFunction(deps_fn);
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dbsp, loader) {
  duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *dbsp_version() {
  return "0.1.0";
}

}
