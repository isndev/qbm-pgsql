/**
 * @file common.cpp
 * @brief Implementation of common PostgreSQL client utilities
 *
 * This file implements the functionality declared in common.h, including:
 * - Connection string parsing
 * - Transaction isolation level conversions
 * - User-defined literals for database connections
 *
 * @note Portions of this code adapted from project: https://github.com/zmij/pg_async.git
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include <array>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "./common.h"

namespace qb {
namespace pg {

namespace {
/**
 * @brief Mapping from isolation level enum values to their string representations
 *
 * OPTIMIZED: Replaced std::map (3 elements, O(log n) + allocations) with
 * std::array and linear search (O(n) but much faster for tiny n, no allocations).
 *
 * For only 3 elements, linear search is significantly faster than tree lookups.
 */
struct IsolationLevelEntry {
    isolation_level level;
    const char     *name;
};

constexpr std::array<IsolationLevelEntry, 3> ISOLATION_TO_STRING{{
    {isolation_level::read_committed, "read committed"},
    {isolation_level::repeatable_read, "repeatable read"},
    {isolation_level::serializable, "serializable"},
}};

/**
 * @brief Linear search for isolation level name
 *
 * @param val Isolation level to find
 * @return Pointer to name if found, nullptr otherwise
 */
constexpr const char *
isolation_level_name(isolation_level val) {
    for (const auto &entry : ISOLATION_TO_STRING) {
        if (entry.level == val) {
            return entry.name;
        }
    }
    return nullptr;
}
} // namespace

/**
 * @brief Output stream operator for isolation_level enum
 *
 * Converts an isolation level to its string representation for display purposes.
 *
 * @param os The output stream
 * @param val The isolation level value to output
 * @return Reference to the output stream for chaining
 */
::std::ostream &
operator<<(::std::ostream &os, isolation_level val) {
    std::ostream::sentry s(os);
    if (s) {
        if (const char *name = isolation_level_name(val)) {
            os << name;
        } else {
            os << "Unknown transaction isolation level " << static_cast<int>(val);
        }
    }
    return os;
}

/**
 * @brief Output stream operator for transaction_mode
 *
 * Converts a transaction mode to its SQL representation for inclusion in
 * BEGIN/START TRANSACTION statements. Only outputs non-default values to
 * keep the resulting SQL concise.
 *
 * @param os The output stream
 * @param val The transaction mode to output
 * @return Reference to the output stream for chaining
 */
::std::ostream &
operator<<(::std::ostream &os, transaction_mode const &val) {
    ::std::ostream::sentry s(os);
    if (s) {
        bool need_comma = false;
        if (val.isolation != isolation_level::read_committed) {
            os << " ISOLATION LEVEL " << val.isolation;
            need_comma = true;
        }
        if (val.read_only) {
            if (need_comma)
                os << ",";
            os << " READ ONLY";
            need_comma = true;
        }
        if (val.deferrable) {
            if (need_comma)
                os << ",";
            os << " DEFERRABLE";
        }
    }
    return os;
}

::std::string
to_string(transaction_mode const &val) {
    ::std::ostringstream os;
    os << val;
    return os.str();
}

/**
 * @brief Parser for PostgreSQL connection strings
 *
 * This class implements a state machine for parsing connection strings in
 * the following formats:
 * - "aliasname=tcp://user:password@localhost:5432[database]"
 * - "ssl://localhost:5432[database]"
 * - "socket:///tmp/.s.PGSQL.5432[database]"
 *
 * The parser extracts the connection parameters (alias, schema, user, password,
 * URI, and database name) from the string and populates a connection_options
 * structure with them.
 */
struct connect_string_parser {
    /**
     * @brief States for the connection string parser state machine
     *
     * These states track the current position within the connection string
     * to correctly parse each component.
     */
    enum state_type {
        alias,         /**< Parsing the optional alias name */
        schema,        /**< Parsing the connection schema (tcp, ssl, socket) */
        schema_slash1, /**< Expecting first slash after schema (in schema://) */
        schema_slash2, /**< Expecting second slash after schema (in schema://) */
        user,          /**< Parsing the username */
        password,      /**< Parsing the password */
        url,           /**< Parsing the host:port or socket path */
        database,      /**< Parsing the database name */
        done,          /**< Parsing completed */
    } state;

    /**
     * @brief Constructor initializes parser to start in alias state
     */
    connect_string_parser()
        : state(alias) {}

