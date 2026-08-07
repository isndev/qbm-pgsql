# qbm-pgsql — asynchronous PostgreSQL client for the qb Actor Framework (QBAF)

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qbm-pgsql @ qb 3.0.0 (C++20 default, C++23
> supported)

A non-blocking PostgreSQL wire-protocol client for the qb Actor Framework (QBAF), built on qb-io: connect, run simple
and prepared SQL, drive transactions and savepoints, and consume LISTEN/NOTIFY — with the same method names for
`co_await` and callback styles.

**Prerequisites:** a working qb framework checkout (see [qb/README.md](https://github.com/isndev/qb/blob/main/README.md)) and a reachable PostgreSQL
server — **See also:** [readme/README.md](./readme/README.md) (technical
index), [readme/connection.md](./readme/connection.md), [readme/transaction.md](./readme/transaction.md)

---

## What this module is

`qbm-pgsql` speaks the PostgreSQL v3 frontend/backend protocol directly over a qb-io socket. There is no `libpq`
dependency: connection handshake, authentication (SCRAM-SHA-256, MD5, cleartext), the simple- and extended-query
protocols, type encoding, and LISTEN/NOTIFY are all implemented on top of `qb::io::async`. All wire I/O is non-blocking
and runs on the qb-io event loop; you reach completion either by `co_await` or through callback overloads that the loop
drains.

The public surface lives in `qb::pg` (internals in `qb::pg::detail`). A single header pulls in everything an application
needs:

```cpp
#include <qbm/pgsql/pgsql.h>   // brings in <qb/io/async.h> transitively
```

`qbm-pgsql` is a **compiled library** (17 translation units), aliased `qbm::pgsql` — static by default, or shared when
`BUILD_SHARED_LIBS`/`QB_BUILD_SHARED_LIBS` is on. It is **not** header-only — link it; including the header alone will
not resolve the protocol, type, and error symbols.

### How it relates to qb-core

The module depends on `qb::core` at the build level (`qb_register_module(... DEPENDS qb-core)`), which transitively
brings in `qb::io`. At the API level you use qb-io types: a `database` object is driven by whatever thread runs
`qb::io::async`. You can use it from a plain executable that calls `qb::io::async::init()` and drives the loop yourself,
or hold a `database` inside a `qb::Actor` and let the actor's `VirtualCore` tick drive the same loop. The client does
not require actors.

---

## Feature overview

| Area                      | What you get                                                                                                                                                                                                                          |
|:--------------------------|:--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Connection**            | Async handshake from a DSN (`tcp://user:pass@host:port[db]`); SCRAM-SHA-256 / MD5 / cleartext auth; per-connection `connect` timeout (`qb::duration`); `disconnect()` + `prepare_reconnect()` to reuse an object; optional keepalive. |
| **TLS**                   | Available when the framework is built with `QB_HAS_SSL` (OpenSSL). The client sends an `SSLRequest` and upgrades the socket via `qb::pg::tcp::ssl::database`. Without SSL, cleartext TCP only.                                        |
| **Simple & prepared SQL** | `execute` / `query` (simple protocol); `prepare` + parameterized `execute` (extended protocol) with a client-side prepared-statement LRU (local eviction only; no server-side `DEALLOCATE`); `execute_file` / `prepare_file`.         |
| **Transactions**          | `begin` / `commit` / `rollback`, `transaction_mode` (isolation, read-only, deferrable), nested `savepoint` / `release_savepoint` / `rollback_savepoint`, and the `with_transaction` coroutine wrapper.                                |
| **Statement timeout**     | `set_timeout(qb::duration)` arms a `SET LOCAL statement_timeout` on the next `BEGIN` (transaction-scoped; distinct from the connect timeout).                                                                                         |
| **Results**               | `results` / `row` / `field` views; `field::as<T>()` and `to()`; `std::optional<T>` for NULL columns; `results.json()`.                                                                                                                |
| **Types**                 | Scalars, `qb::uuid`, `qb::json` / `qb::jsonb`, `bytea`, NUMERIC, DATE/TIME, INTERVAL; `timestamptz` (OID 1184) maps to `qb::wall_time` with integer-microsecond round-trip. See [readme/types.md](./readme/types.md).                 |
| **Pub/sub**               | `notify`, `listen` / `unlisten` / `unlisten_all`, an `on_incoming_notify` hook, and a `notify_co_consumer` with `co_await receive()`.                                                                                                 |
| **Errors**                | `Reply<T>` carries either a result or a typed `error::db_error` (severity, SQLSTATE, detail); `Transaction::await()` returns a `status`.                                                                                              |

---

## Two styles, one API

Each operation has two overload families with the **same method name**:

- **Coroutines** — overloads without callbacks return an awaiter; `co_await` yields a `Reply<T>` (`T` is `resultset`,
  `PreparedQuery`, or `void`). Test for success with `if (reply)` or `reply.ok()`; read the payload with
  `reply.result()` and the failure with `reply.error()`.
- **Callbacks** — overloads taking success/error lambdas return a `transaction&` immediately and enqueue work. The queue
  drains whenever a thread runs `qb::io::async::run_once()` / `run()` on that connection. Call `await()` only when you
  want a synchronous drain on the current thread (tests, init) or a `status` snapshot.

`qb::io::async::run_sync(awaiter)` bridges a single coroutine awaiter to blocking code — the idiom used throughout the
integration tests for setup and one-shot queries.

Pick one style per call stack. Inside a `begin(...)` callback body, use callbacks; do not mix in undriven coroutine
awaiters there. The full coverage matrix (which operations expose callbacks vs. coroutine-only) is
in [readme/queries.md](./readme/queries.md).

---

## Quickstart: connect, query, transact

A self-contained program. It connects, runs one simple query, and commits an insert inside a coroutine transaction.
Build it as a normal executable linked against `qbm::pgsql`.

<!-- src: qbm/pgsql/tests/integration/api/coro-api.cpp (connect / with_transaction / query shapes) -->

```cpp
#include <qbm/pgsql/pgsql.h>
#include <qb/io/async.h>
#include <iostream>

using namespace qb::pg;

int main() {
    qb::io::async::init();                 // once per thread, before the first DB op
    tcp::database db;                       // one TCP session, one protocol state machine

    // connect() returns an awaiter; run_sync bridges it to blocking code.
    if (!qb::io::async::run_sync(db.connect("tcp://user:pass@localhost:5432[mydb]"))) {
        std::cerr << "connect failed\n";
        return 1;
    }

    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        // Simple query: co_await yields Reply<resultset>.
        auto r = co_await db.query("SELECT 42 AS answer");
        if (r.ok() && r.result().size() == 1)
            std::cout << "answer = " << r.result()[0]["answer"].as<int>() << '\n';

        // Transaction: BEGIN, body, then COMMIT (or ROLLBACK on transaction_abort).
        auto tx = co_await with_transaction(db, [](transaction &tr)
                                                    -> qb::io::async::task<void> {
            auto ins = co_await tr.execute(
                "CREATE TEMP TABLE t(v int); INSERT INTO t VALUES (1)");
            if (!ins)
                throw transaction_abort{ins.error()};   // roll back, propagate the error
        });
        if (!tx.ok())
            std::cerr << "transaction failed: " << tx.error().what() << '\n';
    }());

    db.disconnect();
    return 0;
}
```

Key points the example demonstrates:

- `tcp::database` is `qb::pg::detail::Database<qb::io::transport::tcp, void>` — one object owns one session, one
  protocol state machine, and one prepared-statement LRU.
- `transaction` is `qb::pg::transaction`, the public alias for the base transaction type that `tcp::database` derives
  from.
- `with_transaction(db, body)` issues `BEGIN`, runs the coroutine body, and emits `COMMIT` on normal return or
  `ROLLBACK` when the body throws `transaction_abort`. The returned `Reply<T>` carries the body's value (here `void`) or
  the abort error.
- `Reply<T>` is the single result shape: `ok()` / `operator bool` for success, `result()` for the value, `error()` for
  the `db_error`.

For TLS, swap `tcp::database` for `tcp::ssl::database` (available only when `QB_HAS_SSL` is set); the DSN is unchanged
and the client negotiates the upgrade. See [readme/connection.md](./readme/connection.md).

---

## Callback style (ordered async)

The same operations are available without coroutines. Callback overloads enqueue work and return immediately; the loop
runs it. Use `await()` when you need a synchronous drain — common in tests and one-shot init:

<!-- src: qbm/pgsql/tests/integration/api/coro-api.cpp (callback drain via .await()) -->

```cpp
#include <qbm/pgsql/pgsql.h>
#include <qb/io/async.h>

using namespace qb::pg;

qb::io::async::init();
tcp::database db;
if (!qb::io::async::run_sync(db.connect("tcp://user:pass@localhost:5432[mydb]")))
    return;

db.execute("SELECT 1", discard_query, discard_error).await();   // enqueue, then drain

db.begin(
    [](transaction &tr) {
        tr.execute("INSERT INTO t(v) VALUES (1)", discard_query, discard_error);
    },
    [](error::db_error const &) { /* BEGIN failed */ })
  .await();                                                       // COMMIT/ROLLBACK on drain
```

`discard_query` and `discard_error` are the no-op success/error sinks for fire-and-forget steps. There is **no**
`connect(on_ok, on_err)` overload — connect via `co_await` or `run_sync(db.connect(...))`.
See [readme/transaction.md](./readme/transaction.md) for the `then` / `error` chaining rules (inner `transaction&` vs.
root `database`).

---

## Integrate in CMake

Two supported modes, both giving the same target (`qbm::pgsql`) and the same header spelling (`<qbm/pgsql/pgsql.h>`).
Requires CMake 3.24+ (the qb framework's `cmake_minimum_required`).

**Embedded** — add the framework, load the modules, link the alias:

```cmake
add_subdirectory(qb)                                # the qb framework tree
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")  # discovers and registers qbm-pgsql
target_link_libraries(your_target PRIVATE qbm::pgsql)
```

**Installed** — consume a `cmake --install`ed tree. No `find_package(qb)` line is needed; the module's package
config resolves qb itself:

```cmake
find_package(qbm-pgsql CONFIG REQUIRED)             # find_dependency(qb) happens inside
target_link_libraries(your_target PRIVATE qbm::pgsql)
```

Headers land under `<prefix>/include/qbm/pgsql/...` and the CMake files under `<prefix>/lib/cmake/qbm-pgsql/`;
`<prefix>/include/qbm` is the installed spelling of the source tree's `qbm/` root, so the include line is identical in
both modes. `qbm-pgsqlConfig.cmake` fails at configure time if the installed qb is a different version than the one
this module was compiled against, or disagrees with it about `QB_HAS_SSL`.

`qb_load_modules` adds each module subdirectory; `qbm-pgsql`'s own `CMakeLists.txt` guards on `QB_FOUND` and skips
itself if the framework is absent. The include path exposes `pgsql/pgsql.h`.

The C++ standard is inherited from the framework cache variable `QB_CXX_STANDARD` (default `20`, optionally `23`); it is
not set per module. TLS support is inherited too: when the framework finds OpenSSL it defines `QB_HAS_SSL=1` and the
`tcp::ssl::database` alias compiles; otherwise the build emits a status note and only the cleartext transport is
available.

---

## Documentation map

| Document                                               | Covers                                                                                                                                               |
|:-------------------------------------------------------|:-----------------------------------------------------------------------------------------------------------------------------------------------------|
| [readme/README.md](./readme/README.md)                 | Technical index: completion models, mental model, source map, and the example-as-specification test list.                                            |
| [readme/connection.md](./readme/connection.md)         | DSN format, `connection_options`, the connect awaiter, handshake, `disconnect` / `prepare_reconnect`, TLS, auth.                                     |
| [readme/transaction.md](./readme/transaction.md)       | `begin` / `commit` / `rollback`, ordered async and `then` / `error`, optional `await()` and `status`, `with_transaction`, `set_timeout`, savepoints. |
| [readme/queries.md](./readme/queries.md)               | Every operation in both styles; `execute` overloads, prepared statements, files, NOTIFY/LISTEN, discards.                                            |
| [readme/results.md](./readme/results.md)               | `results` / `row` / `field`, `Reply<resultset>`, `as<T>()` / `to()`, JSON output.                                                                    |
| [readme/types.md](./readme/types.md)                   | OID mapping, text vs. binary formats, parameter serialization, `timestamptz` → `qb::wall_time`, NULL handling.                                       |
| [readme/error_handling.md](./readme/error_handling.md) | `Reply`, the `db_error` hierarchy, `status`, SQLSTATE classification, `client_error`.                                                                |
| [readme/testing.md](./readme/testing.md)               | Test environment (`QB_PG_*`), CTest integration, and the test-to-feature map.                                                                        |

---

## Notes and pitfalls

- **The loop must run.** Callback overloads only enqueue. If nothing calls `run_once()` / `run()` (or `await()` for a
  synchronous drain) on the connection's thread, queued queries never reach the wire. In an actor, the `VirtualCore`
  tick drives it for you.
- **Reuse needs a reset.** After `disconnect()`, call `prepare_reconnect()` before `connect()` again on the same
  `database` object — it re-arms the io layer (resets buffers, closes the fd). Connecting without it is undefined.
- **Connect timeout is not the statement timeout.** The `connect(qb::duration)` timeout bounds the handshake.
  `set_timeout(qb::duration)` arms a server-side `SET LOCAL statement_timeout` on the next `BEGIN` and is cleared at
  `COMMIT` / `ROLLBACK`.
- **Timestamps map to `qb::wall_time`.** PostgreSQL `timestamptz` (OID 1184) round-trips as integer microseconds and
  maps to `qb::wall_time` — not `qb::duration`. The retired `qb::Timestamp` / `qb::UtcTimestamp` / `to_timestamp(...)` names no longer exist.
  The wire epoch (microseconds since 2000-01-01, day counts, tz offsets) is an internal native encoding, not a `qb::duration`.
  See [readme/types.md](./readme/types.md).
- **Read NULL with `std::optional`.** Extracting a NULL column into a non-nullable `T` via `field::as<T>()` or
  `field::to(T&)` throws `error::value_is_null` (the field-handler path throws its subclass `error::field_is_null`).
  Extract into `std::optional<T>` instead — `as<std::optional<T>>()` / `to(std::optional<T>&)` returns an empty optional
  for NULL.
- **One style per call stack.** Do not mix undriven coroutine awaiters inside a `begin(...)` callback body.

---

## For AI assistants

This repository publishes machine-readable documentation following the
[llms.txt](https://llmstxt.org/) convention, so a coding agent can read qbm-pgsql without
guessing:

- **[`llms.txt`](./llms.txt)** — the index: a one-paragraph summary, the six rules that decide whether generated qbm-pgsql code is correct, and a link
  list of every document in this repository.
- **[`llms-full.txt`](./llms-full.txt)** — ~18k tokens: `llm/qbm-pgsql.llm.md` (the mental model, invariants and gotchas) and `llm/qbm-pgsql.llm.api.md` (a deterministic public-API reference, every signature verified against the headers under `src/qbm/pgsql/`), concatenated into one fetch.

Both files are generated by `scripts/gen-llms-txt.py` from `llm/` and checked in CI
(`scripts/doc-lint.sh` section 1d), so they cannot drift from the documentation they index.

**Use it over MCP, with nothing to host and nothing to install.**
[GitMCP](https://gitmcp.io) exposes any public GitHub repository as an MCP endpoint and reads
`llms.txt` first (its documented order is `llms.txt`, then an AI-optimised documentation
build, then `README.md`):

```json
{ "mcpServers": { "qbm-pgsql": { "url": "https://gitmcp.io/isndev/qbm-pgsql" } } }
```

Claude Desktop and other clients without native remote-MCP support wrap the same URL:
`"command": "npx", "args": ["mcp-remote", "https://gitmcp.io/isndev/qbm-pgsql"]`.

**Cursor `@Docs`** — add
`https://raw.githubusercontent.com/isndev/qbm-pgsql/main/llms-full.txt`.

## See also

- [qb/README.md](https://github.com/isndev/qb/blob/main/README.md) — framework overview (qb-core actors, qb-io async).
- [readme/README.md](./readme/README.md) — the long-form technical index for this module.
- `qbm/pgsql/tests/` — integration tests are executable documentation; start with `integration/api/coro-api.cpp` (coroutines,
  `with_transaction`, `run_sync`) and `integration/transaction/transaction-basic.cpp` (callback `begin`, savepoints, `await()`).
