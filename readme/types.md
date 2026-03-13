# `qbm-pgsql`: Data Type Handling

This document details how `qbm-pgsql` handles the conversion and mapping between C++ data types and PostgreSQL data types.

## Core Concepts

*   **PostgreSQL OIDs:** PostgreSQL uses Object Identifiers (OIDs) to internally represent data types. The `qbm-pgsql` module uses these OIDs for type identification and conversion.
*   **Wire Formats:** PostgreSQL communicates using two main data formats:
    *   **Text Format:** Human-readable string representations (default for simple queries).
    *   **Binary Format:** Compact, native binary representations (default for prepared statement parameters and results). `qbm-pgsql` primarily uses binary format for parameters.
*   **Type Safety:** The module leverages C++ templates to provide type safety during serialization (C++ to PG) and deserialization (PG to C++).

## Key Internal Components

*   **`qb::pg::oid` (enum class):** (`src/pg_types.h`) Defines enumerators for standard PostgreSQL type OIDs (e.g., `oid::int4`, `oid::text`, `oid::timestamp`).
*   **`qb::pg::detail::type_mapping<T>` (struct template):** (`src/type_mapping.h`) Maps a C++ type `T` to its corresponding PostgreSQL `type_oid`.
    ```cpp
    // Example: Get OID for C++ int
    constexpr qb::pg::integer int_oid = qb::pg::detail::type_mapping<int>::type_oid; // Result: 23 (oid::int4)
    ```
*   **`qb::pg::detail::TypeConverter<T>` (class template):** (`src/type_converter.h`) The core conversion engine. Provides static methods:
    *   `get_oid()`: Returns the PostgreSQL OID for type `T`.
    *   `to_binary(const T& value, std::vector<byte>& buffer)`: Serializes a C++ value to PostgreSQL binary format.
    *   `to_text(const T& value)`: Serializes a C++ value to PostgreSQL text format.
    *   `from_binary(const std::vector<byte>& buffer)`: Deserializes PostgreSQL binary format to a C++ value.
    *   `from_text(const std::string& text)`: Deserializes PostgreSQL text format to a C++ value.
*   **`qb::pg::detail::ParamSerializer` (class):** (`src/param_serializer.h`) Uses `TypeConverter` to serialize parameters passed via `qb::pg::params` into the binary format expected by PostgreSQL for prepared statements.
*   **`qb::pg::detail::ParamUnserializer` (class):** (`src/param_unserializer.h`) Uses `TypeConverter` to deserialize data received from PostgreSQL (primarily used internally by `resultset::field` accessors).
*   **`qb::pg::detail::FieldHandler` (class):** (`src/field_handler.h`) Provides the logic behind `resultset::field::as<T>()` and `.to<T>()`, using `TypeConverter` and `ParamUnserializer` to handle data extraction from result fields, including NULL checks and format detection.

## Supported Type Mappings

