// DBSP DuckDB Extension - Real-Time Incremental Materialized Views
//
// Usage:
//   LOAD 'dbsp';
//   SELECT * FROM dbsp_create_view('view_name', 'table', 'filter', '2 > 100');
//   SELECT * FROM dbsp_insert('table', 1, 100, 250);
//   SELECT * FROM dbsp_query('view_name');

#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include "../include/dbsp_materialized_view.hpp"

#include <mutex>

namespace duckdb {

// Global DBSP state
static dbsp::MaterializedViewManager g_views;
static std::mutex g_mutex;

// ============================================================================
// dbsp_create_view - Create a materialized view
// Usage: SELECT * FROM dbsp_create_view('name', 'table', 'type', 'spec');
//   Types: 'filter', 'aggregate', 'distinct'
//   Spec for filter: 'col_idx op value' e.g., '2 > 100'
//   Spec for aggregate: 'group_col agg_type value_col' e.g., '1 SUM 2'
// ============================================================================

struct CreateViewBindData : public TableFunctionData {
    string result;
    bool done = false;
};

unique_ptr<FunctionData> CreateViewBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<CreateViewBindData>();

    if (input.inputs.size() < 4) {
        throw InvalidInputException("dbsp_create_view(name, table, type, spec)");
    }

    string view_name = input.inputs[0].GetValue<string>();
    string table_name = input.inputs[1].GetValue<string>();
    string view_type = StringUtil::Lower(input.inputs[2].GetValue<string>());
    string spec = input.inputs[3].GetValue<string>();

    std::lock_guard<std::mutex> lock(g_mutex);

    if (view_type == "filter") {
        // Parse spec: "col_idx op value"
        auto parts = StringUtil::Split(spec, ' ');
        if (parts.size() < 3) {
            throw InvalidInputException("Filter spec: 'col_idx op value'");
        }

        int col_idx = std::stoi(parts[0]);
        string op = parts[1];
        string val_str = parts[2];

        // Parse value
        int64_t val_int = 0;
        bool is_int = true;
        try {
            val_int = std::stoll(val_str);
        } catch (...) {
            is_int = false;
        }

        auto pred = [col_idx, op, val_int, val_str, is_int](const dbsp::Row &row) -> bool {
            if (col_idx >= (int)row.columns.size()) return false;
            const auto &col = row.columns[col_idx];

            if (is_int && std::holds_alternative<int64_t>(col)) {
                int64_t lhs = std::get<int64_t>(col);
                if (op == "=") return lhs == val_int;
                if (op == ">") return lhs > val_int;
                if (op == "<") return lhs < val_int;
                if (op == ">=") return lhs >= val_int;
                if (op == "<=") return lhs <= val_int;
                if (op == "!=" || op == "<>") return lhs != val_int;
            }
            if (std::holds_alternative<std::string>(col)) {
                const auto &lhs = std::get<std::string>(col);
                if (op == "=") return lhs == val_str;
                if (op == "!=") return lhs != val_str;
            }
            return false;
        };

        g_views.register_view(std::make_unique<dbsp::FilteredView>(view_name, table_name, pred));
        data->result = "Created filter view: " + view_name;

    } else if (view_type == "aggregate") {
        // Parse spec: "group_col agg_type value_col"
        auto parts = StringUtil::Split(spec, ' ');
        if (parts.size() < 3) {
            throw InvalidInputException("Aggregate spec: 'group_col AGG value_col'");
        }

        int group_col = std::stoi(parts[0]);
        string agg = StringUtil::Upper(parts[1]);
        int value_col = std::stoi(parts[2]);

        dbsp::AggregateView::AggType agg_type = dbsp::AggregateView::AggType::SUM;
        if (agg == "COUNT") agg_type = dbsp::AggregateView::AggType::COUNT;
        else if (agg == "AVG") agg_type = dbsp::AggregateView::AggType::AVG;

        auto key_fn = [group_col](const dbsp::Row &r) -> dbsp::Row {
            dbsp::Row k;
            if (group_col < (int)r.columns.size()) k.columns.push_back(r.columns[group_col]);
            return k;
        };
        auto val_fn = [value_col](const dbsp::Row &r) -> int64_t {
            if (value_col < (int)r.columns.size()) {
                if (auto *p = std::get_if<int64_t>(&r.columns[value_col])) return *p;
                if (auto *p = std::get_if<double>(&r.columns[value_col])) return (int64_t)*p;
            }
            return 0;
        };

        g_views.register_view(std::make_unique<dbsp::AggregateView>(view_name, table_name, key_fn, val_fn, agg_type));
        data->result = "Created aggregate view: " + view_name;

    } else if (view_type == "distinct") {
        g_views.register_view(std::make_unique<dbsp::DistinctView>(view_name, table_name));
        data->result = "Created distinct view: " + view_name;

    } else {
        throw InvalidInputException("Unknown view type: " + view_type);
    }

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("result");
    return std::move(data);
}

void CreateViewFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<CreateViewBindData>();
    if (data.done) return;
    output.SetCardinality(1);
    output.SetValue(0, 0, Value(data.result));
    data.done = true;
}

// ============================================================================
// dbsp_insert / dbsp_delete - Modify data
// Usage: SELECT * FROM dbsp_insert('table', val1, val2, ...);
// ============================================================================

struct ModifyBindData : public TableFunctionData {
    string table_name;
    dbsp::Row row;
    bool is_delete = false;
    bool done = false;
};

unique_ptr<FunctionData> ModifyBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<ModifyBindData>();

    if (input.inputs.size() < 2) {
        throw InvalidInputException("dbsp_insert/delete(table, val1, val2, ...)");
    }

    data->table_name = input.inputs[0].GetValue<string>();

    for (idx_t i = 1; i < input.inputs.size(); i++) {
        auto &val = input.inputs[i];
        switch (val.type().id()) {
            case LogicalTypeId::BIGINT:
            case LogicalTypeId::INTEGER:
            case LogicalTypeId::SMALLINT:
            case LogicalTypeId::TINYINT:
                data->row.columns.push_back(val.GetValue<int64_t>());
                break;
            case LogicalTypeId::DOUBLE:
            case LogicalTypeId::FLOAT:
                data->row.columns.push_back(val.GetValue<double>());
                break;
            case LogicalTypeId::BOOLEAN:
                data->row.columns.push_back(val.GetValue<bool>());
                break;
            default:
                data->row.columns.push_back(val.ToString());
                break;
        }
    }

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("result");
    return std::move(data);
}

void InsertFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<ModifyBindData>();
    if (data.done) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_views.insert_row(data.table_name, data.row);

    output.SetCardinality(1);
    output.SetValue(0, 0, Value("Inserted 1 row into " + data.table_name));
    data.done = true;
}

void DeleteFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<ModifyBindData>();
    if (data.done) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_views.delete_row(data.table_name, data.row);

    output.SetCardinality(1);
    output.SetValue(0, 0, Value("Deleted 1 row from " + data.table_name));
    data.done = true;
}

// ============================================================================
// dbsp_query - Query a view
// Usage: SELECT * FROM dbsp_query('view_name');
// ============================================================================

struct QueryBindData : public TableFunctionData {
    string view_name;
    vector<dbsp::Row> rows;
    idx_t current = 0;
    idx_t num_cols = 0;
};

unique_ptr<FunctionData> QueryBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<QueryBindData>();

    if (input.inputs.empty()) {
        throw InvalidInputException("dbsp_query(view_name)");
    }

    data->view_name = input.inputs[0].GetValue<string>();

    std::lock_guard<std::mutex> lock(g_mutex);
    auto *view = g_views.get_view(data->view_name);
    if (!view) {
        throw InvalidInputException("View not found: " + data->view_name);
    }

    // Collect rows
    const auto &zset = view->get_result();
    for (const auto &[row, weight] : zset) {
        if (weight > 0) {
            data->rows.push_back(row);
            data->num_cols = std::max(data->num_cols, row.columns.size());
        }
    }

    // Determine column types from first row
    if (!data->rows.empty()) {
        const auto &first = data->rows[0];
        for (size_t i = 0; i < first.columns.size(); i++) {
            const auto &col = first.columns[i];
            if (std::holds_alternative<int64_t>(col)) {
                return_types.push_back(LogicalType::BIGINT);
            } else if (std::holds_alternative<double>(col)) {
                return_types.push_back(LogicalType::DOUBLE);
            } else if (std::holds_alternative<bool>(col)) {
                return_types.push_back(LogicalType::BOOLEAN);
            } else {
                return_types.push_back(LogicalType::VARCHAR);
            }
            names.push_back("col" + std::to_string(i));
        }
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
            const auto &val = row.columns[col];
            Value dval;
            if (std::holds_alternative<int64_t>(val)) {
                dval = Value::BIGINT(std::get<int64_t>(val));
            } else if (std::holds_alternative<double>(val)) {
                dval = Value::DOUBLE(std::get<double>(val));
            } else if (std::holds_alternative<bool>(val)) {
                dval = Value::BOOLEAN(std::get<bool>(val));
            } else {
                dval = Value(std::get<std::string>(val));
            }
            output.SetValue(col, count, dval);
        }

        data.current++;
        count++;
    }
    output.SetCardinality(count);
}

