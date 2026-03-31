# Transactions and command queues

**`qb::pg::tcp::database`** (and **`qb::pg::tcp::ssl::database`**) **is** the root **`qb::pg::detail::Transaction`**.
The same object owns the socket, protocol state, **prepared-statement LRU**, and the **queues** that **`execute`**, *
*`prepare`**, **`begin`**, **`listen`**, etc. extend.

**Public alias:** **`qb::pg::transaction`** — type used in callbacks.

**Include:** `#include <pgsql/pgsql.h>`.

---

## Implementation: two queues

From [`transaction.cpp`](../src/transaction.cpp) / [`transaction.h`](../src/transaction.h):

- **`_sub_commands`** — nested **`Transaction`** command objects (**`Begin`**, **`End`**, **`SavePoint`**, *
  *`EndSavePoint`**, **`Then`**, **`Error`**, **`Prepare`**, …) processed in FIFO order.
- **`_queries`** — **`ISqlQuery`** instances (**`BeginQuery`**, **`ResultQuery`**, **`CommitQuery`**, …) that actually
  talk to PostgreSQL.

**`Transaction::await()`** loops **`qb::io::async::run_once()`** until both **`_sub_commands`** and **`_queries`** are
empty, then builds a **`status`** snapshot.

**Callback path = ordered async:** each **`execute(..., cb, err)`** returns **`Transaction&`** immediately after *
*enqueueing**. Completion happens later when something runs **`run_once()`** (listener, VirtualCore, or **`await()`**).
You **do not** need **`await()`** on every call if the io loop keeps running.

---

## `Begin` / `End` (callback transaction blocks)

Defined in [`src/commands.h`](../src/commands.h).

1. **`begin(on_success, on_error, mode)`** pushes a **`Begin`** command, then an **`End`** command (see [
   `transaction.inl`](../src/transaction.inl) **`Transaction::begin`**).
2. **`Begin`** constructor queues **`BeginQuery`** (SQL **`BEGIN …`** plus optional **`SET LOCAL statement_timeout`**
   from [`get_timeout()`](../src/transaction.h)).
3. When **`BeginQuery`** completes successfully, the framework invokes **`on_success(*this)`** where **`this`** is the *
   *`Begin`** command object exposed as **`Transaction&`** — your lambda enqueues **`tr.execute`**, **`tr.savepoint`**,
   etc. onto **that** inner context.
4. When the **`Begin`** command object is **popped**, **`Begin::on_before_pop`** copies **`_result`** into **`End`** and
   calls **`End::on_end_transaction`**, which enqueues **`CommitQuery`** or **`RollbackQuery`** depending on **`_result`
   **.

So **COMMIT/ROLLBACK** for the callback style is **not** a separate `commit(cb, err)` API — it is **`End`**. Explicit *
*`co_await db.commit()`** exists only on the coroutine path ([`transaction_coro.inl`](../src/transaction_coro.inl)).

**`Begin` errors:** failures on **`BeginQuery`** route to **`on_error`** passed to **`begin`**. **`End`** uses its
stored **`on_error`** for commit/rollback failures.

**Exceptions in `on_success`:** if your **`begin`** body throws, **`Begin`** catches, sets **`_result = false`**, and
forwards a **`client_error`** to **`End`**’s error callback ([`commands.h`](../src/commands.h) **`Begin`** constructor).

---

## `Then` / `success` / `error`

Also in [`src/commands.h`](../src/commands.h).

- **`then(cb)`** / **`success(cb)`** (aliases) push a **`Then<CB>`** command. In **`~Then`**, if **`parent()->result()`
  ** is still success, **`cb(*(parent()))`** is invoked with the **same** **`Transaction&`** the **`Then`** was chained
  from (`this` in [`Transaction::then`](../src/transaction.inl)).
- **`error(cb)`** pushes **`Error<CB>`**; in **`~Error`**, if **`parent()->result()`** is failure, *
  *`cb(parent()->error())`** runs.

The lambdas run when these objects are **popped** during queue draining — i.e. **in order** relative to other *
*`_sub_commands`**, not “inline” after the C++ statement.

### Root `db` vs inner `tr` chaining

| Call site                                  | What `parent()` is for `Then`/`Error`                              | Typical meaning                                                                                                                                                                                                                                                        |
|:-------------------------------------------|:-------------------------------------------------------------------|:-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **`db.begin(...).then(f).error(g)`**       | Root **`database`**                                                | **`Then`/`Error` are enqueued **after** the **`Begin`/`End`** pair that **`begin`** just pushed. They run when that pair has been processed — usually **after** COMMIT or ROLLBACK of the block. Use for **follow-up work** that depends on the whole block finishing. |
| **`tr.then(f)` inside the `begin` lambda** | The **`Begin`** sub-transaction (the **`Transaction&`** parameter) | Chains relative to **that** subtree’s queue — use for **intra-block** sequencing before **`End`** runs.                                                                                                                                                                |

There is **no** separate `tr_next` type in the API: **`then`** passes **`*(parent())`** — the parent transaction
reference.

**Exceptions in `Then`/`Error` destructors:** caught; **`parent()->result(false)`** may be set if a grandparent
exists ([`commands.h`](../src/commands.h)).

---

## Optional `await()` and `status` snapshot

**`Transaction::await()`** ([`transaction.cpp`](../src/transaction.cpp) ~155–190):

- Clears **`results()`**, resets **`_error`** / **`_result`** to a neutral baseline, then drains with **`run_once()`**
  until queues are empty.