| C++ Type | PostgreSQL Type | OID | Binary Format | Text Format | Notes |
| :------- | :-------------- | :-- | :------------ | :---------- | :---- |
| **NUMÉRIQUES** |||||
| `bool` | `BOOLEAN` | 16 | ✅ | ✅ | Native binary (1 byte) |
| `int16_t` / `qb::pg::smallint` | `SMALLINT` | 21 | ✅ | ✅ | Network byte order |
| `int32_t` / `qb::pg::integer` | `INTEGER` | 23 | ✅ | ✅ | Network byte order |
| `int64_t` / `qb::pg::bigint` | `BIGINT` | 20 | ✅ | ✅ | Network byte order |
| `float` | `REAL` | 700 | ✅ | ✅ | IEEE 754 float |
| `double` | `DOUBLE PRECISION` | 701 | ✅ | ✅ | IEEE 754 double |
| `qb::pg::detail::numeric` | `NUMERIC` / `DECIMAL` | 1700 | ✅ | ✅ | **Exact precision** - stores as string for precision preservation |
| **CARACTÈRES** |||||
| `std::string` | `TEXT` | 25 | ✅ | ✅ | Variable length |
| `std::string` | `VARCHAR` | 1043 | ✅ | ✅ | Via text converter |
| `std::string` | `CHAR(n)` / `BPCHAR` | 1042 | ✅ | ✅ | Blank-padded char |
| `const char*` | `TEXT`, `VARCHAR` | 25 | ✅ | ✅ | Auto-converted to string |
| `std::string_view` | `TEXT` | 25 | ✅ | ✅ | Zero-copy read-only |
| **BINAIRE** |||||
| `qb::pg::bytea` / `std::vector<byte>` | `BYTEA` | 17 | ✅ | ✅ | Binary data with escaping |
| **DATE/HEURE** |||||
| `qb::Timestamp` | `TIMESTAMP` | 1114 | ✅ | ✅ | Microseconds since 2000-01-01 |
| `qb::UtcTimestamp` | `TIMESTAMPTZ` | 1184 | ✅ | ✅ | UTC storage |
| `qb::pg::detail::pgdate` | `DATE` | 1082 | ✅ | ✅ | **NEW** - Days since 2000-01-01 |
| `qb::pg::detail::pgtime` | `TIME` | 1083 | ✅ | ✅ | **NEW** - Microseconds since midnight |
| `qb::pg::detail::pgtimetz` | `TIMETZ` | 1266 | ✅ | ✅ | **NEW** - Time + timezone offset |
| `std::chrono::duration<Rep,Period>` | `INTERVAL` | 1186 | ✅ | ✅ | 16 bytes: time + days + months |
| **JSON** |||||
| `qb::json` / `nlohmann::json` | `JSON` | 114 | ✅ | ✅ | Text storage |
| `qb::jsonb` / `nlohmann::json` | `JSONB` | 3802 | ✅ | ✅ | **Binary optimized format** |
| **AUTRES** |||||
| `qb::uuid` | `UUID` | 2950 | ✅ | ✅ | 16-byte binary format |
| `std::optional<T>` | *any* | *same* | ✅ | ✅ | NULL when empty |
| `std::vector<T>` | *Array[T]* | *array* | ⚠️ | ⚠️ | Partial array support |
| `std::string` (interim) | `INET` | 869 | ❌ | ✅ | **Text format only** |
| `std::string` (interim) | `CIDR` | 650 | ❌ | ✅ | **Text format only** |
| `std::string` (interim) | `MACADDR` | 829 | ❌ | ✅ | **Text format only** |

## Handling NULL Values

*   **Parameters:** Pass `std::nullopt` or an empty `std::optional<T>` to `qb::pg::params` or the variadic `execute` methods to send a SQL NULL value.
*   **Results:**
    *   Use `field.is_null()` to check if a field is NULL before accessing its value.
    *   Use `field.as<std::optional<T>>()` or `field.to(std::optional<T>&)` to retrieve potentially NULL values without exceptions. The `std::optional` will be empty if the database field was NULL.
    *   Calling `field.as<T>()` (where `T` is not `std::optional`) on a NULL field will throw `qb::pg::error::value_is_null`.

## Binary vs. Text Format

*   **Parameters (`qb::pg::params`):** By default, parameters are sent in **binary format** for efficiency and type safety.
*   **Results (`qb::pg::results`):** The format of results depends on how the query was executed:
    *   **Simple Queries (`db.execute("SELECT ...")`):** Usually return results in **text format** by default.
    *   **Prepared Statements (`db.execute("prepared_name", ...)`):** Usually return results in **binary format** by default.
*   **TypeConverter:** The `TypeConverter` class handles conversions `from_binary` and `from_text` appropriately. The `resultset::field` accessors (`.as<T>()`, `.to<T>()`) internally check the field's format code (`field.description().format_code`) and call the correct `TypeConverter` method.
*   **Manual Conversion:** If needed, you can access the raw `field_buffer` and the `format_code` and perform manual conversion, but using `.as<T>()` is recommended.

## Tuple Conversion

*(Implemented in `src/tuple_converter.h` and integrated via `src/field_handler.h`)*

The `resultset::row::to(...)` methods allow direct conversion of a row into a `std::tuple`.

```cpp
// Assuming row has columns (int, text, bool)

// 1. Using an existing tuple
std::tuple<int, std::string, bool> my_tuple;
row.to(my_tuple);

// 2. Using structured binding (C++17)
auto [id, name, is_active] = std::tuple<int, std::string, bool>();
row.to(std::tie(id, name, is_active)); // std::tie creates a tuple of references

// 3. Using optional for nullable columns
std::tuple<int, std::optional<std::string>, bool> tuple_with_opt;
row.to(tuple_with_opt);
auto [id2, name_opt, active2] = tuple_with_opt;
if (name_opt) { /* Use *name_opt */ }
```

This uses `detail::FieldHandler::convert_row_to_tuple` internally, which leverages template metaprogramming (`std::index_sequence`) to efficiently call the appropriate `.to<T>()` or `.as<T>()` method for each field based on the tuple's types.

---

## New Type Support (Recently Added)

### NUMERIC / DECIMAL - Exact Precision

