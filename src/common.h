/**
 * @file common.h
 * @brief Common types and utilities for PostgreSQL client
 *
 * This file contains the core type definitions and utility classes
 * used throughout the PostgreSQL client library. It includes:
 *
 * - Connection option structures
 * - Field description structures
 * - Binary data handling (bytea)
 * - Transaction isolation levels
 * - Input buffer management
 *
 * The file also documents the type mapping between PostgreSQL and C++
 * data types, serving as a reference for data conversions.
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

#ifndef QBM_PGSQL_NOT_QB_COMMON_H
#define QBM_PGSQL_NOT_QB_COMMON_H

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <qb/uuid.h>
#include <string>
#include <vector>

#include "./pg_types.h"
#include "./util/streambuf.h"

namespace qb {
namespace pg {

/**
 * @brief Buffer type for field data from PostgreSQL query results
 *
 * An input iterator buffer that provides efficient streaming access to field data
 * returned from PostgreSQL queries. This allows processing large data fields
 * without loading the entire content into memory at once.
 */
using field_buffer = qb::util::input_iterator_buffer;

/**
 * @brief Short unique string to refer to a database.
 *
 * A lightweight identifier for database connections that can be used instead of
 * connection strings. Database aliases provide a convenient way to refer to
 * database connections without repeating the full connection parameters.
 *
 * @see @ref connstring
 * @see qb::pg::db_service
 */
struct dbalias : std::string {
    /** @brief Base string type */
    using base_type = std::string;

    /**
     * @brief Default constructor
     *
     * Creates an empty database alias.
     */
    dbalias()
        : base_type() {}

    /**
     * @brief Constructor from string
     *
     * Creates a database alias from an existing string.
     *
     * @param rhs The string to use as a database alias
     */
    explicit dbalias(std::string const &rhs)
        : base_type(rhs) {}

    /**
     * @brief Swap contents with another database alias
     *
     * Efficiently exchanges the contents of this alias with another.
     *
     * @param rhs The database alias to swap with
     */
    void
    swap(dbalias &rhs) /* no_throw */
    {
        base_type::swap(rhs);
    }

    /**
     * @brief Swap contents with a string
     *
     * Efficiently exchanges the contents of this alias with a string.
     *
     * @param rhs The string to swap with
     */
    void
    swap(std::string &rhs) /* no_throw */
    {
        base_type::swap(rhs);
    }

    /**
     * @brief Assignment operator from string
     *
     * Assigns a new value to this database alias from a string.
     *
     * @param rhs The string to assign from
     * @return Reference to this database alias after assignment
     */
    dbalias &
    operator=(std::string const &rhs) {
        dbalias tmp(rhs);
        swap(tmp);
        return *this;
    }
};

/**
 * @brief PostgreSQL connection options
 *
 * This structure contains all the parameters needed to establish a connection
 * to a PostgreSQL database server. It supports both TCP/IP and Unix socket
 * connections, with optional SSL encryption.
 */
struct connection_options {
    dbalias     alias;  /**< Database alias for convenient reference */
    std::string schema; /**< Database connection schema. Currently supported are tcp and
                           socket */
    std::string uri; /**< Database connection URI. `host:port` for tcp, `/path/to/file`
                        for socket */
    std::string database; /**< Database name to connect to */
    std::string user;     /**< Database user name for authentication */
    std::string password; /**< Database user's password for authentication */

    /**
     * @brief Generate an alias from connection parameters
     *
     * Creates a unique alias based on username, database and URI if one was not
     * explicitly provided. This ensures that each connection has a distinct
     * identifier.
     */
    void generate_alias();

    /**
     * @brief Parse a connection string into connection options
     *
     * Parses a formatted connection string into its component parts and returns
     * a populated connection_options structure. Supports various connection formats
     * including TCP, SSL, and Unix socket connections.
     *
     * @code{.cpp}
     * // Full options for a TCP connection
     * connection_options opts =
     * "aliasname=tcp://user:password@localhost:5432[database]"_pg;
     * // SSL connection over TCP
     * opts = "ssl://localhost:5432[database]"_pg;
     * // Connection via UNIX socket
     * opts = "socket:///tmp/.s.PGSQL.5432[database]"_pg;
     * @endcode
     *
     * @param connstr The connection string to parse
     * @return A populated connection_options structure
     * @see connstring
     */
    static connection_options parse(std::string const &connstr);
};

