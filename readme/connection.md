# Connection management

How **`qb::pg::tcp::database`** and **`qb::pg::tcp::ssl::database`** attach to PostgreSQL: DSN parsing, async *
*`connect_awaiter`**, authentication, TLS upgrade, lifecycle, and errors.

**Primary type:** **`qb::pg::detail::Database<QB_IO_, void>`** exposed as **`qb::pg::tcp::database`** (cleartext) or *
*`qb::pg::tcp::ssl::database`** (OpenSSL build).

---

## Types and inheritance

- **`tcp::client<Database, Transport, void>`** — socket, **`qb::io::async::io`**, protocol attachment.
- **`detail::Transaction`** — same object is the **root transaction** ( **`execute`**, **`begin`**, …).

There is **no** synchronous **`bool connect()`**. Connection is started with **`connect()`** returning *
*`connect_awaiter`**.

### No callback overload for `connect`

Unlike **`execute`**, **`listen`**, **`notify`**, etc., there is **no** `connect(dsn, on_ok, on_err)`. Use:

- **`co_await db.connect(...)`** inside a coroutine, or
- **`qb::io::async::run_sync(db.connect(...))`** from synchronous code.

The awaiter type and handshake live in [`pgsql.h`](../pgsql.h) (**`connect_awaiter`** struct, **`Database::connect`**
overloads).

---

## Connection string (DSN)

Parsed by **`qb::pg::connection_options::parse`** (**`src/common.cpp`**).

**General form:**

```text
tcp://[user[:password]@]host[:port][database_name]
```

| Part                    | Meaning                                                                                                               |
|:------------------------|:----------------------------------------------------------------------------------------------------------------------|
| **`tcp://`**            | Used for **both** cleartext and SSL clients. SSL uses **`SSLRequest`** after TCP connect, not a different URL scheme. |
| **`user` / `password`** | Authentication. Special characters in passwords must survive your URL embedding rules.                                |
| **`host`**              | Hostname or IP.                                                                                                       |
| **`port`**              | Optional; default **5432**.                                                                                           |
| **`[database]`**        | Database name in **square brackets** (parser convention in this module).                                              |

**Examples:**

```cpp
qb::pg::tcp::database db("tcp://readonly:secret@db.internal:5432[analytics]");
qb::pg::tcp::database empty;
// Later:
auto a = empty.connect("tcp://test:test@127.0.0.1:5432[test]");
```

**Structured `connection_options`:** There is **no** `Database::connect(connection_options)` overload. Typical flows:

1. **`database(std::string const& dsn)`** — stores parsed options; then **`connect()`** uses **`conn_opts_`**.
2. **`connect(std::string const& dsn)`** — re-parses and starts handshake.

Fields on **`connection_options`** (**`src/common.h`**) include **`user`**, **`password`**, **`database`**, **`uri`** (
host:port), **`connect_timeout`** (seconds, default **10**), TCP keepalive integers, **`application_name`**, etc. To
build options manually, populate the struct and **serialize to the DSN format your parser accepts**, or extend the
module if you need a direct struct connect API.

---

## `connect_awaiter` — starting the session

**Signatures (see `pgsql.h`):**

- **`connect()`** — use stored **`conn_opts_`**.
- **`connect(double timeout_sec)`** — override **this attempt’s** deadline (still uses **`conn_opts_`** for
  host/user/db).
- **`connect(std::string const& dsn)`** — parse then **`connect()`**.
- **`connect(std::string const& dsn, transport&&)`** — for advanced injection (e.g. pooled socket).

**Awaiter behaviour:**

- **`await_ready()`** — **`true`** if already **`is_connected_`**.
- **`co_await`** — suspends until handshake success or failure; **`await_resume()`** is **`true`** iff connected.

**Synchronous / test code:**

```cpp
qb::io::async::init();
qb::pg::tcp::database db;
if (!qb::io::async::run_sync(db.connect("tcp://u:p@h:5432[d]"))) {
    /* not connected */
}
```

