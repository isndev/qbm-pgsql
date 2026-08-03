# Connecting to PostgreSQL

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qbm-pgsql @ qb 3.0.0 (C++20 default, C++23
> supported)

How `qb::pg::tcp::database` (and, under `QB_HAS_SSL`, `qb::pg::tcp::ssl::database`) parses a connection string, opens an
asynchronous TCP socket, authenticates, optionally upgrades to TLS, and what happens to in-flight work when the link
drops.

**Prerequisites:** [README.md](../README.md) (install, `qb_load_modules`, `qbm::pgsql`) — **See also:
** [transaction.md](./transaction.md), [queries.md](./queries.md), [error_handling.md](./error_handling.md), [testing.md](./testing.md)

---

## Summary

A database client is a single object. Construct it (optionally with a connection string), then start the handshake with
`connect()`, which returns an awaiter — there is no blocking `bool connect()`. Drive the awaiter with `co_await` inside
a coroutine, or with `qb::io::async::run_sync(...)` from synchronous code. Authentication (cleartext, MD5, or
SCRAM-SHA-256) runs on the event loop; the awaiter resolves to `true` at `AuthenticationOk` (the resume is deferred to
the coroutine scheduler, by which point `ParameterStatus` / `BackendKeyData` / `ReadyForQuery` have been processed),
`false` on any failure. The same object *is* the root transaction (see [transaction.md](./transaction.md)), so once connected
you call `execute`, `prepare`, `begin`, and friends directly on it.

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

<!-- src: qbm/pgsql/tests/integration/connection/connection-lifecycle.cpp:73-77 -->

`qb::pg::init()` is an optional forward-compatibility initialization hook. It currently performs no required setup — the
field-reader's `ParamUnserializer` is statically constructed, so row decoding works without it (
the [README](../README.md) quickstart and the test suite never call it). Call it once at startup if you want to be
explicit; it is a no-op today, reserved for future per-process type-table setup.
<!-- src: qbm/pgsql/pgsql.cpp:194-201 (qb::pg::init → detail::initialize_field_reader); qbm/pgsql/src/field_reader_integration.cpp -->

The handshake `connect()` drives, end to end:

```mermaid
sequenceDiagram
    participant C as qb::pg::database (client)
    participant S as PostgreSQL server
    opt TLS — tcp::ssl alias
        C->>S: SSLRequest
        S-->>C: S (accept) / N (reject), then TLS handshake
    end
    C->>S: StartupMessage (user, database)
    S-->>C: AuthenticationRequest (cleartext · MD5 · SCRAM-SHA-256)
    loop SCRAM-SHA-256 — multi-step
        C->>S: client-first / client-final
        S-->>C: server-first / server-final
    end
    S-->>C: AuthenticationOk
    S-->>C: ParameterStatus · BackendKeyData · ReadyForQuery
    Note over C: connect() awaiter resolves true at AuthenticationOk<br/>(resume deferred · ParameterStatus/BackendKeyData/ReadyForQuery already seen)<br/>(false on failure · default 10 s deadline)
```

---

## Concepts

### The client object and its aliases

The implementation type is `qb::pg::detail::Database<QB_IO_, NotifyDerived>`. You almost never name it; use the
transport aliases in `qb::pg::tcp`:

| Alias                        | Transport                            | Availability                      |
|:-----------------------------|:-------------------------------------|:----------------------------------|
| `qb::pg::tcp::database`      | `qb::io::transport::tcp` (cleartext) | always                            |
| `qb::pg::tcp::ssl::database` | `qb::io::transport::stcp` (TLS)      | only when `QB_HAS_SSL` is defined |

<!-- src: qbm/pgsql/pgsql.h:2655 (tcp::database), 2681 (tcp::ssl::database) -->

The transport is a **compile-time** choice baked into the alias. The connection string scheme (`tcp`, `ssl`, `socket`)
does **not** switch it: a `tcp://…` string on a `tcp::ssl::database` still negotiates TLS, and an `ssl://…` string on a
`tcp::database` does **not**. Pick the alias for the security you want; the scheme only feeds host/port resolution.
<!-- src: qbm/pgsql/pgsql.h:613-644 -->

