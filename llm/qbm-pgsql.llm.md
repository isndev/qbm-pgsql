<!-- Verified-against: qbm-pgsql @ qb 3.0.0. Source of truth: the headers under qbm/pgsql/src/qbm/pgsql/. -->
# qbm-pgsql — LLM concepts (write correct code)

Asynchronous PostgreSQL client for the qb C++ actor framework. Speaks the
PostgreSQL v3 wire protocol directly over qb-io (no libpq). Compiled library
(**STATIC** by default, **SHARED** when `QB_BUILD_SHARED_LIBS`/`BUILD_SHARED_LIBS`
is set), alias `qbm::pgsql`. Public namespace `qb::pg`; implementation in
`qb::pg::detail`. Umbrella header `<qbm/pgsql/pgsql.h>`. C++20 by default (C++23 with
`QB_CXX_STANDARD=23`).

> **Source of truth.** Verify every signature against the headers under
> `qbm/pgsql/` (esp. `pgsql.h`, `src/qbm/pgsql/transaction.h`, `src/qbm/pgsql/resultset.h`,
> `src/qbm/pgsql/type_mapping.h`, `src/qbm/pgsql/common.h`, `src/qbm/pgsql/pg_reply.h`). The human docs are
> `qbm/pgsql/README.md` and `qbm/pgsql/readme/`. Never invent an API.

---

## 1. Purpose

Connect to PostgreSQL, run simple and prepared SQL, drive transactions /
savepoints, decode typed result sets, and consume LISTEN/NOTIFY — all
non-blocking on the qb-io event loop, with the **same method names** exposed in
two completion styles (coroutine `co_await` and callback). It is a client, not
an actor: you can hold a `database` inside an actor, but the API dependency is
qb-io, not qb-core.

---

## 2. Mental model

- **One async client = one serial wire stream.** `qb::pg::tcp::database` is a
  single object that owns one non-blocking TCP (or TLS) socket. Queries queue
  behind one another and complete in order. It runs on **one** qb-io thread /
  `VirtualCore`; never share a `database` or a `results` across threads.
- **The client _is_ the root `Transaction`.** `database` inherits
  `detail::Transaction`. Once connected you call `execute` / `query` / `prepare`
  / `begin` directly on it; a `begin(...)` hands you a child `transaction&` for
  the in-transaction work.
- **Two interchangeable completion models, same names:**
  - **Coroutine** — single-op overloads **without** callbacks return a
    `[[nodiscard]] pg_reply_awaiter<T>` (`execute`, `query(sql)`, `prepare`,
    `begin`/`commit`/`rollback`, savepoints, `notify`/`listen`). The helpers that
    chain several awaits internally — `query(sql, args...)`, `copy_out`,
    `copy_in`, `query_stream` — instead return `qb::io::async::task<Reply<T>>`.
    Either way `co_await` yields `Reply<T>` (`T` = `resultset`, `PreparedQuery`,
    or `void`), and nothing happens until you `co_await` it (or `run_sync` it).
  - **Callback** — overloads **with** success/error lambdas return
    `transaction&` for fluent chaining and only **enqueue** work. The queue
    drains when a thread runs `qb::io::async::run_once()` / `run()` on that
    connection's thread (an actor's `VirtualCore` tick does this for you), or
    when you call `await()` for a synchronous drain + `status` snapshot.
  - Pick one style per call stack. Do **not** drop an undriven coroutine awaiter
    inside a callback body.
- **Wire protocol.** Simple-query protocol (`execute`/`query`) sends raw SQL;
  extended-query protocol (`prepare` then parameterized `execute`) does
  Parse/Bind/Execute with **binary** parameters and a **client-side** prepared-
  statement LRU (default capacity 100; eviction is local bookkeeping only — no
  server-side `DEALLOCATE`). Framing, auth (SCRAM-SHA-256 / MD5 / cleartext), and type
  codecs are all in-module. SCRAM-SHA-256 is **mutual**: the client verifies the
  server's SASLFinal `ServerSignature` and refuses `AuthenticationOk` if a SCRAM
  exchange started but that signature never verified — so a server that cannot
  prove it knows the credential (impersonator / active MITM) is rejected.
