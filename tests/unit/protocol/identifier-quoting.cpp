/**
 * @file unit/protocol/identifier-quoting.cpp
 * @brief `pg_quote_identifier` — the module's only SQL-string interpolation point.
 *
 * Every user value the module sends to PostgreSQL is bound out-of-band through the extended
 * query protocol, with exactly one exception: a **savepoint name**, which is interpolated into a
 * simple-query string (`savepoint <name>`, `release savepoint <name>`, `rollback to savepoint
 * <name>` — `src/queries.h`). That single interpolation is the module's whole SQL-injection
 * surface, and `pg_quote_identifier` is what closes it, by wrapping the name in double quotes and
 * doubling any embedded double quote (SQL identifier syntax, matching libpq's
 * `PQescapeIdentifier`).
 *
 * The function carries a precise security comment but had **no test of any kind**. That is the
 * dangerous shape: a correct implementation with no regression guard is one refactor away from
 * being silently wrong, and the failure mode is remote SQL execution rather than a crash.
 *
 * The co_await savepoint API additionally rejects non-alphanumeric names up front; these cases
 * therefore target the quoting itself, which is the belt-and-suspenders that also covers the
 * callback API.
 *
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <string>
#include <qbm/pgsql/queries.h>

using qb::pg::detail::pg_quote_identifier;

namespace {

/// A quoted identifier is well-formed iff it starts and ends with `"` and every interior `"`
/// appears as a doubled pair — i.e. the closing quote can never be reached early.
[[nodiscard]] bool
is_single_quoted_identifier(std::string const &s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"')
        return false;
    for (std::size_t i = 1; i + 1 < s.size();) {
        if (s[i] != '"') {
            ++i;
            continue;
        }
        // An interior quote must be immediately followed by another (a doubled pair).
        if (i + 2 >= s.size() || s[i + 1] != '"')
            return false;
        i += 2;
    }
    return true;
}

} // namespace

TEST(PgQuoteIdentifier, PlainNameIsWrappedUnchanged) {
    EXPECT_EQ(pg_quote_identifier("sp1"), "\"sp1\"");
    EXPECT_EQ(pg_quote_identifier(""), "\"\"");
}

TEST(PgQuoteIdentifier, EmbeddedQuotesAreDoubled) {
    EXPECT_EQ(pg_quote_identifier("a\"b"), "\"a\"\"b\"");
    EXPECT_EQ(pg_quote_identifier("\""), "\"\"\"\"");
    EXPECT_EQ(pg_quote_identifier("\"\""), "\"\"\"\"\"\"");
}

// The whole point of the function: no input may terminate the identifier early and start a new
// statement, whatever it contains.
TEST(PgQuoteIdentifier, InjectionAttemptsStayASingleIdentifier) {
    const std::string vectors[] = {
        "s; DROP TABLE users; --",       // the canonical statement-splitting attempt
        "s\"; DROP TABLE users; --",     // close the identifier first, then split
        "s\"\"; DROP TABLE users; --",   // pre-doubled quotes, to defeat naive un-doubling
        "\"; DROP TABLE users; --",      // leading quote
        "s'; DROP TABLE users; --",      // single quote (a literal terminator, not an identifier one)
        "s -- comment",                  // trailing line comment
        "s /* block */",                 // block comment
        "s\nDROP TABLE users",           // newline instead of a separator
        "s\r\nDROP TABLE users",         //
        "s\tDROP TABLE users",           //
        "s\\\"; DROP TABLE users; --",   // backslash escape (PostgreSQL identifiers do not honour it)
        std::string("s\0; DROP", 9),     // embedded NUL
    };

    for (auto const &v : vectors) {
        const auto quoted = pg_quote_identifier(v);
        EXPECT_TRUE(is_single_quoted_identifier(quoted)) << "quoting produced something that is not one literal identifier for input: " << v;
        // Belt and braces: the interior must contain no odd (unescaped) quote run, which is the
        // only way the identifier could be closed early.
        const auto interior = quoted.substr(1, quoted.size() - 2);
        for (std::size_t i = 0; i < interior.size();) {
            if (interior[i] != '"') {
                ++i;
                continue;
            }
            std::size_t run = 0;
            while (i + run < interior.size() && interior[i + run] == '"')
                ++run;
            EXPECT_EQ(run % 2, 0u) << "odd-length quote run inside the identifier for input: " << v;
            i += run;
        }
    }
}

TEST(PgQuoteIdentifier, EveryByteValueSurvivesQuoting) {
    // Exhaustive over the byte domain: the only character with any meaning here is '"'.
    for (int c = 0; c < 256; ++c) {
        const std::string in(1, static_cast<char>(c));
        const auto        quoted = pg_quote_identifier(in);
        ASSERT_TRUE(is_single_quoted_identifier(quoted)) << "byte 0x" << std::hex << c << " broke the quoting";
        EXPECT_EQ(quoted.size(), c == '"' ? 4u : 3u) << "byte 0x" << std::hex << c << " produced an unexpected length";
    }
}