**Handshake** runs incrementally on the event loop (startup, auth rounds, **`BackendKeyData`**, **`ReadyForQuery`**). A
**timer** can fail the attempt with a **timeout** **`db_error`** — separate from **`set_timeout()`** which sets
PostgreSQL **`statement_timeout`** on the **next** **`BEGIN`**.

---

## `disconnect()` and `prepare_reconnect()`

Documented on **`Database`** in [`pgsql.h`](../pgsql.h):

- **`disconnect()`** — tears down the session and triggers a short **`EVRUN_NOWAIT`** pass so pending close I/O is
  observed. Underlying TCP may **shutdown** while keeping the handle in a state where a **new** socket is opened on the
  next connect (see comments near **`prepare_reconnect`** about **`tcp::socket::disconnect()`** vs a fully closed fd).
- **`prepare_reconnect()`** — **must** be called on the **same** **`database`** after **`disconnect()`** before *
  *`connect()`** again: calls **`reset_io_state()`** on the transport and clears handshake / connected flags. **Do not**
  call while SQL commands are still queued on this object.

```cpp
db.disconnect();
db.prepare_reconnect();
(void)qb::io::async::run_sync(db.connect("tcp://..."));
```

---

## Authentication (`Database::on_authentication`)

Handler on **`Database`** in [`pgsql.h`](../pgsql.h) (search **`on_authentication`**). Drives the authentication
sub-state machine from server **`Authentication*`** messages.

| Server code                    | Client behaviour                                                          |
|:-------------------------------|:--------------------------------------------------------------------------|
| **OK**                         | Mark connected; apply socket keepalive from options.                      |
| **Cleartext (3)**              | Send password message.                                                    |
| **MD5 (5)**                    | Salted MD5 per PostgreSQL spec.                                           |
| **SCRAM-SHA-256 (10, 11, 12)** | Nonce, PBKDF2, client proof, **server signature verification** (OpenSSL). |
| **Other**                      | Unsupported → failure path.                                               |

SCRAM includes validation of iteration count and defensive parsing of server-first message fields.

---

## SSL / TLS

When **`QB_HAS_SSL`** is defined:

- Use **`qb::pg::tcp::ssl::database`**.
- DSN remains **`tcp://...`**.
- Transport performs PostgreSQL **SSLRequest**; on **`S`**, upgrades with **`qb::io::transport::stcp`**.

Integration tests: **`qbm-pgsql-test-connection-ssl`**; env **`QB_PG_SSL_DSN`** (see [testing.md](./testing.md)).

---

## Parameter status

After authentication, **`ParameterStatus`** messages update internal maps (**`on_parameter_status`**). Rarely needed in
application code; useful for debugging encoding/timezone.

---

## Incoming asynchronous messages

While idle in **`ReadyForQuery`**, the server may send **NOTICE**, **NOTIFY**, **ParameterStatus**, etc. **NOTIFY** is
delivered to:

- **`notify_co_consumer`** / **`notify_cb_consumer`** → **`deliver_pg_notify`**, or
- Plain **`database`** → **`on_incoming_notify`** handler if set.

Cross-connection ordering: completing **`NOTIFY`** SQL on session A does **not** guarantee session B has already read *
*`NotificationResponse`** — drive B’s loop or await work on B before asserting delivery (see **`test-notify.cpp`** *
*`io_pump`** pattern).

---

## Errors

Failures surface as **`qb::pg::error::connection_error`** or generic **`db_error`** with message/SQLSTATE where
applicable. With **`run_sync(connect(...))`**, a **`false`** result means the awaiter completed without *
*`is_connected_`**.

See [error_handling.md](./error_handling.md).

---

## Related

- [transaction.md](./transaction.md) — **`set_timeout`** vs connect timeout
- [queries.md](./queries.md) — **`listen`/`notify`**
- [testing.md](./testing.md) — DSN env vars  
