# A PostgreSQL actor

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qbm-pgsql @ qb 3.0.0 (C++20 default, C++23
> supported)

How a `qb::pg::tcp::database` lives inside a `qb::Actor`: who drives its I/O, how a handler awaits a query without
stopping the core, what a `kill()` does to a coroutine parked on a reply, and where the synchronous `run_sync` form is
still the right answer.

**Prerequisites:** [connection.md](./connection.md) (open a connection first) — **See also:
** [queries.md](./queries.md), [transaction.md](./transaction.md), [error_handling.md](./error_handling.md),
and, in the framework, [Writing actors](https://github.com/isndev/qb/blob/main/readme/4_qb_core/actor.md)
· [Asynchronous work inside an actor](https://github.com/isndev/qb/blob/main/readme/5_core_io_integration/async_in_actors.md)
· [C++20 coroutines](https://github.com/isndev/qb/blob/main/readme/3_qb_io/coroutines.md)

---

## Summary

A `database` is an ordinary qb-io object. It binds to the event loop of the thread that constructs it, and inside a
`qb::Main` that thread is a `VirtualCore`. So an actor that holds a `database` needs **no pump, no drain, no
`run_once`**: the core already runs its loop once per pass, before it dispatches your handlers, and that pass is what
carries bytes to and from PostgreSQL.

What the actor owes in return is one rule: **never stop returning to the loop.** `co_await` returns; `run_sync`,
`run_once`, `await()` and `cancel()` do not.

```cpp
#include <qb/actor.h>
#include <qb/main.h>
#include <qbm/pgsql/pgsql.h>
#include <memory>
#include <string>

struct LookupRequest : qb::Event {
    int user_id;
    explicit LookupRequest(int id) : user_id(id) {}
};
struct LookupResult : qb::Event {
    // qb::string<N>, not std::string: an event is relocated by memcpy, and a short
    // std::string on libstdc++ points into its own storage. See "Payloads" below.
    qb::string<64> name;   // empty when the row is missing or the query failed
    explicit LookupResult(std::string const &n) : name(n) {}
};

class UserStore : public qb::Actor {
    // shared_ptr, not a plain member: a coroutine that outlives the actor must be able to
    // keep the connection alive while it is parked on it. See "Awaiting a query" below.
    std::shared_ptr<qb::pg::tcp::database> _db;

public:
    qb::io::async::task<bool> onInit() override {
        registerEvent<LookupRequest>(*this);
        registerEvent<qb::KillEvent>(*this);

        _db = std::make_shared<qb::pg::tcp::database>();
        if (!co_await _db->connect("tcp://user:secret@localhost:5432[mydb]"))
            co_return false;                      // init fails; the actor never starts
        auto prep = co_await _db->prepare("by_id", "SELECT name FROM users WHERE id = $1;",
                                          qb::pg::type_oid_sequence{qb::pg::oid::int4});
        co_return prep.ok();
    }

    void on(LookupRequest const &ev) {
        auto              db     = _db;               // copy ALL of it BEFORE spawning
        const int         id     = ev.user_id;
        const qb::ActorId sender = ev.getSource();

        spawn([db, id, sender](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto        reply = co_await db->execute("by_id", qb::pg::params{id});
            std::string name;
            if (reply.ok() && !reply.result().empty())
                name = reply.result()[0][0].as<std::string>();
            ctx.push_to<LookupResult>(sender, std::move(name));   // safe: id copied by value
        });
    }

    void on(qb::KillEvent const &) {
        if (_db)
            _db->disconnect();   // fails every queued query, so no coroutine is left parked
        kill();
    }
};
```

Four decisions in that class carry the whole page, and each has a section below: the client is reached through a
`shared_ptr` the coroutine copies, the handshake happens in `onInit()`, the query runs under `spawn` rather than in the
handler, and the kill handler disconnects **before** it kills.

---

## Concepts

### Payloads

One framework rule shows through in the event definitions above: **an event payload must be trivially *relocatable*,
not merely copyable**, because the engine moves events with `memcpy` and never runs the source destructor. A by-value
`std::string` is not — on libstdc++ a short one addresses its own inline buffer — so a row value goes into a
`qb::string<N>` when it is bounded, or a `std::shared_ptr<std::string>` / `std::vector` when it is not.

The rule is **not** scoped to cross-core delivery: the source pipe `memcpy`s what it already holds when it grows,
and `reply`/`forward` byte-recycle the event, so a same-core `push` is exposed too. There is no compile-time check
and there cannot be one; the debug build scans for it on the cross-core hop only, so a clean debug run is evidence
rather than proof. libc++ recomputes `data()` from `this`, which is why this corrupts on Linux while passing every
macOS test. See [Inter-actor messaging](https://github.com/isndev/qb/blob/main/readme/4_qb_core/messaging.md).

### Who drives the connection

`qb::Main` gives each `VirtualCore` one thread and one `qb::io::async::listener`. Every pass of that core's loop runs
the listener first and dispatches actor events afterwards, so a `database` constructed on that core is serviced by the
same crank that delivers your messages. The framework
owns [that ordering](https://github.com/isndev/qb/blob/main/readme/5_core_io_integration/async_in_actors.md#the-two-call-chains);
what it means here is narrow and worth stating plainly:

| Where the `database` lives | What drives its I/O | What you must call |
|:---|:---|:---|
| A member of an actor, or created in `onInit()` | the owning `VirtualCore`'s loop pass | nothing |
| A local in `main()`, before `qb::Main::start()` | nothing yet — there is no loop running | `qb::io::async::run_sync(...)` |
| A local in a test fixture or a CLI | nothing yet | `qb::io::async::run_sync(...)`, or callbacks + `await()` |

The middle and bottom rows are why `run_sync` exists and why the rest of these pages use it: they describe code whose
thread is the caller's to block. The top row is the one this page is about.

Construct the client on the actor's own thread. An actor's constructor already runs there — the engine builds it on the
worker core, not where you called `addActor` — so a `database` member, a `unique_ptr` filled in the constructor, and one
created inside `onInit()` are all equally correct. Handing an actor a `database` built on another thread is not:
`Transaction` deletes its copy *and* move constructors, so the type will not let you, and the qb-io object underneath is
bound to the wrong loop anyway.
<!-- src: qbm/pgsql/src/qbm/pgsql/transaction.h:83-89 (copy/move deleted) -->

### `onInit()` is where you connect

`onInit()` is a coroutine, so the handshake reads as a straight line: connect, prepare, `co_return true`. While it is
suspended the actor is **Activating** — the engine holds its inbound business events and replays them once init
succeeds — so nothing is served against a half-open connection. `co_return false` (or an uncaught exception) fails the
init and the actor is destroyed without ever handling a message, which is the behaviour you want when the database is
unreachable at startup.

This is the "discover before activate" shape both shipped
examples use (`examples/07-applications/01-taskmanager/src/actors/task_manager.cpp:33-73`): connect PostgreSQL, prepare statements,
connect Redis, only then compile the HTTP routes.

A `co_await` inside `onInit()` is already cancellation-aware for the framework's own operations, because the actor's
`context()` carries its cancellation scope. It is **not** for a `co_await _db.connect(...)`, for the reason the next
section gives. What bounds the handshake instead is its own deadline: `connect(qb::duration)` and the
`connect_timeout` option both fail the awaiter rather than leaving it parked (see [connection.md](./connection.md)).

### Awaiting a query from a handler

An event handler must return. To do work that suspends, hand it to `Actor::spawn`, which launches an isolated coroutine
on this core's scheduler and returns immediately. The framework
owns [the rules that come with it](https://github.com/isndev/qb/blob/main/readme/5_core_io_integration/async_in_actors.md#coroutines-from-a-handler-spawn-and-spawn_detached);
the module-specific consequence is the awkward one, so it gets stated here:

> **A spawned coroutine may not touch the actor's `database` after a `co_await`.** The actor — and therefore the client
> object that is its member — may have been destroyed while the coroutine was parked. Capturing `this`, `&_db`, or any
> member reference is undefined behaviour the moment the first suspension returns.

That is the whole reason the class above holds `std::shared_ptr<qb::pg::tcp::database>` rather than a plain member. The
handler copies the `shared_ptr` before spawning; the coroutine's frame then owns a reference, so the client stays alive
for exactly as long as something is parked on it, whatever happens to the actor. Everything else it needs is copied the
same way — the id and the requester's `ActorId` — and after the `co_await` it speaks only through `ctx`. An event
addressed to an actor that has since died finds no handler and is disposed, rather than going anywhere bad.

A plain `qb::pg::tcp::database` member is not *wrong*; it is correct exactly when no coroutine can still be parked on it
when the actor is destroyed, which you can only guarantee by disconnecting first (below) and never spawning anything
long-lived. The `shared_ptr` costs one atomic per request and removes the reasoning.

Note what you cannot do instead: `database` deletes both its copy and its move constructor, so you cannot capture one by
value, and there is no handle type to capture in its place.

**`Actor::spawn`, not `coro_scheduler().spawn`.** Both launch a coroutine on this core's scheduler and both work. The
difference is ownership: `Actor::spawn` joins the actor's cancellation scope, so `kill()` signals it and
`has_active_coroutines()` counts it, and it hands your callable a `ScopedCoroContext` with the cancellation-aware
operations and the `push_to` / `broadcast` surface. `qb::io::async::coro_scheduler().spawn(...)` gives you a coroutine
tied to the thread and to nothing else — deliberate fire-and-forget, with no context and no way to be told the actor
died. Reach for it only when that is what you mean; `Actor::spawn_detached` is the same intent spelled inside the actor
API, and it at least still gives you a `CoroContext`.

The `disconnect()` in the kill handler is not tidiness. It is the mechanism that ends any coroutine still parked on this
connection — see the next section.

---

## What cancellation does to a parked query, and what does not

`Actor::kill()` cancels the actor's coroutine scope. Whether that reaches a parked coroutine is a property of the
awaiter it is parked on, and
[C++20 coroutines](https://github.com/isndev/qb/blob/main/readme/3_qb_io/coroutines.md#every-awaitable-and-what-cancellation-does-to-it)
owns the full inventory. The entry for this module is one line:

> **`pg_reply_awaiter` is not cancellation-aware.** It registers no `on_cancel` hook and consults no token, so
> `cancel()` — and therefore `kill()` — neither wakes nor unwinds a coroutine parked on `co_await db.query(...)`,
> `execute`, `prepare`, `begin`, `commit`, or `connect`.
> <!-- src: qbm/pgsql/src/qbm/pgsql/pg_awaiter.h:94-119 (await_ready false; await_suspend stores the handle and launches the operation — no token, no hook) -->

The awaiter is a callback bridge and nothing more: `await_ready()` returns `false`, the handle is stored, the operation
is launched with a completion lambda, and resumption goes through `coro_scheduler().schedule_resume`. What it *does*
carry is a `shared_ptr<bool> alive` cleared in its destructor, so a completion that arrives after the awaiter is gone is
a silent no-op instead of a use-after-free.
<!-- src: qbm/pgsql/src/qbm/pgsql/pg_awaiter.h:80-83 (destructor clears alive), :111-113 (the completion checks it first) -->

So a parked `co_await` on this module ends in exactly four ways:

| What happens | What the coroutine sees |
|:---|:---|
| The server answers | `Reply<T>` with `ok()` true or false — the normal path |
| The link drops (server-side kill, network loss, `disconnect()`) | `Reply<T>` with `ok() == false`: `on(disconnected)` calls `fail_all_pending`, which drains every queued query so no caller is left parked |
| Someone destroys the coroutine frame | nothing — the frame is gone, `~pg_awaiter` clears `alive`, and the late reply is dropped |
| Nothing else | it stays parked |

The second row is the one to build on, and it is measured rather than asserted. The integration case
`ServerSideBackendTerminationFailsTheInFlightQuery` kills the backend from a second session while a coroutine is parked
on `SELECT pg_sleep(30)`, and requires the `co_await` to resume with a failed reply — "a hang here is the actual hazard,
since the caller has no other way to learn the socket is gone". It then requires `is_connected()` to go false, so the
next query fails fast instead of enqueuing work on a dead socket.
<!-- src: qbm/pgsql/tests/integration/resilience/coro-cancel-and-connection-loss.cpp:243-246 -->

The third row is measured too, and its subtlety is worth the paragraph. qb's coroutine frames come from a pooled
freelist, so destroying a parked frame returns the block for the *next* spawn to reuse; an un-retracted completion would
then resume somebody else's coroutine. `CoroutineDestroyedWhileParkedOnLiveQuery` arms exactly that — it destroys a frame parked on `pg_sleep(2)`, spawns a second coroutine into the same block, and requires it not to
wake six seconds early — and it also checks the connection afterwards, which is the fact you need: **abandoning a reply
does not desync the stream.** The orphaned reply is consumed and dropped, and the next `co_await` on that connection
gets its own.
<!-- src: qbm/pgsql/tests/integration/resilience/coro-cancel-and-connection-loss.cpp:170-226 -->

### Making a query interruptible

Because the awaiter cannot be woken, an interruptible query is one whose *frame* someone else can destroy. That is what
`ScopedCoroContext::cancellable` does — but it takes a `qb::io::async::task<T>`, not an awaiter, so the query has to be
inside a coroutine first. Wrap it in a named free function; do not use an immediately-invoked lambda, whose closure is
already destroyed by the time the body runs:

```cpp
#include <qb/actor.h>
#include <qbm/pgsql/pgsql.h>
#include <memory>
#include <string>

// A named coroutine, so its parameters live in the frame.
qb::io::async::task<qb::pg::Reply<qb::pg::results>>
run_lookup(std::shared_ptr<qb::pg::tcp::database> db, int id) {
    co_return co_await db->execute("by_id", qb::pg::params{id});
}

// ... inside the handler:
spawn([db, id, sender](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
    try {
        auto reply = co_await ctx.cancellable(run_lookup(db, id));
        ctx.push_to<LookupResult>(sender, reply.ok() && !reply.result().empty()
                                              ? reply.result()[0][0].as<std::string>()
                                              : std::string{});
    } catch (qb::io::async::cancelled_error const &) {
        // The actor was killed while the query was in flight. Nothing to answer.
    }
});
```

On `kill()`, `cancellable`'s `on_cancel` hook fires, destroys the inner frame — which destroys the `pg_awaiter` and
retracts its `alive` flag — and resumes this coroutine with `cancelled_error`. `spawn`'s own wrapper swallows a
`cancelled_error` that escapes a scoped coroutine, so the `try`/`catch` is only needed when you have cleanup of your
own.

Two things this **does not** do, and both matter:

- **It does not stop the query.** PostgreSQL keeps executing the statement to completion; only your coroutine stopped
  waiting. To bound the *server's* work, set a statement timeout with `Transaction::set_timeout(qb::duration)` before
  `begin()`, or fire `cancel()` from off the loop — see [transaction.md](./transaction.md)
  and [connection.md](./connection.md#cancelling-a-running-query).
- **It does not free the connection sooner.** The abandoned statement still occupies the single serial stream until the
  server is done with it, so the next `co_await` on the same `database` queues behind it.

For a deadline rather than a kill, `qb::io::async::with_deadline(run_lookup(db, id), deadline)` has the same shape and
the same two caveats, and throws `timeout_error`. Both are documented, with their teardown order, on
[C++20 coroutines](https://github.com/isndev/qb/blob/main/readme/3_qb_io/coroutines.md#cancellation).

### Do not wrap `with_transaction`

The wrapper works by **destroying** the inner coroutine frame. Destroying a suspended coroutine runs the destructors of
its live locals and nothing else: no `catch` block, no statement after the suspension point. `with_transaction` issues
its `ROLLBACK` from exactly those places — a `catch (transaction_abort)`, a `catch (...)`, and the tail after a failed
`commit`.
<!-- src: qbm/pgsql/src/qbm/pgsql/with_transaction.h:115-130 (the two catch arms and the rollback tail) -->

So a `co_await ctx.cancellable(with_transaction(db, body))` that is cancelled mid-body leaves **`BEGIN` issued and
never ended**. The connection stays inside an open transaction block, and every later statement on it runs in that
abandoned transaction.

The un-wrapped form is the safe one here, for the same reason it was the awkward one above: because the awaiter is not
cancellation-aware, `kill()` does not touch it. The block runs to its own `COMMIT` or `ROLLBACK`, and the result event
is pushed to an actor that may no longer exist, where it is disposed. To bound a transaction, bound it *inside* the
database instead:

- `Transaction::set_timeout(qb::duration)` before `begin()` — a `SET LOCAL statement_timeout`, so PostgreSQL aborts the
  statement and the block, and your `co_await` resumes with a `db_error`.
- `disconnect()` — dropping the session rolls the open transaction back server-side, which is the second thing the
  resilience suite measures: after the backend is terminated mid-transaction, the uncommitted row is invisible from a
  third connection.
  <!-- src: qbm/pgsql/tests/integration/resilience/coro-cancel-and-connection-loss.cpp:298-304 -->

---

## Callbacks inside an actor

The callback overloads are the other half of the module's API, and inside an actor they are the *simplest* half: they
enqueue and return, the core's next loop pass carries the bytes, and your callback runs from that pass. Nothing needs
draining.

```cpp
void on(RefreshRequest const &) {
    _db->execute("REFRESH MATERIALIZED VIEW leaderboard;",
                 [this](qb::pg::transaction &, qb::pg::results) {
                     // Runs on this core's loop, on this actor's thread.
                     _stale = false;
                 },
                 [](qb::pg::error::db_error const &err) {
                     qb::io::cerr() << "refresh failed: " << err.what() << '\n';
                 });
}
```

The two arities are fixed and checked at compile time: the success handler is `(Transaction&, results)` or
`(Transaction&)` alone, and the error handler takes `(error::db_error const&)` — one argument, no transaction. The
shipped no-ops `qb::pg::discard_query` and `qb::pg::discard_error` have exactly those signatures and are the right
placeholder when you read the side effects elsewhere.
<!-- src: qbm/pgsql/src/qbm/pgsql/pgsql.h:2691-2699 (discard_query_results_t; discard_error_t) -->

`this` is safe in a callback in a way it is not in a coroutine, but only because of a difference worth naming: the
callback is invoked from the reply path of a connection the actor owns, and the actor's `KillEvent` handler
disconnects that connection, which fails every queued handler before the actor is destroyed. Take that ordering away —
by killing without disconnecting — and the guarantee goes with it.

**Do not drain from a handler.** `Transaction::await()` and `qb::io::async::run_once()` are for a thread you own. Inside
an actor they stop the `VirtualCore`: `await()` spins the loop until the reply queue empties and `run_once()` blocks
until at least one event arrives, and in both cases this core dispatches no further actor events, ticks no
`ICallback`, and reaps nothing until they return. There is no diagnostic — the guard those calls carry only fires
inside the coroutine scheduler's ready-drain, which an actor handler is not running under.
See [The async runtime](https://github.com/isndev/qb/blob/main/readme/3_qb_io/async_system.md#the-guard-and-what-it-actually-checks)
for the mechanism.

---

## Two calls that block, inside a client that does not

Every operation on this module is non-blocking except two, and both are documented where they live. They are collected
here because an actor is where the cost lands.

- **`cancel()`** opens a second socket with a *blocking* connect and send, capped at `min(connect_timeout, 2 s)`. Firing
  it from a timer on the core's own loop — the natural place — parks the `VirtualCore` for that duration. Run it from a
  thread that is not a core, or accept the stall and keep `connect_timeout` small.
  See [connection.md](./connection.md#cancelling-a-running-query).
- **`disconnect()`** runs the loop once (`EVRUN_NOWAIT`) after tearing the socket down, so the close is observed. That
  is a single non-blocking pass rather than a pump, and it is deliberately not `async::run()` so it stays legal from a
  coroutine — but it is still a re-entrant turn of the loop from inside your handler. Prefer calling it from the
  `KillEvent` handler, where nothing runs after it anyway.
  <!-- src: qbm/pgsql/src/qbm/pgsql/pgsql.h:2512-2531 (disconnect: fail_all_pending, then one EVRUN_NOWAIT pass) -->

---

## Bridging to synchronous code

`qb::io::async::run_sync(awaitable)` drives one awaitable to completion by pumping the current thread's loop. It is the
correct tool wherever **the thread it blocks is yours**: a `main()` before `qb::Main::start()`, a test fixture, a
migration CLI, a one-shot script. Every `run_sync` in the rest of these pages is one of those.

The framework's own best statement of the case is a comment in a shipped example:

> *Pre-engine setup: there is no actor loop yet, so we drive a coroutine to completion synchronously with
> `qb::io::async::run_sync`.*
> — `examples/07-applications/02-auction-house/src/main.cpp:45-46`

```cpp
// main(), before the engine starts: applying a schema once.
int main() {
    qb::io::async::init();
    qb::pg::tcp::database db;
    if (!qb::io::async::run_sync(db.connect("tcp://user:secret@localhost:5432[mydb]")))
        return 1;
    auto r = qb::io::async::run_sync(db.execute_file("schema.sql"));
    if (!r.ok()) {
        qb::io::cerr() << r.error().what() << '\n';
        return 1;
    }
    db.disconnect();

    qb::Main engine;
    engine.addActor<UserStore>(0);
    engine.start();
    engine.join();
    return 0;
}
```

Inside an actor the same call is a defect, and a quiet one. The thread it blocks is the `VirtualCore`: until the
awaitable resolves, this core dispatches no events, so every other actor on it stops. What keeps it hard to notice is
that the pump *does* keep turning the loop — sockets stay serviced, timers fire, other coroutines resume — so the
connection still answers and only actor latency moves.
[Asynchronous work inside an actor](https://github.com/isndev/qb/blob/main/readme/5_core_io_integration/async_in_actors.md#run_sync--the-stack-stays-and-step-6-never-finishes)
owns that mechanism and the two annotated call chains that make it concrete.

---

## Shutting down

Order matters, because the coroutines outlive the handler that started them:

1. **`disconnect()` first.** It fails every queued and in-flight query, which resumes each parked `co_await` with a
   failed `Reply<T>`. Coroutines that were waiting now finish on the next pass instead of hanging.
2. **`kill()` second.** It cancels the actor's scope, which reaches anything parked on a framework awaiter or inside a
   `ctx.cancellable(...)` wrapper.
3. **The destructor runs later**, at the core's reap. `has_active_coroutines()` reports whether anything is still
   outstanding if you want to look before deciding.

Skipping step 1 is the common mistake: `kill()` alone does not touch a bare `co_await db.query(...)`, so the coroutine
stays parked until the statement finishes on its own, then resumes into a world where its actor is gone. That is safe —
the context holds an `ActorId` by value and the event it pushes finds no handler — but it is not prompt, and during a
shutdown it is the difference between exiting now and exiting when the slowest query does.

---

## Pitfalls

- **Calling `run_sync`, `run_once`, or `await()` from a handler.** They stop the `VirtualCore` and nothing reports it.
  `co_await` inside `spawn` is the form that returns.
- **Capturing `this` or `&_db` into a `spawn` body.** The actor may be destroyed while the coroutine is parked. Copy a
  `shared_ptr` and the plain values you need, before the first `co_await`.
- **Expecting `kill()` to interrupt a query.** It does not: the awaiter registers no hook. Wrap the query in
  `ctx.cancellable(...)`, or rely on `disconnect()` to fail it.
- **Expecting `ctx.cancellable(...)` to stop the *server*.** It abandons your wait, not PostgreSQL's work. Use
  `set_timeout` for a statement timeout, or `cancel()` for an out-of-band abort.
- **Passing an awaiter to `ctx.cancellable` / `with_deadline`.** Both take a `qb::io::async::task<T>`. Wrap the
  `co_await` in a named coroutine first — never an immediately-invoked lambda, whose closure dies before the body runs.
- **Sharing one `database` between actors.** A client is one I/O thread and one serial wire stream; it is not
  thread-safe and it cannot be moved. Give each actor its own, or put the database behind a single actor and send it
  events.
- **Firing `cancel()` from a timer on the core.** It blocks the loop for up to `min(connect_timeout, 2 s)`.
- **Killing before disconnecting.** Parked queries then finish on the server's schedule rather than yours.

---

## See also

- [connection.md](./connection.md) — DSN, handshake, TLS, keepalive, `cancel()`, `disconnect()` / `prepare_reconnect()`
- [queries.md](./queries.md) — the coroutine and callback overload of every SQL-facing operation
- [transaction.md](./transaction.md) — `with_transaction`, savepoints, `set_timeout`
- [error_handling.md](./error_handling.md) — `Reply<T>`, `db_error`, SQLSTATE
- [Writing actors](https://github.com/isndev/qb/blob/main/readme/4_qb_core/actor.md) — lifecycle, `onInit`, `kill()`,
  the reap
- [Asynchronous work inside an actor](https://github.com/isndev/qb/blob/main/readme/5_core_io_integration/async_in_actors.md)
  — `spawn`, `defer`, `callback`, and the two call chains
- [C++20 coroutines](https://github.com/isndev/qb/blob/main/readme/3_qb_io/coroutines.md) — every awaitable and what
  cancellation does to it