The `qb::pg::detail::numeric` type provides **exact decimal precision** for financial calculations.

```cpp
#include <pgsql/src/type_converter.h>

// Create numeric from string (preserves precision)
qb::pg::detail::numeric price("999.99");
qb::pg::detail::numeric high_precision("123456789.0123456789");

// Binary serialization (uses text internally for precision)
std::vector<qb::pg::byte> buffer;
qb::pg::detail::TypeConverter<qb::pg::detail::numeric>::to_binary(price, buffer);

// In queries
_db.execute("INSERT INTO products (price) VALUES ($1)", 
            {price.str()});  // Send as text for precision
```

**Features:**
- ✅ Arbitrary precision (no floating-point errors)
- ✅ Binary and text serialization
- ✅ Financial calculation safe

---

### DATE - Calendar Dates

The `qb::pg::detail::pgdate` type handles calendar dates efficiently.

```cpp
#include <pgsql/src/type_converter.h>

// Create from components
qb::pg::detail::pgdate today = qb::pg::detail::pgdate::from_string("2024-12-25");

// Get Unix timestamp
int64_t unix_time = today.to_unix_time();

// Format for display
std::string formatted = today.to_string();  // "2024-12-25"

// Binary serialization (4 bytes)
std::vector<qb::pg::byte> buffer;
qb::pg::detail::TypeConverter<qb::pg::detail::pgdate>::to_binary(today, buffer);
```

**Features:**
- ✅ 4-byte binary format (days since 2000-01-01)
- ✅ Platform-independent date calculations
- ✅ Range: 4713 BC to 5874897 AD

---

### TIME / TIMETZ - Time of Day

The `qb::pg::detail::pgtime` and `qb::pg::detail::pgtimetz` types handle time values.

```cpp
#include <pgsql/src/type_converter.h>

// TIME without timezone
qb::pg::detail::pgtime t = qb::pg::detail::pgtime::from_hmsu(14, 30, 45, 123456);
// Stored as: 14:30:45.123456

// TIMETZ with timezone
qb::pg::detail::pgtimetz tt = qb::pg::detail::pgtimetz::from_hmsu_tz(18, 0, 0, 0, 7200);  // +02:00
// Stored as: 18:00:00+02:00

// String conversion
std::string time_str = t.to_string();    // "14:30:45.123456"
std::string tz_str = tt.to_string();     // "18:00:00+02:00"

// Binary serialization
std::vector<qb::pg::byte> buffer;
qb::pg::detail::TypeConverter<qb::pg::detail::pgtime>::to_binary(t, buffer);      // 12 bytes
qb::pg::detail::TypeConverter<qb::pg::detail::pgtimetz>::to_binary(tt, buffer);   // 16 bytes
```

**Features:**
- ✅ Microsecond precision
- ✅ Timezone support (TIMETZ)
- ✅ Binary and text formats
- ✅ Range: 00:00:00 to 23:59:59.999999

---

### INTERVAL - Time Duration

PostgreSQL INTERVAL maps to `std::chrono::duration`.

```cpp
#include <chrono>
#include <pgsql/src/type_converter.h>

using namespace std::chrono;

// Create intervals
hours h(2);           // 2 hours
minutes m(30);      // 30 minutes
seconds s(45);        // 45 seconds
milliseconds ms(500); // 500 milliseconds

// Binary serialization (16 bytes)
std::vector<qb::pg::byte> buffer;
qb::pg::detail::TypeConverter<hours>::to_binary(h, buffer);

// Text format (recommended for precision)
std::string text = qb::pg::detail::TypeConverter<hours>::to_text(h);
```

**Format:** 16 bytes (microseconds + days + months)

---

## Type Coverage Summary

| Category | Types | Status |
| :------- | :---- | :----- |
| **Numeric** | SMALLINT, INTEGER, BIGINT, REAL, DOUBLE, NUMERIC | 🟢 **Complete** |
| **Character** | TEXT, VARCHAR, CHAR | 🟢 **Complete** |
| **Binary** | BYTEA | 🟢 **Complete** |
| **Date/Time** | TIMESTAMP, TIMESTAMPTZ, **DATE**, **TIME**, **TIMETZ**, **INTERVAL** | 🟢 **Complete** |
| **JSON** | JSON, JSONB | 🟢 **Complete** |
| **Other** | UUID, BOOLEAN | 🟢 **Complete** |
| **Network** | INET, CIDR, MACADDR | 🟡 **Text format** |

**Legend:** 🟢 = Binary + Text formats supported | 🟡 = Text format only 