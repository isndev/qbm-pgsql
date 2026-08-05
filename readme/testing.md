# Integration testing

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qbm-pgsql @ qb 3.0.0 (C++20 default, C++23
> supported)

Build and run the qbm-pgsql test suite under CTest: pure-unit suites run anywhere, while the integration suites need a
reachable PostgreSQL server and skip themselves cleanly when one is absent.

**Prerequisites:** [`../README.md`](../README.md) for the build matrix and `qb_load_modules` wiring; working knowledge
of CTest. — **See also:** [connection.md](./connection.md) (DSN, TLS,
reconnect), [transaction.md](./transaction.md), [queries.md](./queries.md) (LISTEN/NOTIFY).

## Summary

The test tree lives in [`../tests/`](../tests/) and builds one GoogleTest executable per suite. The suites are
executable specification: when this documentation and the code disagree, prefer the test and file a doc bug. Two facts
govern how you run them:

- **Tests are opt-in at configure time.** Nothing under `tests/` is built unless `QB_BUILD_TESTS` is on. The framework
  option defaults to `ON` (<!-- src: qb/cmake/qbConfig.cmake:85 -->), and the qb-dev super-project forces it
  on at its own root `CMakeLists.txt:14` — named in prose rather than as a `src:` citation, because a
  bare `CMakeLists.txt` token resolves against THIS module and would silently range-check
  `qbm/pgsql/CMakeLists.txt` instead — so a default build already produces the binaries.
- **Integration suites need a live server.** Eighteen unit suites have no socket, and two system suites
  (`connect-timeout`, `scram-mitm-refuse`) run with no daemon; the integration suites connect to
  PostgreSQL; each gates its fixture on a successful connect and calls `GTEST_SKIP()` when the server is unreachable, so
  the suite passes (as skipped) rather than failing on a machine with no database.

## Concepts

### Unit suites versus integration suites

The split is structural by tier: `unit/` never opens a socket, `system/` exercises the event loop with no
daemon (a connect-to-dead-host timeout), and `integration/` needs a live server.

| Tier        | Suites                                                                                                                                                                                                                                                                              | Server required |
|-------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------|
| Unit        | `unserializer-primitives`, `typeconverter-codecs`, `result-format-routing`, `param-serializer-encode`, `typeconverter-{scalar,numeric,temporal,array,json,adversarial}`, `datastructures`, `oid-stream`, `protocol-message-codec`, `identifier-quoting`, `dsn-parse`, `scram-and-cancel`, `prepared-storage-lru`, `module-surface` | No              |
| System      | `connect-timeout` (connects to a dead host — no daemon, network-timing dependent), `scram-mitm-refuse` (SCRAM mutual-auth refusal, no daemon)                                                                                                                                      | No              |
| Integration | `connection-lifecycle`, `queries`, `prepared-statements`, `transaction-basic`, `transaction-advanced`, `datatypes-roundtrip`, `wire-formats`, `listen-notify`, `coro-api`, `errors-sqlstate`, `database-api-extra`, `param-roundtrip`, and `connection-ssl` (TLS only)              | Yes             |

The unit suites exercise wire-format encoding and decoding, parameter serialization, type conversion, and
protocol-message framing in isolation — they pass on any host. The integration suites drive a real wire
handshake, prepared statements, transactions, type round-trips, and asynchronous NOTIFY delivery against a server.

<!-- src: qbm/pgsql/tests/CMakeLists.txt:60-113 -->

### How a missing server is handled

Each integration fixture connects in `SetUp()` and skips when the connect fails. There is no global "is the database
up?" probe; the skip is per fixture.

```cpp
// src: qbm/pgsql/tests/shared/pg_integration_fixture.hpp:62-67
void SetUp() override {
    db_ = std::make_unique<qb::pg::tcp::database>();
    if (!pg_try_connect(*db_))
        GTEST_SKIP() << kDaemonUnreachableSentinel << " (postgres at " << dsn_tcp_string() << " not reachable)";
}
```

Because the skip lives in the fixture, every test in that suite reports as *skipped* (not failed) when the server is
down. CTest still marks the suite *passed*. A green CTest run on a host with no database therefore proves only that the
unit suites and the build are sound — it does not prove the integration paths.

### The resource lock

The integration suites share one database at `localhost:5432`, so running them in parallel would let two jobs create and
drop the same temp objects at once. The build serializes them with a CTest resource lock:

```cmake
# src: qbm/pgsql/tests/CMakeLists.txt:39-48
# Every integration suite is registered through qpg_itest, which forwards the lock:
function(qpg_itest NAME RELPATH)
    qb_register_module_test(
            MODULE_NAME pgsql TIER integration TEST_NAME ${NAME}
            # ...
            RESOURCE_LOCK qb_pgsql_integration)
endfunction()
# qb_register_module_test then applies it via
# set_tests_properties(... PROPERTIES RESOURCE_LOCK ...) (src: qb/cmake/qbFunctions.cmake:565-567).
```

