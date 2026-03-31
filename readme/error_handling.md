# Error handling

**`qbm-pgsql`** combines **explicit error values** (**`Reply<T>`**, callback **`on_err`**, **`Transaction::status`**)
with **C++ exceptions** for **row/column decoding** and for **`with_transaction`** control flow (**`transaction_abort`**
is caught; other exceptions propagate after **`ROLLBACK`**).

**Include:** `#include <pgsql/pgsql.h>` — **`db_error`**, **`Reply`**, **`transaction_abort`**, **`sqlstate`**.

---

## Layers (choose the right one)

| Layer                      | Mechanism                                                    | When                                                                                                                                                       |
|:---------------------------|:-------------------------------------------------------------|:-----------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Coroutine operation**    | **`Reply<T>`** — **`ok()`**, **`error()`**, **`result()`** | **`co_await db.execute`**, **`co_await prepare`**, **`co_await begin`**, …                                                                                 |
| **Per-operation callback** | **`on_err`** on **`execute`**, **`prepare`**, **`begin`**, … | Immediate SQL / protocol failure for that command                                                                                                          |
| **Fluent `Error` command** | **`db.error(cb)`** after **`execute`/`begin`/…**             | Runs in **`~Error`** when **`parent()->result()`** is false — receives **`parent()->error()`** ([`src/commands.h`](../src/commands.h) **`Error::~Error`**) |
| **Optional batch drain**   | **`Transaction::await()`** → **`status`**                    | **`explicit operator bool`**, **`status.error()`**, **`status.results()`** — optional if **`run_once`** drains the connection                              |
| **Scoped transaction**     | **`throw transaction_abort{db_error}`**                      | Inside **`with_transaction`**                                                                                                                              |
| **Field access**           | **`value_is_null`**, **`field_type_mismatch`**, …            | **`field.as<T>()`**                                                                                                                                        |
| **User callback bug**      | **`client_error`**                                           | Exception from **`begin`** body or **`Then`** / **`Error`** destructor paths ([`commands.h`](../src/commands.h))                                           |

### Decision tree (callbacks)

1. **Single statement:** use **`execute(..., on_ok, on_err)`** — **`on_err`** is enough.
2. **Several statements, shared failure logging:** enqueue them, then **`.error([](db_error const& e){ … })`** on the
   same **`Transaction&`** so one handler sees the accumulated failure state when the **`Error`** node runs.
3. **Need last **`results`** / global **`status`:** call **`.await()`** once at the end of the chain (or rely on *
   *`run_once`** + side effects in callbacks).

---

## `Reply<T>` (coroutines)

**`src/pg_reply.h`**

```cpp
auto r = co_await db.execute("SELECT …");
if (!r.ok()) {
    qb::pg::error::db_error const& e = r.error();
    // e.what(), e.code, e.sqlstate, …
    co_return;
}
qb::pg::results rows = std::move(r).result();
```

- **`void`** specialization for operations with no payload (e.g. **`co_await db.listen("ch")`**).
- **`operator bool`** and **`ok()`** are equivalent success tests.

**Do not** call **`result()`** on failure without checking **`ok()`** — the value is default-constructed / stale.

---

## `transaction_abort`

**`src/pg_reply.h`**

```cpp
auto ins = co_await tr.execute("INSERT …");
if (!ins.ok())
    throw qb::pg::transaction_abort{ins.error()};
```

**`with_transaction`** catches **`transaction_abort`**, issues **`ROLLBACK`**, returns **`Reply<T>::failure`**
carrying **`err`**. It does **not** rethrow **`transaction_abort`**.

**Other exceptions** after **`begin`**: **`ROLLBACK`**, then **rethrow** — your outer coroutine or **`run_sync`** must
handle them.

---

## `db_error` hierarchy

**`src/error.h`**, **`src/error.cpp`**

| Type                                                                | Role                                                                                                 |
|:--------------------------------------------------------------------|:-----------------------------------------------------------------------------------------------------|
| **`db_error`**                                                      | Base: **`what()`**, **`severity`**, **`code`**, **`detail`**, **`sqlstate`** (**`sqlstate::code`**). |
| **`connection_error`**                                              | Connect / transport / handshake / timeout waiting for **`ReadyForQuery`**.                           |
| **`query_error`**                                                   | Server-reported SQL or protocol errors during a command.                                             |
| **`transaction_closed`**                                            | Operation on a finished transaction (**`query_error`** subclass).                                    |
| **`client_error`**                                                  | User-provided callback threw — see message / nested cause.                                           |
| **`value_is_null`**, **`field_is_null`**, **`field_type_mismatch`** | Result extraction — **not** server errors.                                                           |

---

## Callback error handler

```cpp
db.execute("SELECT * FROM missing_table",
    [](auto&, qb::pg::results) {},
    [](qb::pg::error::db_error const& err) {
        if (err.sqlstate == qb::pg::sqlstate::undefined_table) {
            // handle missing relation
        }
    });
```

Prefer **`sqlstate`** enum comparisons over raw five-character strings when an enumerator exists (**`src/sqlstates.h`
**).

---

## `Transaction::await()` → `status`

**`status`** is **true** only when the drained batch is **clean**: **`_command_ok`** and SQLSTATE still **`unknown_code`
** (**`transaction.h`**).

```cpp
auto st = db.execute("…", …, …).await();
if (!static_cast<bool>(st)) {
    auto& e = st.error();
    (void)e;
}
```

Use **`static_cast<bool>(st)`** when assigning to a **`bool`** variable — **`explicit operator bool`**.

**Snapshot behaviour:** If the transaction already recorded failure before **`await()`**, **`await()`** surfaces that
failure (no silent success).

---

## SQLSTATE

**`src/sqlstates.h`**, **`src/sqlstates.cpp`**,
PostgreSQL [errcodes appendix](https://www.postgresql.org/docs/current/errcodes-appendix.html).

Examples you will see in practice: **`undefined_table`**, **`unique_violation`**, **`query_canceled`** (statement
timeout), **`serialization_failure`**.

---

## Statement timeout errors

With **`set_timeout`** + **`begin()`**, long-running statements fail with a normal **`db_error`** (message / SQLSTATE
often indicate cancel / timeout). Handle like any other **`!Reply::ok()`** path — no separate exception type.

---

## Coroutine + `run_sync`

If **`run_sync`** wraps a **`task`** that **`co_await`**s PostgreSQL:

- **`Reply` failure** is **not** an exception unless **you** **`throw`**.
- **Uncaught exceptions** from the coroutine propagate out of **`run_sync`** (implementation-dependent details — treat
  like any **`task`** boundary).

---

## Related

- [transaction.md](./transaction.md) — **`await()`**, **`with_transaction`**, **`status`**
- [queries.md](./queries.md) — discards, NOTIFY errors
- [results.md](./results.md) — **`as<T>()`** exceptions  
