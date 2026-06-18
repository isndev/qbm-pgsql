# Type mapping

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qbm-pgsql @ qb 2.0.0 (C++20 default, C++23 supported)

How `qbm-pgsql` maps C++ values to PostgreSQL OIDs and wire formats: outbound for bind parameters (`qb::pg::params`) and inbound for result columns (`field::as<T>()`).

**Prerequisites:** [queries.md](./queries.md) (`prepare`, `params`, `execute`) — **See also:** [results.md](./results.md) (reading rows), [error_handling.md](./error_handling.md) (`value_is_null`, `field_type_mismatch`)

**Include:** `#include <pgsql/pgsql.h>` — the public surface (`params`, `oid`, `type_oid_sequence`, `results`) is in namespace `qb::pg`. `qbm-pgsql` is a compiled static library (`qbm::pgsql`); link it with `target_link_libraries(app PRIVATE qbm::pgsql)`. It is not header-only.

The serialization machinery (`TypeConverter`, `ParamSerializer`, `FieldHandler`) lives under `src/` and is referenced here so you can navigate the implementation — application code never includes those files.

---

## Concepts

- **OID.** A PostgreSQL type identifier from the `pg_type` catalog (e.g. `23` = `int4`, `1184` = `timestamptz`). `qbm-pgsql` models the common ones as `enum class qb::pg::oid` ([`src/pg_types.h`](../src/pg_types.h)).
- **`type_oid_sequence`.** `using type_oid_sequence = std::vector<oid>` ([`src/common.h`](../src/common.h)). You pass one to `prepare()` to declare each parameter's type. <!-- src: src/common.h:497 -->
- **`params`.** `using params = detail::QueryParams` ([`pgsql.h`](../pgsql.h)). A heterogeneous container of bind values serialized to PostgreSQL **binary** wire form. <!-- src: pgsql.h:2047 -->
- **`type_mapping<T>`.** Compile-time C++-type → OID lookup ([`src/type_mapping.h`](../src/type_mapping.h)). Drives `get_type_oid<T>()` and `fill_types<T...>()`. Unmapped types resolve to OID `705` (unknown), never an error.
- **`TypeConverter<T>`.** The encode/decode engine ([`src/type_converter.h`](../src/type_converter.h)): `to_binary` / `to_text` (send) and `from_binary` / `from_text` (receive). Unsupported types fail to compile via `static_assert`.

Two facts shape everything below:

