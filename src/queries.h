/**
 * @file queries.h
 * @brief PostgreSQL query representation and management
 *
 * This file defines the data structures and classes for representing
 * and managing SQL queries for the PostgreSQL client, including:
 *
 * - Storage for prepared queries
 * - Parameter binding for prepared statements
 * - SQL query execution with callbacks
 * - Various query types (BEGIN, COMMIT, ROLLBACK, etc.)
 *
 * The implementation follows the PostgreSQL protocol for preparing
 * and executing queries, supporting both simple and prepared statements.
 *
 * @see qb::pg::detail::Transaction
 * @see qb::pg::detail::Database
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

#pragma once

#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <qb/io.h>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/branch_hints.h>
#include <string_view>
#include <type_traits>
#include <vector>

#include "./common.h"
#include "./error.h"
#include "./param_serializer.h"
#include "./protocol.h"
#include "./type_mapping.h"

namespace qb::pg::detail {
using namespace qb::pg;

/**
 * @brief Structure for storing a prepared query definition
 *
 * Contains all the information needed to execute a prepared statement,
 * including its name, SQL expression, parameter types, and result description.
 */
struct PreparedQuery {
    std::string          name;        ///< Name of the prepared query
    std::string          expression;  ///< SQL expression
    std::vector<oid>     param_types; ///< Types of parameters (was type_oid_sequence)
    row_description_type row_description; ///< Description of result columns
};

/**
 * @brief Storage for prepared queries
 *
 * Provides a central repository for all prepared statements in the
 * database session, allowing them to be referenced by name.
 */
class PreparedStorage {
    qb::unordered_map<std::string, PreparedQuery>
        _prepared_queries; ///< Map of query names to definitions

public:
    PreparedStorage() = default;

    /**
     * @brief Checks if a prepared query exists
     *
     * @param name Name of the prepared query
     * @return bool True if the query exists, false otherwise
     */
    bool
    has(std::string_view name) const {
        return _prepared_queries.find(std::string(name)) != _prepared_queries.cend();
    }

    /**
     * @brief Adds a prepared query to storage
     *
     * @param query Prepared query to add
     * @return const PreparedQuery& Reference to the stored query
     */
    const PreparedQuery &
    push(PreparedQuery &&query) {
        return _prepared_queries.emplace(query.name, std::move(query)).first->second;
    }

    /**
     * @brief Retrieves a prepared query by name
     *
     * @param name Name of the prepared query
     * @return PreparedQuery const& Reference to the prepared query
     * @throws std::out_of_range If the query doesn't exist
     */
    PreparedQuery const &
    get(std::string_view name) const {
        return _prepared_queries.at(std::string(name));
    }
};

// Maintain backward compatibility
using PreparedQueryStorage = PreparedStorage;

/**
 * @brief Class for managing query parameters
 *
 * Encapsulates parameters for prepared statements, handling
 * type conversion and binary encoding according to PostgreSQL protocol.
 */
class QueryParams {
    std::vector<byte>    _params;      ///< Serialized parameters
    std::vector<integer> _param_types; ///< OIDs for parameter types

public:
    /**
     * @brief Constructs an empty parameter set
     */
    QueryParams() = default;

    /**
     * @brief Constructs parameter set from variadic arguments
     *
     * Uses template argument deduction to convert various parameter types
     * to their PostgreSQL binary representation.
     *
     * @tparam T Parameter types
     * @param args Parameter values
     */
    template <typename... T>
    QueryParams(T &&...args) {
        if constexpr (sizeof...(T) > 0) {
            // Do not use format_codes_buffer, no longer used

            // Serialize parameters directly
            ParamSerializer serializer;
            serializer.serialize_params(std::forward<T>(args)...);

            // Retrieve serialized data
            _params      = serializer.params_buffer();
            _param_types = serializer.param_types();

            // Check if the beginning of the parameter buffer contains a 'B'
            if (!_params.empty() && _params.size() > sizeof(smallint) &&
                _params[sizeof(smallint)] == 'B') {
                LOG_CRIT("[pgsql] CORRUPTION DETECTED in construction: first byte after "
                         "count = 'B'");
            }
        }
    }

    /**
     * @brief Gets the serialized parameters
     *
     * @return std::vector<byte>& Reference to the serialized parameters
     */
    std::vector<byte> &
    get() {
        return _params;
    }

    /**
     * @brief Gets the serialized parameters (const version)
     *
     * @return const std::vector<byte>& Const reference to the serialized parameters
     */
    const std::vector<byte> &
    get() const {
        return _params;
    }

    /**
     * @brief Gets the parameter types
     *
     * @return const std::vector<integer>& Const reference to parameter OIDs
     */
    const std::vector<integer> &
    param_types() const {
        return _param_types;
    }

