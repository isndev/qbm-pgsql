# qbm-pgsql documentation map

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qbm-pgsql @ qb 3.0.0 (C++20 default, C++23
> supported)

This is the table of contents for the qbm-pgsql narrative documentation: seven topic pages covering connection
management, query execution, transactions, result sets, type mapping, error handling, and integration testing, ordered
as a learning path.

**Prerequisites:** working knowledge of the qb framework — see [`qb/README.md`](https://github.com/isndev/qb/blob/main/README.md) and the qb [
`readme/`](https://github.com/isndev/qb/tree/main/readme/) docs for `qb-io` async, coroutines, and `run_sync`. **See also:** the module front
door [`../README.md`](../README.md) for positioning, the build matrix, and a quickstart.

## What this module is

qbm-pgsql is an asynchronous PostgreSQL client built on the qb-io event loop. It implements the PostgreSQL wire protocol
directly — connection handshake and authentication, simple and prepared statements, transactions and savepoints, result
decoding, type mapping, and LISTEN/NOTIFY — over a single non-blocking TCP (or TLS) session. The public surface lives in
the `qb::pg` namespace; `qb::pg::detail` holds the implementation. The umbrella header is `<qbm/pgsql/pgsql.h>`.

The module is a **compiled static library**, not header-only. The build registers it through `qb_register_module` with a
`SOURCES` list of seventeen translation units (`qbm/pgsql/CMakeLists.txt:61-85`), so consuming it links a real archive
under the alias `qbm::pgsql`. It compiles at C++20 by default and C++23 when `QB_CXX_STANDARD=23`; the standard is
governed by the framework, not the module, and propagates to consumers as a compile feature.

Every database operation has two interchangeable completion models with the same method names:

| Model         | How work finishes                                                                   | Drive it with                                                                                 |
|---------------|-------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| **Coroutine** | Overloads *without* callbacks return an awaiter; `co_await` yields `Reply<T>`.      | `co_await` inside a coroutine, or `qb::io::async::run_sync(...)` from synchronous code.       |
| **Callback**  | Overloads *with* success/error callbacks return `Transaction&` for fluent chaining. | `qb::io::async::run` / `run_once`, optionally `Transaction::await()` for a `status` snapshot. |

Use one style per call stack. Do not place undriven coroutine awaiters inside a callback body — see the "Large-project
conventions" note in `pgsql.h`.

## Integration in one place

You consume qbm-pgsql through the qb module loader, not `find_package`:

<!-- src: qbm/pgsql/README.md:185-187 -->

```cmake
add_subdirectory(qb)                                   # the framework first
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")     # discovers and adds qbm modules
# ...
target_link_libraries(your_app PRIVATE qbm::pgsql)     # links qb::core PUBLIC, qb::io transitively
```

```cpp
#include <qbm/pgsql/pgsql.h>   // connection, transactions, Reply, with_transaction, types, discards
```

`DEPENDS qb-core` in the module's `CMakeLists.txt` is build wiring: it links `qb::core` `PUBLIC`, which brings in
`qb::io` transitively. At the API level the client depends on qb-io (`qb::io::async`, the TCP/TLS transport, the
coroutine `task`, `run_sync`); qb-core is required only when you hold a `database` inside an actor. The module's
`CMakeLists.txt` guards on `QB_FOUND` and returns early if the framework is absent, so `add_subdirectory(qb)` must come
first.

## A note on TLS and time

- **TLS** — there is no pgsql-specific SSL option. Transport security follows the framework-wide `QB_HAS_SSL` (derived
  from OpenSSL detection). With SSL on, the `qb::pg::tcp::ssl::database` alias exists; with it off, the build emits a
  status note and only cleartext TCP is available. Transport is a compile-time choice (`tcp::database` vs
  `tcp::ssl::database`), independent of the connection-string scheme.
- **Time** — connect, statement, and transaction timeouts are `qb::duration` (truncated to whole milliseconds where the
  wire requires it). The PostgreSQL `timestamptz` type (OID 1184) maps to `qb::wall_time`, round-tripped as integer
  microseconds. The PostgreSQL wire epoch (microseconds since 2000-01-01) is an internal native encoding decoded inside
  the type layer — it is never surfaced as `qb::duration`. Retired tokens such as `qb::Timestamp` do not appear anywhere
  in this API.

## Pages

Read top to bottom for a first pass. Each row links the page and gives its one-line scope.

| # | Page                                                | What it covers                                                                                                                                                                                                                                   |
|---|-----------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1 | [Connection management](./connection.md)            | DSN parsing, `connection_options`, the `connect` awaiter (no callback connect), handshake and authentication (MD5, SCRAM-SHA-256, cleartext), `disconnect` / `prepare_reconnect`, keepalive, and TLS.                                            |
| 2 | [Query execution](./queries.md)                     | Simple and prepared statements: `execute` / `query` / `prepare`, parameter binding, `execute_file` / `prepare_file`, the prepared-statement LRU, LISTEN / NOTIFY, and the `discard_*` no-op callbacks.                                           |
| 3 | [Transactions and command queues](./transaction.md) | `begin` / `commit` / `rollback`, `transaction_mode` (isolation, read-only, deferrable), savepoints, the callback `then` / `error` chain and optional `await()`, the coroutine `with_transaction` helper, `transaction_abort`, and `set_timeout`. |
| 4 | [Result sets and row access](./results.md)          | `results`, `row`, and `field`; `as<T>()` and tuple extraction; `std::optional<T>` for NULL; the deep-snapshot ownership of coroutine replies; and `results.json()`.                                                                              |
| 5 | [Data types and wire formats](./types.md)           | The OID and `TypeConverter` model; scalars, `qb::wall_time` (`timestamptz`), `qb::uuid`, JSON/JSONB, BYTEA, NUMERIC, DATE/TIME; `type_mapping`, `nullable<T>`, and binary versus text formats.                                                   |
| 6 | [Error handling](./error_handling.md)               | `Reply<T>` success/failure, `error::db_error`, SQLSTATE codes, `error::client_error` and `value_is_null`, and the callback `status` snapshot after `await()`.                                                                                    |
| 7 | [Integration testing](./testing.md)                 | The `QB_PG_*` environment variables (`QB_PG_DSN`, `QB_PG_SSL_DSN`, `QB_PG_INVALID_DSN`), running the suite under CTest, and the test-to-feature map.                                                                                             |

## Suggested learning order

The numbering is the recommended path; you do not need all of it for every task.

1. **Connect (1).** Start here — every operation needs a connected `database`, and the page establishes the
   coroutine-versus-callback split you will use throughout.
2. **Run statements (2, 4).** Query execution and result access are the core of day-to-day work. Read them together:
   page 2 sends SQL, page 4 reads what comes back.
3. **Group work atomically (3).** Transactions, savepoints, and `with_transaction` build on the single-statement model
   from pages 2 and 4.
4. **Map your data (5).** The type page is reference material; consult it when binding parameters or decoding columns,
   especially for timestamps, UUIDs, JSON, and NUMERIC.
5. **Handle failure (6).** `Reply<T>`, SQLSTATE, and the `status` snapshot apply to every operation; read this once you
   are past the happy path.
6. **Test it (7).** The integration suite is executable documentation; the test map points you at the example that
   exercises any feature.

If you only need to issue one-shot queries, read pages 1, 2, and 4. If you are writing transactional workloads, add page

3. Reach for pages 5 and 6 as reference whenever a type or error question comes up.

## Examples as specification

The integration tests under [`../tests/`](../tests/) are executable documentation. When a signature or behavior is
unclear, grep a test name and read it:

- `integration/api/coro-api.cpp` — coroutines, `with_transaction`, savepoints, `run_sync`.
- `integration/transaction/transaction-basic.cpp` — callback `begin`, nested savepoints, `await()`.
- `integration/notify/listen-notify.cpp` — LISTEN/NOTIFY, `notify_co_consumer`, and pump ordering.
- `integration/transaction/transaction-advanced.cpp` — timeouts, constraints, cursors, `set_timeout`.
- `integration/prepared/prepared-statements.cpp` — the prepared-statement LRU, eviction, and large results.
- `integration/protocol/wire-formats.cpp` — COPY edges, binary columns, and end-to-end protocol coverage.

## See also

- [`../README.md`](../README.md) — module positioning, build matrix, and quickstart.
- [`qb/README.md`](https://github.com/isndev/qb/blob/main/README.md) — the qb framework this module builds on.
- The qb framework [`readme/`](https://github.com/isndev/qb/tree/main/readme/) — `qb-io` async, coroutines, and `run_sync`.