- **`run_sync` bridge.** `qb::io::async::run_sync(awaiter)` blocks the current
  thread until one awaiter resolves — the idiom for setup / tests / one-shot
  code outside a coroutine.

---

## 3. Core concepts (minimal compilable snippets)

All snippets assume:

```cpp
#include <qbm/pgsql/pgsql.h>
#include <qb/io/async.h>
using namespace qb::pg;             // database, results, params, oid, Reply, ...
```

### 3.1 Connect

`connect()` returns an awaiter — there is **no** blocking `bool connect()` and
**no** `connect(on_ok, on_err)` callback overload. Drive it with `co_await` or
`run_sync`. Result is `bool` (`true` once the handshake reached
`ReadyForQuery`).

```cpp
qb::io::async::init();
qb::pg::tcp::database db;

// DSN form: scheme://[user[:password]@]host[:port][database]
// NOTE: the database name is in SQUARE BRACKETS, not "/db".
if (!qb::io::async::run_sync(db.connect("tcp://user:secret@localhost:5432[mydb]"))) {
    // handshake failed — read state via db.error() / db.has_error()
}
```

Overloads (all return `connect_awaiter`): `connect()` (stored options),
`connect(qb::duration timeout)` (override handshake deadline),
`connect(std::string const& dsn)` (re-parse then connect),
`connect(connection_options opts)` (fully-specified options — the only way to set
the fields the DSN string cannot carry, esp. `ssl_verify`),
`connect(dsn, transport_io_type&& raw_io)` (adopt a pooled socket).

`connection_options` fields (parsed from the DSN or set by hand):
`alias`, `schema`, `uri` (`host:port` or `/path` for a Unix socket), `database`,
`user`, `password`, `connect_timeout` (`qb::duration`, **default 10 s**),
`ssl_verify` (`ssl_verify_mode`, **default `ssl_verify_mode::none`**),
`ssl_root_cert`/`ssl_cert`/`ssl_key` (optional PEM paths: private CA + client-cert mTLS),
`keepalive_interval/idle/probes`. The DSN only carries
user/password/database/host:port; set the rest on the struct (the DSN **cannot**
carry TLS fields). User-defined literals: `"...dsn..."_pg → connection_options`,
`"label"_db → dbalias`.

**TLS verification — `ssl_verify_mode { none, full }`.** `none` (default) is
encrypt-only with no certificate check (≈ libpq `sslmode=require`), and logs a
one-time warning; `full` verifies the chain **and** hostname (≈ libpq
`verify-full`). Note that even under `none`, SCRAM-SHA-256 still mutually
authenticates the *server* (see §2) — it is the TLS channel and any non-SCRAM
auth that stay unauthenticated. libpq's intermediate
`verify-ca` is intentionally not offered. A **private CA** (`ssl_root_cert`) and a
**client certificate** for mutual TLS (`ssl_cert` + `ssl_key`, both required) are
optional PEM paths on the struct; a bad path fails the connect **closed**. Because
the DSN string cannot carry these, set them on a `connection_options` and use the
`connect(connection_options)` overload:

```cpp
auto o = qb::pg::connection_options::parse(dsn);   // also: "...dsn..."_pg
o.ssl_verify    = qb::pg::ssl_verify_mode::full;    // verify chain + hostname
o.ssl_root_cert = "internal-ca.pem";                // optional: validate against a private CA
o.ssl_cert = "client.pem"; o.ssl_key = "client.key"; // optional: client certificate (mTLS)
co_await db.connect(o);                              // tcp::ssl::database
```

### 3.2 Simple query (coroutine + callback)

```cpp
// Coroutine: co_await yields Reply<resultset>.
qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
    auto r = co_await db.query("SELECT 42 AS answer");   // query == execute(sql)
    if (r) {                                             // if (r) / r.ok()
        int answer = r.result()[0]["answer"].as<int>();  // r.result() is the resultset
        std::cout << answer << '\n';
    } else {
        std::cerr << r.error().what() << '\n';           // r.error() is error::db_error
    }
    co_return;
}());

// Callback: success lambda is (transaction& tr, results result).
db.execute("SELECT 1",
    [](transaction& tr, results result) { /* use result here (borrowing!) */ },
    [](error::db_error const& e)        { /* failure */ })
  .await();                                              // synchronous drain
```

