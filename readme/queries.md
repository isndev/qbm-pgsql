# Query execution

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qbm-pgsql @ qb 2.6.0 (C++20 default, C++23
> supported)

Run simple queries, extended (prepared) queries, SQL files, and `LISTEN`/`NOTIFY` against a `qb::pg::tcp::database` (or
`qb::pg::tcp::ssl::database` on an OpenSSL build), using either the coroutine or the callback overload of each
SQL-facing operation.

**Prerequisites:** [connection.md](./connection.md) (open a connection first) — **See also:
** [transaction.md](./transaction.md), [results.md](./results.md), [types.md](./types.md), [error_handling.md](./error_handling.md)

**Include:** `#include <pgsql/pgsql.h>` — public surface is in namespace `qb::pg`.

`qbm-pgsql` is a compiled static library (`qbm::pgsql`); link it with `target_link_libraries(app PRIVATE qbm::pgsql)`.
It is not header-only.

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

The overload selects the command type from your lambda's arity via `if constexpr (std::is_invocable_v<...>)`
([`transaction.inl`](../src/transaction.inl)). Only these two arities are valid; any other success
signature stops compilation when the selected command instantiates against your callback.

**Coroutine:** only **`co_await execute(expr)`** → **`Reply<resultset>`**; no SFINAE split (always a resultset
payload, possibly empty). Check **`reply.ok()`** before **`reply.result()`**.

---

## Protocol paths (wire)

| API                                     | Typical wire path             | Notes                                                                    |
|:----------------------------------------|:------------------------------|:-------------------------------------------------------------------------|
| **`execute("SELECT …")`**               | Simple query (**Q**)          | Server often returns **text** columns — [types.md](./types.md).          |
| **`prepare` + `execute(name, params)`** | Parse / Bind / Execute / Sync | Parameters **binary**; result formats from OID heuristics.               |
| **`LISTEN` / `NOTIFY` / `UNLISTEN`**    | Simple query                  | Payload/channel safety: [`src/pg_notify_sql.h`](../src/pg_notify_sql.h). |

One connection = **one serial** stream ([`transaction.cpp`](../src/transaction.cpp) **`_queries`** queue).

The extended (prepared) path on the wire:

```mermaid
sequenceDiagram
    participant C as client (qb::pg)
    participant S as PostgreSQL server
    C->>S: Parse (name, SQL, param OIDs)
    C->>S: Bind (params, binary)
    C->>S: Execute · Sync
    S-->>C: ParseComplete · BindComplete
    S-->>C: RowDescription
    loop each row
        S-->>C: DataRow
    end
    S-->>C: CommandComplete · ReadyForQuery
    Note over C: decoded into qb::pg::results<br/>field.as&lt;T&gt;() · std::optional&lt;T&gt; for NULL
```

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

**`query(sql)`** is the coroutine simple-query entry point — it returns the same
**`pg_reply_awaiter<resultset>`** as **`co_await execute(sql)`** ([`transaction.h`](../src/transaction.h)). There is no
callback
**`query`** overload; use **`execute(sql, cb, err)`** for the callback path.

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

## Inline parameterized query (coroutine)

For a one-shot parameterized query, **`co_await db.query(sql, args...)`** binds the arguments and runs them in a single
call — no manual `prepare` name, no `type_oid_sequence`:

```cpp
auto r = co_await db.query("SELECT id, name FROM users WHERE id = $1", user_id);
if (r) for (auto [id, name] : r.result().all<int, std::string>()) { /* ... */ }

auto sum = co_await db.query("SELECT $1::int + $2::int", 2, 3);   // -> 5
```

Parameter OIDs are deduced from the C++ argument types. Internally it runs through the **unnamed** prepared statement (
`""`), so it does **not** pollute the prepared-statement cache and keeps full per-column **binary** result decoding.
Cost is two server round-trips (Parse+Describe, then Bind+Execute) — the same as a manual `prepare`+`execute`, but one
call; it returns **`qb::io::async::task<Reply<resultset>>`**. The overload requires at least one bound argument, so
`query(sql)` with no args still resolves to the simple-query awaiter above. For a hot, repeated query prefer a **named**
`prepare` (one round-trip after the first).

---

## Bulk data — `COPY` (coroutine)

`COPY` is PostgreSQL's high-throughput bulk path. Rows are never buffered in a result set in either direction, but only
**`copy_out`** (and **`query_stream`** below) is truly **constant-memory**. **`copy_in` is not** — it drains the whole
`source` into the output pipe before yielding (see the caveat under `copy_in`).

**`COPY … TO STDOUT`** → `db.copy_out(sql, sink)` delivers each `CopyData` chunk to `sink` as it arrives, in **constant
memory** regardless of export size:

```cpp
std::ofstream f("users.csv");
auto r = co_await db.copy_out("COPY users TO STDOUT (FORMAT csv)",
                              [&](std::string_view chunk){ f.write(chunk.data(), chunk.size()); });
```

