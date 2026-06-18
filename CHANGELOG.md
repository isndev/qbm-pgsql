# Changelog

All notable changes to the qbm-pgsql module are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the module tracks the qb framework's
[Semantic Versioning](https://semver.org/). Framework-wide policy is in the qb
[VERSIONING](../../qb/VERSIONING.md) document.

## [Unreleased]

Tracks changes on the development branch not yet part of a tagged release.

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

[Unreleased]: https://github.com/isndev/qbm-pgsql/compare/main...HEAD