`query(sql)` is just `execute(sql)` exposed for `co_await` ergonomics inside
`with_transaction` bodies. `discard_query` / `discard_error` are the no-op
success/error sinks for fire-and-forget callback steps.

### 3.3 Prepared statements

`prepare(name, sql, type_oid_sequence types = {})` then parameterized
`execute(name, params)`. Parameters are sent in **binary**. `params` is
`detail::QueryParams`; build it from values with brace-init.
`type_oid_sequence = std::vector<oid>`.

```cpp
// Coroutine prepare + execute:
qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
    auto p = co_await db.prepare("ins_user",
                 "INSERT INTO users(name, age) VALUES ($1, $2)",
                 {oid::text, oid::int4});                 // optional OID hints
    if (!p) co_return;                                    // Reply<PreparedQuery>

    auto e = co_await db.execute("ins_user", params{std::string("ada"), 36});
    if (!e) std::cerr << e.error().what() << '\n';
    co_return;
}());

// Callback prepare + execute:
db.prepare("sel_user", "SELECT name FROM users WHERE id = $1", {oid::int4},
           discard_prepare, discard_error)
  .execute("sel_user", params{42},
           [](transaction&, results r) { /* ... */ }, discard_error)
  .await();
```

- **NULL parameter** → bind `std::optional<T>{}` (empty) → SQL NULL.
- `prepare_file(name, path, types)` / `execute_file(path)` load SQL from a file
  (file read is **blocking** on the I/O thread).
- The prepared-statement cache is a **bounded LRU** (default 100); heavy churn
  evicts entries. A statement must be prepared before it is executed.

### 3.4 Transactions + savepoints

**Coroutine — prefer `with_transaction`** (auto BEGIN / COMMIT / ROLLBACK):

```cpp
qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
    auto tx = co_await with_transaction(db, [](transaction& tr)
                                            -> qb::io::async::task<void> {
        auto a = co_await tr.execute("INSERT INTO t(v) VALUES (1)");
        if (!a) throw transaction_abort{a.error()};      // → ROLLBACK, Reply::failure
        co_await tr.savepoint("sp1");                    // SAVEPOINT sp1
        auto b = co_await tr.execute("INSERT INTO t(v) VALUES (2)");
        if (!b) co_await tr.rollback_savepoint("sp1");   // ROLLBACK TO sp1
        co_return;
    });
    // tx is Reply<void> (or Reply<T> if the body returns T). Normal return → COMMIT.
    if (!tx) std::cerr << "tx failed: " << tx.error().what() << '\n';
    co_return;
}());
```

- Body returns normally → `COMMIT`. Throw `transaction_abort{err}` → `ROLLBACK`,
  mapped to `Reply::failure(err)` (no rethrow). Any other exception after BEGIN
  → `ROLLBACK` and rethrow.
- Isolation/mode: `with_transaction(db, transaction_mode mode, body)`.
  `transaction_mode{ isolation_level::read_committed|repeatable_read|serializable,
  read_only, deferrable }` (defaults: read_committed, false, false).
- Manual coroutine flow also exists: `co_await tr.begin()` / `begin(mode)`,
  `co_await tr.commit()`, `co_await tr.rollback()`,
  `savepoint/release_savepoint/rollback_savepoint(name)` (all yield
  `Reply<resultset>`; savepoint names must be non-empty, ≤63 chars,
  `[A-Za-z0-9_]`). The name is **quoted as a SQL identifier** (double-quoted,
  embedded `"` doubled) on **both** the coroutine and callback paths before it
  goes into the simple-query string, so a savepoint name can never inject SQL.

**Callback** — `begin(on_success, on_error, mode = {})`; chain on the inner
`transaction&` with `then` / `success` / `error`; commit is automatic on chain
completion, rollback automatic on any failure:

```cpp
db.begin(
    [](transaction& tr) {
        tr.execute("INSERT INTO t(v) VALUES (1)", discard_query, discard_error);
    },
    [](error::db_error const& e) { /* rolled back */ })
  .await();
```

### 3.5 Results / `as<T>()`

