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

| C++ Type                        | PostgreSQL Type(s)         | OID (`qb::pg::oid`) | Notes                                                      |
| :------------------------------ | :------------------------- | :------------------ | :--------------------------------------------------------- |
| `bool`                          | `BOOLEAN`                  | `boolean` (16)      |                                                            |
| `int16_t` (`qb::pg::smallint`)  | `SMALLINT`                 | `int2` (21)         |                                                            |
| `int32_t` (`qb::pg::integer`) | `INTEGER`                  | `int4` (23)         |                                                            |
| `int64_t` (`qb::pg::bigint`)    | `BIGINT`                   | `int8` (20)         |                                                            |
| `float`                         | `REAL`                     | `float4` (700)      |                                                            |
| `double`                        | `DOUBLE PRECISION`         | `float8` (701)      |                                                            |
| `std::string`                   | `TEXT`, `VARCHAR`, `CHAR`  | `text` (25)         | Also `varchar`(1043), `bpchar`(1042)                       |
| `const char*`                   | `TEXT`, `VARCHAR`, `CHAR`  | `text` (25)         |                                                            |
| `std::string_view`              | `TEXT`, `VARCHAR`, `CHAR`  | `text` (25)         |                                                            |
| `std::vector<char>`             | `BYTEA`                    | `bytea` (17)        | Binary data                                                |
| `std::vector<unsigned char>`    | `BYTEA`                    | `bytea` (17)        | Binary data                                                |
| `qb::pg::bytea`                 | `BYTEA`                    | `bytea` (17)        | Binary data                                                |
| `qb::uuid`                      | `UUID`                     | `uuid` (2950)       |                                                            |
| `qb::Timestamp`                 | `TIMESTAMP WITHOUT TIMEZONE` | `timestamp` (1114)  |                                                            |
| `qb::UtcTimestamp`              | `TIMESTAMP WITH TIMEZONE`    | `timestamptz` (1184)| Stored as UTC internally by PostgreSQL                     |
| `qb::json`                      | `JSON`                     | `json` (114)        | Stored as text                                             |
| `qb::jsonb`                     | `JSONB`                    | `jsonb` (3802)      | Stored in optimized binary format                          |
| `std::optional<T>`              | *Type of T* (nullable)     | *(Same as T)*       | Maps to SQL NULL when empty                                |
| `std::vector<T>`                | *Array of T's Type*       | *Array OID*         | e.g., `std::vector<int>` maps to `INTEGER[]` (`oid::int4_array`, 1007) |

*(Note: Some types like `NUMERIC` or specific date/time/interval types might require custom handling or string conversion.)*

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