There are also `notify_cb_consumer` / `notify_co_consumer` aliases for dedicated LISTEN/NOTIFY clients;
see [queries.md](./queries.md).

### `connection_options`

Every connection is configured through `qb::pg::connection_options`, defined in [`src/common.h`](../src/common.h). You
rarely build it by hand — a connection string is parsed into it — but these are the fields it carries:

| Field                | Type              | Meaning / default                                                                                                 |
|:---------------------|:------------------|:------------------------------------------------------------------------------------------------------------------|
| `alias`              | `qb::pg::dbalias` | optional short label; auto-generated from `user@uri[database]` if unset                                           |
| `schema`             | `std::string`     | transport scheme from the connection string (`tcp`, `socket`, …)                                                  |
| `uri`                | `std::string`     | `host:port` for TCP, `/path/to/socket` for a Unix socket                                                          |
| `database`           | `std::string`     | target database name                                                                                              |
| `user`               | `std::string`     | role used for authentication and the startup message                                                              |
| `password`           | `std::string`     | secret for cleartext / MD5 / SCRAM auth                                                                           |
| `connect_timeout`    | `qb::duration`    | handshake deadline; **default 10 s**                                                                              |
| `ssl_verify`         | `ssl_verify_mode` | TLS certificate verification level: `none` (encrypt only, **default**) or `full` (verify chain + host); see below |
| `ssl_root_cert`      | `std::string`     | PEM CA file/dir trusted IN ADDITION to the system store (libpq `sslrootcert`); lets `full` validate a private CA    |
| `ssl_cert`, `ssl_key`| `std::string`     | PEM client certificate + private key for mutual TLS (libpq `sslcert`/`sslkey`); **both** required to take effect    |
| `keepalive_interval` | `int`             | seconds between TCP keepalive probes; **0 = disabled (default)**                                                  |
| `keepalive_probes`   | `int`             | unanswered probes before the socket is considered dead; default 3                                                 |
| `keepalive_idle`     | `int`             | idle seconds before the first probe; default 60                                                                   |

<!-- src: qbm/pgsql/src/common.h:149-186 -->