```cpp
results rs = r.result();                  // from Reply<resultset>, or the callback arg
for (auto row : rs) {
    int         id   = row["id"].as<int>();              // by name
    std::string name = row[1].as<std::string>();         // by index
}
rs.size();                 // row count   |  rs.empty()  | bool(rs) == has rows
rs.rows_affected();        // INSERT/UPDATE/DELETE count (use this for DML — bool(rs) is row-presence only)
rs.columns_size();         // column count
rs.json();                 // qb::json array (every value stringified)

// Tuple extraction:
std::string nm; int ag;
row.to(nm, ag);                                          // by position
row.to({"name", "age"}, nm, ag);                         // by column name

// NULL-safe read — non-optional as<T> on a NULL throws error::value_is_null:
auto maybe = row["middle_name"].as<std::optional<std::string>>();  // nullopt if NULL
if (row["middle_name"].is_null()) { /* ... */ }
```

**Ownership rule (critical):** `row` and `field` are non-owning views into the
parent `results`; they must not outlive it. The `results` passed to a **callback**
is a *borrowing* handle that does **not** extend lifetime — call
`rs.deep_snapshot()` to keep rows after the callback returns. Coroutine
`Reply<resultset>` already holds a `deep_snapshot()` (safe to keep).

### 3.6 Type mapping (incl. timestamptz → `qb::wall_time`)

C++ ⇄ PostgreSQL via `detail::type_mapping<T>` (OID for params) and
`detail::TypeConverter<T>` (binary/text codec). Key mappings:

| C++ type | PostgreSQL | `oid` |
|---|---|---|
| `bool` | boolean | 16 |
| `qb::pg::smallint` / `int16_t` | smallint | 21 |
| `qb::pg::integer` / `int32_t` | integer | 23 |
| `qb::pg::bigint` / `int64_t` | bigint | 20 |
| `float` / `double` | real / double precision | 700 / 701 |
| `std::string`, `std::string_view`, `const char*` | text/varchar/char | 25 |
| `qb::pg::bytea`, `std::vector<byte>` | bytea | 17 |
| `qb::uuid` | uuid | 2950 |
| `qb::json` / `qb::jsonb` | json / jsonb | 114 / 3802 |
| **`qb::wall_time`** | **timestamptz** | **1184** |

```cpp
// WRITE a timestamptz parameter (qb::wall_time is a UTC instant on system_clock):
co_await db.prepare("ins_ev", "INSERT INTO ev(at) VALUES ($1)", {oid::timestamptz});
co_await db.execute("ins_ev", params{qb::wall_from_unix_seconds(1700000000)});  // or qb::wall_now()

// READ a timestamptz column back:
qb::wall_time at = rs[0][0].as<qb::wall_time>();
```

- **`timestamptz` (OID 1184) maps to `qb::wall_time`**, round-tripped as integer
  microseconds. A `timestamp` column (OID 1114) also decodes into `qb::wall_time`
  via the identical wire layout.
- The PostgreSQL wire epoch (int64 microseconds since 2000-01-01 UTC) is an
  **internal native encoding** inside the type layer; it is never surfaced as a
  `qb::duration`.
- `qb::duration` is the type for *timeouts* (connect / statement), not for
  timestamp column values.
- **FORBIDDEN — these retired types are NOT in this API; never emit them:**
  `qb::Timestamp`, `qb::Duration`, `qb::TimePoint`, `qb::UtcTimestamp`,
  `qb::LocalTimestamp`, `to_timestamp(`, `to_time_point(`. Old pgsql docs map
  TIMESTAMP to these — that mapping is wrong. Use `qb::wall_time` for
  timestamp/timestamptz and `qb::duration` for timeouts.
- Unmapped C++ types **fail to compile** for both `type_mapping` (`static_assert`)
  and `TypeConverter` (`static_assert`) — the generic OID-705 (unknown) fallback
  was removed. Add a `type_mapping<>` specialization (and `TypeConverter`
  `from_binary`/`to_binary`) for any new type.

### 3.7 Statement timeout

`set_timeout(qb::duration)` must be called **before** `begin()`; it emits
`SET LOCAL statement_timeout = N` (N in **milliseconds**, truncated) in the same
round-trip as the next BEGIN, so it is transaction-scoped and cleared at
COMMIT/ROLLBACK. A zero/negative duration omits it. This is the server statement
timeout, **not** a socket idle timeout, and **not** the connect timeout.

