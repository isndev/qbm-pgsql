# Changelog

All notable changes to the qbm-pgsql module are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the module tracks the qb framework's
[Semantic Versioning](https://semver.org/). Framework-wide policy is in the qb
[VERSIONING](https://github.com/isndev/qb/blob/main/VERSIONING.md) document.

## [Unreleased]

Tracks changes on the development branch not yet part of a tagged release. The module version is
**3.0.0**, in lockstep with the qb framework; see the qb CHANGELOG for what makes that release major.

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
  beside the umbrella they implement, so `pgsql.h`'s `./src/…` includes became `./…` and the installed
  `.inl` files moved from `include/qbm/pgsql/src/` to `include/qbm/pgsql/`.
  `tests/`, `readme/`, `scripts/` and `cmake/` live BESIDE `src/`, never inside it, which is what
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

## [2.6.0] - 2026-06-29

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

[Unreleased]: https://github.com/isndev/qbm-pgsql/compare/v2.6.0...HEAD
[2.6.0]: https://github.com/isndev/qbm-pgsql/releases/tag/v2.6.0