`connect_timeout` is a `qb::duration` (the framework's `std::chrono`-based duration). A non-positive value falls back to
the 10 s default. Internally the deadline is converted to libev seconds via `qb::detail::to_ev_seconds`; you never deal
with that conversion.
<!-- src: qbm/pgsql/pgsql.h:608-611 -->

> The connection string carries `user`, `password`, `database`, and the `host:port`. The remaining fields (
`connect_timeout`, `ssl_verify`, keepalive) are **not** expressed in the string — set them on a `connection_options`
> struct and pass it to the `connect(connection_options)` overload, or use the dedicated methods (
`connect(qb::duration)`,
`enable_keepalive(...)`). For TLS verification:
>
> ```cpp
> auto opts = qb::pg::connection_options::parse("tcp://user:secret@db:5432[app]");
> opts.ssl_verify    = qb::pg::ssl_verify_mode::full;   // verify chain + hostname
> opts.ssl_root_cert = "certs/internal-ca.pem";         // OPTIONAL: validate against a PRIVATE CA
> opts.ssl_cert      = "certs/client.pem";              // OPTIONAL: client certificate for mutual TLS
> opts.ssl_key       = "certs/client.key";              //           (ssl_cert + ssl_key together)
> co_await db.connect(opts);   // ssl:// database; a bad CA/cert/key path fails the connect CLOSED
> ```
>
> Under the hood the `ssl://` database builds a value-semantic `qb::io::ssl::Context` from these options
> (system trust by default; `ssl_root_cert` adds a private CA; `ssl_cert`+`ssl_key` present a client
> certificate) and hands it to the async STARTTLS connector — so custom-CA and client-certificate (mTLS)
> auth now work over PostgreSQL's `SSLRequest` upgrade.

### Connection string and user-defined literals

`connection_options::parse(std::string const&)` reads the connection string. Its general form is:

```text
[alias=]scheme://[user[:password]@]host[:port][database]
```

| Part            | Notes                                                                                 |
|:----------------|:--------------------------------------------------------------------------------------|
| `alias=`        | optional leading label, e.g. `readonly=tcp://…`                                       |
| `scheme`        | `tcp`, `ssl`, or `socket` (used to build the `qb::io::uri`; see the alias note above) |
| `user:password` | credentials, `@`-separated from the host                                              |
| `host:port`     | port optional; PostgreSQL default is `5432`                                           |
| `[database]`    | database name in **square brackets** — this module's convention, not a libpq URI      |

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

For TLS, the example DSNs use a `tcp://` scheme and let the chosen alias (`tcp::ssl::database`) drive the SSLRequest
negotiation. A plain `tcp://…` string is fine for an SSL client.
<!-- src: qbm/pgsql/tests/shared/test_config.hpp:11-13,39-43 -->

### Construct, then connect

You can pass the connection string to the constructor (it parses into the stored options but does **not** connect), or
pass it to `connect()`:

```cpp
qb::pg::tcp::database db("tcp://user:secret@localhost:5432[mydb]");  // stores options
// ... later, on the I/O thread:
co_await db.connect();                          // uses the stored options
```

<!-- src: qbm/pgsql/pgsql.h:1646-1657 (explicit Database(std::string const&)) -->

The `connect()` overloads, all returning `connect_awaiter`:

| Overload                                                      | Behavior                                                                     |
|:--------------------------------------------------------------|:-----------------------------------------------------------------------------|
| `connect()`                                                   | connect using the stored `connection_options`                                |
| `connect(qb::duration timeout)`                               | same, with a per-attempt deadline overriding `connect_timeout`               |
| `connect(std::string const& dsn)`                             | re-parse `dsn` into the stored options, then connect                         |
| `connect(connection_options opts)`                            | replace the stored options with `opts` (e.g. to set `ssl_verify`), then connect |
| `connect(std::string const& dsn, transport_io_type&& raw_io)` | adopt an already-connected socket (e.g. from a pool), then run the handshake |

<!-- src: qbm/pgsql/pgsql.h:1708-1750 (the five connect() overloads) -->

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

<!-- src: qbm/pgsql/tests/integration/connection/connection-lifecycle.cpp:79-90 -->

The awaiter's `await_ready()` returns `true` if the object is already connected (so re-awaiting a live client is a
no-op), and `await_resume()` returns `is_connected_` — i.e. the `co_await` / `run_sync` result is `true` only when the
handshake reached `AuthenticationOk` (the point where `is_connected_` is set; the deferred resume means
`ParameterStatus` / `BackendKeyData` / `ReadyForQuery` have typically been processed by the time the coroutine runs).
<!-- src: qbm/pgsql/pgsql.h:1692-1705 (connect_awaiter await_ready / await_suspend / await_resume) -->

---

## Startup and authentication

After the TCP socket is connected, the client switches to the PostgreSQL protocol, starts its read/write watchers, and
sends the **startup message**: protocol version 3.0 plus null-terminated `user` and `database` name/value pairs (and any
client options).
<!-- src: qbm/pgsql/pgsql.h:690-694 (switch_protocol + start + send_startup_message), 767-783 (create_startup_message) -->

The server then drives one of the supported authentication exchanges, handled in `on_authentication`:

| Server request                      | Client response                                                                                                                                             |
|:------------------------------------|:------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `Authentication OK` (0)             | mark connected, apply keepalive settings, resume the awaiter                                                                                                |
| Cleartext (3)                       | send the password in a `PasswordMessage`                                                                                                                    |
| MD5 (5)                             | send `md5` + salted `md5(md5(password+user)+salt)`                                                                                                          |
| SCRAM-SHA-256(-PLUS) (10 / 11 / 12) | negotiate channel binding → client nonce → **verify the server nonce extends it** → PBKDF2 salted password → client proof, then verify the server signature |
| anything else                       | reaches the `default:` branch and throws                                                                                                                    |

<!-- src: qbm/pgsql/pgsql.h (on_authentication) -->