```cpp
db.set_timeout(std::chrono::seconds{5});   // applies to the NEXT begin()
co_await db.begin();
```

### 3.8 Error handling

- **Coroutine:** every awaited op yields `Reply<T>`. Test with `if (reply)` /
  `reply.ok()`; read payload with `reply.result()`, failure with
  `reply.error()` (an `error::db_error`).
- **Callback:** pass an `error::db_error`-taking lambda; or after `await()`
  inspect the returned `status` (`if (st)`, `st.results()`, `st.error()`).
- **Exceptions** come from result decoding: `error::value_is_null` /
  `field_is_null` (non-optional read of a NULL — use `std::optional<T>`),
  `error::field_type_mismatch` (bad conversion). All library exceptions derive
  from `error::db_error : std::runtime_error`.
- **Classify programmatically** via the structured SQLSTATE enum, not strings:

```cpp
if (reply.error().sqlstate == qb::pg::sqlstate::unique_violation) { /* 23505 */ }
```

`db_error` carries `what()`, `severity`, `code` (SQLSTATE string), `detail`,
`sqlstate` (`qb::pg::sqlstate::code` enum). Exceptions thrown **inside** a user
callback are wrapped as `error::client_error`.

### 3.9 LISTEN / NOTIFY

Plain database: `notify` / `listen` / `unlisten` / `unlisten_all` (callback or
`co_await` → `Reply<void>`); incoming NOTIFY is only logged unless you set
`db.on_incoming_notify([](qb::pg::notification&& n){ ... })`. For a queue-based
consumer use the dedicated client:

```cpp
qb::pg::tcp::notify_co_consumer sub;                     // alias: notify_cb_consumer (same type)
qb::io::async::run_sync(sub.connect("tcp://user:secret@localhost:5432[mydb]"));

qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
    co_await sub.listen("events");
    while (auto n = co_await sub.receive()) {             // task<std::optional<notification>>
        // n->channel, n->payload, n->server_backend_pid
    }                                                    // nullopt when the channel closes (disconnect)
    co_return;
}());
```

`notification { int server_backend_pid; std::string channel; std::string payload; }`.
The `notify` publisher side must use a normal (non-pooled) connection.

### 3.10 Bulk COPY, streaming, and out-of-band control

These live on `database` in `pgsql.h`. The COPY / streaming helpers chain several
awaits internally, so they return `qb::io::async::task<Reply<T>>` (still
`co_await`-only, yielding `Reply<T>`) rather than a `pg_reply_awaiter<T>`.

**`copy_out(sql, sink)` — `COPY … TO STDOUT`, constant memory.** Runs a
`COPY … TO STDOUT` and delivers each `CopyData` chunk to `sink` **as it arrives**;
nothing is buffered into a result set, so arbitrarily large exports stream in
constant memory. The `string_view` is valid only during the call — copy what you
keep. `-> task<Reply<resultset>>`, `ok()` on success.

```cpp
std::string out;
co_await db.copy_out("COPY users TO STDOUT (FORMAT csv)",
                     [&](std::string_view chunk){ out.append(chunk); });
```

**`copy_in(sql, source)` / `copy_in(sql, data)` — `COPY … FROM STDIN`.** Feeds the
server `CopyData` from `source` (a `std::function<std::optional<std::string>()>`,
called until it returns `std::nullopt`, then `CopyDone`), or the convenience
overload sends one whole `std::string` payload. A throwing `source` aborts the
load with `CopyFail` and yields an error; the connection stays usable. Both
`-> task<Reply<resultset>>`, `ok()` carries the `COPY n` count.

```cpp
co_await db.copy_in("COPY t (id, v) FROM STDIN",
    [&]() -> std::optional<std::string> { return next_line(); });  // nullopt to finish
co_await db.copy_in("COPY t (id, v) FROM STDIN", "1\ta\n2\tb\n");   // whole-payload form
```

> **`copy_in` is NOT constant-memory.** It drains the entire `source` into the
> output pipe synchronously (bounded only by the write-buffer ceiling). Only
> `copy_out` and `query_stream` are true constant-memory paths.

