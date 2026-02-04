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
//   SELECT * FROM dbsp_create_view('high_value', 'SELECT * FROM orders WHERE amount > 100');
//   SELECT * FROM dbsp_create_view('totals', 'SELECT customer_id, SUM(amount) FROM orders GROUP BY customer_id');
//
//   -- Cascading views (views on views)
//   SELECT * FROM dbsp_create_view('vip_totals', 'SELECT * FROM totals WHERE SUM > 1000');
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
#include "duckdb/main/extension_util.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include "dbsp_cdc.hpp"

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

unique_ptr<FunctionData> TrackBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<TrackBindData>();

    if (input.inputs.empty()) {
        throw InvalidInputException("dbsp_track(table_name)");
    }

    data->table_name = input.inputs[0].GetValue<string>();

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("result");
    return std::move(data);
}

void TrackFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<TrackBindData>();
    if (data.done) return;

    auto &manager = dbsp_native::get_cdc_manager();
    bool ok = manager.track_table(context, data.table_name);

    output.SetCardinality(1);
    if (ok) {
        auto schema = manager.get_table_schema(data.table_name);
        string cols = schema ? std::to_string(schema->columns.size()) + " columns" : "";
        output.SetValue(0, 0, Value("Tracking table: " + data.table_name + " (" + cols + ")"));
    } else {
        output.SetValue(0, 0, Value("Failed to track: " + data.table_name));
    }
    data.done = true;
}

// ============================================================================
// dbsp_create_view - Create a materialized view
// Usage (SQL): SELECT * FROM dbsp_create_view('name', 'SELECT ... FROM ...');
// Usage (simple): SELECT * FROM dbsp_create_view('name', 'table', 'type', 'spec');
// ============================================================================

struct CreateViewBindData : public TableFunctionData {
    string view_name;
    string sql_or_table;
    string view_type;
    string spec;
    bool is_sql_mode = false;
    bool done = false;
};

unique_ptr<FunctionData> CreateViewBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<CreateViewBindData>();

    if (input.inputs.size() < 2) {
        throw InvalidInputException("dbsp_create_view(name, sql) or dbsp_create_view(name, table, type, spec)");
    }

    data->view_name = input.inputs[0].GetValue<string>();
    data->sql_or_table = input.inputs[1].GetValue<string>();

    // Detect mode: if second arg starts with SELECT, it's SQL mode
    string upper = StringUtil::Upper(StringUtil::Trim(data->sql_or_table));
    if (upper.rfind("SELECT", 0) == 0) {
        data->is_sql_mode = true;
    } else if (input.inputs.size() >= 4) {
        data->view_type = input.inputs[2].GetValue<string>();
        data->spec = input.inputs[3].GetValue<string>();
        data->is_sql_mode = false;
    } else {
        throw InvalidInputException("Use SQL syntax: dbsp_create_view('name', 'SELECT ...')");
    }

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("result");
    return std::move(data);
}

void CreateViewFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<CreateViewBindData>();
    if (data.done) return;

    auto &manager = dbsp_native::get_cdc_manager();
    string result;

    if (data.is_sql_mode) {
        // SQL mode - use the parser
        bool ok = manager.create_view(context, data.view_name, data.sql_or_table);
        if (ok) {
            auto info = manager.get_view_info(data.view_name);
            result = "Created view: " + data.view_name + " (sources: ";
            for (size_t i = 0; i < info.source_tables.size(); i++) {
                if (i > 0) result += ", ";
                result += info.source_tables[i];
            }
            result += ")";
        } else {
            result = "Error: " + manager.last_error();
        }
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
                sql = "SELECT " + parts[0] + ", " + parts[1] + "(" + parts[2] + ") FROM " + table + " GROUP BY " + parts[0];
            }
        } else if (type == "distinct") {
            sql = "SELECT DISTINCT * FROM " + table;
        } else {
            sql = "SELECT * FROM " + table;
        }

        bool ok = manager.create_view(context, data.view_name, sql);
        result = ok ? "Created " + type + " view: " + data.view_name
                    : "Error: " + manager.last_error();
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