SCRAM-SHA-256 is the modern PostgreSQL default and is fully implemented, with **both** mutual-authentication checks RFC
5802 requires: (1) the server's combined nonce must begin with the exact client nonce and extend it (rejected before any
proof is derived — guards against a man-in-the-middle or replay that does not faithfully continue our exchange), and (2)
the server signature (`v=`) in the `AuthenticationSASLFinal` message is recomputed and compared **constant-time** on the
raw HMAC bytes. This second check is **enforced**: once a SCRAM exchange has started, `AuthenticationOk` is **refused**
unless that server signature verified — an impersonating server or active MITM that skips (or fails) `SASLFinal` and
sends a bare `AuthenticationOk` is rejected, not trusted, because the client's own proof leaks nothing that would stop
it. The gate is re-armed at the start of every handshake (in `on_transport_ready`, the single choke point all connect
paths funnel through), so a bare reconnect cannot carry a stale "verified" flag across connections. GSS/SSPI/Kerberos and
other schemes are **not** implemented: they hit the `default:` throw. That throw is contained at the `noexcept`
`onMessage` boundary, which drops the connection and resumes the pending `connect` awaiter with an error — it does
**not** call `std::terminate`. The same containment applies to a malformed SCRAM server message (including a mismatched
nonce). You therefore see an unsupported or hostile auth method as a failed connect, not a descriptive auth-method error.
<!-- src: qbm/pgsql/pgsql.h:739-749 (gate re-armed in on_transport_ready), 976-987 (AuthenticationOk refused without a verified server signature), 1169-1172 (default: throw) -->

The `AuthenticationOk` row in the table above is therefore conditional: it marks the connection ready *only after* the
mutual-auth gate is satisfied for a SCRAM handshake.

**Channel binding (SCRAM-SHA-256-PLUS).** Over TLS, the client parses the mechanisms the server offers and, when it sees
`SCRAM-SHA-256-PLUS`, negotiates **`tls-server-end-point` channel binding** (RFC 5929): a hash of the server certificate
is mixed into the SCRAM proof, binding the authentication to this exact TLS channel. An active man-in-the-middle holding
valid credentials cannot relay it onto another channel — the bound proof would not verify. The gs2 cbind-flag follows
RFC 5802: `p=tls-server-end-point` when bound; `y` over TLS when the server did *not* advertise `-PLUS` (so the server
can detect a downgrade that stripped it); `n` on a cleartext link. Negotiation is automatic; check the result with
`db.used_channel_binding()`.
<!-- src: qbm/pgsql/pgsql.h (on_authentication, used_channel_binding); qb/src/qb/io/tcp/ssl/socket.cpp (tls_server_end_point) -->
<!-- src: qbm/pgsql/pgsql.h:1027-1050 (gs2 cbind-flag negotiation), 1101-1106 (cbind_input) -->

---

## TLS

TLS is **not** a pgsql-specific build option. It follows the framework-wide `QB_HAS_SSL`, which is auto-detected from
OpenSSL. When OpenSSL is present, `QB_HAS_SSL` is defined and the `qb::pg::tcp::ssl::database` alias exists; when it is
absent, the build is cleartext TCP only and that alias does not compile.
<!-- src: qbm/pgsql/CMakeLists.txt:26-29 -->

TLS is PostgreSQL's in-band negotiation (STARTTLS-style), not a separate port or scheme. The whole exchange is **fully
asynchronous** — driven by the event loop, never blocking it:

1. The client connects the plain TCP socket (async).
2. It sends the 8-byte **SSLRequest** packet (length 8, request code `80877103` / `0x04D2162F`).
3. The server replies with one byte: `S` (proceed with TLS) or `N` (decline).
4. On `S`, the client drives the TLS handshake on the same fd (event-loop pumped) and continues the PostgreSQL startup
   over the encrypted channel.

This runs through qb-io's generic opportunistic-TLS primitive,
`qb::io::async::tcp::starttls_connect<Socket, Negotiator>()` — a first-class connector capability that any STARTTLS
protocol can reuse (the pgsql side provides `qb::pg::detail::postgres_ssl_negotiator`). There is **no** blocking `send`/
`recv`/`SSL_connect` on the connect path.
<!-- src: qbm/pgsql/pgsql.h (start_connect_from_awaiter, postgres_ssl_negotiator); qb/src/qb/io/async/tcp/connector.h (starttls_connect) -->