// ============================================================================
// dbsp_views - List views
// ============================================================================

struct ListBindData : public TableFunctionData {
    vector<string> names;
    idx_t current = 0;
};

unique_ptr<FunctionData> ListBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<string> &names) {
    auto data = make_uniq<ListBindData>();

    std::lock_guard<std::mutex> lock(g_mutex);
    data->names = g_views.list_views();

    return_types.push_back(LogicalType::VARCHAR);
    names.push_back("view_name");
    return_types.push_back(LogicalType::BIGINT);
    names.push_back("rows");

    return std::move(data);
}

void ListFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &data = input.bind_data->CastNoConst<ListBindData>();

    idx_t count = 0;
    while (data.current < data.names.size() && count < STANDARD_VECTOR_SIZE) {
        const auto &name = data.names[data.current];

        std::lock_guard<std::mutex> lock(g_mutex);
        auto *view = g_views.get_view(name);

        output.SetValue(0, count, Value(name));
        output.SetValue(1, count, Value::BIGINT(view ? view->row_count() : 0));

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
        std::lock_guard<std::mutex> lock(g_mutex);
        bool ok = g_views.remove_view(name.GetString());
        return StringVector::AddString(result, ok ? "Dropped" : "Not found");
    });
}

// ============================================================================
// Extension Entry Point
// ============================================================================

static void LoadInternal(DatabaseInstance &instance) {
    // dbsp_create_view
    TableFunction create_fn("dbsp_create_view", {}, CreateViewFunc, CreateViewBind);
    create_fn.varargs = LogicalType::VARCHAR;
    ExtensionUtil::RegisterFunction(instance, create_fn);

    // dbsp_insert
    TableFunction insert_fn("dbsp_insert", {}, InsertFunc, ModifyBind);
    insert_fn.varargs = LogicalType::ANY;
    ExtensionUtil::RegisterFunction(instance, insert_fn);

    // dbsp_delete
    TableFunction delete_fn("dbsp_delete", {}, DeleteFunc, ModifyBind);
    delete_fn.varargs = LogicalType::ANY;
    ExtensionUtil::RegisterFunction(instance, delete_fn);

    // dbsp_query
    TableFunction query_fn("dbsp_query", {LogicalType::VARCHAR}, QueryFunc, QueryBind);
    ExtensionUtil::RegisterFunction(instance, query_fn);

    // dbsp_views
    TableFunction list_fn("dbsp_views", {}, ListFunc, ListBind);
    ExtensionUtil::RegisterFunction(instance, list_fn);

    // dbsp_drop
    ScalarFunction drop_fn("dbsp_drop", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DropScalar);
    ExtensionUtil::RegisterFunction(instance, drop_fn);
}

void DbspExtension::Load(DuckDB &db) {
    LoadInternal(*db.instance);
}

std::string DbspExtension::Name() {
    return "dbsp";
}

std::string DbspExtension::Version() const {
    return "0.1.0";
}

} // namespace duckdb

extern "C" {
DUCKDB_EXTENSION_API void dbsp_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB db_wrapper(db);
    db_wrapper.LoadExtension<duckdb::DbspExtension>();
}

DUCKDB_EXTENSION_API const char *dbsp_version() {
    return duckdb::DuckDB::LibraryVersion();
}
}
