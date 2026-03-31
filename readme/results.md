# Result sets and row access

How to read **rows** and **columns** from **`qb::pg::results`** (callback API and *
*`Transaction::await()` → `status.results()`**) and from **`Reply<resultset>`** after **`co_await`**.

**Header:** `#include <pgsql/pgsql.h>` pulls **`resultset.h`** / **`result_impl.h`** through the transaction stack — you
do not include them separately in application code.

---

## Implementation sketch

- **`result_impl`** ([`src/result_impl.h`](../src/result_impl.h), [`result_impl.cpp`](../src/result_impl.cpp)) holds row
  buffers, **`RowDescription`**, command tag.
- **`ResultQuery`** ([`src/commands.h`](../src/commands.h)) drives simple-query / query completion callbacks; *
  *`on_new_data_row`** / **`on_command_complete`** fill **`result_impl`** on the active **`Transaction`**.
- **`resultset`** ([`src/resultset.h`](../src/resultset.h)) is a non-owning view; **`qb::pg::results`** aliases *
  *`detail::resultset`** (**`pgsql.h`**).
- Coroutine path: successful **`co_await`** moves or copies snapshot into **`Reply<resultset>`._value** ([
  `pg_reply.h`](../src/pg_reply.h)) so later I/O does not invalidate the rowset.

---

## Coroutine path: `Reply<resultset>` / `results`

**`co_await db.execute("SELECT …")`** returns **`pg_reply_awaiter<resultset>`**, which completes to *
*`Reply<resultset>`**. **`qb::pg::results`** is a **type alias** for **`qb::pg::detail::resultset`** (**`pgsql.h`**) —
the names are interchangeable.

On success, **`result()`** exposes the rowset. Prefer **`std::move(r).result()`** if **`r`** is about to die; otherwise
**`r.result()`** gives an lvalue reference to the internal value.

```cpp
auto r = co_await db.execute("SELECT id, name FROM users LIMIT 3");
if (!r.ok())
    co_return;

qb::pg::results data = std::move(r).result();
for (auto const& row : data) {
    int id = row["id"].as<int>();
    std::string name = row["name"].as<std::string>();
}
```

**`Reply`** API (**`src/pg_reply.h`**): **`ok()`**, **`explicit operator bool`**, **`error()`**, **`result()`** (*
*`&` / `&&`** overloads).

---

## Callback path: `results` parameter

```cpp
db.execute("SELECT id, name FROM users",
    [](qb::pg::transaction&, qb::pg::results result) {
        for (auto const& row : result) {
            int id = row["id"].as<int>();
            (void)row["name"].as<std::string>();
        }
    },
    [](qb::pg::error::db_error const&) {});
```

**Lifetime:** **`row`** and **`field`** proxies reference **parent storage** inside **`results`**. Do not store **`row`
** or **`field`** past the **`results`** object’s lifetime.

---

## `qb::pg::results` (container of rows)

**`src/resultset.h`**, backed by **`result_impl`**.

| Capability     | Notes                                                                       |
|:---------------|:----------------------------------------------------------------------------|
| **Iteration**  | Range-based **`for`**, **`begin()` / `end()`**                              |
| **Size**       | **`size()`**, **`empty()`**                                                 |
| **Random row** | **`operator[]`** (0-based)                                                  |
| **Metadata**   | **`columns_size()`**, **`field(i)`**, **`row_description()`**               |
| **JSON**       | **`results.json()`** → **`qb::json`** (handy for logging or HTTP responses) |

```cpp
if (results.empty())
    return;

for (qb::pg::resultset::row const& row : results) {
    (void)row.row_index();
}
```

---

## `qb::pg::resultset::row`

- **`row[i]`** — column index (0-based).
- **`row["name"]`** — case-sensitive column name; throws if missing (see tests for exact error type).
- **`row.to(tuple)`** / **`row.to(std::tie(a, b, c))`** — fill a tuple or tied references ([types.md](./types.md)).
- **`row.row_index()`** — 0-based index in the result set.

---

## `qb::pg::resultset::field`

- **`as<T>()`** — decode to **`T`**; throws **`qb::pg::error::value_is_null`** if SQL NULL and **`T`** is not *
  *`std::optional<U>`**.
- **`to(value&)`** — out-parameter style; returns **false** for NULL on some paths — prefer **`std::optional`** or *
  *`is_null()`** for clarity.
- **`is_null()`**, **`name()`**, **`description()`**, **`input_buffer()`** (advanced / debugging).

**NULL-safe decoding:** **`field.as<std::optional<U>>()`** — empty optional when NULL.

**Type mismatches:** Wrong **`T`** for wire type → **`field_type_mismatch`** (
see [error_handling.md](./error_handling.md)).

**Text vs binary:** **`description().format_code`** reflects what the server sent; converters branch in **`FieldHandler`
** (**`field_handler.h`**). Simple queries often yield **text** columns; prepared queries often yield **binary** for
scalars — [types.md](./types.md).

---

## `await()` → `status.results()`

```cpp
auto st = db.execute("SELECT 1 AS x", qb::pg::discard_query, qb::pg::discard_error).await();
if (st) {
    qb::pg::results r = st.results();
    (void)r[0]["x"].as<int>();
}
```

**`status.results()`** returns a view tied to the **`status`** object’s last **`result_impl`** for that drain — same
iteration rules as above.

---

## Commands with no rows

**`INSERT` / `UPDATE` / `DELETE`** without **`RETURNING`** typically produce an **empty** **`results`** with **`ok()`**
true at the **`Reply`** level — still check **`ok()`** for constraint violations.

**`DDL`** (e.g. **`CREATE TABLE`**) — empty rowset; success is **`ok()`** / clean **`status`**.

---

## JSON export

```cpp
qb::json j = result.json();
```

Useful for diagnostics, admin APIs, or quick serialization — not a substitute for typed **`as<T>()`** in hot paths.

---

## Copy and large results

The module’s **COPY protocol** support is limited (see root **README** limitations). For large **SELECT** results,
streaming is not the same as libpq’s single-row mode — prefer **cursors** / **`FETCH`** in SQL (*
*`test-transaction-advanced.cpp`** patterns) or pagination.

---

## Related

- [types.md](./types.md) — conversions, **`params`**, NULL, tuple decode
- [error_handling.md](./error_handling.md) — **`value_is_null`**, **`field_type_mismatch`**
- [queries.md](./queries.md) — **`execute`**, prepared paths  