/**
 * @brief Transaction isolation levels for PostgreSQL
 *
 * The isolation level of a transaction determines what data the transaction
 * can see when other transactions are running concurrently. Higher isolation
 * levels provide stronger guarantees about transaction independence but may
 * reduce concurrency.
 *
 * For detailed information, see the
 * [PostgreSQL
 * documentation](http://www.postgresql.org/docs/current/static/sql-set-transaction.html)
 */
enum class isolation_level {
    read_committed,  /**< Each statement in the transaction sees only rows committed
                        before it began  (default) */
    repeatable_read, /**< All statements in the transaction see only rows committed
                        before the first query is executed */
    serializable /**< All statements in the transaction see only rows committed before
                    the first query, and     transactions can only be committed if they
                    could be executed one at a     time */
};

/**
 * @brief Stream output operator for isolation level
 *
 * Converts an isolation level enum to its string representation for display or logging.
 *
 * @param os The output stream
 * @param val The isolation level to output
 * @return Reference to the output stream after writing
 */
::std::ostream &operator<<(::std::ostream &os, isolation_level val);

/**
 * @brief Transaction mode configuration
 *
 * Specifies the behavior of a database transaction, including its isolation level,
 * read-only status, and deferrable property. These settings affect the transaction's
 * consistency guarantees and performance characteristics.
 */
struct transaction_mode {
    isolation_level isolation =
        isolation_level::read_committed; /**< Isolation level for the transaction */
    bool read_only =
        false; /**< Whether the transaction is read-only (no writes allowed) */
    bool deferrable = false; /**< Whether the transaction is deferrable (only applies to
                                serializable transactions) */

    /**
     * @brief Default constructor
     *
     * Creates a transaction mode with default settings (read_committed, read-write,
     * non-deferrable).
     */
    constexpr transaction_mode() {}

    /**
     * @brief Constructor with explicit parameters
     *
     * Creates a transaction mode with the specified isolation level, read-only flag,
     * and deferrable flag.
     *
     * @param i The isolation level for the transaction
     * @param ro Whether the transaction is read-only (true) or read-write (false)
     * @param def Whether the transaction is deferrable (true) or non-deferrable (false)
     */
    explicit constexpr transaction_mode(isolation_level i, bool ro = false,
                                        bool def = false)
        : isolation{i}
        , read_only{ro}
        , deferrable{def} {}
};

/**
 * @brief Stream output operator for transaction mode
 *
 * Converts a transaction mode structure to its string representation for display or
 * logging.
 *
 * @param os The output stream
 * @param val The transaction mode to output
 * @return Reference to the output stream after writing
 */
::std::ostream &operator<<(::std::ostream &os, transaction_mode const &val);

/**
 * @brief Description of a field returned by the PostgreSQL backend
 *
 * This structure contains metadata about a column in a query result set,
 * including its name, data type, and format. This information is used for
 * properly interpreting and converting the field data.
 */
struct field_description {
    /**
     * @brief The field name.
     *
     * The column name or alias as returned by the query.
     */
    std::string name;

    /**
     * @brief The table OID that the field belongs to
     *
     * If the field can be identified as a column of a specific table,
     * this contains the object ID of the table; otherwise zero.
     */
    integer table_oid;

    /**
     * @brief The column number in the source table
     *
     * If the field can be identified as a column of a specific table,
     * this contains the attribute number of the column; otherwise zero.
     */
    smallint attribute_number;

    /**
     * @brief The object ID of the field's data type
     *
     * Identifies the PostgreSQL data type of the field, which is used
     * for type conversion and formatting.
     */
    oid type_oid;

    /**
     * @brief The data type size in bytes
     *
     * The storage size of the data type (see pg_type.typlen).
     * Note that negative values denote variable-width types.
     */
    smallint type_size;

    /**
     * @brief The type modifier for interpretation
     *
     * The type modifier (see pg_attribute.atttypmod) provides additional
     * type-specific information, such as precision and scale for numeric types
     * or length limits for string types.
     */
    integer type_mod;

    /**
     * @brief The format code for field data
     *
     * Indicates whether the field data is in text (0) or binary (1) format.
     * In a RowDescription returned from the statement variant of Describe,
     * the format code is not yet known and will always be zero.
     */
    protocol_data_format format_code;