unique_ptr<FunctionData> NotifyBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<NotifyBindData>();

    if (input.inputs.size() < 2) {
        throw InvalidInputException("dbsp_notify_insert/delete(table, val1, val2, ...)");
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

void NotifyInsertFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<NotifyBindData>();
    if (data.done) return;

    auto &manager = dbsp_native::get_cdc_manager();
    manager.on_insert(data.table_name, data.row);

    output.SetCardinality(1);
    output.SetValue(0, 0, Value("Notified insert into " + data.table_name));
    data.done = true;
}

void NotifyDeleteFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<NotifyBindData>();
    if (data.done) return;

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

unique_ptr<FunctionData> SyncBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<string> &names) {
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

void SyncFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<SyncBindData>();
    if (data.done) return;

    auto &manager = dbsp_native::get_cdc_manager();

    if (data.sync_all) {
        manager.sync_all(context);
        output.SetCardinality(1);
        output.SetValue(0, 0, Value("Synced all tracked tables"));
    } else {
        bool ok = manager.sync_table(context, data.table_name);
        output.SetCardinality(1);
        output.SetValue(0, 0, Value(ok ? "Synced: " + data.table_name : "Failed to sync"));
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

unique_ptr<FunctionData> QueryBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
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

void QueryFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<QueryBindData>();

    idx_t count = 0;
    while (data.current < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
        const auto &row = data.rows[data.current];

        for (idx_t col = 0; col < output.ColumnCount() && col < row.columns.size(); col++) {
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

unique_ptr<FunctionData> ListViewsBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
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

void ListViewsFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
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

unique_ptr<FunctionData> ListTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<ListTablesBindData>();

    auto &manager = dbsp_native::get_cdc_manager();
    data->tables = manager.list_tracked_tables();

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("table_name");
    return_types.push_back(LogicalType::BIGINT);
    names.push_back("columns");

    return std::move(data);
}

void ListTablesFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<ListTablesBindData>();

    auto &manager = dbsp_native::get_cdc_manager();

    idx_t count = 0;
    while (data.current < data.tables.size() && count < STANDARD_VECTOR_SIZE) {
        const auto &table = data.tables[data.current];
        const auto *schema = manager.get_table_schema(table);

        output.SetValue(0, count, Value(table));
        output.SetValue(1, count, Value::BIGINT(schema ? schema->columns.size() : 0));

        data.current++;
        count++;
    }
    output.SetCardinality(count);
}

// ============================================================================
// dbsp_drop - Drop a view
// ============================================================================

void DropScalar(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t name) {
        auto &manager = dbsp_native::get_cdc_manager();
        bool ok = manager.drop_view(name.GetString());
        return StringVector::AddString(result, ok ? "Dropped" : manager.last_error());
    });
}

// ============================================================================
// dbsp_drop_cascade - Drop a view and all dependent views
// ============================================================================

void DropCascadeScalar(DataChunk &args, ExpressionState &state, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t name) {
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
    string filepath;
    bool to_file = false;
    bool done = false;
};

unique_ptr<FunctionData> SaveBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<SaveBindData>();

    if (!input.inputs.empty()) {
        data->filepath = input.inputs[0].GetValue<string>();
        data->to_file = true;
    }

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("result");
    return std::move(data);
}

void SaveFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<SaveBindData>();
    if (data.done) return;

    auto &manager = dbsp_native::get_cdc_manager();
    bool ok;
    string msg;

    if (data.to_file) {
        ok = manager.save_to_file(data.filepath);
        msg = ok ? "Saved to file: " + data.filepath : "Error: " + manager.last_error();
    } else {
        ok = manager.save_to_table(context);
        msg = ok ? "Saved to DuckDB table: _dbsp_views" : "Error: " + manager.last_error();
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
    string filepath;
    bool from_file = false;
    bool done = false;
};

unique_ptr<FunctionData> LoadBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<LoadBindData>();

    if (!input.inputs.empty()) {
        data->filepath = input.inputs[0].GetValue<string>();
        data->from_file = true;
    }

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("result");
    return std::move(data);
}

void LoadFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<LoadBindData>();
    if (data.done) return;

    auto &manager = dbsp_native::get_cdc_manager();
    bool ok;
    string msg;

    if (data.from_file) {
        ok = manager.load_from_file(context, data.filepath);
        msg = ok ? "Loaded from file: " + data.filepath : "Error: " + manager.last_error();
    } else {
        ok = manager.load_from_table(context);
        msg = ok ? "Loaded from DuckDB table" : "Error: " + manager.last_error();
    }

    // Count loaded views
    auto views = manager.list_views();
    if (ok) {
        msg += " (" + std::to_string(views.size()) + " views)";
    }

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
    vector<std::pair<string, string>> deps;  // (name, type: "depends_on" or "depended_by")
    idx_t current = 0;
};

unique_ptr<FunctionData> DepsBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<string> &names) {
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

void DepsFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
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
// Extension Entry Point
// ============================================================================

static void LoadInternal(DatabaseInstance &instance) {
    // dbsp_track
    TableFunction track_fn("dbsp_track", {LogicalType::VARCHAR}, TrackFunc, TrackBind);
    ExtensionUtil::RegisterFunction(instance, track_fn);

    // dbsp_create_view (varargs for both SQL and simple mode)
    TableFunction create_fn("dbsp_create_view", {}, CreateViewFunc, CreateViewBind);
    create_fn.varargs = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(instance, create_fn);

    // dbsp_notify_insert
    TableFunction notify_insert_fn("dbsp_notify_insert", {}, NotifyInsertFunc, NotifyBind);
    notify_insert_fn.varargs = LogicalType::ANY;
    ExtensionUtil::RegisterFunction(instance, notify_insert_fn);

    // dbsp_notify_delete
    TableFunction notify_delete_fn("dbsp_notify_delete", {}, NotifyDeleteFunc, NotifyBind);
    notify_delete_fn.varargs = LogicalType::ANY;
    ExtensionUtil::RegisterFunction(instance, notify_delete_fn);

    // dbsp_sync
    TableFunction sync_fn("dbsp_sync", {}, SyncFunc, SyncBind);
    sync_fn.varargs = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(instance, sync_fn);

    // dbsp_query
    TableFunction query_fn("dbsp_query", {LogicalType::VARCHAR}, QueryFunc, QueryBind);
    ExtensionUtil::RegisterFunction(instance, query_fn);

    // dbsp_views
    TableFunction list_views_fn("dbsp_views", {}, ListViewsFunc, ListViewsBind);
    ExtensionUtil::RegisterFunction(instance, list_views_fn);

    // dbsp_tables
    TableFunction list_tables_fn("dbsp_tables", {}, ListTablesFunc, ListTablesBind);
    ExtensionUtil::RegisterFunction(instance, list_tables_fn);

    // dbsp_drop
    ScalarFunction drop_fn("dbsp_drop", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DropScalar);
    ExtensionUtil::RegisterFunction(instance, drop_fn);

    // dbsp_drop_cascade
    ScalarFunction drop_cascade_fn("dbsp_drop_cascade", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DropCascadeScalar);
    ExtensionUtil::RegisterFunction(instance, drop_cascade_fn);

    // dbsp_save
    TableFunction save_fn("dbsp_save", {}, SaveFunc, SaveBind);
    save_fn.varargs = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(instance, save_fn);

    // dbsp_load
    TableFunction load_fn("dbsp_load", {}, LoadFunc, LoadBind);
    load_fn.varargs = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(instance, load_fn);

    // dbsp_deps
    TableFunction deps_fn("dbsp_deps", {LogicalType::VARCHAR}, DepsFunc, DepsBind);
    ExtensionUtil::RegisterFunction(instance, deps_fn);
}

void DbspExtension::Load(DuckDB &db) {
    LoadInternal(*db.instance);
}

std::string DbspExtension::Name() {
    return "dbsp";
}

std::string DbspExtension::Version() const {
    return "3.0.0";
}

} // namespace duckdb

extern "C" {
DUCKDB_EXTENSION_API void dbsp_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB db_wrapper(db);
    db_wrapper.LoadExtension<duckdb::DbspExtension>();
}

DUCKDB_EXTENSION_API const char *dbsp_version() {
    return "3.0.0";
}
}
