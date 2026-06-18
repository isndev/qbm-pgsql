# Connecting to PostgreSQL

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qbm-pgsql @ qb 2.0.0 (C++20 default, C++23 supported)

How `qb::pg::tcp::database` (and, under `QB_HAS_SSL`, `qb::pg::tcp::ssl::database`) parses a connection string, opens an asynchronous TCP socket, authenticates, optionally upgrades to TLS, and what happens to in-flight work when the link drops.

**Prerequisites:** [README.md](../README.md) (install, `qb_load_modules`, `qbm::pgsql`) — **See also:** [transaction.md](./transaction.md), [queries.md](./queries.md), [error_handling.md](./error_handling.md), [testing.md](./testing.md)

---

## Summary

A database client is a single object. Construct it (optionally with a connection string), then start the handshake with `connect()`, which returns an awaiter — there is no blocking `bool connect()`. Drive the awaiter with `co_await` inside a coroutine, or with `qb::io::async::run_sync(...)` from synchronous code. Authentication (cleartext, MD5, or SCRAM-SHA-256) runs on the event loop; the awaiter resolves to `true` once the server reaches `ReadyForQuery`, `false` on any failure. The same object *is* the root transaction (see [transaction.md](./transaction.md)), so once connected you call `execute`, `prepare`, `begin`, and friends directly on it.

```cpp
#include <pgsql/pgsql.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>

qb::io::async::init();
qb::pg::tcp::database db;
if (!qb::io::async::run_sync(db.connect("tcp://user:secret@localhost:5432[mydb]"))) {
    // handshake failed — see db error state / error_handling.md
}
```
<!-- src: qbm/pgsql/tests/test-connection.cpp:97-99 -->

`qb::pg::init()` is an optional forward-compatibility initialization hook. It currently performs no required setup — the field-reader's `ParamUnserializer` is statically constructed, so row decoding works without it (the [README](../README.md) quickstart and the test suite never call it). Call it once at startup if you want to be explicit; it is a no-op today, reserved for future per-process type-table setup.
<!-- src: qbm/pgsql/pgsql.cpp:47-55; qbm/pgsql/src/field_reader_integration.cpp:42-45 -->

---

## Concepts

### The client object and its aliases

The implementation type is `qb::pg::detail::Database<QB_IO_, NotifyDerived>`. You almost never name it; use the transport aliases in `qb::pg::tcp`:

| Alias | Transport | Availability |
|:------|:----------|:-------------|
| `qb::pg::tcp::database` | `qb::io::transport::tcp` (cleartext) | always |
| `qb::pg::tcp::ssl::database` | `qb::io::transport::stcp` (TLS) | only when `QB_HAS_SSL` is defined |

<!-- src: qbm/pgsql/pgsql.h:2090,2116 -->

The transport is a **compile-time** choice baked into the alias. The connection string scheme (`tcp`, `ssl`, `socket`) does **not** switch it: a `tcp://…` string on a `tcp::ssl::database` still negotiates TLS, and an `ssl://…` string on a `tcp::database` does **not**. Pick the alias for the security you want; the scheme only feeds host/port resolution.
<!-- src: qbm/pgsql/pgsql.h:700-773 -->

There are also `notify_cb_consumer` / `notify_co_consumer` aliases for dedicated LISTEN/NOTIFY clients; see [queries.md](./queries.md).

### `connection_options`

Every connection is configured through `qb::pg::connection_options`, defined in [`src/common.h`](../src/common.h). You rarely build it by hand — a connection string is parsed into it — but these are the fields it carries:

| Field | Type | Meaning / default |
|:------|:-----|:------------------|
| `alias` | `qb::pg::dbalias` | optional short label; auto-generated from `user@uri[database]` if unset |
| `schema` | `std::string` | transport scheme from the connection string (`tcp`, `socket`, …) |
| `uri` | `std::string` | `host:port` for TCP, `/path/to/socket` for a Unix socket |
| `database` | `std::string` | target database name |
| `user` | `std::string` | role used for authentication and the startup message |
| `password` | `std::string` | secret for cleartext / MD5 / SCRAM auth |
| `connect_timeout` | `qb::duration` | handshake deadline; **default 10 s** |
| `tls_verify_peer` | `bool` | verify the server certificate on TLS connections; **default `false`** |
| `keepalive_interval` | `int` | seconds between TCP keepalive probes; **0 = disabled (default)** |
| `keepalive_probes` | `int` | unanswered probes before the socket is considered dead; default 3 |
| `keepalive_idle` | `int` | idle seconds before the first probe; default 60 |