**`COPY … FROM STDIN`** → `db.copy_in(sql, source)` pulls chunks from `source` (return `std::nullopt` to finish) and
sends each as `CopyData`, then `CopyDone`. A one-shot overload takes the whole payload:

```cpp
// streaming source (one row per call):
co_await db.copy_in("COPY t (id, v) FROM STDIN",
                    [&]() -> std::optional<std::string> { return next_line(); });
// or all at once:
co_await db.copy_in("COPY t FROM STDIN", std::string{"1\tx\n2\ty\n"});
```

The chunk bytes are the COPY wire format the statement selects (text/CSV: rows ending in `\n`; binary: the framed binary
stream — chunks need not align to rows). Both return `Reply<resultset>` (`ok()` on success, the row count in the
`COPY n` tag; the result set is empty). A failing COPY — bad table, or a `source` that throws — resolves the awaiter
with the **error** (the client sends `CopyFail`) and leaves the connection usable. A throwing `source` never corrupts
the protocol stream.

> **`copy_in` is _not_ constant-memory.** Unlike `copy_out`, it does **not** back-pressure on the socket: when the
`CopyInResponse` arrives it calls `source` in a tight loop and writes every returned chunk into the output pipe *
*synchronously**, only sending `CopyDone` once `source` returns `std::nullopt`. The entire input is therefore staged in
> the connection's write buffer before the awaiter yields. That buffer has a ceiling — exceed it and the connection is
> force-disconnected — so size your input (or your own external batching of separate `copy_in` calls) accordingly. A
`source` callback that lazily reads a row at a time bounds your *application* memory, but the bytes still pile up in the
> pipe; it does not make the transfer constant-memory. Only `copy_out` and `query_stream` stream in constant memory.
<!-- src: qbm/pgsql/pgsql.h (copy_out, copy_in, on_copy_data, on_copy_in_response) -->

---

## Streaming a large result — `query_stream` (coroutine)

A plain `query()` buffers the **whole** result set. To process a result too large to fit in memory, *
*`db.query_stream(sql, batch_size, on_row)`** runs the query through a server-side `CURSOR` and invokes `on_row` for
each row, fetching `batch_size` rows per round trip — only one batch is ever held, so memory is constant regardless of
result size.

```cpp
std::uint64_t total = 0;
co_await db.query_stream("SELECT amount FROM ledger", 1000,
                         [&](auto row){ total += row[0].template as<long>(); });
```

Cursors need a transaction. If the connection is **idle**, `query_stream` opens its own (`BEGIN` … `COMMIT`, or
`ROLLBACK` on failure); if it is **already in a transaction** (`db.in_transaction()`), the cursor is declared there and
only closed — the caller's transaction is untouched. It returns `Reply<void>` (`ok()` once the whole result streamed,
else the server error), and the connection stays usable after a failure. If `on_row` throws, the cursor is closed (and a
self-opened transaction rolled back) and the exception is **rethrown**. The `row` passed to `on_row` is a view valid
only for that call — copy out anything you keep.
<!-- src: qbm/pgsql/pgsql.h (query_stream, in_transaction) -->

---

## `execute_file` / `prepare_file`

Both read the file then delegate to **`execute` / `prepare`**. The two paths report a file error differently:

- **Callback overloads** ([`transaction.inl`](../src/transaction.inl)) invoke **`on_error`** with an
  **`error::query_error`**, then **rethrow** it — wrap the call in `try`/`catch` if a missing or unreadable file
  must not propagate.
- **Coroutine overloads** ([`transaction_coro.inl`](../src/transaction_coro.inl)) do not throw; they surface the
  file error as a failed **`Reply`** (check **`reply.ok()`**). The coroutine **`prepare_file`** yields
  **`Reply<PreparedQuery>`**, matching **`co_await prepare`**.

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

**Delivery:** register **`db.on_incoming_notify([](qb::pg::notification&& n) { … })`** on a plain **`tcp::database`** ([
`pgsql.h`](../pgsql.h)). **`qb::pg::notification`** holds **`channel`**, **`payload`**, and the originating backend
PID **`server_backend_pid`**. The handler runs on the I/O thread when a `NotificationResponse` arrives.

**`notify_co_consumer`:** optional **`on_notify`**, **`on_notify_dropped`**, **`co_await receive()`** —
**`notify_cb_consumer`** is an alias of the same class ([`pgsql.h`](../pgsql.h)). See
[`tests/integration/notify/listen-notify.cpp`](../tests/integration/notify/listen-notify.cpp) for the `io_pump` and ordering patterns.

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

- [transaction.md](./transaction.md) — **`begin`**, **`then`** / **`error`**, **`await()`**
- [results.md](./results.md) — **`results`**, **`Reply`**
- [types.md](./types.md) — **`params`**, OIDs, `timestamptz` ↔ **`qb::wall_time`**
- [error_handling.md](./error_handling.md) — **`error::db_error`**, the **`Error`** command