`connection-ssl` joins the same lock when it is built. CTest will not run two `qb_pgsql_integration` holders
concurrently, so `ctest -j` stays safe for the module. The lock does not coordinate across separate `ctest` invocations
or across machines — point each runner at its own database, or run them sequentially.

### Test names and binaries

`qb_register_module_test` names each CTest entry and binary
`qbm-pgsql-test-<tier>-<name>` — for example `qbm-pgsql-test-integration-connection-lifecycle`
(<!-- src: qb/cmake/qbFunctions.cmake:1002-1003, qb/cmake/qbFunctions.cmake:1029-1032 -->) — and places the executable in
`${CMAKE_BINARY_DIR}/bin/tests` with that directory as its working
directory (<!-- src: qb/cmake/qbFunctions.cmake:511-514, qb/cmake/qbFunctions.cmake:553-555 -->). Each test carries `tier:<tier>` and
`module:qbm-pgsql` CTest labels plus a per-tier timeout (unit 60 s, integration 300 s)
(<!-- src: qb/cmake/qbFunctions.cmake:494-495 -->). Each binary links `GTest::gtest_main`, so it
accepts the usual `--gtest_filter`, `--gtest_list_tests`, and `--gtest_repeat` flags.

`connection-ssl` is the one conditional suite: it is registered inside an `if (QB_HAS_SSL)` guard, only when
`QB_HAS_SSL` is set, because it links the `qb::pg::tcp::ssl::database` alias that exists only with
OpenSSL (<!-- src: qbm/pgsql/tests/CMakeLists.txt:104-106 -->).

## Configuring the server

The suites read their connection strings from the environment via [`test_config.hpp`](../tests/shared/test_config.hpp), which
is the single source of truth for the defaults. Set these before running CTest to point every suite at your server.

| Variable            | Role                                                                                                                                                                         | Default                                                   |
|---------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------|
| `QB_PG_DSN`         | Primary DSN for `qb::pg::tcp::database`.                                                                                                                                     | `tcp://test:test@localhost:5432[test]`                    |
| `QB_PG_SSL_DSN`     | DSN for `qb::pg::tcp::ssl::database` (only read when `QB_HAS_SSL`). `tcp://` is valid — the client sends a PostgreSQL `SSLRequest` and upgrades when the server answers `S`. | same as `QB_PG_DSN`                                       |
| `QB_PG_INVALID_DSN` | DSN that must *fail* authentication; used by negative connection tests.                                                                                                      | `tcp://test:__qb_invalid_password__@localhost:5432[test]` |

<!-- src: qbm/pgsql/tests/shared/test_config.hpp:33-53 -->

The default fixture is a role `test` with password `test` owning a database `test`. Create that, or override
`QB_PG_DSN`, before expecting the integration suites to do anything but skip.

The DSN grammar (`tcp://user:password@host:port[database]`, plus the TLS alias) is documented
in [connection.md](./connection.md); the test config only wraps it.

### The invalid-auth case

`ConnectionLifecycle.ConnectWithInvalidCredentials` connects with `QB_PG_INVALID_DSN` and asserts the attempt is
*rejected*. If your `pg_hba.conf` uses `trust` for local connections, the wrong password is accepted anyway; the test
detects this and skips rather than fail, but only when you have not set `QB_PG_INVALID_DSN` yourself:

```cpp
// src: qbm/pgsql/tests/integration/connection/connection-lifecycle.cpp:104-113
const bool connected = qb::io::async::run_sync(invalid_db->connect(qb::pg::test::dsn_invalid_auth_string()));

if (connected) {
    if (std::getenv("QB_PG_INVALID_DSN") == nullptr) {
        GTEST_SKIP() << "Server accepted the default wrong-password DSN (likely `trust` "
                        "in pg_hba). Set QB_PG_INVALID_DSN to a DSN that must fail auth.";
    }
    FAIL() << "QB_PG_INVALID_DSN was accepted; it must fail authentication.";
}
ASSERT_FALSE(connected);
```

To exercise the negative path under a `trust` policy, set `QB_PG_INVALID_DSN` to an endpoint that still verifies
passwords (a `md5` or `scram-sha-256` socket with a wrong password).

### The TLS case

`connection-ssl` connects over `QB_PG_SSL_DSN` and skips when TLS cannot complete, again only if you have not set the
variable yourself — the gate is the `ssl_dsn_pinned()` helper keyed on the `QB_PG_SSL_DSN` environment variable: when it
is unset a failed TLS probe skips, and when it is set a failed probe is a hard failure (<!-- src: qbm/pgsql/tests/integration/connection/connection-ssl.cpp:55-57,81-90 -->). On a build without OpenSSL the suite is not
compiled at all, so there is nothing to skip.

Both TLS fixtures print the `QBM_INTEGRATION_SKIP_DAEMON_UNREACHABLE` sentinel when they skip, so CTest reports the
binary as **Skipped**. Until 3.0 they did not, and a run with no reachable PostgreSQL reported `connection-ssl` as
**Passed** while every one of its cases had skipped internally — the one `REQUIRES live` binary that looked like
coverage it had not produced.

## Steps

### Build and run from the super-project

From a configured build tree, build the module's tests and run them through CTest. The exact `bin/tests` path varies
with your generator and build directory.

