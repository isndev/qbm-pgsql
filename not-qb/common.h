/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

/**
 * @page conversions Datatype conversions
 *
 *    ## PostrgreSQL to C++ datatype conversions
 *
 *    ### Numeric types
 *    [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-numeric.html)
 *    PostgreSQL            | C++
 *    ------------------- | -------------------
 *    smallint            | qb::pg::smallint
 *    integer             | qb::pg::integer
 *    bigint              | qb::pg::bigint
 *    decimal             | @todo decide MPFR or GMP - ?
 *    numeric             | @todo decide MPFR or GMP - ?
 *  real                  | float
 *  double precision      | double
 *  smallserial           | qb::pg::smallint
 *  serial                | qb::pg::integer
 *  bigserial             | qb::pg::bigint
 *
 *  ### Character types
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-character.html)
 *    PostgreSQL          | C++
 *    ------------------- | -------------------
 *  varchar(n)            | std::string
 *  character(n)          | std::string
 *  text                  | std::string
 *
 *  ### Monetary type
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-money.html)
 *    PostgreSQL          | C++
 *    ------------------- | -------------------
 *    money               | @todo decide MPFR or GMP - ?
 *
 *  ### Binary type
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-binary.html)
 *    PostgreSQL          | C++
 *    ------------------- | -------------------
 *    bytea               | qb::pg::bytea
 *
 *  ### Datetime type
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-datetime.html)
 *  [PostgreSQL date/time
 * support](http://www.postgresql.org/docs/9.4/static/datetime-appendix.html)
 *  [Boost.DateTime
 * library](http://www.boost.org/doc/libs/1_58_0/doc/html/date_time.html) PostgreSQL |
 * C++
 *    ------------------- | -------------------
 *  timestamp             |
 *  timestamptz           |
 *  date                  |
 *  time                  |
 *  time with tz          |
 *  interval              |
 *
 *
 *    ### Boolean type
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-boolean.html)
 *    PostgreSQL          | C++
 *    ------------------- | -------------------
 *    boolean             | bool
 *
 *  ### Enumerated types
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-enum.html)
 *
 *  ### Geometric types
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-geometric.html)
 *
 *  ### Network address types
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-net-types.html)
 *
 *    PostgreSQL          | C++
 *    ------------------- | -------------------
 *    cidr                |
 *    inet                | boost::asio::ip::address
 *    macaddr             |
 *
 *  ### Bit String types
 *  [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-bit.html) PostgreSQL
 * | C++
 *    ------------------- | -------------------
 *    bit(n)              | std::bitset<n>
 *    bit varying(n)      | std::bitset<n> @todo create a signature structure
 *
 *    ### Text search types
 *    [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-textsearch.html)
 *
 *    ### UUID type
 *    [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-uuid.html)
 *    PostgreSQL          | C++
 *    ------------------- | -------------------
 *    uuid                | boost::uuid
 *
 *    ### XML type
 *    [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-xml.html) PostgreSQL
 * | C++
 *    ------------------- | -------------------
 *    xml                 | std::string
 *
 *    ### JSON types
 *    [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/datatype-json.html)
 *    PostgreSQL          | C++
 *    ------------------- | -------------------
 *    json                | std::string
 *
 *    ### Arrays
 *    [PostgreSQL documentation](http://www.postgresql.org/docs/9.4/static/arrays.html)
 *    PostgreSQL          | C++
 *    ------------------- | -------------------
 *    type array(n)       | std::vector< type mapping >
 *
 *    ### Composite types
 *    [PostgreSQL documentation](http://www.postgresql.org/docs/9.4/static/rowtypes.html)
 *
 *    ### Range types
 *    [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/rangetypes.html) PostgreSQL
 * | C++
 *    ------------------- | -------------------
 *    int4range           |
 *    int8range           |
 *    numrange            |
 *    tsrange             |
 *    tstzrange           |
 *    daterange           |
 */

#ifndef QBM_PGSQL_NOT_QB_COMMON_H
#define QBM_PGSQL_NOT_QB_COMMON_H

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <qb/uuid.h>

#include "pg_types.h"
#include "util/streambuf.h"

namespace qb {
namespace pg {

/**
 * @brief Binary data, matches PostgreSQL `bytea` type
 */
struct bytea : std::vector<byte> {
    using base_type = std::vector<byte>;

    bytea()
        : base_type() {}

    bytea(std::initializer_list<byte> args)
        : base_type(args) {}

    void
    swap(bytea &rhs) {
        base_type::swap(rhs);
    }

    void
    swap(base_type &rhs) {
        base_type::swap(rhs);
    }
};

using field_buffer = qb::util::input_iterator_buffer;

/**
 * @brief Short unique string to refer a database.
 * Signature structure, to pass instead of connection string
 * @see @ref connstring
 * @see qb::pg::db_service
 */
struct dbalias : std::string {
    using base_type = std::string;

    dbalias()
        : base_type() {}
    explicit dbalias(std::string const &rhs)
        : base_type(rhs) {}

    void
    swap(dbalias &rhs) /* no_throw */
    {
        base_type::swap(rhs);
    }
    void
    swap(std::string &rhs) /* no_throw */
    {
        base_type::swap(rhs);
    }

