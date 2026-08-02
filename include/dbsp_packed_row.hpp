// dbsp_packed_row.hpp — compact byte encoding for join-index rows (Phase 2d)
//
// A boxed DuckDBRow costs ~450-600B resident (vector<duckdb::Value> payload +
// shared_ptr block + map node). Join indexes hold tens of millions of them —
// the dominant operator-state class. This codec packs a row into a
// self-describing byte string (~1 tag byte + payload per value): ints,
// doubles and short strings land at tens of bytes per row.
//
// Self-describing (a type tag per value) so key rows — whose types come from
// arbitrary key expressions — need no schema plumbing. Any value of an
// unsupported type makes encode() return false; the caller keeps that node on
// the boxed path (packed_ok_ = false), so coverage is an optimization knob,
// never a correctness question.
//
// Encoding is CANONICAL per value (same value → same bytes) which makes the
// packed bytes usable as hash-map keys directly.

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types/value.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace dbsp_native {

namespace packed {

enum Tag : uint8_t {
  T_NULL = 0,
  T_BOOL_F = 1,
  T_BOOL_T = 2,
  T_I32 = 3,
  T_I64 = 4,
  T_DOUBLE = 5,
  T_VARCHAR = 6,
  T_DATE = 7,
  T_TIMESTAMP = 8,
  T_FLOAT = 9,
  T_I16 = 10,
  T_I8 = 11,
  T_U32 = 12,
  T_U64 = 13,
};

inline void put_raw(std::string &out, const void *p, size_t n) {
  out.append(reinterpret_cast<const char *>(p), n);
}

template <typename T> inline void put(std::string &out, Tag t, T v) {
  out.push_back(static_cast<char>(t));
  put_raw(out, &v, sizeof(T));
}

// Append one value. Returns false on an unsupported type (caller falls back
// to boxed storage for the whole node).
inline bool encode_value(std::string &out, const duckdb::Value &v) {
  using duckdb::LogicalTypeId;
  if (v.IsNull()) {
    out.push_back(static_cast<char>(T_NULL));
    // NULLs still need the type for exact round-trip; store the type id so
    // decode can rebuild Value(type) (comparisons/emission stay typed).
    out.push_back(static_cast<char>(static_cast<uint8_t>(v.type().id())));
    return true;
  }
  switch (v.type().id()) {
  case LogicalTypeId::BOOLEAN:
    out.push_back(static_cast<char>(duckdb::BooleanValue::Get(v) ? T_BOOL_T
                                                                 : T_BOOL_F));
    return true;
  case LogicalTypeId::TINYINT:
    put(out, T_I8, static_cast<int8_t>(v.GetValue<int8_t>()));
    return true;
  case LogicalTypeId::SMALLINT:
    put(out, T_I16, static_cast<int16_t>(v.GetValue<int16_t>()));
    return true;
  case LogicalTypeId::INTEGER:
    put(out, T_I32, v.GetValue<int32_t>());
    return true;
  case LogicalTypeId::BIGINT:
    put(out, T_I64, v.GetValue<int64_t>());
    return true;
  case LogicalTypeId::UINTEGER:
    put(out, T_U32, v.GetValue<uint32_t>());
    return true;
  case LogicalTypeId::UBIGINT:
    put(out, T_U64, v.GetValue<uint64_t>());
    return true;
  case LogicalTypeId::FLOAT:
    put(out, T_FLOAT, v.GetValue<float>());
    return true;
  case LogicalTypeId::DOUBLE:
    put(out, T_DOUBLE, v.GetValue<double>());
    return true;
  case LogicalTypeId::DATE:
    put(out, T_DATE, duckdb::DateValue::Get(v).days);
    return true;
  case LogicalTypeId::TIMESTAMP:
    put(out, T_TIMESTAMP, duckdb::TimestampValue::Get(v).value);
    return true;
  case LogicalTypeId::VARCHAR: {
    const std::string &s = duckdb::StringValue::Get(v);
    out.push_back(static_cast<char>(T_VARCHAR));
    const uint32_t n = static_cast<uint32_t>(s.size());
    put_raw(out, &n, sizeof(n));
    out.append(s);
    return true;
  }
  default:
    return false; // DECIMAL/HUGEINT/INTERVAL/nested: boxed path
  }
}

// Encode a whole row (column count prefix + values). Returns false on any
// unsupported value; `out` contents are unspecified then.
template <typename RowT> inline bool encode_row(std::string &out, const RowT &row) {
  out.clear();
  const uint32_t n = static_cast<uint32_t>(row.columns.size());
  put_raw(out, &n, sizeof(n));
  for (const auto &v : row.columns) {
    if (!encode_value(out, v)) {
      return false;
    }
  }
  return true;
}

template <typename T> inline T get_raw(const char *&p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  p += sizeof(T);
  return v;
}

inline duckdb::Value decode_value(const char *&p) {
  using duckdb::Value;
  const Tag t = static_cast<Tag>(static_cast<uint8_t>(*p++));
  switch (t) {
  case T_NULL: {
    const auto id =
        static_cast<duckdb::LogicalTypeId>(static_cast<uint8_t>(*p++));
    return Value(duckdb::LogicalType(id));
  }
  case T_BOOL_F:
    return Value::BOOLEAN(false);
  case T_BOOL_T:
    return Value::BOOLEAN(true);
  case T_I8:
    return Value::TINYINT(get_raw<int8_t>(p));
  case T_I16:
    return Value::SMALLINT(get_raw<int16_t>(p));
  case T_I32:
    return Value::INTEGER(get_raw<int32_t>(p));
  case T_I64:
    return Value::BIGINT(get_raw<int64_t>(p));
  case T_U32:
    return Value::UINTEGER(get_raw<uint32_t>(p));
  case T_U64:
    return Value::UBIGINT(get_raw<uint64_t>(p));
  case T_FLOAT:
    return Value::FLOAT(get_raw<float>(p));
  case T_DOUBLE:
    return Value::DOUBLE(get_raw<double>(p));
  case T_DATE:
    return Value::DATE(duckdb::date_t(get_raw<int32_t>(p)));
  case T_TIMESTAMP:
    return Value::TIMESTAMP(duckdb::timestamp_t(get_raw<int64_t>(p)));
  case T_VARCHAR: {
    const uint32_t n = get_raw<uint32_t>(p);
    std::string s(p, n);
    p += n;
    return Value(std::move(s));
  }
  }
  return Value(); // unreachable for well-formed input
}

// Decode into an existing row object (RowT must expose columns.push_back).
template <typename RowT>
inline void decode_row(const std::string &bytes, RowT &row) {
  const char *p = bytes.data();
  const uint32_t n = get_raw<uint32_t>(p);
  for (uint32_t i = 0; i < n; i++) {
    row.columns.push_back(decode_value(p));
  }
}

// True when every column type round-trips through this codec — the gate
// for packed storage (join indexes, shared arrangements).
inline bool types_ok(const duckdb::vector<duckdb::LogicalType> &ts) {
  using duckdb::LogicalTypeId;
  if (ts.empty()) {
    return false; // unknown schema: stay boxed
  }
  for (const auto &t : ts) {
    switch (t.id()) {
    case LogicalTypeId::BOOLEAN:
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::DATE:
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::VARCHAR:
      continue;
    default:
      return false;
    }
  }
  return true;
}

} // namespace packed

} // namespace dbsp_native