```bash
# Configure once (QB_BUILD_TESTS is ON by default).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run every pgsql suite; -j is safe — the resource lock serializes the
# integration suites onto one database.
ctest --test-dir build -R '^qbm-pgsql-test-' -j --output-on-failure
```

`-R '^qbm-pgsql-test-'` selects exactly the module's suites by their registered prefix. CTest reports each suite as
passed, failed, or — when the server is absent — passed with skipped cases.

### Run a single suite or test

Run one suite through CTest, or invoke its binary directly for `--gtest_filter`:

```bash
# One suite via CTest.
ctest --test-dir build -R '^qbm-pgsql-test-integration-coro-api$' --output-on-failure

# One test via the binary (path is generator-dependent).
./build/bin/tests/qbm-pgsql-test-integration-coro-api --gtest_filter='*WithTransaction*'
```

### Run only the unit suites

To validate encoding and protocol framing on a host with no database, select the no-server suites:

```bash
ctest --test-dir build -R '^qbm-pgsql-test-unit-' --output-on-failure
```

## What is covered

Use the suites as a feature map: grep a behavior, read the matching test, then build against the pattern it
demonstrates.

| Topic                                                             | Start with                                                                                              |
|-------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| Connect, reconnect, DSN, invalid auth                             | `integration/connection/connection-lifecycle.cpp`, `unit/dsn/dsn-parse.cpp`                             |
| Connect timeout                                                   | `system/connection/connect-timeout.cpp`                                                                 |
| TLS upgrade and SSL handshake                                     | `integration/connection/connection-ssl.cpp` (built only with `QB_HAS_SSL`)                             |
| Coroutines, `with_transaction`, savepoints, `run_sync`            | `integration/api/coro-api.cpp`                                                                          |
| Callback `begin` / `then` / `error`, nested savepoints, `await()` | `integration/transaction/transaction-basic.cpp`                                                        |
| Timeouts (`set_timeout`), constraints, cursors, advanced SQL      | `integration/transaction/transaction-advanced.cpp`                                                     |
| LISTEN / NOTIFY, consumer, pump ordering                          | `integration/notify/listen-notify.cpp`                                                                 |
| Prepared-statement LRU, eviction, large results                   | `integration/prepared/prepared-statements.cpp`, `unit/prepared/prepared-storage-lru.cpp`               |
| Type round-trips, including `qb::wall_time` (`timestamptz`)       | `unit/types/typeconverter-*.cpp` (unit), `integration/datatypes/datatypes-roundtrip.cpp` (live)        |
| COPY edges, binary columns, end-to-end protocol                   | `integration/protocol/wire-formats.cpp`, `unit/protocol/protocol-message-codec.cpp`                    |
| Simple/prepared `execute` / `query`, parameter binding            | `integration/query/queries-execution.cpp`, `integration/api/database-api-extra.cpp`                    |
| Parameter serialization and parsing (wire units)                  | `unit/serialization/param-serializer-encode.cpp`, `integration/serialization/param-roundtrip.cpp`, `unit/wire/unserializer-primitives.cpp` |
| `Reply<T>`, `db_error`, SQLSTATE, `value_is_null`                 | `integration/errors/errors-sqlstate.cpp`                                                               |

## Pitfalls

- **A green CTest run can mean "all skipped."** On a host without a database, every integration suite passes by
  skipping. Read the CTest summary for the skipped count, or run with `--output-on-failure` and look for `SKIPPED`,
  before claiming the integration paths are covered. Only the unit suites and the build are validated in that case.
- **Do not run two database-backed runners against one server.** The resource lock serializes suites *within a
  single `ctest` invocation*, not across invocations or machines. Two `ctest` processes pointed at the same
  `localhost:5432` will collide on temp objects. Give each runner its own database.
- **`trust` in `pg_hba.conf` hides the negative-auth test.** With `trust`, the wrong-password test silently skips. Set
  `QB_PG_INVALID_DSN` to a password-verifying endpoint to keep that assertion live.
- **The default DSN expects a specific fixture.** Role `test` / password `test` / database `test`. If that does not
  exist, set `QB_PG_DSN`; otherwise the integration suites do nothing but skip.
- **The SSL suite needs OpenSSL at build time.** Without `QB_HAS_SSL` there is no `connection-ssl` binary and no
  `qb::pg::tcp::ssl::database` alias to test. Configuring `QB_PG_SSL_DSN` has no effect on a cleartext build.

## See also

- [connection.md](./connection.md) — DSN grammar, TLS, `disconnect` / `prepare_reconnect`.
- [transaction.md](./transaction.md) — `begin` / `commit` / `rollback`, `set_timeout`, `with_transaction`.
- [queries.md](./queries.md) — `execute` / `query` / `prepare`, LISTEN / NOTIFY.
- [`../README.md`](../README.md) — module positioning, build matrix, and `qb_load_modules` wiring.
- [`../tests/shared/test_config.hpp`](../tests/shared/test_config.hpp) — the authoritative DSN defaults; grep it when adding a suite
  or environment variable.