    dbalias &
    operator=(std::string const &rhs) {
        dbalias tmp(rhs);
        swap(tmp);
        return *this;
    }
};

/**
 * @brief Postgre connection options
 */
struct connection_options {
    dbalias alias;      /**< Database alias */
    std::string schema; /**< Database connection schema. Currently supported are tcp and
                           socket */
    std::string uri; /**< Database connection uri. `host:port` for tcp, `/path/to/file`
                        for socket */
    std::string database; /**< Database name */
    std::string user;     /**< Database user name */
    std::string password; /**< Database user's password */

    /**
     * Generate an alias from username, database and uri if the alias was not
     * provided.
     */
    void generate_alias();
    /**
     * Parse a connection string
     * @code{.cpp}
     * // Full options for a TCP connection
     * connection_options opts =
     * "aliasname=tcp://user:password@localhost:5432[database]"_pg;
     * // SSL connection over TCP
     * opts = "ssl://localhost:5432[database]"_pg;
     * // Connection via UNIX socket
     * opts = "socket:///tmp/.s.PGSQL.5432[database]"_pg;
     * @endcode
     * @see connstring
     */
    static connection_options parse(std::string const &);
};

/**
 * The isolation level of a transaction determines what data the transaction
 * can see when other transactions are running concurrently
 * [PostgreSQL
 * documentation](http://www.postgresql.org/docs/9.4/static/sql-set-transaction.html)
 */
enum class isolation_level {
    read_committed,  //!< read_committed
    repeatable_read, //!< repeatable_read
    serializable     //!< serializable
};

::std::ostream &operator<<(::std::ostream &os, isolation_level val);

struct transaction_mode {
    isolation_level isolation = isolation_level::read_committed;
    bool read_only = false;
    bool deferrable = false;

    constexpr transaction_mode() {}
    explicit constexpr transaction_mode(isolation_level i, bool ro = false,
                                        bool def = false)
        : isolation{i}
        , read_only{ro}
        , deferrable{def} {}
};

::std::ostream &operator<<(::std::ostream &os, transaction_mode const &val);

/**
 * @brief Description of a field returned by the backend
 */
struct field_description {
    /** @brief The field name.
     */
    std::string name;
    /** @brief If the field can be identified as a column of a specific table,
     * the object ID of the table; otherwise zero.
     */
    integer table_oid;
    /** @brief If the field can be identified as a column of a specific table,
     * the attribute number of the column; otherwise zero.
     */
    smallint attribute_number;
    /** @brief The object ID of the field's data type. */
    oid type_oid;
    /** @brief The data type size (see pg_type.typlen). Note that negative
     * values denote variable-width types.
     */
    smallint type_size;
    /** @brief The type modifier (see pg_attribute.atttypmod). The meaning of
     * the modifier is type-specific.
     */
    integer type_mod;
    /**
     * @brief The format code being used for the field.
     * Currently will be zero (text) or one (binary). In a RowDescription
     * returned from the statement variant of Describe, the format code is not
     * yet known and will always be zero.
     */
    protocol_data_format format_code;
    integer max_size; /**< Maximum size of the field in the result set */
};
/**
 * @brief Result set's row description
 */
using row_description_type = std::vector<field_description>;

//@{
/** @name Forward declarations */
class resultset;
// class transaction;
class basic_connection;
namespace error {
class db_error;
class connection_error;
class query_error;
} // namespace error
//@}
//@{
/** @name Pointer types */
// using transaction_ptr = std::shared_ptr<transaction>;
using connection_ptr = std::shared_ptr<basic_connection>;
//@}

/** @brief  */
using client_options_type = std::map<std::string, std::string>;
using type_oid_sequence = std::vector<oid>;

using simple_callback = std::function<void()>;
/** @brief Callback for error handling */
using error_callback = std::function<void(error::db_error const &)>;
/** @brief Callback for starting a transaction */
// using transaction_callback = std::function<void(transaction_ptr)>;

/** @brief Callback for query results */
// using query_result_callback = std::function<void(transaction_ptr, resultset, bool)>;
/** @brief Callback for a query error */
using query_error_callback = std::function<void(error::query_error const &)>;

namespace options {

const std::string HOST = "host";
const std::string PORT = "port";
const std::string USER = "user";
const std::string DATABASE = "database";
const std::string CLIENT_ENCODING = "client_encoding";
const std::string APPLICATION_NAME = "application_name";

} // namespace options

namespace events {
// struct begin {
//     // TODO Transaction read-only, defferreable
//     transaction_callback started;
//     error_callback error;
//     transaction_mode mode = transaction_mode{};
//
//     begin()
//         : started{}
//         , error{} {}
//     begin(transaction_callback tcb, error_callback ecb,
//           transaction_mode const &m = transaction_mode{})
//         : started{tcb}
//         , error{ecb}
//         , mode{m} {}
// };
} // namespace events

} // namespace pg
} // namespace qb

qb::pg::dbalias operator"" _db(const char *, size_t n);

/**
 * User-defined literal for a PostgreSQL connection string
 * @code{.cpp}
 * // Full options for a TCP connection
 * connection_options opts = "aliasname=tcp://user:password@localhost:5432[database]"_pg;
 * // SSL connection over TCP
 * opts = "ssl://localhost:5432[database]"_pg;
 * // Connection via UNIX socket
 * opts = "socket:///tmp/.s.PGSQL.5432[database]"_pg;
 * @endcode
 */
qb::pg::connection_options operator"" _pg(const char *, size_t);

#endif /* QBM_PGSQL_NOT_QB_COMMON_H */
