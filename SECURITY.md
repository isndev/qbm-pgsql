<!-- Verified-against: qbm-pgsql @ qb 2.6.0 (C++20 default, C++23 supported) -->

# Security policy

qbm-pgsql is a module of the qb actor framework. Vulnerability reporting and disclosure follow the
framework's process — see the qb [SECURITY policy](../../qb/SECURITY.md). **Do not report security issues
through public GitHub issues, pull requests, or discussions.**

## Supported versions

Security fixes target the module version that ships with the supported qb framework release (the `2.0.x`
line). See the framework policy for details.

## Module attack surface

qbm-pgsql parses untrusted bytes from a PostgreSQL server connection. When reporting, identify the affected
component:

- The startup and authentication exchange, handled before a session is trusted.
- Wire-protocol message framing and parsing on the qb-io boundary.
- `RowDescription` and `DataRow` decoding, including the binary decoders for each type.
- The timestamp/`timestamptz` binary decoder (epoch and fractional handling).
- Prepared-statement `Parse`/`Bind` parameter handling and limits.

The module fails closed on hostile input: handler exceptions are contained at the `noexcept` `onMessage`
boundary (mitigating a pre-authentication denial of service), a `Parse` with more than 32767 parameter
types is rejected, a malformed frame drops the connection rather than reconnecting, and the timestamp binary
decoder is bounded against over-read. Report any case where these protections can be bypassed, or where a
malicious server response reaches an unsafe path.

A connection should be made only to a trusted database server over a trusted network or TLS; treat the
server as part of your trust boundary.