**`query_stream(sql, batch_size, on_row)` — server-side cursor, constant memory.**
Streams a large result through a server-side `CURSOR` (`DECLARE` / `FETCH`
`batch_size` rows per round trip), invoking `on_row(row)` per row; only one batch
is ever in memory. When the connection is idle it wraps its own transaction
(`BEGIN`/`COMMIT`, `ROLLBACK` on failure); when already `in_transaction()` it
declares the cursor in the caller's transaction and just closes the cursor. An
`on_row` exception is rethrown after the cursor is closed (and any self-opened
transaction rolled back). `batch_size` is clamped to ≥ 1. `-> task<Reply<void>>`.

```cpp
std::uint64_t n = 0;
co_await db.query_stream("SELECT * FROM huge", 1000, [&](auto row){ ++n; });
```

> **Overlapping streams SHARE the self-opened block.** Cursor names are unique per
> connection (`qb_stream_cursor_<n>`), so streams may overlap on one `database`. A
> session has one transaction: the first stream `BEGIN`s, later ones only join, and
> the last one out ends it — `COMMIT`, or `ROLLBACK` if ANY participant failed. A
> server error in one stream therefore aborts the block for the others that overlap
> it. A caller-opened transaction is never joined and never ended. Do NOT start a
> stream while your own `begin()` is still in flight: caller ownership is
> recognised only once that `BEGIN` completed (`in_transaction()` mirrors the last
> ReadyForQuery), so the stream would open a block of its own.

**`cancel() -> bool` — out-of-band query cancellation.** Sends a PostgreSQL
`CancelRequest` on a short-lived **separate** connection (libpq `PQcancel`
style), using the backend PID + secret captured at connect. The in-flight query
on the main link fails with `sqlstate::query_canceled` (**57014**). Returns
`true` if the request was delivered, `false` if no backend key is known
(never connected) or the out-of-band socket failed — a `false` does **not** mean
the query survived.

> **`cancel()` is SYNCHRONOUS / BLOCKING** (capped at ≤ 2 s) and **plaintext even
> when the main connection is SSL** — unlike the rest of the client it briefly
> blocks the calling thread, so do not treat it as non-blocking. A server that
> mandates SSL on every connection will reject the cancel.

**Connection introspection (all `[[nodiscard]]`, libpq-style):**

```cpp
bool t = db.in_transaction();          // true if backend is in a block ('T') or failed block ('E')
bool b = db.used_channel_binding();    // true iff SCRAM-SHA-256-PLUS (tls-server-end-point) was negotiated
std::optional<std::string_view> v = db.parameter_status("server_version");  // PQparameterStatus
int  ver = db.server_version();        // PQserverVersion, e.g. 160002 (16.2); 0 if unknown
int  pid = db.backend_pid();           // PQbackendPID; 0 if not connected
```

`parameter_status(key)` returns the server `ParameterStatus` report for keys like
`"server_version"`, `"server_encoding"`, `"client_encoding"`, `"TimeZone"`,
`"integer_datetimes"`, `"standard_conforming_strings"` — `std::nullopt` if the
server never sent it. The `string_view` is valid while the connection is alive.

---

## 4. Invariants

- One `database` lives on one I/O thread; one serial wire stream. Don't block the
  loop in a callback/coroutine except via `co_await`.
- `connect()` is an awaiter — `co_await` or `run_sync` it; a discarded
  `db.connect(...)` does nothing. There is no blocking `connect()` and no
  `connect` callback overload.
- After `disconnect()` you must call `prepare_reconnect()` before re-`connect()`ing
  the **same** object (it closes the fd, resets buffers/disposed state). Drain or
  fail pending queries first.
- On a lost connection the disconnect handler calls `fail_all_pending(...)`, so
  every queued query and sub-transaction resumes its awaiter with an error
  instead of hanging.
- Prepared-statement parameters are always **binary**; `params` serialization
  must match the declared OID's binary input. Each bind value is length-prefixed
  by a signed int32, so a single parameter ≥ 2 GiB is **rejected** (throws
  `std::length_error`) rather than truncated — the wire length would otherwise
  wrap and desynchronize the stream.
- `set_timeout` only affects the **next** `begin()`; it's transaction-scoped.
- **No nested transactions.** `with_transaction` on a connection already inside a
  block fails fast with a `client_error` (PostgreSQL has one transaction per
  session and would silently flatten the nesting, losing isolation). Use
  savepoints to nest units of work.