- **If the queues were already empty on entry** but a **failure was already recorded** (e.g. synchronous validation
  before any async step), **`await()`** restores that failure into **`status`** instead of reporting a false success.

**`status`** ([`transaction.h`](../src/transaction.h)):

- **`explicit operator bool()`** — true only when **`_command_ok`** and **`_error.sqlstate == unknown_code`**.
- **`results()`** / **`error()`** — last batch payload.

**`qb::pg::await(db)`** — thin forwarder to **`db.await()`**.

Use **`await()`** when you need a **blocking drain on the current thread** or a **`status`** object. Omit it when the *
*actor / listener** continuously calls **`run_once()`**.

---

## Starting a transaction (callbacks)

```cpp
db.begin(
    [](qb::pg::transaction& tr) {
        tr.execute("INSERT INTO t(v) VALUES (1)", qb::pg::discard_query, qb::pg::discard_error);
        tr.execute("UPDATE t SET v = 2 WHERE v = 1", qb::pg::discard_query, qb::pg::discard_error);
    },
    [](qb::pg::error::db_error const& err) {
        (void)err; // BEGIN failed or End error path
    },
    qb::pg::transaction_mode{});
```

**Optional synchronous drain:**

```cpp
auto st = db.begin(/* … */).await();
if (!static_cast<bool>(st)) { /* st.error() */ }
```

**Post-block chain on root (runs after `Begin`/`End` pair):**

```cpp
db.begin(/* … */, on_begin_err)
    .then([](qb::pg::transaction& tr) { (void)tr; /* next enqueue on root */ })
    .error([](qb::pg::error::db_error const& e) { (void)e; });
```

---

## Starting a transaction (coroutines)

```cpp
auto br = co_await db.begin();
if (!br.ok()) { co_return; }

auto er = co_await db.execute("INSERT INTO t(v) VALUES (1)");
if (!er.ok()) {
    (void)co_await db.rollback();
    co_return;
}

auto cr = co_await db.commit();
if (!cr.ok())
    (void)co_await db.rollback();
```

**`set_timeout`:** stored on the connection; next **`begin()`** (callback or coroutine) merges *
*`SET LOCAL statement_timeout`** into the **`BEGIN`** round-trip ([`BeginQuery`](../src/queries.h)).
See [connection.md](./connection.md) vs connect deadline.

---

## `set_timeout` / `get_timeout`

| Call                              | Effect                                                                          |
|:----------------------------------|:--------------------------------------------------------------------------------|
| **`set_timeout(n)`**, **`n > 0`** | Next **`begin()`** appends **`SET LOCAL statement_timeout`** after **`BEGIN`**. |
| **`set_timeout(0)`**              | Disables for subsequent begins.                                                 |
| **Scope**                         | Transaction-local on server until COMMIT/ROLLBACK.                              |
| **No `begin()`**                  | Autocommit **`execute`** does not inject it; use raw SQL if needed.             |

---

## `with_transaction` (coroutines only)

Implemented in [`src/coro_with_transaction.hpp`](../src/with_transaction.h): **`co_await begin`**, body **`task`
**, **`COMMIT`** or **`ROLLBACK`** on success / **`transaction_abort`** / exception / commit failure.

```cpp
auto r = co_await qb::pg::with_transaction(db, [](qb::pg::detail::Transaction& tr)
    -> qb::io::async::task<int> {
        auto q = co_await tr.query("SELECT 1");
        if (!q.ok())
            throw qb::pg::transaction_abort{q.error()};
        co_return 42;
    });
```

**Nesting:** avoid second **`BEGIN`** on same connection; use **`savepoint`** for nested units.

---

## Savepoints

**Callback — open block:**

```cpp
tr.savepoint("sp1",
    [](qb::pg::transaction& inner) {
        inner.execute("INSERT INTO t VALUES (1)", qb::pg::discard_query, qb::pg::discard_error);
    },
    [](qb::pg::error::db_error const&) {});
```

**Coroutine:** **`co_await tr.savepoint("name")`**, **`co_await tr.release_savepoint("name")`**, *
*`co_await tr.rollback_savepoint("name")`** ([`transaction_coro.inl`](../src/transaction_coro.inl)).

**API gap:** there are **no** callback overloads for **`release_savepoint` / `rollback_savepoint`** — only **`co_await`
**. Inside a callback transaction, issue **`ROLLBACK TO SAVEPOINT` / `RELEASE SAVEPOINT`** via **`execute`** if needed.

Semantics follow **`SavePoint` / `EndSavePoint`** in [`commands.h`](../src/commands.h); see *
*`test-transaction-advanced.cpp`** for edge cases.

---

## Isolation and modes

**`qb::pg::transaction_mode`** ([`common.h`](../src/common.h)): isolation, **`read_only`**, **`deferrable`**. Read-only
still allows temp table writes (PostgreSQL).

---

## `commit` / `rollback` (coroutines only)

**`co_await tr.commit()`** / **`co_await tr.rollback()`** → **`Reply<resultset>`**. No callback twins; use **`begin`**
’s **`End`** for implicit commit/rollback in callback style.

---

## Related documentation

- [queries.md](./queries.md) — **`execute`**, prepared SQL, NOTIFY/LISTEN
- [error_handling.md](./error_handling.md) — **`Reply`**, **`Error` command**, **`status`**
- [connection.md](./connection.md) — connect vs statement timeout  
