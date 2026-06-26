/**
 * @file test_config.hpp
 * @brief Shared PostgreSQL integration test configuration (DSN from environment)
 *
 * CI and local machines can point all pgsql tests at the same role/database without
 * editing sources. Defaults match the historical qb-dev fixture: user `test`, database
 * `test`, host `localhost`, port `5432`.
 *
 * Environment variables:
 * - `QB_PG_DSN` — primary connection string for TCP tests (default below).
 * - `QB_PG_SSL_DSN` — DSN for `tcp::ssl::database` tests when OpenSSL is enabled
 *   (defaults to the same as `QB_PG_DSN`; `tcp://` is fine — the client sends
 *   PostgreSQL SSLRequest and upgrades when the server responds `S`).
 * - `QB_PG_INVALID_DSN` — must fail authentication (default: correct user/db but wrong
 *   password). Use this if your server uses `trust` for some sockets — point tests at a
 *   DSN that still requires password verification.
 */
#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

namespace qb::pg::test {

[[nodiscard]] inline std::string_view
env_sv(char const *key, char const *fallback) noexcept {
    char const *v = std::getenv(key);
    return (v && v[0] != '\0') ? std::string_view{v} : std::string_view{fallback};
}

/** Primary DSN for cleartext `tcp::database` integration tests. */
[[nodiscard]] inline std::string_view
dsn_tcp() noexcept {
    return env_sv("QB_PG_DSN", "tcp://test:test@localhost:5432[test]");
}

#ifdef QB_HAS_SSL
/** DSN for `tcp::ssl::database` tests. */
[[nodiscard]] inline std::string_view
dsn_ssl() noexcept {
    return env_sv("QB_PG_SSL_DSN", "tcp://test:test@localhost:5432[test]");
}
#endif

/**
 * DSN that must fail (wrong password by default). Override with `QB_PG_INVALID_DSN` if
 * your `pg_hba.conf` would still accept the default (e.g. trust on local socket).
 */
[[nodiscard]] inline std::string_view
dsn_invalid_auth() noexcept {
    return env_sv("QB_PG_INVALID_DSN", "tcp://test:__qb_invalid_password__@localhost:5432[test]");
}

[[nodiscard]] inline std::string
dsn_tcp_string() {
    return std::string{dsn_tcp()};
}

#ifdef QB_HAS_SSL
[[nodiscard]] inline std::string
dsn_ssl_string() {
    return std::string{dsn_ssl()};
}
#endif

[[nodiscard]] inline std::string
dsn_invalid_auth_string() {
    return std::string{dsn_invalid_auth()};
}

} // namespace qb::pg::test
