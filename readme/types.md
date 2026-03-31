# Data types and wire formats

How **`qbm-pgsql`** maps C++ values to PostgreSQL **OIDs** and to **binary** / **text** wire formats for **parameters
** (**`qb::pg::params`**) and **result columns** (**`resultset::field::as<T>()`**).

**Application code:** `#include <pgsql/pgsql.h>` only. **`params`**, **`oid`**, **`type_oid_sequence`**, and **`results`
** are all available from that header. Internal machinery (**`TypeConverter`**, **`ParamSerializer`**, **`FieldHandler`
**) lives under **`src/`** and is referenced here so you can navigate the implementation, not because you should include
those files in apps.

---

## Concepts

- **OIDs:** PostgreSQL type identifiers. **`qb::pg::oid`** (**`src/pg_types.h`**) lists common built-in OIDs used in *
  *`prepare(..., type_oid_sequence{…})`**.
- **Parameters:** Extended-query **Bind** sends parameter values; the stack serializes **`params`** to **binary** for
  non-NULL values (see below).
- **Result columns:** Format depends on the query path — **simple query** vs **prepared** — see **Binary vs text**
  below.
- **Type mapping:** **`qb::pg::detail::type_mapping<T>`** maps C++ **`T`** → OID for templates; **`field::as<T>()`**
  uses **`FieldHandler`** + **`TypeConverter`** to decode.

---

## `params` and `prepare`

You declare parameter types when **preparing**:

```cpp
auto pr = co_await db.prepare("ins",
    "INSERT INTO t (n, label) VALUES ($1, $2)",
    qb::pg::type_oid_sequence{qb::pg::oid::int4, qb::pg::oid::text});
if (!pr.ok()) { /* … */ }

auto r = co_await db.execute("ins", qb::pg::params{42, std::string{"x"}});
```

**NULL parameters:** use **`std::nullopt`** or empty **`std::optional<T>`** in **`params`** to send SQL NULL.

Arity and OID list must match **`$1`…`$n`** in the statement.

---

## Supported type mappings (reference)

| C++ / usage                                                  | PostgreSQL                    |  OID (typical)   |     Params (binary)      |                                       Result decode                                        |
|:-------------------------------------------------------------|:------------------------------|:----------------:|:------------------------:|:------------------------------------------------------------------------------------------:|
| **`bool`**                                                   | `BOOLEAN`                     |        16        |           Yes            |                                            Yes                                             |
| **`int16_t`**, **`qb::pg::smallint`**                        | `SMALLINT`                    |        21        |           Yes            |                                            Yes                                             |
| **`int32_t`**, **`qb::pg::integer`**                         | `INTEGER`                     |        23        |           Yes            |                                            Yes                                             |
| **`int64_t`**, **`qb::pg::bigint`**                          | `BIGINT`                      |        20        |           Yes            |                                            Yes                                             |
| **`float`**                                                  | `REAL`                        |       700        |           Yes            |                                            Yes                                             |
| **`double`**                                                 | `DOUBLE PRECISION`            |       701        |           Yes            |                                            Yes                                             |
| **`qb::pg::detail::numeric`** (internal)                     | `NUMERIC` / `DECIMAL`         |       1700       |           Yes            | Yes — prefer **`as`/`to`** or exact decimal via SQL text if you do not use the detail type |
| **`std::string`**, **`std::string_view`**, **`const char*`** | `TEXT` / `VARCHAR` / `BPCHAR` | 25 / 1043 / 1042 |           Yes            |                                            Yes                                             |
| **`qb::pg::bytea`**, **`std::vector<byte>`**                 | `BYTEA`                       |        17        |           Yes            |                                            Yes                                             |
| **`qb::Timestamp`**                                          | `TIMESTAMP`                   |       1114       |           Yes            |                                            Yes                                             |
| **`qb::UtcTimestamp`**                                       | `TIMESTAMPTZ`                 |       1184       |           Yes            |                                            Yes                                             |
| **`qb::pg::detail::pgdate`** (internal)                      | `DATE`                        |       1082       |           Yes            |                                            Yes                                             |
| **`qb::pg::detail::pgtime`**                                 | `TIME`                        |       1083       |           Yes            |                                            Yes                                             |
| **`qb::pg::detail::pgtimetz`**                               | `TIMETZ`                      |       1266       |           Yes            |                                            Yes                                             |
| **`std::chrono::duration<…>`**                               | `INTERVAL`                    |       1186       |           Yes            |                                            Yes                                             |
| **`qb::json`**, **`nlohmann::json`**                         | `JSON`                        |       114        |           Yes            |                                            Yes                                             |
| **`qb::jsonb`**                                              | `JSONB`                       |       3802       |           Yes            |                                    Yes (binary on wire)                                    |
| **`qb::uuid`**                                               | `UUID`                        |       2950       |           Yes            |                                            Yes                                             |
| **`std::optional<T>`**                                       | same as `T`                   |        —         |   NULL when disengaged   |                                   NULL → empty optional                                    |
| **`std::vector<T>`**                                         | PostgreSQL arrays             |    array OID     | Partial / type-dependent |                           Partial — verify for your element type                           |
| **`std::string`** as carrier                                 | `INET`, `CIDR`, `MACADDR`     |  869, 650, 829   |        Text path         |                                        Text decode                                         |

