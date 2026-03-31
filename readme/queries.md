# Query execution

How **`qbm-pgsql`** runs **simple** queries, **extended** (prepared) queries, **files**, and **LISTEN/NOTIFY** — *
*coroutine** and **callback** overloads for every SQL-facing operation except where noted
in [transaction.md](./transaction.md) / [connection.md](./connection.md).

**Include:** `#include <pgsql/pgsql.h>`.

---

## Ordered async (callbacks)

All callback overloads **enqueue** and return **`Transaction&`** immediately. Completion runs under *
*`qb::io::async::run_once()`** (or **`Transaction::await()`**). **`await()`** after each call is **optional** — use it
only when you need a synchronous drain on the current thread. See [transaction.md](./transaction.md).

---

## `execute` callback SFINAE

From [`src/transaction.inl`](../src/transaction.inl) **`Transaction::execute(expr, on_success, on_error)`**:

| Your success lambda           | Command object    | Use case                                                                |
|:------------------------------|:------------------|:------------------------------------------------------------------------|
| **`(Transaction&, results)`** | **`ResultQuery`** | **`SELECT`** / any statement that returns rows you read in the callback |
| **`(Transaction&)`** only     | **`Query`**       | Fire-and-forget or you read side effects elsewhere                      |

Wrong arity fails at compile time (`static_assert` message in **`transaction.inl`**).

**Coroutine:** only **`co_await execute(expr)`** → **`Reply<resultset>`**; no SFINAE split (always a resultset
payload, possibly empty).

---

## Protocol paths (wire)

| API                                     | Typical wire path             | Notes                                                                        |
|:----------------------------------------|:------------------------------|:-----------------------------------------------------------------------------|
| **`execute("SELECT …")`**               | Simple query (**Q**)          | Server often returns **text** columns — [types.md](./types.md).              |
| **`prepare` + `execute(name, params)`** | Parse / Bind / Execute / Sync | Parameters **binary**; result formats from OID heuristics.                   |
| **`LISTEN` / `NOTIFY` / `UNLISTEN`**    | Simple query                  | Payload/channel safety: [`src/pg_notify_sql.h`](../src/pg_notify_sql.h). |

One connection = **one serial** stream ([`transaction.cpp`](../src/transaction.cpp) **`_queries`** queue).

---

## Simple SQL (no prior `prepare`)

**Coroutine**

```cpp
auto r = co_await db.execute("SELECT version()");
if (!r.ok())
    co_return;
qb::pg::results rows = std::move(r).result();
```

**Callback — with rowset**

```cpp
db.execute(
    "SELECT id, name FROM users WHERE id = 1",
    [](qb::pg::transaction& tr, qb::pg::results rs) {
        (void)tr;
        for (auto const& row : rs)
            (void)row["id"].as<int>();
    },
    [](qb::pg::error::db_error const&) {});
```

**Callback — no rowset**

```cpp
db.execute("NOTIFY chan, 'x'", [](qb::pg::transaction&) {}, [](qb::pg::error::db_error const&) {});
```

**`query(sql)`** — alias of **`execute(sql)`** ([`transaction.h`](../src/transaction.h)).

**Blocking:** **`qb::io::async::run_sync(db.execute("SELECT 1"))`** — no **`.await()`** on **`pg_reply_awaiter`**.

---

## Prepared statements

**Storage:** **`PreparedQueryStorage`** LRU in [`src/queries.h`](../src/queries.h).

**Coroutine — prepare + execute**

```cpp
auto pr = co_await db.prepare("by_email",
    "SELECT id FROM users WHERE email = $1",
    qb::pg::type_oid_sequence{qb::pg::oid::text});
if (!pr.ok()) { /* … */ }
auto r = co_await db.execute("by_email", qb::pg::params{email});
```

**Callback — prepare + execute**

```cpp
db.prepare(
    "by_email",
    "SELECT id FROM users WHERE email = $1",
    qb::pg::type_oid_sequence{qb::pg::oid::text},
    [](qb::pg::transaction&, qb::pg::detail::PreparedQuery const&) {},
    [](qb::pg::error::db_error const&) {});
db.execute("by_email", qb::pg::params{email},
    [](qb::pg::transaction&, qb::pg::results rs) { (void)rs; },
    [](qb::pg::error::db_error const&) {});
```

