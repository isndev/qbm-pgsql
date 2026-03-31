# Integration testing

The **`qbm/pgsql/tests/`** tree builds **many** GoogleTest executables (see **`tests/CMakeLists.txt`**). They act as *
*executable specification** for the module: when documentation and code disagree, **prefer the tests** and file a doc
bug.

CTest registers them with **`RESOURCE_LOCK qb_pgsql_integration`** so **two jobs do not hit one server at once**.

---

## Requirements

- **PostgreSQL** reachable from the build host (default **`localhost:5432`**).
- Role + database matching the DSN (historical default: user **`test`**, password **`test`**, DB **`test`**).
- **Invalid-auth tests:** the server must **reject** the configured bad password. If **`pg_hba.conf`** uses **`trust`**
  for local users, set **`QB_PG_INVALID_DSN`** to a DSN that still fails authentication (e.g. wrong password against *
  *`md5`** / **`scram-sha-256`**).

---

## Environment variables

| Variable                | Role                                                                                                                                                                                     |
|:------------------------|:-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **`QB_PG_DSN`**         | Primary DSN for **`qb::pg::tcp::database`**. Default: **`tcp://test:test@localhost:5432[test]`**                                                                                         |
| **`QB_PG_SSL_DSN`**     | DSN for **`qb::pg::tcp::ssl::database`** when OpenSSL is enabled. Default: same as **`QB_PG_DSN`**. **`tcp://`** is valid: client sends **`SSLRequest`** and upgrades on server **`S`**. |
| **`QB_PG_INVALID_DSN`** | DSN that must **fail** authentication (used by negative connection tests).                                                                                                               |

**`qbm-pgsql-test-connection-ssl`:** if TLS cannot complete with your server config, set **`QB_PG_SSL_DSN`** to a
known-good endpoint or accept **skip** where the test allows it.

**Single source of truth in tree:** **`qbm/pgsql/tests/test_config.hpp`** — grep it when adding new tests or env vars.

---

## Registered tests (from `tests/CMakeLists.txt`)

The **`QB_PGSQL_TESTS`** list is the authoritative set (plus **`connection-ssl`** when **`QB_HAS_SSL`**):

**`param-serializer`**, **`param-unserializer`**, **`param-parsing`**, **`protocol-message`**, **`connection`**, *
*`data-types`**, **`data-types-integration`**, **`prepared-statements`**, **`transaction`**, **`transaction-advanced`**,
**`operations`**, **`queries`**, **`params`**, **`error-handling`**, **`protocol-integration`**, **`pgsql-coro-api`**, *
*`notify`**, and optionally **`connection-ssl`**.

CTest names follow **`qbm-pgsql-test-<name>`** (e.g. **`qbm-pgsql-test-pgsql-coro-api`**).

### What to read for a topic

| Topic                                          | Start with                                                           |
|:-----------------------------------------------|:---------------------------------------------------------------------|
| Connect / reconnect / DSN                      | **`test-connection.cpp`**, **`test-connection-ssl.cpp`**             |
| Coroutines, **`with_transaction`**, savepoints | **`test-pgsql-coro-api.cpp`**                                        |
| Timeouts, advanced SQL                         | **`test-transaction-advanced.cpp`**                                  |
| NOTIFY / LISTEN / consumer                     | **`test-notify.cpp`**                                                |
| Prepared LRU, large results                    | **`test-prepared-statements.cpp`**                                   |
| Types round-trip                               | **`test-data-types*.cpp`**                                           |
| Protocol / COPY edges                          | **`test-protocol-integration.cpp`**, **`test-protocol-message.cpp`** |
| Wire encoding units                            | **`test-param-*.cpp`**                                               |

---

## Running tests

From your build tree (paths vary with generator):

```bash
cd /path/to/build/qbm/pgsql
ctest -R qbm-pgsql --output-on-failure
```

Single binary (example):

```bash
/path/to/build/.../bin/tests/qbm-pgsql-test-pgsql-coro-api --gtest_filter='*WithTransaction*'
```

---

## Troubleshooting

| Symptom                       | Check                                                                                                       |
|:------------------------------|:------------------------------------------------------------------------------------------------------------|
| **Connection refused**        | PostgreSQL listening, firewall, **`QB_PG_DSN`** host/port                                                   |
| **Authentication failed**     | User/password/database; **`pg_hba.conf`**                                                                   |
| **SSL test skipped / failed** | **`QB_PG_SSL_DSN`**, server **`ssl`**, certificate trust                                                    |
| **Invalid-auth test failed**  | **`QB_PG_INVALID_DSN`** must actually fail — not **`trust`**                                                |
| **Flaky NOTIFY tests**        | Drive the subscriber loop (**`run_once`/`io_pump`**) before assertions — cross-connection ordering is async |

---

## Fluent `.then` / `.error` coverage

Integration tests exercise **callback `begin`**, **`savepoint`**, **`execute`**, and **`await()`** heavily (*
*`test-transaction.cpp`**), and coroutines (**`test-pgsql-coro-api.cpp`**). There are **no** dedicated tests that chain
**`.then()` / `.error()`** on the root **`database`** after **`begin`**; behaviour is defined in **`src/commands.h`** (*
*`Then`**, **`Error`**). When adding regressions, mirror patterns from [transaction.md](./transaction.md).

---

## Related

- [connection.md](./connection.md) — DSN, SSL, reconnect
- [transaction.md](./transaction.md) — **`set_timeout`**, **`then`/`error`**
- [queries.md](./queries.md) — NOTIFY/LISTEN patterns  