    /**
     * @brief Parses a connection string into connection options
     *
     * Processes the input string character by character, advancing through
     * parser states based on special delimiter characters ('=', ':', '@', '[', ']')
     * and collecting the relevant connection parameters.
     *
     * @param literal The connection string to parse
     * @return A populated connection_options structure
     * @throws std::runtime_error If the connection string has invalid format
     */
    connection_options
    operator()(std::string const &literal) {
        std::string        current;
        connection_options opts;
        for (auto p = literal.begin(); p != literal.end(); ++p) {
            switch (state) {
                case schema_slash1:
                case schema_slash2:
                    if (*p != '/')
                        throw std::runtime_error("invalid connection string");
                    state = static_cast<state_type>(state + 1);
                    break;
                default: {
                    if (*p == '=') {
                        if (state == alias) {
                            opts.alias.swap(current);
                            state = schema;
                        }
                    } else if (*p == ':') {
                        // current string is a scheme or a user or host
                        switch (state) {
                            case alias:
                            case schema:
                                opts.schema.swap(current);
                                state = schema_slash1;
                                break;
                            case user:
                                opts.user.swap(current);
                                state = password;
                                break;
                            case url:
                                current.push_back(*p);
                                break;
                            default:
                                current.push_back(*p);
                                break;
                        }
                    } else if (*p == '@') {
                        // current string is a user or a password
                        switch (state) {
                            case user:
                                opts.user.swap(current);
                                state = url;
                                break;
                            case password:
                                opts.password.swap(current);
                                state = url;
                                break;
                            default:
                                current.push_back(*p);
                                break;
                        }
                    } else if (*p == '[') {
                        switch (state) {
                            case user:
                            case url:
                                opts.uri.swap(current);
                                state = database;
                                break;
                            case password:
                                opts.uri.swap(opts.user);
                                opts.uri.push_back(':');
                                opts.uri += current;
                                current.clear();
                                state = database;
                                break;
                            default:
                                current.push_back(*p);
                                break;
                        }
                    } else if (*p == ']') {
                        // current string is database
                        if (state == database) {
                            opts.database.swap(current);
                            state = done;
                        } else {
                            current.push_back(*p);
                        }
                    } else if (!std::isspace(static_cast<unsigned char>(*p))) {
                        // Add any non-whitespace character to the current token.
                        // Cast to unsigned char first: passing a negative char (a high-bit
                        // byte in a password/database name) to std::isspace is undefined
                        // behavior and could misclassify it as space and drop it.
                        current.push_back(*p);
                    }
                }
            }
        }
        return opts;
    }
};

/**
 * @brief Generates a default alias for a connection from its parameters
 *
 * Creates a unique identifier for the connection based on user, URI, and database
 * when no explicit alias is provided. The generated alias has the format:
 * "user@uri[database]".
 */
void
connection_options::generate_alias() {
    std::ostringstream al;
    al << user << "@" << uri << "[" << database << "]";
    alias = al.str();
}

/**
 * @brief Parses a connection string into a connection_options structure
 *
 * This static method delegates to the connect_string_parser class to process
 * the connection string and extract the connection parameters.
 *
 * @param literal The connection string to parse
 * @return A populated connection_options structure
 */
connection_options
connection_options::parse(std::string const &literal) {
    return connect_string_parser()(literal);
}

} // namespace pg
} // namespace qb

/**
 * @brief User-defined literal for creating a database alias
 *
 * Allows creating database aliases with a convenient syntax:
 * ```cpp
 * auto alias = "mydb"_db;
 * ```
 *
 * @param v The string literal to convert to a database alias
 * @param n The length of the string literal (unused, automatically provided by compiler)
 * @return A new database alias object containing the specified string
 */
qb::pg::dbalias
operator""_db(const char *v, size_t) {
    return qb::pg::dbalias{std::string{v}};
}

/**
 * @brief User-defined literal for creating connection options from a string
 *
 * Allows creating connection options with a convenient syntax:
 * ```cpp
 * auto opts = "tcp://user:password@localhost:5432[database]"_pg;
 * ```
 *
 * @param literal The connection string to parse
 * @param n The length of the string literal (unused, automatically provided by compiler)
 * @return A new connection_options object populated from the connection string
 */
qb::pg::connection_options
operator""_pg(const char *literal, size_t) {
    return qb::pg::connection_options::parse(literal);
}