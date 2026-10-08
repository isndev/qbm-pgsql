# Changelog

All notable changes to the qbm-pgsql module are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the module tracks the qb framework's
[Semantic Versioning](https://semver.org/). Framework-wide policy is in the qb
[VERSIONING](https://github.com/isndev/qb/blob/main/VERSIONING.md) document.

## [Unreleased]

### Changed

- **A multi-statement simple query whose statements return rows of different shapes now fails instead of
  mixing them (Huly QB-122).** `execute("SELECT …; SELECT …", …)` collected every statement's rows into ONE
  result under the LAST RowDescription, undocumented: the earlier statement's rows were then decoded against
  the later statement's columns -- a `field_type_mismatch` or an `out_of_range` at best, a wrong value in
  silence at worst. Statements of the same shape (column count, types, formats) still merge into one result
  as before; a different shape arriving after rows were collected fails the query with a `client_error`, once
  the server has finished with it, so the connection stays usable -- exactly the queries that returned wrong
  data. An earlier statement that returned no rows is superseded, as before. Documented in
  `readme/results.md` and the llm gotchas.

### Fixed

- **Typed JSONB reads reject decimal value changes (Huly QB-945).**
  `field.as<qb::jsonb>()` now throws `client_error` if nlohmann would round a
  PostgreSQL JSONB number, including nested decimals and integers outside its
  integer ranges and `1e400` beyond the DOM floating range. Malformed JSON
  keeps its separate parse error; equal decimal values with different spellings remain valid.
  `field.jsonb_text()` exposes the server's canonical text without parsing or
  copying, with OID/format/version checks and backing-row lifetime;
  `jsonb_text_copy()` keeps an owning copy. The `qb::jsonb` layout is unchanged.
- **Overlapping inline parameterized queries keep their own SQL (Huly QB-106).**
  `query(sql, args...)` now keeps Parse/Describe and Bind/Execute in one queued command.
  Two coroutines on one connection can no longer replace the unnamed statement between
  those phases and return another query's plausible result. The two server round-trips
  and per-column result formats remain as before.
- **Present empty text and binary numeric values stay correct through `field::as<std::optional<T>>()` (Huly QB-646).**
  SQL NULL metadata now decides whether the optional is empty; a present value is decoded through `T` with
  its column format and OID. `results::json()` keeps an empty string as `""` rather than JSON null, and
  binary `int8` read as `optional<double>` converts numerically instead of reinterpreting its bytes.
- **Binary JSON and JSONB decoding keeps arrays of pairs as arrays (Huly QB-944).**
  `field.as<qb::jsonb>()` and the direct JSON binary decoder used to reinterpret
  `[[1,2]]` as an object despite PostgreSQL sending ordinary JSON text with no
  object marker. Both now parse that text without changing its structure; binary
  version checks and parse errors still fail loudly.
- **`results::json()` decodes binary result columns before stringifying them (Huly QB-943).**
  Prepared `int8`, `float8` and other binary columns used to become JSON strings containing
  PostgreSQL wire bytes. The export keeps its existing string-valued contract: scalar
  values use the column OID's text formatter, JSONB keeps PostgreSQL's canonical text
  exactly (including pair arrays and arbitrary-precision decimals), SQL NULL is JSON
  null, and empty TEXT is `""`. BYTEA is `\x` hex; one-dimensional arrays preserve
  NULL elements and non-1 lower bounds as `[lower:upper]=...`. Unsupported binary OIDs,
  JSONB versions and overflowing array bounds fail loudly instead of exporting bytes
  or a changed value.
- **Fragmented `ReadyForQuery` completes as soon as its final byte arrives (Huly QB-626).** The
  framer no longer asks for another five bytes after it has already consumed the header; a
  six-byte response split 5+1 or 1+4+1 no longer leaves the next query waiting.
- **A local prepared-statement error leaves the connection ready (Huly QB-659).** A fluent
  `.error(...)` branch with no query no longer clears readiness permanently; a following
  valid query is sent without waiting for a server response that cannot arrive.
- **`disconnect()` from a query callback no longer re-enters a completed query (Huly QB-627).**
  The active query is removed before its callback, and queued failures wait until that callback
  returns so its transaction owner stays alive. The same lifetime guard covers fluent
  `.then(...)` / `.error(...)` callbacks invoked while a transaction command is popped;
  disconnect there cannot destroy the parent before the traversal finishes. Success and
  error callbacks complete once; queued work fails once.
- **Callback commands on a disconnected database fail immediately (Huly QB-917).** The
  common command queue rejects callback `execute`, prepared `execute` and `prepare` before
  serialization, delivers their error callback once, and records a failed status for
  `await()`, matching the coroutine overloads' closed-handle behavior.
- **`notify_co_consumer::receive()` serves the next connection (Huly QB-252).** A disconnect closes the consumer's
  queue, and a channel closes for good: after a reconnect every NOTIFY was dropped -- and logged as "buffer full" --
  and `receive()` yielded `std::nullopt` for ever, so the documented advice was to build a new consumer. The queue
  is now replaced on the first `receive()` or NOTIFY of the next connection, carrying over in order what was
  received and not yet read. LISTEN still has to be re-issued: the subscription belongs to the session.
- **`disconnect()` is safe from a coroutine (Huly QB-253).** It closed the socket and then ran one `EVRUN_NOWAIT`
  loop pass to observe the close, and from a coroutine body that pass re-entered the coroutine scheduler: an abort in
  a debug build, which the documentation answered with "call it from a callback or `main()`" and the
  `07-listen-notify` example by not calling it. It now completes the teardown in the call through qb-io's
  `disconnect_now()`, as qbm-redis's `disconnect()` does -- no loop pass, so no other watcher, deferred callback or
  coroutine runs under the caller -- and still fails every in-flight and queued query first.
- **A reconnection without `prepare_reconnect()` no longer sends the previous connection's queries first (Huly
  QB-202).** `disconnect()` fails the queued queries, but their bytes stayed in the client's output buffer, and the
  bare reconnect path -- `connect()` after `disconnect()`, supported and tested -- sent them ahead of the
  StartupMessage: the server refused the connection. Every connect path now clears both buffers, with the protocols,
  where it installs the new transport (qb-io's `reset_for_reconnect()`).

### Documentation

- **The stale `@todo COPY` comment in `pgsql.h` is gone (Huly QB-120)** -- it said COPY was "not a product
  feature yet" while `copy_in` / `copy_out` ship and are tested; the comment now says what is supported and
  that CopyBoth is not.
- **`readme/types.md` no longer says vectors of other element types fail to bind** -- binding works for any
  element type with a PostgreSQL array companion (`uuid`, `numeric`, `json` / `jsonb`, the date and time types);
  decoding them back is what is still limited to the seven listed.
- **Two `readme/transaction.md` citations re-derived (Huly QB-254).** The `begin` and `savepoint` ranges of
  `commands.h` started one line early, on the blank line above each; found by the strengthened
  `scripts/cite-check.py`.

## [3.2.1] - 2026-09-24

Lockstep release with the qb 3.2.1 train; no change in this repository (the version says compatible, this section says unchanged).

## [3.2.0] - 2026-09-21

### Added

- **`cancel_async()` -- the out-of-band CancelRequest without blocking the event loop, TLS
  included (Huly QB-113).** `cancel()` opens its second socket with a BLOCKING connect + send,
  capped at `min(connect_timeout, 2 s)`; fired from a timer on the core's loop -- the natural place
  -- it parked every actor of that core for the duration, and it sent the request in plaintext even
  on a secure database, which a `hostssl`-only server rejects. It was the one blocking call in the
  client, listed as such in both llm docs and three readme pages. `cancel_async()` (`task<bool>`)
  is the same request driven by the async connector the session itself connects through:
  `qb::io::async::tcp::connect` for a plain database, the STARTTLS connector with
  `postgres_ssl_negotiator` -- SSLRequest, the server's `'S'`, the TLS handshake -- for a secure
  one, the coroutine suspended meanwhile; then the 16 bytes written non-blocking (a would-block
  parks the coroutine on the socket's readiness). A secure database never falls back to
  plaintext: a server that declines SSL on the cancel connection fails the cancel, the rule the
  session's connect applies. Same connect budget as `cancel()`, same verdict (57014 on the
  awaiting caller, the connection survives). `cancel()` stays, unchanged on the wire: the two share
  `cancel_request_packet()`, and the TLS context builder is shared with the session's connect
  (`make_client_tls_context()`). Tests: a fake backend that hands out the BackendKeyData and reads
  the second connection (`system/connection/cancel-request-wire.cpp`: the exact 16 bytes, the
  loop's next turn running BEFORE the cancel completes, `cancel()` sending the same bytes, both
  false on a never-connected database), the coroutine twin of `CancelInFlightQuery` and a TLS twin
  in `connection-ssl` whose session is asserted TLS through `pg_stat_ssl`.

### Fixed

- **Array serde is fail-loud (Huly QB-109).** A NULL element decoded to a default-constructed
  element (`{1,NULL,3}` read as `{1,0,3}`, `{'a',NULL,'c'}` as `{"a","","c"}`); it throws
  `value_is_null` now, naming the way out, unless the column is read as
  `std::vector<std::optional<T>>` -- supported for every array type on both the result and the
  parameter side, where a `nullopt` element binds as the -1 length with the has-null flag raised.
  A multi-dimensional array was flattened row-major; it throws `field_type_mismatch` (unnest it
  in SQL, or read the column as text). A malformed value decoded to an empty or PARTIAL vector; it
  throws `client_error`. And the TEXT format (the simple query protocol) is parsed and rendered --
  `from_text` returned `{}` and `to_text` `""` for every array -- with quoting and escapes, the
  bare `NULL`, `{}`, the `[lo:hi]=` decoration and whitespace-inside-a-bare-element refused as
  `array_in` refuses it; the multi-dimensional literal is refused like its binary twin.
  `decode_pg_array` / `encode_pg_array` / `parse_pg_array_text` / `render_pg_array_text` are the
  four functions, `TypeConverter<std::vector<std::optional<T>>>` the new specialisations.

## [3.1.0] - 2026-08-30

Lockstep release with the qb 3.1.0 train. In this repository only two `qbFunctions.cmake`
citations re-keyed (a comment grew upstream).

## [3.0.1] - 2026-08-29

Lockstep patch with the qb 3.0.1 train (cut for qb-examples, Huly QB-4). In this repository only
tooling: a nightly CI run against qb's `develop` of the day, and `scripts/gen-llms-txt.py` strips
HTML comments by scanning rather than by regex.

## [3.0.0] - 2026-08-20

Tracks changes not yet part of a tagged release. Since 2026-08-11 that is **both** branches:
`main` was fast-forwarded to `develop` for the release, so the module version is **3.0.0** on either,
in lockstep with the qb framework; see the qb CHANGELOG for what makes that release major.

### Removed

- **BREAKING — `resultset.inl`, `transaction.inl` and `transaction_coro.inl` no longer exist.**
  Every definition moved verbatim into the header that already included it, at exactly the position
  the `#include` occupied, so none changed and none was dropped. The counts are identical on both
  axes before and after — 43 `template<…>` headers and 22 `inline` definitions in total:

  | was | now | `template<` | `inline` |
  |---|---|---|---|
  | `resultset.inl` (204 L) | tail of `resultset.h` (was included from `resultset.h:839`) | 18 → 18 | 5 → 5 |
  | `transaction.inl` (528 L) | tail of `commands.h` (was included from `commands.h:703`) | 25 → 25 | 17 → 17 |
  | `transaction_coro.inl` (262 L) | tail of `commands.h`, inside `namespace qb::pg::detail` (was included from `transaction.inl:526`) | *(counted with `commands.h` above)* | |

  (`transaction.inl` is 528 lines, not the 527 `wc -l` reports — it ships without a trailing
  newline.)

  The bodies land in `commands.h`, **not** `transaction.h`, because `commands.h` is the header that
  closes the declaration cycle: `transaction.h` only declares `Transaction`, while the bodies need
  the complete command types that `commands.h` defines (and `commands.h` includes `transaction.h`
  at `:26`). This is the same structural constraint that put `qb::Actor`'s bodies at the tail of
  `VirtualCore.h`. `transaction.cpp`, which used to include `transaction.inl` directly, now reaches
  them through `commands.h`.

  Verified by comparing the preprocessed token stream of `commands.h` and `resultset.h` before and
  after: both are identical (`commands.h` differs by exactly one blank line and zero tokens). A
  control confirmed the comparison can fail. `resultset.h:1-838` and `commands.h:1-702` are
  unchanged, so existing citations into either class still land.

  This only breaks a consumer who included a fragment **directly** — `#include
  <qbm/pgsql/resultset.inl>` and friends. None was a supported spelling: two of the three could not
  compile alone and qb's installed-header gate carried them as named "by-design fragment"
  exclusions. Use `<qbm/pgsql/resultset.h>`, `<qbm/pgsql/commands.h>`, or the `<qbm/pgsql/pgsql.h>`
  umbrella.

### Fixed

- **`CMakeLists.txt` had no `cmake_minimum_required()`, and a standalone configure died on an
  unhelpful error.** CMake reported `No cmake_minimum_required command is present` alongside
  `Unknown CMake command "qb_status_message"` — which reads like a missing include rather than
  "wrong entry point". This module is built from the qb-dev superproject, which loads qb's CMake
  helpers first; an *installed* qb ships none of them (`lib/cmake/qb/` carries only `qbConfig`,
  `qbConfigVersion`, `qbTargets` and the `Find` modules), so pointing `CMAKE_PREFIX_PATH` at one
  does not help — the exact mistake the old error invited. There is now a
  `cmake_minimum_required(VERSION 3.24)` and a guard that names the constraint and points at the
  superproject root and the `package` preset.

- **`transaction_coro.inl` ran seven `#include` directives inside `namespace qb::pg::detail`.**
  It was spliced into `transaction.inl:526`, which is *between* that namespace's braces, so its own
  `#include <cctype> <filesystem> <fstream> <sstream> <string>` and two local includes were
  processed in there. They were harmless only because `transaction.inl`'s block, at namespace scope,
  had already pulled the same headers in first — the fragment silently depended on its includer to
  neutralise its own includes. Deleting `<fstream>` from that block was measured to reparse
  `<fstream>` inside the namespace, declaring `qb::pg::detail::std` and producing 20 errors led by
  `no template named 'basic_streambuf'; did you mean '::std::basic_streambuf'?`.

  The merge hoists all seven to namespace scope, which makes that unreachable. Confirmed free: the
  preprocessed token stream is unchanged, because the includes were no-ops at both positions.

### Changed

- **Logging call sites use qb's prefixed `QB_LOG_*` macros** (65 sites). qb 3.0.0 renamed
  `LOG_DEBUG` / `LOG_VERB` / `LOG_INFO` / `LOG_WARN` / `LOG_CRIT` to `QB_LOG_*` because the
  unprefixed spellings — three of which are also POSIX `<syslog.h>` names — reached every consumer
  of this module's umbrella header and silently replaced a consumer's own. qb still defines the
  unprefixed names as `#ifndef`-guarded aliases, and that guard is exactly why these call sites had
  to move: a consumer who defines `LOG_INFO` first now keeps their definition, and this module's
  headers would otherwise have started logging through *it*.

- **BREAKING — the public include prefix is now `<qbm/pgsql/...>`** (was `<pgsql/...>`). Every consumer
  edits its `#include` lines: `#include <pgsql/pgsql.h>` becomes `#include <qbm/pgsql/pgsql.h>`. The CMake
  target is unchanged (`qbm::pgsql`), and so is the installed location `<prefix>/include/qbm/pgsql/`.
  The old spelling existed only because `qb_register_module` made this module's include root its
  PARENT directory — the superproject's `qbm/`, which does not exist in this repository at all — and
  mirrored it with `<prefix>/include/qbm` on the consumer's include path. That put the maximally
  generic top-level name `pgsql` in every consumer's include namespace. Now the module's own `src/`
  IS the include root and is copied verbatim to `<prefix>/include`, so `<qbm/pgsql/...>` is the same
  string in this tree and in an installed prefix, and the two cannot drift.
- **The source tree moved to `src/qbm/pgsql/`** — one pure `git mv`, 100 % rename detection, zero
  content change, so `git blame` and every line-numbered citation survive intact. The module's old inner `src/` is gone as a *level*, not as content: its 42 files now sit directly
  beside the umbrella they implement, so `pgsql.h`'s `./src/…` includes became `./…` and every header
  that used to install under `include/qbm/pgsql/src/` now installs directly under
  `include/qbm/pgsql/`. (The three `.inl` files this once described are **gone** — see *Removed*
  above; nothing named `.inl` ships in 3.0.)
  `tests/`, `readme/` and `scripts/` live BESIDE `src/`, never inside it, which is what
  makes a stray `#include <tests/fixture.h>` impossible rather than merely unlikely.
  The test suite now includes the shipped spelling instead of resolving `"../pgsql.h"` by string
  concatenation onto a `-I <mod>/tests` flag.
- **`project(qbm-pgsql VERSION ...)` is now `3.0.0`**, tracking `QB_FRAMEWORK_VERSION`. It had been
  left at `2.6.0` while the framework moved on. The module is not standalone-configurable (it calls
  `qb_register_module` / `qb_add_test`, which an installed qb does not ship), so its version can only
  ever mean "the qb this was built against" — and the structural breaks queued for 3.0.0 land hardest
  in the modules, where a package still claiming `2.6.0` would be actively misleading.
- **`scripts/doc-lint.sh` now validates the *value* of the `Verified-against:` markers**, not just
  their presence. It previously checked only that the marker existed, which is how every page in this
  module sat at `qb 2.6.0` across two version bumps unnoticed. The expected version is read from
  `project(qbm-pgsql VERSION ...)` — the one authoritative version available when this repo is checked
  out alone, as it is in its own CI — and cross-checked against `QB_FRAMEWORK_VERSION` whenever a qb
  tree is reachable. A version it cannot determine is a hard stop, never a skip.

## [2.6.0] - 2026-08-02

Aligned with the qb 2.6.0 framework release.

### Changed

- Test suite restructured into tiered `unit/` / `system/` / `integration/` / `benchmark/` directories.

### Fixed

- Reconnect after a failed transaction no longer surfaces a stale `55P02`.
- Empty `bytea` values round-trip as an empty buffer instead of being decoded as NULL.

### Documentation

- Narrative and reference docs overhauled; every page carries code-verified `src:` citations enforced by
  `scripts/doc-lint.sh`.

## [2.0.0]

Aligns qbm-pgsql with the qb 2.0 framework (C++20 baseline) and hardens the PostgreSQL wire-protocol paths.

### Changed

- Time handling migrated to the canonical chrono model: connect, statement, and transaction timeouts are
  `qb::duration`; the `timestamptz` type maps to `qb::wall_time` (OID 1184, integer-microsecond
  round-tripping). The PostgreSQL wire epoch (microseconds since 2000-01-01, day counts, tz offsets) is kept
  native by design. The retired `qb::Timestamp` / `qb::Duration` types are gone.
- Adapted to the qb C++20 baseline (`QB_CXX_STANDARD=20`, optional 23).

### Fixed

- Uninitialized column count on a malformed `RowDescription`.
- Undefined behavior passing a plain `char` to `std::isspace` / `std::isdigit`.
- `QueryParams` forwarding constructor no longer hijacks copy/move.
- Removed an uncompilable success-only `prepare()` overload and dead, never-compiled tuple-conversion files.
- All queued queries now fail on disconnect, so no coroutine awaiter hangs.
- The connection-deadline timer is owned by the connection and cannot outlive the `Database`.

### Security

- Contain handler exceptions at the `noexcept` `onMessage` boundary (pre-authentication denial-of-service).
- Reject a `PARSE` with more than 32767 parameter types (Bind/Parse symmetry).
- Drop the connection via `not_ok()` on a malformed frame instead of attempting a reconnect.
- Fixed a heap over-read in the timestamp binary decoder for 9–11 byte fields.

[Unreleased]: https://github.com/isndev/qbm-pgsql/compare/v3.2.1...HEAD
[3.2.1]: https://github.com/isndev/qbm-pgsql/compare/v3.2.0...v3.2.1
[3.2.0]: https://github.com/isndev/qbm-pgsql/compare/v3.1.0...v3.2.0
[3.1.0]: https://github.com/isndev/qbm-pgsql/compare/v3.0.1...v3.1.0
[3.0.1]: https://github.com/isndev/qbm-pgsql/compare/v3.0.0...v3.0.1
[3.0.0]: https://github.com/isndev/qbm-pgsql/compare/v2.6.0...v3.0.0
[2.6.0]: https://github.com/isndev/qbm-pgsql/releases/tag/v2.6.0
