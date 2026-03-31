# qbm-pgsql

**PostgreSQL wire-protocol client for [QB](https://github.com/isndev/qb)** — a **qb-io** module: C++23, async I/O on the
**qb-io** event loop (libev), **`co_await`** or callbacks, same API.

<p align="center">
  <img src="https://img.shields.io/badge/PostgreSQL-13%2B-blue.svg" alt="PostgreSQL"/>
  <img src="https://img.shields.io/badge/C%2B%2B-23-blue.svg" alt="C++23"/>
  <img src="https://img.shields.io/badge/qb--io-module-orange.svg" alt="qb-io module"/>
  <img src="https://img.shields.io/badge/Cross--Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg" alt="Cross Platform"/>
  <img src="https://img.shields.io/badge/License-Apache%202.0-green.svg" alt="License"/>
</p>

---

## Why this module

|                         |                                                                                                                                                                                                                                   |
|:------------------------|:----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **qb-io**               | Implemented **on qb-io** only: **`qb::io::async`**, TCP/TLS transport, coroutine **`task`**, **`run_sync`**. Use from a plain executable or from any code that drives the io listener.                                            |
| **qb-core (optional)**  | **qb-core** is the **actor runtime** (actors, VirtualCore, events) — a **different** QB library. This client does not “belong to” qb-core; you *may* hold a **`database`** inside an actor and drive it with the same qb-io loop. |
| **One header**          | `#include <pgsql/pgsql.h>` — connection, transactions, **`Reply`**, **`with_transaction`**, **`run_sync`**, types, discards.                                                                                                    |
| **Two styles, one API** | Same method names: overloads **without** callbacks → **`co_await`**; **with** callbacks → **`Transaction&`** + **`await()`**.                                                                                                     |
| **Production-oriented** | SCRAM-SHA-256 / MD5 / cleartext auth, prepared-statement **LRU**, server **`statement_timeout`** helper, **LISTEN/NOTIFY**, structured errors + SQLSTATE.                                                                         |

---

## Feature overview

- **Connection** — async handshake, DSN (`tcp://user:pass@host:port[db]`), optional TLS (**`tcp::ssl::database`**),
  reconnect via **`prepare_reconnect()`**
- **Simple & prepared SQL** — **`execute` / `query`**, **`prepare`**, **`params`**, **`type_oid_sequence`**, *
  *`execute_file` / `prepare_file`**
- **Transactions** — **`begin` / `commit` / `rollback`**, **`transaction_mode`** (isolation, read-only, deferrable), *
  *`with_transaction`** + **`transaction_abort`**
- **Savepoints** — **`savepoint`** (callback + coroutine); **`release_savepoint` / `rollback_savepoint`** — *
  *coroutine-only** (see [API coverage](#callback-vs-coro-api-coverage))
- **Timeouts** — **`set_timeout(ms)`** → next **`BEGIN`** adds **`SET LOCAL statement_timeout`** (distinct from connect
  timeout)
- **Results** — **`results` / `row` / `field`**, **`as<T>()`**, tuples, **`std::optional`** for NULL, **`results.json()`
  **
- **Types** — scalars, timestamps, UUID, JSON/JSONB, BYTEA, intervals, NUMERIC/DATE/TIME paths (
  see [readme/types.md](./readme/types.md))
- **Pub/sub** — **`notify`**, **`listen` / `unlisten`**, **`on_incoming_notify`**, **`notify_co_consumer`** + *
  *`receive()`**
- **Errors** — **`Reply<T>`**, **`db_error`**, **`sqlstate`**, **`Transaction::await()` → `status`**

Details: **[readme/README.md](./readme/README.md)** and the topic guides below.

---

## qb-io vs qb-core (read this)

| Piece       | What it is                                                                                                                                                                                                 |
|:------------|:-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **qb-io**   | I/O stack: libev, non-blocking sockets, **`qb::io::async`**, coroutine integration. **qbm-pgsql is a qb-io module** — all wire I/O and scheduling go through this layer.                                   |
| **qb-core** | **Actor system** of the framework: **`qb::Actor`**, messaging, lifecycle. **Not** where this client is defined; you optionally **use** the client **from** actors that share the same thread / io context. |

**CMake (this repo):** `qb_register_module(… DEPENDS qb-core …)` attaches **qbm-pgsql** to the QB **module** graph. That
line is **build wiring**, not a claim that PostgreSQL support is an “actor-core feature”. At the API level you depend on
**qb-io** types (`#include <qb/io/async.h>` is pulled via **`pgsql.h`**).

When **`QB_HAS_SSL`** is on, TLS follows the qb-io OpenSSL-enabled transport.

---

## Integrate in CMake

```cmake
add_subdirectory(qb)   # your QB tree
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")
target_link_libraries(your_target PRIVATE qbm::pgsql)
```

Install / include layout should expose **`pgsql/pgsql.h`** (via **`qb_modules`** / **`QB_PATH`**).

---

## Single public entry

```cpp
#include <pgsql/pgsql.h>
```

- **qb-io only** (CLI, tests): call **`qb::io::async::init()`** once before first use; drive **`run` / `run_once`** as
  usual.
- **With qb-core:** the **VirtualCore / engine** typically owns io init; your actor still uses **`tcp::database`** —
  same qb-io completion rules.

---

## Coroutines first, callbacks always

Throughout this module, **prefer coroutines** for readable linear code. **Callbacks** remain first-class for fluent
chains, minimal allocations, or code that must not use **`co_await`**.

| Topic      | Coroutines                                                        | Callbacks                                                                                        |
|:-----------|:------------------------------------------------------------------|:-------------------------------------------------------------------------------------------------|
| Completion | **`co_await`** → **`Reply<T>`**                                 | Success/error lambdas; work is **enqueued** and completes under **`run_once`** / your actor loop |
| Connection | **`co_await db.connect(dsn)`** or **`run_sync(db.connect(dsn))`** | No `connect(on_ok, on_err)` — use **`run_sync`** or **`co_await`**                               |
| SQL        | **`co_await db.execute("…")`**                                    | **`db.execute("…", on_ok, on_err)`** → **`Transaction&`**                                        |
| Discards   | N/A                                                               | **`discard_query`**, **`discard_error`**, **`discard_prepare`**                                  |

### Callback API = ordered async

Callback overloads **do not block** the calling thread by themselves: they push commands onto the transaction’s internal
queues (**`_sub_commands`**, **`_queries`** in [`src/transaction.cpp`](src/transaction.cpp)). PostgreSQL I/O runs when
something calls **`qb::io::async::run_once()`** (or **`run`**) on that thread — typically your **qb-io** listener or *
*qb-core** VirtualCore tick.

- You **do not** have to call **`Transaction::await()`** for correctness if the event loop keeps draining the
  connection.
- Use **`.await()`** when you want a **synchronous drain** on the current thread (tests, init, “wait until this batch
  finishes”) or to read **`status`** / **`st.results()`**.
- After **`begin`**, chain **`.then()`** / **`.error()`** on the **root** **`database`** for follow-up work that depends
  on the outcome of the queued pipeline. **`then` / `success` / `error`** are implemented as **`Then` / `Error`**
  command objects whose logic runs when popped from the queue (see [`src/commands.h`](src/commands.h), [
  `src/transaction.inl`](src/transaction.inl)).

**Fluent pattern (optional `.await()`):**

```cpp
using namespace qb::pg;
db.begin(
    [](qb::pg::transaction& tr) {
        tr.execute("INSERT INTO t(v) VALUES (1)", discard_query, discard_error);
    },
    [](qb::pg::error::db_error const&) { /* BEGIN failed */ })
    .then([](qb::pg::transaction& tr) {
        tr.execute("SELECT count(*) AS c FROM t", discard_query, discard_error);
    })
    .error([](qb::pg::error::db_error const& e) {
        (void)e; /* earlier step failed — inspect e / SQLSTATE */
    });
// Omit .await() if run_once() continues async (e.g. actor). Or:
// auto st = db.await();
```

**Chaining note:** **`.then` / `.error` on `db` after `begin(...)`** appends to the **root** queue **after** the *
*`Begin` / `End`** pair that `begin` just enqueued — so that **`Then` / `Error` typically runs after COMMIT or ROLLBACK
** of that block. For steps **inside** the same open transaction before commit, call **`tr.then(...)` / `tr.error(...)`
** on the **`Transaction&`** passed to the **`begin`** lambda (see [readme/transaction.md](./readme/transaction.md)).

**Rules:**

1. **One style per call stack** — inside a **`begin(...)`** *callback*, do not mix undriven coroutine awaiters; use
   callbacks only there.
2. There is **no** **`.await()`** on **`pg_reply_awaiter`** — use **`co_await`** or **`qb::io::async::run_sync`**.

### Callback vs coro API coverage

| API                                                                                          | Callback overloads                     | Coroutine / `run_sync` only    |
|:---------------------------------------------------------------------------------------------|:---------------------------------------|:-------------------------------|
| **`connect`**                                                                                | —                                      | **`connect_awaiter`**          |
| **`execute`**, **`prepare`**, prepared **`execute`**, **`execute_file`**, **`prepare_file`** | yes                                    | **`co_await`**                 |
| **`notify`**, **`listen`**, **`unlisten`**, **`unlisten_all`**                               | yes                                    | **`co_await`**                 |
| **`begin`** (transaction block)                                                              | yes — **`End`** issues COMMIT/ROLLBACK | **`co_await begin()`**         |
| **`commit`**, **`rollback`** (explicit)                                                      | —                                      | **`co_await`** only            |
| **`savepoint(name, …)`** (open block)                                                        | yes                                    | **`co_await savepoint(name)`** |
| **`release_savepoint`**, **`rollback_savepoint`**                                            | —                                      | **`co_await`** only            |

---

## Connection & reconnect

**Coroutine**

```cpp
if (!co_await db.connect("tcp://user:pass@localhost:5432[mydb]"))
    return; // not connected

// … work …

db.disconnect();
db.prepare_reconnect();   // required before connect() on the same object again
if (!co_await db.connect("tcp://user:pass@localhost:5432[mydb]"))
    return;
```

**Callback-style drain (e.g. init / test)**

```cpp
qb::io::async::init();
qb::pg::tcp::database db;
if (!qb::io::async::run_sync(db.connect("tcp://user:pass@localhost:5432[mydb]")))
    return;

db.execute("SELECT 1", qb::pg::discard_query, qb::pg::discard_error).await();

db.disconnect();
db.prepare_reconnect();
(void)qb::io::async::run_sync(db.connect("tcp://user:pass@localhost:5432[mydb]"));
```

- **TLS:** **`qb::pg::tcp::ssl::database`** — same **`tcp://…`** DSN; client sends **SSLRequest** when the server
  supports it.
- **Connect timeout:** from **`connection_options`** (handshake), **not** the same as **`set_timeout()`** (PostgreSQL *
  *`statement_timeout`** on **`BEGIN`**).

More: [readme/connection.md](./readme/connection.md).

---

## Queries (simple SQL)

**Coroutine**

```cpp
auto r = co_await db.query("SELECT version() AS v");  // alias of execute
if (!r.ok())
    return;
auto rows = std::move(r).result();
if (!rows.empty())
    (void)rows[0]["v"].as<std::string>();
```

**Callback**

```cpp
db.execute(
    "SELECT version() AS v",
    [](qb::pg::transaction&, qb::pg::results rs) {
        if (!rs.empty())
            (void)rs[0]["v"].as<std::string>();
    },
    [](qb::pg::error::db_error const&) {});
// No await() if the io loop keeps calling run_once(). Use db.await() only to
// synchronously drain this connection’s queue on this thread (tests, init).
```

**Blocking helper (tests, glue code)**

```cpp
auto r = qb::io::async::run_sync(db.execute("SELECT 1"));
if (r.ok()) { /* r.result() */ }
```

---

## Transactions

**Coroutine — manual**

```cpp
auto b = co_await db.begin();
if (!b.ok()) { /* … */ }

auto ins = co_await db.execute("INSERT INTO t(v) VALUES (1)");
if (!ins.ok()) {
    (void)co_await db.rollback();
    return;
}

auto c = co_await db.commit();
if (!c.ok())
    (void)co_await db.rollback();
```

**Coroutine — scoped (`with_transaction`)**

```cpp
auto reply = co_await qb::pg::with_transaction(db, [](qb::pg::detail::Transaction& tr)
    -> qb::io::async::task<int> {
        auto sel = co_await tr.query("SELECT 1 AS x");
        if (!sel.ok())
            throw qb::pg::transaction_abort{sel.error()};
        co_return 42;
    });
if (!reply.ok()) { /* rolled back; reply.error() */ }
```

**Callback — fluent block**

```cpp
db.begin(
    [](qb::pg::transaction& tr) {
        tr.execute("INSERT INTO t(v) VALUES (1)", qb::pg::discard_query, qb::pg::discard_error);
    },
    [](qb::pg::error::db_error const&) {},
    qb::pg::transaction_mode{});  // optional: isolation, read_only, deferrable

auto st = db.await();
if (!static_cast<bool>(st)) { /* st.error() */ }
```

**Chaining:** **`tr.then` / `tr.error` / `tr.success`** inside the **`begin`** body, or **`db.then` / `db.error`** after
**`begin(...)`** (post-commit / post-rollback) — see [readme/transaction.md](./readme/transaction.md).

**Savepoints** — **`savepoint(..., cb, err)`** (callback) or **`co_await savepoint`**. *
*`release_savepoint` / `rollback_savepoint`**: **coroutine-only** (use raw SQL in a callback transaction if needed).

---

## Statement timeout (`set_timeout`)

**Coroutine**

```cpp
db.set_timeout(1500);  // ms → next BEGIN adds SET LOCAL statement_timeout on the server
auto wt = co_await qb::pg::with_transaction(db, [](auto& tr) -> qb::io::async::task<void> {
    (void)co_await tr.execute("SELECT pg_sleep(10)");  // may fail with timeout error
    co_return;
});
```

**Callback**

```cpp
db.set_timeout(1500);
db.begin(
    [](qb::pg::transaction& tr) {
        tr.execute("SELECT pg_sleep(10)", qb::pg::discard_query, qb::pg::discard_error);
    },
    [](qb::pg::error::db_error const&) {});
// .await() optional — use when you need a synchronous drain on this thread
```

---

## Prepared statements (LRU cache)

**Coroutine**

```cpp
auto prep = co_await db.prepare("u", "UPDATE t SET v = $1 WHERE id = $2",
    qb::pg::type_oid_sequence{qb::pg::oid::text, qb::pg::oid::int4});
if (!prep.ok()) { /* … */ }

auto ex = co_await db.execute("u", qb::pg::params{std::string("x"), 1});
```

**Callback**

```cpp
using namespace qb::pg;
db.prepare("u", "UPDATE t SET v = $1 WHERE id = $2",
    type_oid_sequence{oid::text, oid::int4},
    discard_prepare, discard_error);
db.execute("u", params{std::string("x"), 1}, discard_query, discard_error);
// Or .await() once at the end if you need a blocking drain on this thread
```

Named statements are cached per connection with **LRU eviction** — safe for long-lived sessions.

**Files:** **`execute_file`**, **`prepare_file`** — same dual API (coroutine overloads + callback + **`await()`**).

---

## LISTEN / NOTIFY

**Coroutine — publish**

```cpp
(void)co_await db.notify("events", R"({"k":1})");
```

**Callback — publish**

```cpp
db.notify("events", R"({"k":1})", qb::pg::discard_query, qb::pg::discard_error);
```

**Coroutine — subscribe**

```cpp
(void)co_await db.listen("chan");
db.on_incoming_notify([](qb::pg::notification&& n) { (void)n; });
```

**Callback — subscribe**

```cpp
db.listen("chan", qb::pg::discard_query, qb::pg::discard_error);
db.on_incoming_notify([](qb::pg::notification&& n) { (void)n; });
```

**Queue consumer:** **`qb::pg::tcp::notify_co_consumer`** with **`co_await receive()`** (
see [readme/queries.md](./readme/queries.md)). Async ordering across connections is event-loop driven.

---

## Results & errors (short)

- **Coroutine:** always check **`Reply::ok()`** before **`result()`**; use **`r.error()`** for **`db_error`** /
  SQLSTATE.
- **Callback:** handle each op’s **`on_err`**; use **`.error(...)`** on the fluent chain for segment failures; call *
  *`await()`** only when you need a **`status`** snapshot (**`st.results()`**, **`st.error()`**) or a full drain on this
  thread.
- **Row access:** **`row["col"].as<T>()`**, **`std::optional`** for NULL; wrong type or NULL throws documented
  exceptions — [readme/error_handling.md](./readme/error_handling.md), [readme/results.md](./readme/results.md).

---

## Optional: using alongside **qb-core** (actor runtime)

**qb-core** = actor framework. **qbm-pgsql** = qb-io client. They compose when your actor runs on a core that already
drives **qb-io**.

Below: connect in **`onInit`**, issue SQL with **callbacks** (fits message-driven **`run_once`**). Alternatively use a *
*`task`** + **`co_await`** on the same thread’s coroutine scheduler (see `test-pgsql-coro-api.cpp`).

```cpp
#include <pgsql/pgsql.h>
#include <qb/core/Actor.h>
#include <qb/core/Event.h>

class DbActor : public qb::Actor {
    qb::pg::tcp::database db_{"tcp://app:secret@db:5432[appdb]"};

public:
    bool onInit() override {
        registerEvent<qb::KillEvent>(*this);
        if (!qb::io::async::run_sync(db_.connect()))
            return false;
        db_.execute(
            "SELECT count(*)::bigint AS n FROM users",
            [](qb::pg::transaction&, qb::pg::results rs) {
                if (!rs.empty())
                    (void)rs[0]["n"].as<int64_t>();
            },
            [](qb::pg::error::db_error const&) {});
        return true;
    }

    void on(qb::KillEvent const&) override {
        db_.disconnect();
        kill();
    }
};
```

For **coroutine-heavy** actor code, schedule a **`task`** that **`co_await`**s PostgreSQL on the same thread’s *
*`coro_scheduler()`** (`qbm/pgsql/tests/test-pgsql-coro-api.cpp`).

---

## How it sits on the stack (qb-io)

| Layer                             | Role                                                                                                           |
|:----------------------------------|:---------------------------------------------------------------------------------------------------------------|
| **qb-io**                         | libev listener, TCP/TLS, **`run` / `run_once`**, coroutine **`task`**, **`run_sync`** — **this module’s host** |
| **`qb::protocol::pgsql`**         | Framed messages, dispatch to **`Database::on`**                                                                |
| **`qb::pg::detail::Database`**    | Handshake, auth, protocol state, NOTIFY, prepared cache                                                        |
| **`qb::pg::detail::Transaction`** | Command queue: **`execute`**, **`prepare`**, **`begin`**, savepoints                                           |

**qb-core** sits **beside** this picture: it schedules actors that may **call into** the same qb-io loop; it is not a
layer inside the PostgreSQL client.

Your handle is **`qb::pg::tcp::database`** or **`qb::pg::tcp::ssl::database`** — it **is** the root **`Transaction`**.

---

## Intentional limits

| Area           | Note                                                          |
|:---------------|:--------------------------------------------------------------|
| **COPY**       | Limited / not exposed as a first-class streaming API for apps |
| **GSSAPI**     | Not implemented                                               |
| **Pooling**    | External; reuse **`database`** + **`prepare_reconnect()`**    |
| **Pipelining** | One serial command stream per connection                      |

---

## Requirements

- **qb-io** (libev, **`qb::io::async`**) — **required**; this module is built on it
- **C++23**
- **CMake** 3.14+ and QB **qbm** module registration (see `CMakeLists.txt`: `DEPENDS qb-core` for the monorepo graph)
- **PostgreSQL** 13+ (typical)
- **OpenSSL** when **`QB_HAS_SSL`** (TLS client + SCRAM)
- **qb-core** — **only if** you integrate with the **actor** runtime (`qb::Actor`, VirtualCore, …)

---

## Documentation map

| Doc                                                    | Contents                                                                            |
|:-------------------------------------------------------|:------------------------------------------------------------------------------------|
| [readme/README.md](./readme/README.md)                 | Index, mental model, **source map**                                                 |
| [readme/connection.md](./readme/connection.md)         | DSN, SSL, auth, **`prepare_reconnect`**, timeouts                                   |
| [readme/transaction.md](./readme/transaction.md)       | **`begin`**, **`with_transaction`**, savepoints, **`await()`/`status`**, fluent API |
| [readme/queries.md](./readme/queries.md)               | Prepared SQL, files, LISTEN/NOTIFY, discards                                        |
| [readme/results.md](./readme/results.md)               | Rows, fields, **`Reply`**, JSON                                                   |
| [readme/types.md](./readme/types.md)                   | OIDs, binary/text, **`params`**, NULL                                               |
| [readme/error_handling.md](./readme/error_handling.md) | **`Reply`**, **`transaction_abort`**, SQLSTATE                                    |
| [readme/testing.md](./readme/testing.md)               | **`QB_PG_*`**, CTest, integration tests                                             |

**Executable examples:** `qbm/pgsql/tests/test-pgsql-coro-api.cpp`, `test-notify.cpp`, `test-transaction-advanced.cpp`,
`test-prepared-statements.cpp`, `test-connection.cpp`, …

---

## License

Apache License 2.0 — see [LICENSE](./LICENSE).

Part of the **[QB](https://github.com/isndev/qb)** ecosystem.