**Alternate argument order:** **`execute(name, on_success, params)`** ([`transaction.h`](../src/transaction.h)).

---

## `execute_file` / `prepare_file`

**Implementation:** [`transaction.inl`](../src/transaction.inl) reads the file then delegates to **`execute` / `prepare`
**; file errors can throw **`query_error`** after invoking **`on_error`**.

**Coroutine**

```cpp
auto r = co_await db.execute_file(std::filesystem::path{"schema.sql"});
auto pr = co_await db.prepare_file("mig", std::filesystem::path{"mig.sql"},
    qb::pg::type_oid_sequence{});
```

**Callback**

```cpp
db.execute_file(std::filesystem::path{"schema.sql"},
    [](qb::pg::transaction&, qb::pg::results rs) { (void)rs; },
    [](qb::pg::error::db_error const&) {});
db.prepare_file("mig", std::filesystem::path{"mig.sql"},
    qb::pg::type_oid_sequence{},
    [](qb::pg::transaction&, qb::pg::detail::PreparedQuery const&) {},
    [](qb::pg::error::db_error const&) {});
```

---

## NOTIFY (publisher)

**Coroutine**

```cpp
auto r = co_await db.notify("chan", "payload");
(void)co_await db.notify("chan"); // omit payload in SQL
```

**Callback**

```cpp
db.notify("chan", "payload", qb::pg::discard_query, qb::pg::discard_error);
db.notify("chan", qb::pg::discard_query, qb::pg::discard_error);
```

---

## LISTEN / UNLISTEN

**Coroutine**

```cpp
(void)co_await db.listen("chan");
(void)co_await db.unlisten("chan");
(void)co_await db.unlisten_all();
```

**Callback**

```cpp
db.listen("chan", qb::pg::discard_query, qb::pg::discard_error);
db.unlisten("chan", qb::pg::discard_query, qb::pg::discard_error);
db.unlisten_all(qb::pg::discard_query, qb::pg::discard_error);
```

**Delivery:** register **`db.on_incoming_notify([](qb::pg::notification&& n) { … })`** on plain **`tcp::database`** ([
`pgsql.h`](../pgsql.h)). **`notification`** holds channel, payload, process id.

**`notify_co_consumer`:** optional **`on_notify`**, **`on_notify_dropped`**, **`co_await receive()`** — same class as *
*`notify_cb_consumer`**. See **`test-notify.cpp`** for **`io_pump`** / ordering.

---

## `constexpr` discards

```cpp
using namespace qb::pg;
db.execute("VACUUM ANALYZE", discard_query, discard_error);
db.prepare("n", "SELECT 1", {}, discard_prepare, discard_error);
```

Defined in **`pgsql.h`**.

---

## Quick matrix

| Goal              | Coroutine                                           | Callback                                  |
|:------------------|:----------------------------------------------------|:------------------------------------------|
| Simple SQL        | **`co_await execute` / `query`**                    | **`execute(..., cb, err)`**               |
| Prepare           | **`co_await prepare`**                              | **`prepare(..., cb, err)`**               |
| Prepared exec     | **`co_await execute(name, params)`**                | **`execute(name, params, cb, err)`**      |
| File              | **`co_await execute_file` / `prepare_file`**        | **`execute_file` / `prepare_file`** + cbs |
| NOTIFY            | **`co_await notify`**                               | **`notify`** + cbs                        |
| LISTEN / UNLISTEN | **`co_await listen` / `unlisten` / `unlisten_all`** | same + cbs                                |

---

## QB actor vs standalone

| Context        | Pattern                                                                                                 |
|:---------------|:--------------------------------------------------------------------------------------------------------|
| **Actor**      | Enqueue callbacks; **`run_once`** between messages; or **`task` + co_await** on **`coro_scheduler()`**. |
| **Test / CLI** | **`run_sync`**, or **`await()`** after chains.                                                          |

---

## Related

- [transaction.md](./transaction.md) — **`begin`**, **`then`/`error`**, **`await()`**
- [results.md](./results.md) — **`results`**, **`Reply`**
- [types.md](./types.md) — **`params`**, OIDs
- [error_handling.md](./error_handling.md) — **`on_err`**, **`Error` command**  
