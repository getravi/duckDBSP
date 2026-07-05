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

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include "dbsp_cdc.hpp"
#include "dbsp_context_state.hpp"
#include "dbsp_parser_extension.hpp"
#include "dbsp_recovery.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

// Helper: Ensure DBSPContextState is attached to the context
// This is necessary because OnConnectionOpened isn't always called for the
// initial connection when loading the extension
static void EnsureContextState(ClientContext &context) {
  auto params = context.registered_state->Get<dbsp_native::DBSPContextState>(
      "dbsp_cdc_state");
  if (!params) {
    std::cerr << "DBSP: Lazily attaching ContextState to " << &context << "\n";
    context.registered_state->GetOrCreate<dbsp_native::DBSPContextState>(
        "dbsp_cdc_state");
  }
}

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
  EnsureContextState(context);
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
      formatted_error =
          "Failed to track table '" + data.table_name + "': " + formatted_error;
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

  // Detect mode: if second arg starts with SELECT or WITH (CTEs, including
  // WITH RECURSIVE), it's SQL mode
  string trimmed = data->sql_or_table;
  string upper = StringUtil::Upper(trimmed);
  if (upper.rfind("SELECT", 0) == 0 || upper.rfind("WITH", 0) == 0) {
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
  EnsureContextState(context);
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
  EnsureContextState(context);
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
  EnsureContextState(context);
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
  EnsureContextState(context);
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
  const auto *view = manager.get_view(data->view_name);
  const auto *schema = manager.get_view_schema(data->view_name);

  if (!view) {
    throw InvalidInputException("View not found: " + data->view_name);
  }

  // Collect rows using scan (supports ordered iteration for sort/limit views)
  view->scan(
      [&](const dbsp_native::DuckDBRow &row, dbsp_native::Weight weight) {
        if (weight > 0) {
          for (int64_t i = 0; i < weight; i++) {
            data->rows.push_back(row);
          }
        }
      });

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
  EnsureContextState(context);
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
    msg =
        ok ? "Saved to file: " + data.target : "Error: " + manager.last_error();
  } else {
    // Table mode - not supported from table functions due to DuckDB
    // transaction restrictions
    ok = false;
    msg = "Error: DuckDB table persistence not supported (use JSON files "
          "instead). Try: dbsp_save('view_name', 'file.json', 'json')";
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
  string source; // Table name or file path
  string format; // "table" or "json"
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
    // Table mode - not supported from table functions due to DuckDB
    // transaction restrictions
    ok = false;
    msg = "Error: DuckDB table persistence not supported (use JSON files "
          "instead). Try: dbsp_load('file.json', 'json')";
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
// dbsp_auto_sync - Enable/disable automatic CDC on transaction commit
// Usage: SELECT * FROM dbsp_auto_sync(true);   -- Enable
//        SELECT * FROM dbsp_auto_sync(false);  -- Disable
//        SELECT * FROM dbsp_auto_sync();       -- Query status
// ============================================================================

struct AutoSyncBindData : public TableFunctionData {
  bool enable = false;
  bool query_only = false;
  bool done = false;
};

unique_ptr<FunctionData> AutoSyncBind(ClientContext &context,
                                      TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types,
                                      vector<string> &names) {
  auto data = make_uniq<AutoSyncBindData>();

  if (input.inputs.empty()) {
    data->query_only = true;
  } else {
    data->enable = input.inputs[0].GetValue<bool>();
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void AutoSyncFunc(ClientContext &context, TableFunctionInput &input,
                  DataChunk &output) {
  EnsureContextState(context);
  auto &data = input.bind_data->CastNoConst<AutoSyncBindData>();
  if (data.done)
    return;

  auto &manager = dbsp_native::get_cdc_manager();

  if (data.query_only) {
    bool enabled = manager.is_auto_sync_enabled();
    output.SetCardinality(1);
    output.SetValue(
        0, 0,
        Value(string("Auto-sync is ") + (enabled ? "ENABLED" : "DISABLED")));
  } else {
    if (data.enable) {
      manager.enable_auto_sync();
      output.SetCardinality(1);
      output.SetValue(
          0, 0,
          Value("Auto-sync ENABLED: views will update on transaction commit"));
    } else {
      manager.disable_auto_sync();
      output.SetCardinality(1);
      output.SetValue(
          0, 0,
          Value("Auto-sync DISABLED: use dbsp_sync() for manual updates"));
    }
  }
  data.done = true;
}

// ============================================================================
// dbsp_use_planner - Enable/disable the planner frontend (Phase B)
// Usage: SELECT * FROM dbsp_use_planner(true);   -- Enable
//        SELECT * FROM dbsp_use_planner(false);  -- Disable
//        SELECT * FROM dbsp_use_planner();       -- Query status
// ============================================================================

struct UsePlannerBindData : public TableFunctionData {
  bool enable = false;
  bool query_only = false;
  bool done = false;
};

unique_ptr<FunctionData> UsePlannerBind(ClientContext &context,
                                        TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types,
                                        vector<string> &names) {
  auto data = make_uniq<UsePlannerBindData>();

  if (input.inputs.empty()) {
    data->query_only = true;
  } else {
    data->enable = input.inputs[0].GetValue<bool>();
  }

  return_types.push_back(LogicalType::VARCHAR);
  names.push_back("result");
  return std::move(data);
}

void UsePlannerFunc(ClientContext &context, TableFunctionInput &input,
                    DataChunk &output) {
  EnsureContextState(context);
  auto &data = input.bind_data->CastNoConst<UsePlannerBindData>();
  if (data.done)
    return;

  // The planner is the only frontend since Phase C5 (the bespoke parser was
  // deleted); toggling is a no-op kept for backwards compatibility
  output.SetCardinality(1);
  output.SetValue(0, 0,
                  Value("Planner frontend is ENABLED (always: the bespoke "
                        "parser was removed in Phase C5)"));
  (void)data.query_only;
  (void)data.enable;
  data.done = true;
}

// ============================================================================
// Materialized View DDL - CREATE MATERIALIZED VIEW
// ============================================================================

struct CreateMaterializedViewData : public TableFunctionData {
  string view_name;
  string select_query;
  bool done = false;
};

unique_ptr<FunctionData> CreateMaterializedViewBind(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &names) {
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
  EnsureContextState(context);
  auto &state = input.bind_data->CastNoConst<CreateMaterializedViewData>();

  if (state.done) {
    output.SetCardinality(0);
    return;
  }

  auto &manager = dbsp_native::get_cdc_manager();
  bool success =
      manager.create_view(context, state.view_name, state.select_query);

  if (!success) {
    string error = manager.last_error();
    if (error.find("DBSP-E") == string::npos) {
      error = "Failed to create materialized view '" + state.view_name +
              "': " + error;
    }
    throw InvalidInputException(error);
  }

  // Return success message
  output.SetCardinality(1);
  auto info = manager.get_view_info(state.view_name);
  string sources = "";
  for (size_t i = 0; i < info.source_tables.size(); i++) {
    if (i > 0)
      sources += ", ";
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
DropMaterializedViewBind(ClientContext &context, TableFunctionBindInput &input,
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
    throw InvalidInputException("Materialized view does not exist: " +
                                state.view_name);
  }

  // Check dependencies
  auto dependents = manager.get_dependent_views(state.view_name);

  if (!dependents.empty() && !state.cascade) {
    // Build error message
    string dep_list = "";
    for (size_t i = 0; i < dependents.size() && i < 5; i++) {
      if (i > 0)
        dep_list += ", ";
      dep_list += dependents[i];
    }
    if (dependents.size() > 5) {
      dep_list += "... and " + std::to_string(dependents.size() - 5) + " more";
    }

    throw InvalidInputException(
        "Cannot drop materialized view '" + state.view_name +
        "': other views depend on it (" + dep_list + ")\n" +
        "Use DROP MATERIALIZED VIEW " + state.view_name +
        " CASCADE to drop with dependents");
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
    message +=
        " (and " + std::to_string(dropped_count - 1) + " dependent views)";
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

unique_ptr<FunctionData> RefreshMaterializedViewBind(
    ClientContext &context, TableFunctionBindInput &input,
    vector<LogicalType> &return_types, vector<string> &names) {
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
  output.SetValue(
      0, 0,
      Value("Materialized view '" + state.view_name +
            "' is always up-to-date (automatic incremental refresh)"));

  state.done = true;
}

} // namespace duckdb

// ============================================================================
// Parser Extension Plan Function
// ============================================================================

namespace dbsp_native {

ParserExtensionPlanResult
MaterializedViewPlan(ParserExtensionInfo *info, ClientContext &context,
                     unique_ptr<ParserExtensionParseData> parse_data_p) {

  ParserExtensionPlanResult result;

  // Handle CREATE MATERIALIZED VIEW
  if (auto *create_data =
          dynamic_cast<::dbsp_native::CreateMaterializedViewParseData *>(
              parse_data_p.get())) {
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
  if (auto *drop_data =
          dynamic_cast<::dbsp_native::DropMaterializedViewParseData *>(
              parse_data_p.get())) {
    TableFunction func("drop_materialized_view",
                       {LogicalType::VARCHAR, LogicalType::BOOLEAN},
                       DropMaterializedViewExecute, DropMaterializedViewBind);

    result.function = func;
    result.parameters.push_back(Value(drop_data->view_name));
    result.parameters.push_back(Value(drop_data->cascade));
    result.return_type = StatementReturnType::QUERY_RESULT;

    return result;
  }

  // Handle REFRESH MATERIALIZED VIEW
  if (auto *refresh_data =
          dynamic_cast<::dbsp_native::RefreshMaterializedViewParseData *>(
              parse_data_p.get())) {
    TableFunction func("refresh_materialized_view", {LogicalType::VARCHAR},
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

// Extensions
// ============================================================================

// Extensions
// ============================================================================

using namespace duckdb;

// Extension Callback & Loading
// ============================================================================

class DBSPExtensionCallback : public ExtensionCallback {
public:
  void OnConnectionOpened(ClientContext &context) override {
    std::cerr << "DBSP: OnConnectionOpened called (context: " << &context
              << ")\n";

    // First-time initialization: recover from crash if needed.
    // Skip for DBSP's own helper connections (recovery itself opens
    // connections; recursing here would re-enter recovery).
    static bool recovery_done = false;
    if (!recovery_done && dbsp_native::internal_query_depth == 0) {
      // Set before recovering: connections opened during recovery must not
      // re-trigger it.
      recovery_done = true;
      auto &recovery_manager = dbsp_native::get_recovery_manager();

      // Get database path from context
      std::string db_path = "";
      try {
        // Try to get database path from attached database
        auto &db_manager = DatabaseManager::Get(context);
        auto default_db = db_manager.GetDatabase(context, DEFAULT_SCHEMA);
        if (default_db) {
          db_path = default_db->GetName();
        }
      } catch (...) {
        // If we can't get db path, use default recovery path
      }

      // Perform recovery (will initialize persistence and load views)
      recovery_manager.recover_from_crash(context, db_path);
    }

    // Attach our context state for transaction hooking
    auto &config = DBConfig::GetConfig(context);

    // Register the context state to receive transaction events
    context.registered_state->GetOrCreate<dbsp_native::DBSPContextState>(
        "dbsp_cdc_state");

    std::cerr << "DBSP: ContextState registered via registered_state\n";
  }
};

static void LoadInternal(ExtensionLoader &loader) {
  auto &instance = loader.GetDatabaseInstance();
  auto &config = DBConfig::GetConfig(instance);

  // Register extension callback
  ExtensionCallback::Register(config, make_shared_ptr<DBSPExtensionCallback>());

  // Register table functions
  TableFunction track_func("dbsp_track", {LogicalType::VARCHAR}, TrackFunc,
                           TrackBind);
  loader.RegisterFunction(track_func);

  TableFunction create_view_func("dbsp_create_view",
                                 {LogicalType::VARCHAR, LogicalType::VARCHAR},
                                 CreateViewFunc, CreateViewBind);
  // Optional args for simple mode
  create_view_func.varargs = LogicalType::ANY;
  loader.RegisterFunction(create_view_func);

  // Create Materialized View DDL support
  // Note: We cannot register actual SQL syntax here (ParserExtension needed for
  // that) But we can expose the function so our parser extension can call it ?
  // Or just rely on the table function

  // Register Create Materialized View Table Function (internal)
  TableFunction create_mv_func("dbsp_create_materialized_view",
                               {LogicalType::VARCHAR, LogicalType::VARCHAR},
                               CreateMaterializedViewExecute,
                               CreateMaterializedViewBind);
  loader.RegisterFunction(create_mv_func);

  TableFunction insert_func("dbsp_notify_insert", {LogicalType::VARCHAR},
                            NotifyInsertFunc, NotifyBind);
  insert_func.varargs = LogicalType::ANY; // For column values
  loader.RegisterFunction(insert_func);

  TableFunction delete_func("dbsp_notify_delete", {LogicalType::VARCHAR},
                            NotifyDeleteFunc, NotifyBind);
  delete_func.varargs = LogicalType::ANY; // For column values
  loader.RegisterFunction(delete_func);

  TableFunction sync_func("dbsp_sync", {}, SyncFunc, SyncBind);
  sync_func.varargs = LogicalType::VARCHAR; // Optional table name
  loader.RegisterFunction(sync_func);

  TableFunction query_func("dbsp_query", {LogicalType::VARCHAR}, QueryFunc,
                           QueryBind);
  loader.RegisterFunction(query_func);

  TableFunction list_views_func("dbsp_views", {}, ListViewsFunc, ListViewsBind);
  loader.RegisterFunction(list_views_func);

  TableFunction list_tables_func("dbsp_tables", {}, ListTablesFunc,
                                 ListTablesBind);
  loader.RegisterFunction(list_tables_func);

  TableFunction deps_func("dbsp_deps", {LogicalType::VARCHAR}, DepsFunc,
                          DepsBind);
  loader.RegisterFunction(deps_func);

  TableFunction save_func("dbsp_save", {}, SaveFunc, SaveBind);
  save_func.varargs = LogicalType::VARCHAR;
  loader.RegisterFunction(save_func);

  TableFunction load_func("dbsp_load", {}, LoadFunc, LoadBind);
  load_func.varargs = LogicalType::VARCHAR;
  loader.RegisterFunction(load_func);

  TableFunction auto_sync_func("dbsp_auto_sync", {}, AutoSyncFunc,
                               AutoSyncBind);
  auto_sync_func.varargs = LogicalType::BOOLEAN;
  loader.RegisterFunction(auto_sync_func);

  TableFunction use_planner_func("dbsp_use_planner", {}, UsePlannerFunc,
                                 UsePlannerBind);
  use_planner_func.varargs = LogicalType::BOOLEAN;
  loader.RegisterFunction(use_planner_func);

  // Register scalar functions
  CreateScalarFunctionInfo drop_func_info(
      ScalarFunction("dbsp_drop_view", {LogicalType::VARCHAR},
                     LogicalType::VARCHAR, DropScalar));
  loader.RegisterFunction(drop_func_info);

  // Alias: dbsp_drop (used by tests and shorter API)
  CreateScalarFunctionInfo drop_alias_info(ScalarFunction(
      "dbsp_drop", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DropScalar));
  loader.RegisterFunction(drop_alias_info);

  CreateScalarFunctionInfo drop_cascade_func_info(
      ScalarFunction("dbsp_drop_view_cascade", {LogicalType::VARCHAR},
                     LogicalType::VARCHAR, DropCascadeScalar));
  loader.RegisterFunction(drop_cascade_func_info);

  // Alias: dbsp_drop_cascade (used by tests and shorter API)
  CreateScalarFunctionInfo drop_cascade_alias_info(
      ScalarFunction("dbsp_drop_cascade", {LogicalType::VARCHAR},
                     LogicalType::VARCHAR, DropCascadeScalar));
  loader.RegisterFunction(drop_cascade_alias_info);

  // Initialize Parser Extension for SQL syntax support
  auto &extension_manager = instance.GetExtensionManager();
  // We need to register the parser extension
  ParserExtension::Register(
      config, dbsp_native::CreateMaterializedViewParserExtension());
}

#ifndef EXT_VERSION_DBSP
#define EXT_VERSION_DBSP "1.0.0"
#endif

namespace duckdb {

class DbspExtension : public Extension {
public:
  void Load(ExtensionLoader &loader) override;
  std::string Name() override;
  std::string Version() const override;
};

void DbspExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }

std::string DbspExtension::Name() { return "dbsp"; }

std::string DbspExtension::Version() const { return EXT_VERSION_DBSP; }

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dbsp, loader) {
  duckdb::DbspExtension extension;
  extension.Load(loader);
}
}