    /**
     * @brief Gets the number of parameters
     *
     * @return smallint The number of parameters
     */
    smallint
    param_count() const {
        if (_params.size() >= sizeof(smallint)) {
            // Extract the number of parameters from the buffer
            smallint count;
            std::memcpy(&count, _params.data(), sizeof(smallint));
            return ntohs(count); // Convert from network byte order to host byte order
        }
        return 0;
    }

    /**
     * @brief Checks if the parameter set is empty
     *
     * @return bool True if there are no parameters, false otherwise
     */
    bool
    empty() const {
        return _params.empty();
    }
};

/**
 * @brief Interface for SQL queries
 *
 * Base class for all SQL query implementations, providing a common
 * interface for getting the query message and handling callbacks.
 */
class ISqlQuery {
public:
    ISqlQuery() = default;

    virtual ~ISqlQuery() = default;

    /**
     * @brief Checks if the query is valid
     *
     * @return bool True if the query is valid, false otherwise
     */
    virtual bool
    is_valid() const {
        return true;
    }

    /**
     * @brief Gets the PostgreSQL protocol message for the query
     *
     * @return message Message to send to the server
     */
    virtual message get() const = 0;

    /**
     * @brief Called when the query succeeds
     */
    virtual void on_success() const = 0;

    /**
     * @brief Called when the query fails
     *
     * @param err Error information
     */
    virtual void on_error(error::db_error const &err) const = 0;
};

/**
 * @brief Base implementation of SQL query with callbacks
 *
 * Provides a base implementation for SQL queries with success and error callbacks.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class SqlQuery : public ISqlQuery {
    CB_SUCCESS _on_success; ///< Success callback
    CB_ERROR   _on_error;   ///< Error callback

public:
    /**
     * @brief Constructs a SQL query with callbacks
     *
     * @param success Success callback
     * @param error Error callback
     */
    SqlQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : _on_success(std::forward<CB_SUCCESS>(success))
        , _on_error(std::forward<CB_ERROR>(error)) {}

    virtual ~SqlQuery() = default;

    /**
     * @brief Calls the success callback
     */
    void
    on_success() const final {
        _on_success();
    }

    /**
     * @brief Calls the error callback
     *
     * @param err Error information
     */
    void
    on_error(error::db_error const &err) const final {
        _on_error(err);
    }
};