<!-- src: qbm/pgsql/src/common.h:149-174 -->

`connect_timeout` is a `qb::duration` (the framework's `std::chrono`-based duration). A non-positive value falls back to the 10 s default. Internally the deadline is converted to libev seconds via `qb::detail::to_ev_seconds`; you never deal with that conversion.
<!-- src: qbm/pgsql/pgsql.h:616-621 -->

> The connection string carries `user`, `password`, `database`, and the `host:port`. The remaining fields (`connect_timeout`, `tls_verify_peer`, keepalive) are **not** expressed in the string — set them on the struct, or use the dedicated methods (`connect(qb::duration)`, `enable_keepalive(...)`) and the `tls_verify_peer` field before connecting. There is no libpq-style `sslmode` parsing.

### Connection string and user-defined literals

`connection_options::parse(std::string const&)` reads the connection string. Its general form is:

```text
[alias=]scheme://[user[:password]@]host[:port][database]
```

| Part | Notes |
|:-----|:------|
| `alias=` | optional leading label, e.g. `readonly=tcp://…` |
| `scheme` | `tcp`, `ssl`, or `socket` (used to build the `qb::io::uri`; see the alias note above) |
| `user:password` | credentials, `@`-separated from the host |
| `host:port` | port optional; PostgreSQL default is `5432` |
| `[database]` | database name in **square brackets** — this module's convention, not a libpq URI |

Examples, including the two user-defined literals — `_pg` builds a `connection_options`, `_db` builds a `dbalias`:

```cpp
#include <pgsql/pgsql.h>
using namespace qb::pg;

// Parse to connection_options with the _pg literal:
connection_options opts = "readonly=tcp://user:password@localhost:5432[analytics]"_pg;

// Unix-socket form:
connection_options sock = "socket:///tmp/.s.PGSQL.5432[analytics]"_pg;

// A short alias with the _db literal:
auto label = "analytics"_db;
```
<!-- src: qbm/pgsql/src/common.h:192-199,565-586 -->

For TLS, the example DSNs use a `tcp://` scheme and let the chosen alias (`tcp::ssl::database`) drive the SSLRequest negotiation. A plain `tcp://…` string is fine for an SSL client.
<!-- src: qbm/pgsql/tests/test_config.hpp:10-14,41-42 -->

### Construct, then connect

You can pass the connection string to the constructor (it parses into the stored options but does **not** connect), or pass it to `connect()`:

```cpp
qb::pg::tcp::database db("tcp://user:secret@localhost:5432[mydb]");  // stores options
// ... later, on the I/O thread:
co_await db.connect();                          // uses the stored options
```
<!-- src: qbm/pgsql/pgsql.h:1585-1587 -->

The `connect()` overloads, all returning `connect_awaiter`:

| Overload | Behavior |
|:---------|:---------|
| `connect()` | connect using the stored `connection_options` |
| `connect(qb::duration timeout)` | same, with a per-attempt deadline overriding `connect_timeout` |
| `connect(std::string const& dsn)` | re-parse `dsn` into the stored options, then connect |
| `connect(std::string const& dsn, transport_io_type&& raw_io)` | adopt an already-connected socket (e.g. from a pool), then run the handshake |

<!-- src: qbm/pgsql/pgsql.h:1642-1666 -->

There is **no** callback overload for `connect` (unlike `execute` / `prepare`). Use one of:

- `co_await db.connect(...)` inside a coroutine, or
- `qb::io::async::run_sync(db.connect(...))` from synchronous code (tests and bootstrap paths).

```cpp
// Coroutine form:
qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
    bool ok = co_await db.connect("tcp://user:secret@localhost:5432[mydb]");
    // ...
}());
```
<!-- src: qbm/pgsql/tests/test-connection.cpp:105-111 -->

The awaiter's `await_ready()` returns `true` if the object is already connected (so re-awaiting a live client is a no-op), and `await_resume()` returns `is_connected_` — i.e. the `co_await` / `run_sync` result is `true` only when the handshake reached `ReadyForQuery`.
<!-- src: qbm/pgsql/pgsql.h:1623-1636 -->

---

## Startup and authentication

After the TCP socket is connected, the client switches to the PostgreSQL protocol, starts its read/write watchers, and sends the **startup message**: protocol version 3.0 plus null-terminated `user` and `database` name/value pairs (and any client options).
<!-- src: qbm/pgsql/pgsql.h:645-649,790-806 -->

The server then drives one of the supported authentication exchanges, handled in `on_authentication`:

| Server request | Client response |
|:---------------|:----------------|
| `Authentication OK` (0) | mark connected, apply keepalive settings, resume the awaiter |
| Cleartext (3) | send the password in a `PasswordMessage` |
| MD5 (5) | send `md5` + salted `md5(md5(password+user)+salt)` |
| SCRAM-SHA-256 (10 / 11 / 12) | client nonce → PBKDF2 salted password → client proof, then verify the server signature |
| anything else | reaches the `default:` branch and throws |

<!-- src: qbm/pgsql/pgsql.h:978-1029 -->

SCRAM-SHA-256 is the modern PostgreSQL default and is fully implemented (including server-signature verification). GSS/SSPI/Kerberos and other schemes are **not** implemented: they hit the `default:` throw. That throw is contained at the `noexcept` `onMessage` boundary, which drops the connection and resumes the pending `connect` awaiter with an error — it does **not** call `std::terminate`. The same containment applies to a malformed SCRAM server message. You therefore see an unsupported or hostile auth method as a failed connect, not a descriptive auth-method error.
<!-- src: qbm/pgsql/pgsql.h:448-472,1140 -->

---

## TLS

TLS is **not** a pgsql-specific build option. It follows the framework-wide `QB_HAS_SSL`, which is auto-detected from OpenSSL. When OpenSSL is present, `QB_HAS_SSL` is defined and the `qb::pg::tcp::ssl::database` alias exists; when it is absent, the build is cleartext TCP only and that alias does not compile.
<!-- src: qbm/pgsql/CMakeLists.txt:26-29 -->

The TLS handshake is PostgreSQL's in-band negotiation, not a separate port or scheme:

1. The client connects the plain TCP socket.
2. It sends the 8-byte **SSLRequest** packet (length 8, request code `80877103` / `0x04D2162F`).
3. The server replies with one byte: `S` (proceed with TLS) or `N` (decline).
4. On `S`, the client performs the TLS handshake on the same fd and continues the PostgreSQL startup over the encrypted channel. On `N`, it continues in cleartext.

<!-- src: qbm/pgsql/pgsql.h:670-773 -->

```cpp
#ifdef QB_HAS_SSL
#include <pgsql/pgsql.h>

qb::pg::tcp::ssl::database db;
bool ok = qb::io::async::run_sync(db.connect("tcp://user:secret@db.internal:5432[mydb]"));
#endif
```
<!-- src: qbm/pgsql/tests/test-connection-ssl.cpp:84,108 -->

> **Server-certificate verification is OFF by default.** `tls_verify_peer` defaults to `false`, which preserves this client's historical behavior and matches libpq's `prefer` / `require` semantics: the link is encrypted but the server's certificate chain and hostname are **not** validated. Set `tls_verify_peer = true` on the options before connecting to enable qb-io's chain + hostname verification. There is no `verify-ca` / `verify-full` `sslmode` modeling yet.
<!-- src: qbm/pgsql/src/common.h:160-169; qbm/pgsql/pgsql.h:741-742 -->

---

## Connection lifecycle

### Health check and keepalive

`is_connection_alive()` returns `false` if the client is not connected, otherwise inspects the socket's `SO_ERROR`. It performs **no** wire round-trip, so a half-open or silently dropped connection can read as alive until a keepalive probe or the next query fails.
<!-- src: qbm/pgsql/pgsql.h:1710-1733 -->

TCP keepalive is configured through `connection_options` or `enable_keepalive(int interval, int idle = 60, int probes = 3)`. Settings are applied to the socket **after** the connection is established (on `Authentication OK`); calling `enable_keepalive` before connecting only stores them. An `interval` of 0 leaves keepalive disabled.
<!-- src: qbm/pgsql/pgsql.h:1681-1700,1739-1793 -->

```cpp
qb::pg::tcp::database db;
db.enable_keepalive(/*interval=*/10, /*idle=*/60, /*probes=*/3);  // stored now, applied on connect
co_await db.connect("tcp://user:secret@localhost:5432[mydb]");
```
<!-- src: qbm/pgsql/pgsql.h:1691-1700 -->

### Disconnect and reconnect

`disconnect()` tears down the session and runs the event loop once (`EVRUN_NOWAIT`) so the close I/O is observed; it is safe to call from a coroutine or nested I/O path where `async::run()` would throw.
<!-- src: qbm/pgsql/pgsql.h:1888-1896 -->

To reuse the **same** object for a new connection, call `prepare_reconnect()` after `disconnect()` and before the next `connect()`. It closes the underlying fd, resets the I/O buffers and `qb::io::async::io` disposed state, and clears the handshake/connected flags. Do **not** call it while SQL is still queued on this object — drain or fail the queue first.
<!-- src: qbm/pgsql/pgsql.h:1851-1879 -->

```cpp
ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn)));
db.disconnect();
db.prepare_reconnect();
ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn)));
```
<!-- src: qbm/pgsql/tests/test-connection.cpp:136-143 -->

For a fresh connection you do not need `prepare_reconnect()` — a newly constructed client is ready to `connect()`.

### Fail-all-on-disconnect

A connection is a **single serial stream**: queries queue behind one another. When the link drops, the disconnect handler does more than fail the one in-flight query — it calls `fail_all_pending(...)` on the root transaction, which drains **every** queued query and pending sub-transaction so their callers' awaiters resume with the error instead of hanging forever. `fail_all_pending` swaps the queues out before draining, so an error callback that enqueues new work does not re-enter the traversal.
<!-- src: qbm/pgsql/pgsql.h:1822-1849; qbm/pgsql/src/transaction.h:211 -->

This also covers a malformed wire message and an unsupported/hostile auth method: both mark the protocol invalid (`not_ok()`), which disposes the I/O layer and fires `event::disconnected`, whose handler fails pending work and resumes a pending `connect` awaiter with an error.
<!-- src: qbm/pgsql/pgsql.h:396-411,448-472 -->

---

## Pitfalls

- **No blocking `connect()`.** `connect()` returns an awaiter; nothing happens until you `co_await` it or pass it to `run_sync`. A discarded `db.connect(...)` does **not** connect.
- **Scheme does not pick TLS.** Security is the alias (`tcp::database` vs `tcp::ssl::database`), a compile-time choice. The `tcp` / `ssl` / `socket` scheme in the string only feeds host resolution.
- **TLS does not verify by default.** `tls_verify_peer` is `false`. For untrusted networks, set it to `true` before connecting. Encryption without verification does not protect against an active man-in-the-middle.
- **Database name uses square brackets.** `…:5432[mydb]`, not `…/mydb`. This is the module's parser convention.
- **Reuse needs `prepare_reconnect()`.** After `disconnect()`, you must call `prepare_reconnect()` before connecting the *same* object again. Skipping it leaves the I/O layer disposed and the next handshake will not start.
- **`is_connection_alive()` is local-only.** It reads `SO_ERROR`, not the wire. Use keepalive or treat a query failure as the real liveness signal.
- **Connect-time timeout vs statement timeout are different.** `connect(qb::duration)` and `connect_timeout` bound the *handshake*. `Transaction::set_timeout(qb::duration)` sets PostgreSQL `statement_timeout` for the next `BEGIN` — see [transaction.md](./transaction.md). Do not conflate them.
- **Time types.** Connection deadlines are `qb::duration`. The retired tokens `qb::Timestamp`, `qb::Duration`, `qb::TimePoint`, `to_timestamp(`, and `to_time_point(` must not appear in your code — they no longer exist in this API. (PostgreSQL `timestamptz` values map to `qb::wall_time` on the result side; see [types.md](./types.md).)

---

## See also

- [transaction.md](./transaction.md) — the client *is* the root transaction; `set_timeout` vs connect timeout
- [queries.md](./queries.md) — `execute` / `prepare`, and the LISTEN/NOTIFY consumers
- [error_handling.md](./error_handling.md) — connection and query error types
- [testing.md](./testing.md) — `QB_PG_DSN` / `QB_PG_SSL_DSN` environment variables for the test suite
- [README.md](../README.md) — install, `qb_load_modules`, linking `qbm::pgsql`