- TLS verification is **off by default** (`ssl_verify = ssl_verify_mode::none`):
  the channel is encrypted but the server certificate is unverified (a one-time
  warning is logged). SCRAM-SHA-256 still mutually authenticates the server; it
  is the TLS channel and non-SCRAM auth that are exposed. Set `ssl_verify =
  ssl_verify_mode::full` on a `connection_options` and pass it to
  `connect(connection_options)` for chain/hostname checks (the DSN string cannot
  carry it). There is no libpq `sslmode` string parsing — the level is the
  `ssl_verify` field.
- `Transaction` (and `database`) is non-copyable and non-movable; hold by
  pointer/reference.
- `results` is a `shared_ptr<const result_impl>` (cheap to copy); `row`/`field`
  are non-owning views — never outlive their `results`. Callback `results` are
  borrowing; `deep_snapshot()` to retain.

---

## 5. Gotchas

- **DSN database name is in `[brackets]`:** `...:5432[mydb]`, not `.../mydb`.
- **Scheme does not pick TLS.** Transport is the alias (`tcp::database` vs
  `tcp::ssl::database`), a compile-time choice. `ssl://` in a string on a
  `tcp::database` does **not** enable TLS; `tcp://` on a `tcp::ssl::database`
  still negotiates TLS.
- **`tcp::ssl::database` only exists when `QB_HAS_SSL` is defined** (OpenSSL
  found). Guard SSL code with `#ifdef QB_HAS_SSL`.
- **Callbacks only enqueue.** Nothing reaches the wire until `run_once()` /
  `run()` / `await()` runs on the connection's thread (an actor's tick does it).
- **`bool(results)` ≠ DML success.** It reports row-presence; an INSERT/UPDATE/
  DELETE that returns no rows is falsy. Use `rows_affected()`.
- **Borrowing results.** Using `row`/`field` after a callback returns is UB
  unless you took `deep_snapshot()`. Coroutine `Reply<resultset>` is already a
  snapshot.
- **NULL read throws.** `field.as<T>()` on a NULL non-optional `T` throws
  `error::value_is_null`. Read as `std::optional<T>` or check `is_null()`.
- **`results.json()` stringifies everything** — numbers/bools become JSON
  strings, not native JSON types.
- **`execute_file` / `prepare_file` read the file synchronously** (blocking) on
  the I/O thread.
- **Never use the retired time tokens** (§3.6). `timestamptz`/`timestamp` →
  `qb::wall_time`; timeouts → `qb::duration`.
- **COPY uses the dedicated helpers** (§3.10): `copy_out` for `COPY … TO STDOUT`,
  `copy_in` for `COPY … FROM STDIN`. Issuing a `COPY … FROM STDIN` via plain
  `execute`/`query` (no `copy_in()` source registered) makes the `CopyInResponse`
  surface a `client_error`.
- **`copy_in` is not constant-memory** (it drains the whole source into the
  output pipe); only `copy_out` and `query_stream` stream in constant memory.
- **Overlapping `query_stream` calls share one transaction** (§3.10). They do not
  each get their own: last one out commits, and rolls back if any of them failed.
  Start a stream only *after* your own `begin()` has completed, never while it is
  still in flight.
- **`cancel()` blocks** (synchronous, ≤ 2 s, plaintext even over SSL) — the one
  call in this client that briefly stalls the calling thread.

---

## 6. Build / integration

```cmake
add_subdirectory(qb)                                   # framework first (sets QB_FOUND)
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")     # discovers qbm modules
target_link_libraries(your_app PRIVATE qbm::pgsql)     # links qb::core PUBLIC, qb::io transitively
```

```cpp
#include <qbm/pgsql/pgsql.h>   // connection, transactions, Reply, with_transaction, types, discards
```

Compiled library (17 TUs; STATIC by default, SHARED when
`QB_BUILD_SHARED_LIBS`/`BUILD_SHARED_LIBS` is set), alias `qbm::pgsql`, C++20
default / C++23 selectable via `QB_CXX_STANDARD`. SSL transport follows framework-wide
`QB_HAS_SSL` (auto-detected from OpenSSL) — no per-module SSL toggle. The
module's `CMakeLists.txt` guards on `QB_FOUND`, so `add_subdirectory(qb)` must
come first.