> **A secure database requires TLS.** If the server declines SSL (`N`), the connect **fails** —
`qb::pg::tcp::ssl::database` is encrypt-or-nothing (libpq `sslmode=require` semantics). For cleartext, use the plain
`qb::pg::tcp::database`, which never sends an SSLRequest. (This replaces an earlier `N` "fallback" that produced an
> unusable handle-less `ssl::socket`.)

```cpp
#ifdef QB_HAS_SSL
#include <pgsql/pgsql.h>

qb::pg::tcp::ssl::database db;
bool ok = qb::io::async::run_sync(db.connect("tcp://user:secret@db.internal:5432[mydb]"));
#endif
```

<!-- src: qbm/pgsql/tests/integration/connection/connection-ssl.cpp:106-118 -->

> **Server-certificate verification is OFF by default.** `ssl_verify` defaults to `ssl_verify_mode::none` (≈ libpq
`sslmode=require`): the link is encrypted but the server's certificate chain and hostname are **not** validated, and the
client logs a one-time `LOG_WARN` so an unverified secure connection is never silent. Note the scope: SCRAM-SHA-256
mutual auth (enforced — see [Startup and authentication](#startup-and-authentication)) still authenticates the *server*
even here; it is the TLS channel itself and any non-SCRAM auth that stay unprotected against an active MITM. Set
`ssl_verify = ssl_verify_mode::full` (≈ libpq `verify-full`) on the options before connecting to enable qb-io's chain +
> hostname verification — checked during the handshake, before any data flows, so a verification failure aborts the
> connect. libpq's intermediate `verify-ca` (chain without hostname) is intentionally not offered: it accepts a valid
> certificate issued for a *different* host, leaving an active-MITM window. `disable`/`prefer` map to the transport
> choice — `tcp::database` never sends an SSLRequest; `tcp::ssl::database` requires TLS.
<!-- src: qbm/pgsql/src/common.h:160-169; qbm/pgsql/pgsql.h:634-644 -->

---

## Connection lifecycle

### Health check and keepalive

`is_connection_alive()` returns `false` if the client is not connected, otherwise inspects the socket's `SO_ERROR`. It
performs **no** wire round-trip, so a half-open or silently dropped connection can read as alive until a keepalive probe
or the next query fails.
<!-- src: qbm/pgsql/pgsql.h:1827-1858 (is_connection_alive) -->

TCP keepalive is configured through `connection_options` or
`enable_keepalive(int interval, int idle = 60, int probes = 3)`. Settings are applied to the socket **after** the
connection is established (on `Authentication OK`); calling `enable_keepalive` before connecting only stores them. An
`interval` of 0 leaves keepalive disabled.
<!-- src: qbm/pgsql/pgsql.h:1793-1812 (enable_keepalive), 988-991 (applied on AuthenticationOk), 2279-2326 (apply_keepalive_settings) -->

```cpp
qb::pg::tcp::database db;
db.enable_keepalive(/*interval=*/10, /*idle=*/60, /*probes=*/3);  // stored now, applied on connect
co_await db.connect("tcp://user:secret@localhost:5432[mydb]");
```

<!-- src: qbm/pgsql/pgsql.h:1793-1812 (enable_keepalive) -->

### Connection introspection

Once connected, three read-only accessors expose what the server reported during the handshake. They are local lookups —
no wire round-trip — and map onto the corresponding libpq calls:

| Accessor                                 | Returns                                                                                                                                                                                                                                                                          | libpq equivalent    |
|:-----------------------------------------|:---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|:--------------------|
| `parameter_status(std::string_view key)` | `std::optional<std::string_view>` — the value of a server `ParameterStatus` report (e.g. `"server_version"`, `"server_encoding"`, `"client_encoding"`, `"TimeZone"`, `"standard_conforming_strings"`, `"application_name"`), or `std::nullopt` if the server never sent that key | `PQparameterStatus` |
| `server_version()`                       | `int` — the server version as a libpq-style integer: `16.2 → 160002`, `9.6.24 → 90624`; `0` if `server_version` is unknown                                                                                                                                                       | `PQserverVersion`   |
| `backend_pid()`                          | `int` — the backend process id captured from `BackendKeyData` at connect time (the same PID `cancel()` targets), or `0` if not connected                                                                                                                                         | `PQbackendPID`      |

```cpp
co_await db.connect("tcp://user:secret@localhost:5432[mydb]");
if (db.server_version() < 140000)
    log_warn("server is older than PostgreSQL 14");
if (auto tz = db.parameter_status("TimeZone"))
    log_info("server TimeZone = " + std::string(*tz));
int pid = db.backend_pid();  // 0 until connected
```

The returned `std::string_view` from `parameter_status` is valid only while this connection is alive.
<!-- src: qbm/pgsql/pgsql.h:1885-1898 (parameter_status), 1900-1907 (backend_pid), 1909-1951 (server_version) -->

### Cancelling a running query

`cancel()` aborts the query currently executing on the connection. Because the link is a single serial stream busy with
the in-flight statement, the cancel travels **out of band**: it opens a short-lived second connection and sends a
PostgreSQL `CancelRequest` built from the backend PID + secret captured at connect time (`BackendKeyData`). The server
aborts the running statement, which surfaces to the awaiting caller as an ordinary `db_error` with
`sqlstate::query_canceled` (57014) — the connection itself stays usable.

```mermaid
sequenceDiagram
    participant App as caller (timer / actor)
    participant Live as live connection (parked on the query)
    participant C2 as out-of-band socket (plaintext)
    participant S as PostgreSQL server
    App->>C2: cancel() — blocking connect (cap ≤ 2 s)
    C2->>S: CancelRequest (backend PID + secret from BackendKeyData)
    S-->>Live: abort the running statement
    Live-->>App: db_error sqlstate::query_canceled (57014)
    Note over C2: 16-byte packet, no reply — best-effort
```

This mirrors libpq's `PQcancel`: it is **synchronous and best-effort** (the request is a 16-byte packet, no reply). It
returns `true` if the request was sent, `false` if the client never finished a handshake (no key yet) or the out-of-band
socket failed — a `false` does not prove the query survived. Call it from another execution context than the parked
`co_await` — a timer, a signal handler, or a separate actor:

```cpp
// Abort a query that runs too long, from a one-shot timer.
qb::io::async::callback([&db] { db.cancel(); }, std::chrono::seconds(2));
auto r = co_await db.query("SELECT slow_report()");
if (!r.ok() && r.error().sqlstate == qb::pg::sqlstate::query_canceled)
    handle_timeout();
```

<!-- src: qbm/pgsql/pgsql.h:2173-2231 (cancel) -->

> **`cancel()` is SYNCHRONOUS / BLOCKING — it is *not* non-blocking like the rest of the client.** Unlike every other
> operation (which is event-loop-driven and `co_await`-only), `cancel()` opens its out-of-band socket and sends the
`CancelRequest` with a **blocking** connect + send. It briefly **parks the calling thread**, so if you fire it from a
> one-shot timer ON the I/O loop (the usual pattern above), it stalls that loop for the duration. The blocking connect
> is
**capped at ≤ 2 s** (it targets the same already-reachable endpoint as the live link, so it is normally sub-millisecond;
> the cap only bounds the pathological unreachable case). To keep the worst case short, set a small `connect_timeout` on
> the connection (the cap is `min(connect_timeout, 2 s)`), or run `cancel()` off the I/O thread.

> The cancel connection is **plaintext** even when the main link is SSL. A server that mandates SSL on every
> connection (`hostssl`-only) will reject it; SSL-tunnelled cancellation is a future enhancement, tracked with
`sslmode`/
`verify-full`. For a server-side guard that needs no client action, set a `statement_timeout` (or the client
`set_timeout`, see [transaction.md](./transaction.md)).

### Disconnect and reconnect

`disconnect()` tears down the session and runs the event loop once (`EVRUN_NOWAIT`) so the close I/O is observed; it is
safe to call from a coroutine or nested I/O path where `async::run()` would throw.
<!-- src: qbm/pgsql/pgsql.h:2432-2458 (disconnect) -->

To reuse the **same** object for a new connection, call `prepare_reconnect()` after `disconnect()` and before the next
`connect()`. It closes the underlying fd, resets the I/O buffers and `qb::io::async::io` disposed state, and clears the
handshake/connected flags. Do **not** call it while SQL is still queued on this object — drain or fail the queue first.
<!-- src: qbm/pgsql/pgsql.h:2388-2430 (prepare_reconnect) -->

```cpp
ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn)));
db.disconnect();
db.prepare_reconnect();
ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn)));
```

<!-- src: qbm/pgsql/tests/integration/connection/connection-lifecycle.cpp:120-135 -->

For a fresh connection you do not need `prepare_reconnect()` — a newly constructed client is ready to `connect()`.

### Fail-all-on-disconnect

A connection is a **single serial stream**: queries queue behind one another. When the link drops, the disconnect
handler does more than fail the one in-flight query — it calls `fail_all_pending(...)` on the root transaction, which
drains **every** queued query and pending sub-transaction so their callers' awaiters resume with the error instead of
hanging forever. `fail_all_pending` swaps the queues out before draining, so an error callback that enqueues new work
does not re-enter the traversal.
<!-- src: qbm/pgsql/pgsql.h:2347-2386 (on(disconnected)), 2374 (fail_all_pending call); qbm/pgsql/src/transaction.h:214-222 -->

This also covers a malformed wire message and an unsupported/hostile auth method: both mark the protocol invalid (
`not_ok()`), which disposes the I/O layer and fires `event::disconnected`, whose handler fails pending work and resumes
a pending `connect` awaiter with an error.
<!-- src: qbm/pgsql/pgsql.h:335-359 (onMessage noexcept containment -> not_ok) -->

---

## Pitfalls

- **No blocking `connect()`.** `connect()` returns an awaiter; nothing happens until you `co_await` it or pass it to
  `run_sync`. A discarded `db.connect(...)` does **not** connect.
- **Scheme does not pick TLS.** Security is the alias (`tcp::database` vs `tcp::ssl::database`), a compile-time choice.
  The `tcp` / `ssl` / `socket` scheme in the string only feeds host resolution.
- **TLS does not verify by default.** `ssl_verify` is `ssl_verify_mode::none`. For untrusted networks, set it to
  `ssl_verify_mode::full` (via the `connect(connection_options)` overload) before connecting. Encryption without
  verification does not protect against an active man-in-the-middle.
- **Database name uses square brackets.** `…:5432[mydb]`, not `…/mydb`. This is the module's parser convention.
- **Reuse needs `prepare_reconnect()`.** After `disconnect()`, you must call `prepare_reconnect()` before connecting the
  *same* object again. Skipping it leaves the I/O layer disposed and the next handshake will not start.
- **`is_connection_alive()` is local-only.** It reads `SO_ERROR`, not the wire. Use keepalive or treat a query failure
  as the real liveness signal.
- **Connect-time timeout vs statement timeout are different.** `connect(qb::duration)` and `connect_timeout` bound the
  *handshake*. `Transaction::set_timeout(qb::duration)` sets PostgreSQL `statement_timeout` for the next `BEGIN` —
  see [transaction.md](./transaction.md). Do not conflate them.
- **Time types.** Connection deadlines are `qb::duration`. The retired tokens `qb::Timestamp`, `qb::Duration`,
  `qb::TimePoint`, `to_timestamp(`, and `to_time_point(` must not appear in your code — they no longer exist in this
  API. (PostgreSQL `timestamptz` values map to `qb::wall_time` on the result side; see [types.md](./types.md).)

---

## See also

- [transaction.md](./transaction.md) — the client *is* the root transaction; `set_timeout` vs connect timeout
- [queries.md](./queries.md) — `execute` / `prepare`, and the LISTEN/NOTIFY consumers
- [error_handling.md](./error_handling.md) — connection and query error types
- [testing.md](./testing.md) — `QB_PG_DSN` / `QB_PG_SSL_DSN` environment variables for the test suite
- [README.md](../README.md) — install, `qb_load_modules`, linking `qbm::pgsql`