    /**
     * @brief Maximum size of the field in bytes
     *
     * The maximum storage size for the field data in the result set.
     */
    integer max_size;
};

/**
 * @brief Collection of field descriptions for a query result
 *
 * Contains metadata for all columns in a query result set, describing
 * the structure of the data returned by a query.
 */
using row_description_type = std::vector<field_description>;

//@{
/** @name Forward declarations */
/**
 * @brief Forward declaration of the resultset class
 *
 * Represents the results of a database query, containing rows and fields.
 */
class resultset;

/**
 * @brief Forward declaration of the basic_connection class
 *
 * Represents a connection to a PostgreSQL database server.
 */
class basic_connection;

/**
 * @brief Namespace containing error classes for PostgreSQL operations
 */
namespace error {
/**
 * @brief Base class for all database errors
 *
 * Provides common functionality for PostgreSQL error handling.
 */
class db_error;

/**
 * @brief Error related to database connection failures
 *
 * Thrown when connection establishment fails or an existing connection is lost.
 */
class connection_error;

/**
 * @brief Error related to query execution failures
 *
 * Thrown when a query cannot be executed or returns an error from the server.
 */
class query_error;
} // namespace error
//@}

//@{
/** @name Pointer types */
/**
 * @brief Shared pointer to a connection object
 *
 * Used for reference-counted management of database connections.
 */
using connection_ptr = std::shared_ptr<basic_connection>;
//@}

/**
 * @brief Client configuration options
 *
 * Map of key-value pairs for configuring client behavior.
 */
using client_options_type = std::map<std::string, std::string>;

/**
 * @brief Sequence of PostgreSQL type OIDs
 *
 * Used for parameter types in prepared statements.
 */
using type_oid_sequence = std::vector<oid>;

/**
 * @brief Simple callback with no parameters
 *
 * Used for notification of events without data.
 */
using simple_callback = std::function<void()>;

/**
 * @brief Callback for error handling
 *
 * Called when a database error occurs to provide custom error handling.
 */
using error_callback = std::function<void(error::db_error const &)>;

/**
 * @brief Callback for query errors
 *
 * Called specifically for errors that occur during query execution.
 */
using query_error_callback = std::function<void(error::query_error const &)>;

/**
 * @brief Namespace containing connection option constants
 *
 * Standard option names for connection configuration.
 */
namespace options {

/**
 * @brief Database server hostname or IP address
 */
const std::string HOST = "host";

/**
 * @brief Database server port number
 */
const std::string PORT = "port";

/**
 * @brief Database user name for authentication
 */
const std::string USER = "user";

/**
 * @brief Database name to connect to
 */
const std::string DATABASE = "database";

/**
 * @brief Character encoding for client-server communication
 */
const std::string CLIENT_ENCODING = "client_encoding";

/**
 * @brief Application name for identification in server logs
 */
const std::string APPLICATION_NAME = "application_name";

} // namespace options

} // namespace pg
} // namespace qb

/**
 * @brief User-defined literal for creating a database alias
 *
 * Allows creating database aliases with a convenient syntax.
 *
 * @code{.cpp}
 * auto alias = "mydb"_db;
 * @endcode
 *
 * @param str The string literal to convert
 * @param n The length of the string literal
 * @return A database alias object
 */
qb::pg::dbalias operator"" _db(const char *str, size_t n);

/**
 * @brief User-defined literal for PostgreSQL connection strings
 *
 * Parses a connection string literal into a connection_options structure.
 * This provides a convenient syntax for creating connection options.
 *
 * @code{.cpp}
 * // Full options for a TCP connection
 * connection_options opts = "aliasname=tcp://user:password@localhost:5432[database]"_pg;
 * // SSL connection over TCP
 * opts = "ssl://localhost:5432[database]"_pg;
 * // Connection via UNIX socket
 * opts = "socket:///tmp/.s.PGSQL.5432[database]"_pg;
 * @endcode
 *
 * @param str The connection string literal to parse
 * @param n The length of the string literal
 * @return A populated connection_options structure
 */
qb::pg::connection_options operator"" _pg(const char *str, size_t n);

#endif /* QBM_PGSQL_NOT_QB_COMMON_H */