/**
 * @brief Query for beginning a transaction
 *
 * Creates a BEGIN statement with optional transaction mode (isolation level, etc.).
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class BeginQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    transaction_mode _mode; ///< Transaction mode

public:
    /**
     * @brief Constructs a BEGIN query
     *
     * @param mode Transaction mode
     * @param success Success callback
     * @param error Error callback
     */
    BeginQuery(transaction_mode mode, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _mode(mode) {}

    /**
     * @brief Creates the BEGIN message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        ::std::ostringstream cmd;
        cmd << "BEGIN " << _mode;

        LOG_DEBUG("[pgsql] Send BEGIN: \"" << cmd.str() << "\"");
        message m(query_tag);
        m.write(cmd.str());
        return m;
    }
};

/**
 * @brief Query for committing a transaction
 *
 * Creates a COMMIT statement to finalize a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class CommitQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
public:
    /**
     * @brief Constructs a COMMIT query
     *
     * @param success Success callback
     * @param error Error callback
     */
    CommitQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error)) {}

    /**
     * @brief Creates the COMMIT message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send COMMIT");
        message m(query_tag);
        m.write("commit");
        return m;
    }
};

/**
 * @brief Query for rolling back a transaction
 *
 * Creates a ROLLBACK statement to abort a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class RollbackQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
public:
    /**
     * @brief Constructs a ROLLBACK query
     *
     * @param success Success callback
     * @param error Error callback
     */
    RollbackQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error)) {}

    /**
     * @brief Creates the ROLLBACK message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send ROLLBACK");
        message m(query_tag);
        m.write("rollback");
        return m;
    }
};

/**
 * @brief Query for creating a savepoint
 *
 * Creates a SAVEPOINT statement within a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class SavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string const &_name; ///< Savepoint name

public:
    /**
     * @brief Constructs a SAVEPOINT query
     *
     * @param name Savepoint name
     * @param success Success callback
     * @param error Error callback
     */
    SavePointQuery(std::string const &name, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _name(name) {}

    /**
     * @brief Creates the SAVEPOINT message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send SAVEPOINT " << _name);
        message m(query_tag);
        m.write("savepoint " + _name);
        return m;
    }
};

/**
 * @brief Query for releasing a savepoint
 *
 * Creates a RELEASE SAVEPOINT statement within a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class ReleaseSavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string const &_name; ///< Savepoint name

public:
    /**
     * @brief Constructs a RELEASE SAVEPOINT query
     *
     * @param name Savepoint name
     * @param success Success callback
     * @param error Error callback
     */
    ReleaseSavePointQuery(std::string const &name, CB_SUCCESS &&success,
                          CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _name(name) {}

    /**
     * @brief Creates the RELEASE SAVEPOINT message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send RELEASE SAVEPOINT " << _name);
        message m(query_tag);
        m.write("release savepoint " + _name);
        return m;
    }
};

/**
 * @brief Query for rolling back to a savepoint
 *
 * Creates a ROLLBACK TO SAVEPOINT statement within a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class RollbackSavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string const &_name; ///< Savepoint name

public:
    /**
     * @brief Constructs a ROLLBACK TO SAVEPOINT query
     *
     * @param name Savepoint name
     * @param success Success callback
     * @param error Error callback
     */
    RollbackSavePointQuery(std::string const &name, CB_SUCCESS &&success,
                           CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _name(name) {}

    /**
     * @brief Creates the ROLLBACK TO SAVEPOINT message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send ROLLBACK TO SAVEPOINT " << _name);
        message m(query_tag);
        m.write("rollback to savepoint " + _name);
        return m;
    }
};

/**
 * @brief Query for executing a simple SQL statement
 *
 * Creates a query for direct execution of SQL expressions.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class SimpleQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    const std::string _expression; ///< SQL expression

public:
    /**
     * @brief Constructs a simple query
     *
     * @param expr SQL expression
     * @param success Success callback
     * @param error Error callback
     */
    SimpleQuery(std::string &&expr, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _expression(std::move(expr)) {}

    /**
     * @brief Creates the query message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send QUERY \"" << _expression << "\"");
        message m(query_tag);
        m.write(_expression);

        return m;
    }
};

/**
 * @brief Query for preparing a statement
 *
 * Creates a query for preparing a named statement according to the PostgreSQL protocol.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class ParseQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    PreparedQuery const &_query; ///< Prepared query definition

public:
    ParseQuery(PreparedQuery const &query, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _query(query) {}

    [[nodiscard]] message
    get() const final {
        LOG_DEBUG("[pgsql] Send PARSE QUERY \"" << _query.expression << "\"");
        message cmd(parse_tag);
        cmd.write(_query.name);
        cmd.write(_query.expression);
        cmd.write((smallint) _query.param_types.size());
        for (auto oid_val : _query.param_types) {
            cmd.write(
                static_cast<integer>(oid_val)); // déjà un integer, pas besoin de cast
        }

        message describe(describe_tag);
        describe.write('S');
        describe.write(_query.name);
        cmd.pack(describe);
        cmd.pack(message(sync_tag));

        return cmd;
    }
};

/**
 * @brief Prepared statement execution
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class ExecuteQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    const PreparedStorage &_storage;    ///< Prepared statement storage
    std::string            _query_name; ///< Query name to execute
    QueryParams            _params;     ///< Query parameters

public:
    /**
     * @brief Constructs an execute query
     *
     * @param storage Prepared statement storage
     * @param query_name Query name to execute
     * @param params Query parameters
     * @param success Success callback
     * @param error Error callback
     */
    ExecuteQuery(const PreparedStorage &storage, std::string_view query_name,
                 QueryParams &&params, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _storage(storage)
        , _query_name(query_name)
        , _params(std::move(params)) {}

    bool
    is_valid() const final {
        if (qb::likely(_storage.has(_query_name)))
            return true;
        LOG_CRIT("[pgsql] Error prepared query " << _query_name << " not registered");
        return false;
    }

    message
    get() const final {
        const auto &query = _storage.get(_query_name);
        message     cmd(bind_tag);

        // Exact format expected by PostgreSQL for a Bind message:
        // 1. Portal name (empty = unnamed)
        cmd.write("");

        // 2. Prepared statement name
        cmd.write(query.name);

        // 3. Format codes section - 1 code for all parameters
        // Format 1 = binary
        cmd.write((smallint) 1); // Number of format codes
        cmd.write((smallint) 1); // Format = 1 (binary)

        // 4. Total number of parameters
        smallint param_count = _params.param_count();
        cmd.write(param_count);

        // 5. Parameter values
        if (!_params.empty() && param_count > 0) {
            // Skip the count in the parameters buffer
            const std::vector<byte> &param_buffer = _params.get();
            if (param_buffer.size() > sizeof(smallint)) {
                const byte *data      = param_buffer.data() + sizeof(smallint);
                size_t      data_size = param_buffer.size() - sizeof(smallint);

                // Copy the raw data
                auto out = cmd.output();
                std::copy(data, data + data_size, out);
            }
        }

        // 6. Result format = 0 (none)
        cmd.write((smallint) 0);

        // 7. Execute message (empty portal, no row limit)
        message execute(execute_tag);
        execute.write("");
        execute.write(0);
        cmd.pack(execute);

        // 8. Sync message
        cmd.pack(message(sync_tag));
        return cmd;
    }
};

} // namespace qb::pg::detail