1. **Bind parameters are always sent in binary** — a single format code (1) for every parameter. There is no text fallback on the send path, so a value's binary encoding must match what the server expects for its declared OID.
2. **Result columns are decoded per column** by `format_code`. The client requests binary for scalar OIDs and text for string-like / catalog OIDs in `Bind`; see [Binary versus text on the wire](#binary-versus-text-on-the-wire).

---

## Time and timestamps

PostgreSQL `timestamptz` (OID `1184`) maps to **`qb::wall_time`** — the framework's canonical UTC instant (`std::chrono::system_clock::time_point`). <!-- src: src/type_mapping.h:147-149 -->

```cpp
// type_mapping.h
template <> struct type_mapping<qb::wall_time> { static constexpr integer type_oid = 1184; }; // timestamptz
```

On the wire a timestamp is an `int64` big-endian count of **microseconds since 2000-01-01 00:00:00 UTC** (the PostgreSQL epoch). The converter performs an exact integer shift between that epoch and the Unix epoch (`946684800` seconds) with no floating-point rounding. <!-- src: src/type_converter.h:789-803 -->

Reads of **both** `timestamp` (OID `1114`) and `timestamptz` (OID `1184`) columns decode into `qb::wall_time` — the two share an identical micros-since-2000 wire layout. <!-- src: src/type_converter.h:760-762 -->

```cpp
#include <pgsql/pgsql.h>
using namespace qb::pg;

// Read a timestamptz column. (src: tests/test-data-types-integration.cpp:1354)
qb::wall_time created = result[0][0].as<qb::wall_time>();
```

> **The retired time tokens are gone.** `qb::Timestamp`, `qb::UtcTimestamp`, `qb::LocalTimestamp`, `qb::Duration`, `qb::TimePoint`, and helpers like `to_timestamp(...)` / `to_time_point(...)` no longer exist anywhere in `qbm-pgsql`. Use `qb::wall_time` for every timestamp. The "micros-since-2000" value is an **internal wire encoding** computed inside `TypeConverter`; it is never a `qb::duration` and is never surfaced to your code.

`connect`, `statement`, and `transaction` timeouts are a different concern — they are `qb::duration` (see [connection.md](./connection.md) and [transaction.md](./transaction.md)), not part of the type-mapping table.

---

## OID ↔ C++ type table

The table below is the **complete set of types that `type_mapping<T>` recognizes** (the send-side auto-deduction surface) plus how each decodes inbound. A blank "Bind via `params`" means `type_mapping<T>` has no specialization for that C++ type: you can still bind a literal whose mapped type covers it (e.g. an `int` for an `int4` column) and still decode it with `as<T>()`, but `get_type_oid<T>()` would report `705`. See [Types without a `type_mapping` entry](#types-without-a-type_mapping-entry).

| C++ type | PostgreSQL | OID | Bind via `params` | `as<T>()` decode |
|:---------|:-----------|:---:|:-----------------:|:----------------:|
| `bool` | `boolean` | 16 | yes | yes |
| `int16_t` / `qb::pg::smallint` | `smallint` | 21 | yes | yes |
| `int32_t` / `qb::pg::integer` | `integer` | 23 | yes | yes |
| `int64_t` / `qb::pg::bigint` | `bigint` | 20 | yes | yes |
| `float` | `real` | 700 | yes | yes |
| `double` | `double precision` | 701 | yes | yes |
| `std::string`, `std::string_view`, `const char*`, `char[N]` | `text` | 25 | yes (text=25) | yes |
| `qb::pg::bytea`, `std::vector<char>`, `std::vector<unsigned char>` | `bytea` | 17 | yes | yes |
| `qb::wall_time` | `timestamptz` | 1184 | yes | yes (also reads `timestamp` 1114) |
| `qb::uuid` | `uuid` | 2950 | yes | yes |
| `qb::json` | `json` | 114 | yes | yes |
| `qb::jsonb` | `jsonb` | 3802 | yes | yes |
| `std::optional<T>` | same OID as `T` | OID of `T` | yes (NULL when empty) | yes (NULL → empty) |

<!-- src: src/type_mapping.h:70-155 -->

`varchar` (1043) and `bpchar` (1042) columns also decode into `std::string` — the bind side always declares string parameters as `text` (25), which the server coerces. Reading a `text`/`varchar`/`bpchar` column with `as<std::string>()` works regardless of the declared parameter OID.

### Numeric scalars

Integers and floats round-trip in fixed-width binary. `int4` decode also accepts a 2-byte (`int2`) or 8-byte (`int8`) field — useful because aggregates such as `COUNT(*)` return `int8`; an out-of-range `int8` throws `std::runtime_error` rather than silently truncating. <!-- src: src/type_converter.h:362-377 -->

`float` / `double` carry `NaN`, `Infinity`, and `-Infinity` correctly in both binary and text. <!-- src: src/type_converter.h:512-539 -->

### Text and binary blobs

- **Strings.** `std::string`, `std::string_view`, `const char*`, and `char[N]` all map to `text` (25). UTF-8 bytes are sent verbatim with a length prefix; no null terminator is transmitted.
- **`bytea`.** `qb::pg::bytea` is a `std::vector<char>` subclass (OID 17). Binary form is the raw bytes; text form is PostgreSQL hex (`\x...`). Plain `std::vector<char>` and `std::vector<unsigned char>` map to `bytea` as well. <!-- src: src/type_converter.h:205-213, 292-302 -->

### Boolean

`bool` sends a single `0`/`1` byte. On decode, the binary path reads one raw byte; the text path accepts `t`, `true`, `1`, `yes`, `y`, `on` as true. <!-- src: src/type_converter.h:384-391, 540-542 -->

### JSON and JSONB

- **`qb::json` → `json` (114).** Sent and received as JSON text with a length prefix.
- **`qb::jsonb` → `jsonb` (3802).** Sent in PostgreSQL's `jsonb_recv` binary form (a version byte `1` followed by UTF-8 JSON). Unlike string-like types, **`jsonb` stays binary on the result wire** — it is not in the text-preferring set. <!-- src: src/type_converter.h:1011-1024, 1161-1170; src/common.h:391-398 -->

```cpp
#include <pgsql/pgsql.h>
using namespace qb::pg;

qb::jsonb doc = result[0][0].as<qb::jsonb>(); // jsonb column, binary on the wire
```

`qb::json` / `qb::jsonb` are `nlohmann::json` (see [`qb/json.h`]). Both decoders fold PostgreSQL's `[[key, value], ...]` array form back into a JSON object when they detect it.

### UUID

`qb::uuid` ↔ `uuid` (2950). Binary form is the 16 raw bytes; text form is the canonical `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`. Decode accepts either the bare 16 bytes or a 4-byte-prefixed 20-byte buffer. <!-- src: src/type_converter.h:646-758 -->

```cpp
qb::uuid id = result[0][0].as<qb::uuid>(); // src: tests/test-data-types-integration.cpp:1227
```

### Arrays

PostgreSQL array types (`int4[]`, `text[]`, …) are **not** auto-mapped — there is no `type_mapping` or `TypeConverter` specialization for `std::vector<T>` as an array (the only vector specializations are `bytea`). Bind array values by passing a literal array string and a `text`/array OID, and read them back with `as<std::string>()` (the array text form, e.g. `{1,2,3}`), parsing in your domain layer. Do not expect a `std::vector<int>` parameter to become an `int4[]`.

---

## NULL handling

- **Sending NULL.** Put `std::nullopt` or an empty `std::optional<T>` in `params`. The binary encoder writes the `-1` length sentinel for a disengaged optional. <!-- src: src/type_converter.h:244-253 -->
- **Reading NULL.** Use `field.is_null()`, or extract into `std::optional<U>` for a non-throwing decode (`std::nullopt` on NULL).
- **Reading NULL into a non-optional `T`** raises `qb::pg::error::value_is_null`.

```cpp
#include <pgsql/pgsql.h>
using namespace qb::pg;

// Send a SQL NULL for $2.
auto r = co_await db.execute("ins", params{42, std::optional<std::string>{}});

// Read a possibly-NULL column without throwing.
std::optional<std::string> label = row[1].as<std::optional<std::string>>();
```

`std::optional<T>` inherits `T`'s OID, so an `std::optional<int32_t>` parameter is still declared as `int4` (23). NULL detection in `from_binary` reads the length sentinel with `std::memcpy` (avoiding an unaligned load), so an aligned `-1` prefix is recognized reliably. <!-- src: src/type_mapping.h:152-155; src/type_converter.h:447-463 -->

---

## Declaring parameter types in `prepare`

`prepare()` takes a `type_oid_sequence` whose entries line up positionally with `$1 … $n`:

```cpp
#include <pgsql/pgsql.h>
using namespace qb::pg;

// src: tests/test-data-types-integration.cpp:154,198,280 (brace-init of oid)
auto pr = co_await db.prepare(
    "ins",
    "INSERT INTO t (n, label, at) VALUES ($1, $2, $3)",
    type_oid_sequence{oid::int4, oid::text, oid::timestamptz});
if (!pr) { /* inspect pr.error() */ }

auto ex = co_await db.execute("ins",
    params{42, std::string{"x"}, qb::wall_now()});
```

The parameter count and the declared-type count must each fit in a signed 16-bit field (≤ 32767); `prepare` rejects more declared types and `params` serialization throws `std::length_error` past 32767 values, because a wrapped `int16` count would desynchronize the wire stream. <!-- src: src/queries.h:732-746; src/param_serializer.h:101-118 -->

If you omit a type (`type_oid_sequence{}` or fewer entries than parameters), the server infers it. Explicit OIDs are safer for overloaded operators and for `NULL` parameters whose type the server cannot infer.

---

## Binary versus text on the wire

- **Parameters:** always binary (format code 1, applied uniformly). <!-- src: factbook line 296 -->
- **Result columns:** chosen per column by `type_oid_prefers_binary_result_format(oid)` ([`src/common.h`](../src/common.h)). After `Bind`, the client patches each `RowDescription` column's `format_code` to match what it requested, via `sync_field_format_codes_with_extended_query_bind` (because `Describe('S')` always reports `0`). <!-- src: src/common.h:382-429 -->
  - **Text on the wire** (format code 0): `text`, `varchar`, `bpchar`, `unknown`, `xml`, `cstring`, `json`, `tsvector`, `tsquery`, `gtsvector`, `name`, the `reg*` catalog types, and `money` (`cash`). These decode through the text path so they match `std::string` consumption.
  - **Binary on the wire** (format code 1): everything else — integers, floats, `bool`, `bytea`, `timestamp`/`timestamptz`, `uuid`, and `jsonb`.

A **simple** query (`execute("SELECT …")` without a prepared statement) commonly returns text columns (format code 0) regardless of OID; `field::as<T>()` branches on the actual `format_code` and routes to `from_text` or `from_binary` accordingly, so the same `as<T>()` call works on either path. <!-- src: src/field_handler.h:227-232 -->

If an extension OID (for example `citext`) is misclassified, cast it to `text` in SQL, or extend the `switch` in `common.h`.

---

## Types without a `type_mapping` entry

`src/type_converter.h` defines working `TypeConverter` specializations for several PostgreSQL types that **have no `type_mapping<T>` entry**. Their `TypeConverter::get_oid()` returns the right OID, so decoding with `as<T>()` works, but `get_type_oid<T>()` / `fill_types<T...>()` resolve them to `705` (unknown). When you bind these, **declare the OID explicitly** in `prepare`'s `type_oid_sequence`:

| C++ type (`qb::pg::detail`) | PostgreSQL | OID | Notes |
|:----------------------------|:-----------|:---:|:------|
| `numeric` | `numeric` / `decimal` | 1700 | Wraps an exact decimal **string**; sent and received as text to preserve arbitrary precision. `numeric::operator+` is a placeholder that concatenates `"(a+b)"` — it does **not** compute a sum. |
| `pgdate` | `date` | 1082 | Days since 2000-01-01. |
| `pgtime` | `time` | 1083 | Microseconds since midnight. |
| `pgtimetz` | `timetz` | 1266 | Microseconds since midnight plus a tz offset (seconds). |
| `std::chrono::duration<Rep, Period>` | `interval` | 1186 | Only the microseconds component round-trips; `days` and `months` are hard-coded `0` on send and ignored on receive — lossy for real intervals carrying days/months. |

<!-- src: src/type_converter.h:1298-1868 -->

These live in `qb::pg::detail`, not the public `qb::pg` surface. For exact decimals, most teams skip the marker type and bind a decimal **string** with the `numeric` OID, then read the column with `field.as<std::string>()`:

```cpp
#include <pgsql/pgsql.h>
using namespace qb::pg;

auto pr = co_await db.prepare("ins_amt",
    "INSERT INTO ledger (amount) VALUES ($1)",
    type_oid_sequence{oid::numeric});             // declare NUMERIC explicitly
auto ex = co_await db.execute("ins_amt", params{std::string{"1234.5600"}});

// Read it back as an exact string; convert in your domain layer.
std::string amount = row[0].as<std::string>();
```

> **One footgun to know.** Among `from_binary` specializations the length-prefix assumption differs: `wall_time` / `uuid` / `bool` expect the 4-byte wire length prefix **already stripped** (the protocol layer does this before `TypeConverter` is called), whereas `pgdate` / `pgtime` / `pgtimetz` read at `buffer.data() + 4` (assuming the prefix is still present). On the normal `as<T>()` path the framework feeds each converter the buffer it expects, so this only bites if you call `TypeConverter<...>::from_binary` directly on a hand-built buffer. <!-- src: factbook lines 322,327 -->

---

## Tuple extraction

`resultset::row::to` fills a `std::tuple` (or a `std::tie` of references), applying the same per-element rules as `as<T>()`:

```cpp
#include <pgsql/pgsql.h>
using namespace qb::pg;

std::tuple<int, std::string, bool> data;
row.to(data);

int a;
std::optional<std::string> b;   // NULL-safe element
bool c;
row.to(std::tie(a, b, c));
```

The handler walks columns by index, converting each per its declared type. <!-- src: src/field_handler.h:207-211 -->

---

## Pitfalls

- **Do not reach for retired timestamp types.** `qb::Timestamp` / `qb::UtcTimestamp` / `qb::LocalTimestamp` and `to_timestamp(...)` no longer exist. Every timestamp is `qb::wall_time`.
- **`numeric`/`pgdate`/`pgtime`/`pgtimetz`/`duration` bind to OID `705` if auto-deduced.** Pass the explicit OID in `prepare`'s `type_oid_sequence`, or the server will receive an `unknown`-typed parameter.
- **Arrays are not auto-mapped.** A `std::vector<int>` parameter does not become `int4[]`; bind/read array text and parse it yourself.
- **`std::chrono::duration` → `interval` is lossy.** Only microseconds survive; days and months are dropped.
- **`numeric::operator+` does not add.** It string-concatenates; never use it as arithmetic.
- **`bool` / `int4` binary widths are flexible on decode.** `int4` accepts `int2`/`int8` fields (handy for `COUNT(*)`), but an out-of-range `int8` throws.
- **Out-of-range timestamp text formatting throws.** Formatting a `wall_time` outside `gmtime`'s range raises `error::client_error("timestamp out of range for text conversion")`. <!-- src: src/type_converter.h:320-321 -->

---

## See also

- [queries.md](./queries.md) — `prepare`, `params`, `execute`
- [results.md](./results.md) — iterating rows, `as<T>()`
- [error_handling.md](./error_handling.md) — `value_is_null`, `field_type_mismatch`
- [connection.md](./connection.md) — connection and `qb::duration` timeouts

### Implementation map (contributors)

| Concern | File |
|:--------|:-----|
| OID enum + C++ aliases | [`src/pg_types.h`](../src/pg_types.h) |
| C++ → OID (`type_mapping<T>`) | [`src/type_mapping.h`](../src/type_mapping.h) |
| Encode / decode (`TypeConverter`) | [`src/type_converter.h`](../src/type_converter.h) |
| `params` → `Bind` buffer | [`src/param_serializer.h`](../src/param_serializer.h) |
| Column decode (`as<T>`) | [`src/field_handler.h`](../src/field_handler.h), [`src/param_unserializer.h`](../src/param_unserializer.h) |
| Result-format selection | [`src/common.h`](../src/common.h) (`type_oid_prefers_binary_result_format`) |
