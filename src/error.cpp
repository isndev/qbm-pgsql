/**
 * @file error.cpp
 * @brief Implementation of PostgreSQL error handling classes
 *
 * This file implements the exception classes defined in error.h for handling
 * PostgreSQL-related errors. It provides the actual implementation of constructors
 * for various error types, including:
 *
 * - Base database errors (db_error)
 * - Connection errors (connection_error)
 * - Query execution errors (query_error)
 * - Client-side errors (client_error)
 * - NULL value access errors (value_is_null)
 *
 * Each constructor is implemented to properly initialize the exception with
 * appropriate error messages, severity levels, and SQL state codes.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

#include "./error.h"

namespace qb {
namespace pg {
namespace error {

/**
 * @brief Implements the basic database error constructor (string version)
 *
 * Initializes a basic error with just a message string and sets the SQL state
 * to an unknown code.
 *
 * @param what_arg Error message describing the issue
 */
db_error::db_error(std::string const &what_arg)
    : std::runtime_error(what_arg)
    , sqlstate(sqlstate::unknown_code) {}

/**
 * @brief Implements the basic database error constructor (C-string version)
 *
 * Initializes a basic error with just a C-string message and sets the SQL state
 * to an unknown code.
 *
 * @param what_arg Error message describing the issue (C-string)
 */
db_error::db_error(char const *what_arg)
    : std::runtime_error(what_arg)
    , sqlstate(sqlstate::unknown_code) {}

/**
 * @brief Implements the detailed database error constructor
 *
 * Initializes a fully-specified database error with PostgreSQL error details
 * and automatically derives the SQL state code from the PostgreSQL error code.
 *
 * @param message Main error message describing the issue
 * @param s PostgreSQL error severity (ERROR, FATAL, etc.)
 * @param c PostgreSQL error code string
 * @param d Additional error details or context
 */
db_error::db_error(std::string const &message, std::string s, std::string c, std::string d)
    : std::runtime_error(message)
    , severity(s)
    , code(c)
    , detail(d)
    , sqlstate(sqlstate::code_to_state(code)) {}

/**
 * @brief Implements the connection error constructor (string version)
 *
 * Creates a connection-specific error with the given message.
 *
 * @param what_arg Error message describing the connection problem
 */
connection_error::connection_error(std::string const &what_arg)
    : db_error(what_arg) {}

/**
 * @brief Implements the connection error constructor (C-string version)
 *
 * Creates a connection-specific error with the given message.
 *
 * @param what_arg Error message describing the connection problem (C-string)
 */
connection_error::connection_error(char const *what_arg)
    : db_error(what_arg) {}

/**
 * @brief Implements the query error constructor (string version)
 *
 * Creates a query-specific error with the given message.
 *
 * @param what_arg Error message describing the query issue
 */
query_error::query_error(std::string const &what_arg)
    : db_error(what_arg) {}

/**
 * @brief Implements the query error constructor (C-string version)
 *
 * Creates a query-specific error with the given message.
 *
 * @param what_arg Error message describing the query issue (C-string)
 */
query_error::query_error(char const *what_arg)
    : db_error(what_arg) {}

/**
 * @brief Implements the detailed query error constructor
 *
 * Creates a fully-specified query error with all PostgreSQL error information.
 * Uses the base class constructor to initialize all fields and derive the SQL state.
 *
 * @param message Main error message describing the issue
 * @param s PostgreSQL error severity (ERROR, FATAL, etc.)
 * @param c PostgreSQL error code string
 * @param d Additional error details or context
 */
query_error::query_error(std::string const &message, std::string s, std::string c, std::string d)
    : db_error(message, s, c, d) {}

/**
 * @brief Implements the client error constructor (string version)
 *
 * Creates a client-side error with the given message.
 *
 * @param what_arg Error message describing the client-side issue
 */
client_error::client_error(std::string const &what_arg)
    : db_error(what_arg) {}

/**
 * @brief Implements the client error constructor (C-string version)
 *
 * Creates a client-side error with the given message.
 *
 * @param what_arg Error message describing the client-side issue (C-string)
 */
client_error::client_error(char const *what_arg)
    : db_error(what_arg) {}

/**
 * @brief Implements the client error constructor from an existing exception
 *
 * Wraps an existing exception with a client error, preserving the original
 * exception's message but prefixing it to indicate it was thrown by client code.
 *
 * @param ex Original exception to wrap
 */
client_error::client_error(std::exception const &ex)
    : db_error(std::string("Client thrown exception: ") + ex.what()) {}

/**
 * @brief Implements the NULL value error constructor
 *
 * Creates an error indicating a NULL value was encountered when attempting
 * to extract data from a specific field.
 *
 * @param field_name Name of the field that contains NULL
 */
value_is_null::value_is_null(std::string const &field_name)
    : db_error("Value in field " + field_name + " is null") {}

} // namespace error
} // namespace pg
} // namespace qb