For **NUMERIC** with strict decimal semantics in application code, many teams bind **text** (`$1` as **`text`**) with a
decimal string produced by their domain layer, or rely on **`field.as<std::string>()`** / **`double`** knowing the
trade-offs.

---

## NULL handling

- **Out:** **`std::nullopt`** or empty **`optional`** in **`params`** → SQL NULL.
- **In:** **`field.is_null()`**; **`field.as<std::optional<U>>()`** for non-throwing NULL handling.
- **`field.as<T>`** with non-optional **`T`** on NULL → **`qb::pg::error::value_is_null`**.

---

## Binary vs text on the wire

- **Parameters:** Extended **Bind** uses **binary** parameter encoding for non-NULL values (format codes per protocol
  rules in the implementation).
- **Results:**
    - **Simple query** (`execute("SELECT …")` without a named prepared statement): the server commonly returns **text**
      columns (**format code 0** in **`RowDescription`**).
    - **Prepared / extended:** before decoding, the client may patch **`RowDescription`** so each column’s *
      *`format_code`** matches what was requested in **Bind** — see **`type_oid_prefers_binary_result_format`** in [
      `src/common.h`](../src/common.h) (starts ~line 372, **`switch` on `oid`**). **Binary** for many scalars (ints,
      floats, bool, timestamps, UUID, **JSONB**, numeric, …); **text** for string-like and catalog types (**`text`**, *
      *`varchar`**, **`json`**, **`xml`**, **`money`**, **`reg*`**, …).

If a **citext** or other extension OID is misclassified, **cast in SQL** to **`text`** or extend the **`switch`** in *
*`common.h`**.

**`field::as<T>()`** — [`src/field_handler.h`](../src/field_handler.h) branches on **`format_code`** and delegates to *
*`TypeConverter`** ([`type_converter.h`](../src/type_converter.h)).

---

## Encode / decode pipeline (contributors)

1. **`params`** → **`ParamSerializer`** ([`param_serializer.h`](../src/param_serializer.h)) → Bind message bytes.
2. Inbound field bytes → **`ParamUnserializer`** / **`FieldHandler`** → **`as<T>()`** / **`to(ref)`**.
3. OID ↔ C++ mapping templates: **`type_mapping.h`**, **`type_converter.h`**.

---

## Tuple extraction

**`resultset::row::to`** fills **`std::tuple`** / **`std::tie`** (**`tuple_converter.h`** + **`field_handler.h`**):

```cpp
std::tuple<int, std::string, bool> row_data;
row.to(row_data);

int a;
std::optional<std::string> b;
bool c;
row.to(std::tie(a, b, c));
```

Template machinery walks columns by index and applies the same rules as **`as<T>()`** / **`to(ref)`** per element type.

---

## Implementation map (contributors)

| Component               | File                                                      | Role                                                               |
|:------------------------|:----------------------------------------------------------|:-------------------------------------------------------------------|
| OID enumerations        | **`src/pg_types.h`**                                      | **`oid::int4`**, …                                                 |
| C++ → OID               | **`src/type_mapping.h`**                                  | **`type_mapping<T>::type_oid`**                                    |
| Serialize / deserialize | **`src/type_converter.h`**                                | **`to_binary`**, **`from_binary`**, **`to_text`**, **`from_text`** |
| **`params` → Bind**     | **`src/param_serializer.h`**                              | Binary buffer layout                                               |
| Column decode           | **`src/param_unserializer.h`**, **`src/field_handler.h`** | **`as<T>`** implementation                                         |
| Result format choice    | **`common.h`**                                            | **`type_oid_prefers_binary_result_format`**                        |

---

## Related

- [results.md](./results.md) — iteration, **`as<T>`**, JSON
- [queries.md](./queries.md) — **`prepare`**, **`params`**
- [error_handling.md](./error_handling.md) — **`value_is_null`**, **`field_type_mismatch`**  
