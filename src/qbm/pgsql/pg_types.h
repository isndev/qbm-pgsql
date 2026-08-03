/**
 * @file pg_types.h
 * @brief PostgreSQL data types and OID definitions
 *
 * This file contains type definitions that map PostgreSQL data types to C++ types,
 * along with Object Identifier (OID) values for PostgreSQL built-in data types.
 * It provides the foundation for type conversion between the database and application.
 *
 * The file includes:
 * - Mappings for basic numeric types (smallint, integer, bigint)
 * - UUID type definitions
 * - PostgreSQL protocol constants
 * - OID enumeration for all standard PostgreSQL data types
 * - Type category enumerations
 * - Protocol data format definitions
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <iosfwd>
#include <limits>
#include <stdexcept>
#include <qb/json.h>
#include <qb/uuid.h>

namespace qb {
namespace pg {

// Basic type definitions
/**
 * @brief 2-byte integer, to match PostgreSQL `smallint` and `smallserial` types
 */
using smallint = int16_t;
/**
 * @brief 2-byte unsigned integer
 */
using usmallint = uint16_t;
/**
 * @brief 4-byte integer, to match PostgreSQL `integer` and `serial` types
 */
using integer = int32_t;
/**
 * @brief 4-byte unsigned integer
 */
using uinteger = uint32_t;

/**
 * @brief Narrow a byte length to the PostgreSQL wire's signed int32 length prefix, rejecting an
 *        oversize value instead of truncating it.
 *
 * Every variable-length bind value is framed as `[int32 byte-length][payload]`. A value ≥ 2 GiB
 * would wrap (or go negative) in the int32 length while its true bytes are still appended,
 * desynchronizing the Bind message and the whole connection stream. This is the single choke point
 * used by EVERY length-prefix write (scalar strings, arrays, json/jsonb, bytea, param serializer)
 * so the invariant cannot be bypassed on any bind path. A parameter this large is realistic on a
 * 64-bit host, so the check is real defense, not just theoretical.
 */
[[nodiscard]] inline integer
checked_param_length(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<integer>::max())) {
        throw std::length_error("qb::pg: parameter value exceeds the 2 GiB PostgreSQL wire length limit");
    }
    return static_cast<integer>(size);
}
/**
 * @brief 8-byte integer, to match PostgreSQL `bigint` and `bigserial` types
 */
using bigint = int64_t;
/**
 * @brief 8-byte unsigned integer
 */
using ubigint = uint64_t;

/**
 * @brief UUID type definition
 *
 * Maps PostgreSQL's UUID type to qb's UUID implementation.
 * Standard 128-bit universally unique identifier.
 */
using uuid = qb::uuid;

/**
 * @brief JSON type definition
 *
 * Maps PostgreSQL's JSON type to qb's JSON implementation.
 * Represents JSON data stored in a vector.
 */
using json = qb::json;

/**
 * @brief JSONB type definition
 *
 * Maps PostgreSQL's JSONB type to qb's JSON implementation.
 * Represents JSON data stored in a vector.
 */
using jsonb = qb::jsonb;

/**
 * @brief PostgreSQL protocol version
 *
 * Version 3.0 of the PostgreSQL wire protocol.
 * Encoded as (major << 16), where major is 3.
 */
constexpr const integer PROTOCOL_VERSION = (3 << 16); // 3.0

/**
 * @brief Maximum value of a single message's 32-bit length field (inclusive of the 4
 *        length bytes), for memory / DoS safety on the client.
 *
 * PostgreSQL can send large CopyData chunks; 256 MiB is a practical ceiling for this
 * client. Raise only if you control server limits and need larger single messages.
 */
constexpr uinteger PG_PROTOCOL_MAX_MESSAGE_BYTES = 256U * 1024U * 1024U;

/**
 * @brief 1-byte char or byte type.
 *
 * Basic unit for binary data storage and transmission.
 */
using byte = char;

/**
 * @brief Binary data, matches PostgreSQL `bytea` type
 *
 * Represents arbitrary binary data stored in a vector.
 * Extends std::vector<byte> with additional convenience methods.
 */
struct bytea : std::vector<byte> {
    /**
     * @brief Base type for bytea (std::vector<byte>)
     */
    using base_type = std::vector<byte>;

    /**
     * @brief Default constructor, creates an empty bytea
     */
    bytea()
        : base_type() {}

    /**
     * @brief Constructor from initializer list of bytes
     * @param args List of bytes to initialize with
     */
    bytea(std::initializer_list<byte> args)
        : base_type(args) {}

    /**
     * @brief Swap contents with another bytea
     * @param rhs The bytea to swap with
     */
    void
    swap(bytea &rhs) {
        base_type::swap(rhs);
    }

    /**
     * @brief Swap contents with a std::vector<byte>
     * @param rhs The vector to swap with
     */
    void
    swap(base_type &rhs) {
        base_type::swap(rhs);
    }
};

/**
 * @brief Nullable data type wrapper
 *
 * A template that wraps any type to make it nullable,
 * representing PostgreSQL's NULL value concept.
 *
 * @tparam T The type that can be nullable
 * @see [std::optional
 * documentation](http://www.boost.org/doc/libs/1_58_0/libs/optional/doc/html/index.html)
 */
template <typename T>
using nullable = std::optional<T>;

/**
 * @brief Object Identifier (OID) enumeration for PostgreSQL data types
 *
 * Contains the standard OIDs for all built-in PostgreSQL data types.
 * These values match those in the pg_type system catalog table.
 * OIDs are used throughout the PostgreSQL system to identify types, tables, and other
 * objects.
 */
enum class oid : int {
    invalid           = 0,    /**< InvalidOid: no type / unmapped (PostgreSQL InvalidOid) */
    boolean           = 16,   /**< Boolean type (true/false) */
    bytea             = 17,   /**< Variable-length binary data */
    char_             = 18,   /**< Single character */
    name              = 19,   /**< System identifier (limited-length string) */
    int8              = 20,   /**< 8-byte integer (bigint) */
    int2              = 21,   /**< 2-byte integer (smallint) */
    int2_vector       = 22,   /**< Array of int2, used in system tables */
    int4              = 23,   /**< 4-byte integer (integer) */
    regproc           = 24,   /**< Registered procedure */
    text              = 25,   /**< Variable-length string */
    oid_t             = 26,   /**< Object identifier type (renamed to avoid conflicts) */
    tid               = 27,   /**< Tuple identifier (physical location of row) */
    xid               = 28,   /**< Transaction identifier */
    cid               = 29,   /**< Command identifier */
    oid_vector        = 30,   /**< Array of OIDs, used in system tables */
    json              = 114,  /**< JSON data */
    xml               = 142,  /**< XML data */
    pg_node_tree      = 194,  /**< PostgreSQL internal node tree representation */
    pg_ddl_command    = 32,   /**< PostgreSQL internal DDL command representation */
    point             = 600,  /**< Geometric point (x,y) */
    lseg              = 601,  /**< Geometric line segment */
    path              = 602,  /**< Geometric path */
    box               = 603,  /**< Geometric box */
    polygon           = 604,  /**< Geometric polygon */
    line              = 628,  /**< Geometric line */
    float4            = 700,  /**< 4-byte floating-point number */
    float8            = 701,  /**< 8-byte floating-point number */
    abstime           = 702,  /**< Absolute time (deprecated) */
    reltime           = 703,  /**< Relative time (deprecated) */
    tinterval         = 704,  /**< Time interval (deprecated) */
    unknown           = 705,  /**< Unknown type */
    circle            = 718,  /**< Geometric circle */
    cash              = 790,  /**< Monetary amount */
    macaddr           = 829,  /**< MAC address */
    inet              = 869,  /**< IPv4 or IPv6 address */
    cidr              = 650,  /**< IPv4 or IPv6 network */
    json_array        = 199,  /**< Array of json */
    boolean_array     = 1000, /**< Array of boolean */
    bytea_array       = 1001, /**< Array of bytea */
    int2_array        = 1005, /**< Array of int2 */
    int4_array        = 1007, /**< Array of int4 */
    text_array        = 1009, /**< Array of text */
    bpchar_array      = 1014, /**< Array of bpchar (char(n)) */
    varchar_array     = 1015, /**< Array of varchar */
    int8_array        = 1016, /**< Array of int8 */
    oid_array         = 1028, /**< Array of OIDs */
    float4_array      = 1021, /**< Array of float4 */
    float8_array      = 1022, /**< Array of float8 */
    acl_item          = 1033, /**< Access control list item */
    cstring_array     = 1263, /**< Array of C strings */
    bpchar            = 1042, /**< Blank-padded string (char(n)) */
    varchar           = 1043, /**< Variable-length string with limit (varchar(n)) */
    date              = 1082, /**< Date */
    time              = 1083, /**< Time of day */
    timestamp         = 1114, /**< Date and time */
    timestamptz       = 1184, /**< Date and time with time zone */
    interval          = 1186, /**< Time interval */
    timetz            = 1266, /**< Time of day with time zone */
    date_array        = 1182, /**< Array of date */
    time_array        = 1183, /**< Array of time */
    timestamp_array   = 1115, /**< Array of timestamp */
    timestamptz_array = 1185, /**< Array of timestamptz */
    interval_array    = 1187, /**< Array of interval */
    timetz_array      = 1270, /**< Array of timetz */
    numeric_array     = 1231, /**< Array of numeric */
    bit               = 1560, /**< Fixed-length bit string */
    varbit            = 1562, /**< Variable-length bit string */
    numeric           = 1700, /**< Exact numeric with selectable precision */
    refcursor         = 1790, /**< Reference to cursor */
    regprocedure      = 2202, /**< Registered procedure (with args) */
    regoper           = 2203, /**< Registered operator */
    regoperator       = 2204, /**< Registered operator (with args) */
    regclass          = 2205, /**< Registered class */
    regtype           = 2206, /**< Registered type */
    regrole           = 4096, /**< Registered role */
    regtypearray      = 2211, /**< Array of registered types */
    uuid              = 2950, /**< Universally unique identifier */
    uuid_array        = 2951, /**< Array of uuid */
    lsn               = 3220, /**< Log sequence number */
    tsvector          = 3614, /**< Text search vector */
    gtsvector         = 3642, /**< GiST index internal text search vector */
    tsquery           = 3615, /**< Text search query */
    regconfig         = 3734, /**< Registered text search configuration */
    regdictionary     = 3769, /**< Registered text search dictionary */
    jsonb             = 3802, /**< Binary JSON data */
    jsonb_array       = 3807, /**< Array of jsonb */
    int4_range        = 3904, /**< Range of integers */
    record            = 2249, /**< Anonymous composite type */
    record_array      = 2287, /**< Array of records */
    cstring           = 2275, /**< C string */
    any               = 2276, /**< Pseudo-type representing any type */
    any_array         = 2277, /**< Pseudo-type representing any array */
    void_             = 2278, /**< Pseudo-type representing void */
    trigger           = 2279, /**< Trigger function */
    evttrigger        = 3838, /**< Event trigger function */
    language_handler  = 2280, /**< Language handler */
    internal          = 2281, /**< Internal data type */
    opaque            = 2282, /**< Obsolete, used for function parameters */
    any_element       = 2283, /**< Pseudo-type representing any array element */
    any_non_array     = 2776, /**< Pseudo-type representing any non-array type */
    any_enum          = 3500, /**< Pseudo-type representing any enum type */
    fdw_handler       = 3115, /**< Foreign data wrapper handler */
    any_range         = 3831  /**< Pseudo-type representing any range type */
};

/**
 * @brief Output stream operator for OID values
 *
 * Allows printing OID enum values to output streams.
 *
 * @param out The output stream
 * @param val The OID value to output
 * @return std::ostream& Reference to the output stream
 */
std::ostream &operator<<(std::ostream &out, oid val);

/**
 * @brief Input stream operator for OID values
 *
 * Allows reading OID enum values from input streams.
 *
 * @param in The input stream
 * @param val The OID value to read into
 * @return std::istream& Reference to the input stream
 */
std::istream &operator>>(std::istream &in, oid &val);

/**
 * @brief Map a scalar element type OID to its canonical PostgreSQL array type OID.
 *
 * Used when binding a `std::vector<T>` parameter: the Bind message must carry the
 * concrete array OID (e.g. `_int4` = 1007), never the `anyarray` pseudo-type (2277),
 * which PostgreSQL rejects as an invalid parameter type. Returns `oid::invalid` (0)
 * for an element type that has no canonical array companion here, so the caller can
 * fail loudly instead of emitting a wire-invalid Bind.
 *
 * @param element The scalar element type OID.
 * @return The array type OID, or oid::invalid if unmapped.
 */
constexpr oid
array_oid_for_element(oid element) noexcept {
    switch (element) {
        case oid::boolean:
            return oid::boolean_array;
        case oid::bytea:
            return oid::bytea_array;
        case oid::int2:
            return oid::int2_array;
        case oid::int4:
            return oid::int4_array;
        case oid::int8:
            return oid::int8_array;
        case oid::oid_t:
            return oid::oid_array;
        case oid::float4:
            return oid::float4_array;
        case oid::float8:
            return oid::float8_array;
        case oid::numeric:
            return oid::numeric_array;
        case oid::text:
            return oid::text_array;
        case oid::varchar:
            return oid::varchar_array;
        case oid::bpchar:
            return oid::bpchar_array;
        case oid::uuid:
            return oid::uuid_array;
        case oid::json:
            return oid::json_array;
        case oid::jsonb:
            return oid::jsonb_array;
        case oid::date:
            return oid::date_array;
        case oid::time:
            return oid::time_array;
        case oid::timetz:
            return oid::timetz_array;
        case oid::timestamp:
            return oid::timestamp_array;
        case oid::timestamptz:
            return oid::timestamptz_array;
        case oid::interval:
            return oid::interval_array;
        default:
            return oid::invalid;
    }
}

/**
 * @brief Type codes for various PostgreSQL types
 *
 * These codes identify the fundamental category of a PostgreSQL type
 * as defined in pg_type.typtype.
 */
enum class code : char {
    base       = 'b', /**< Base data type (e.g., int4) */
    composite  = 'c', /**< Composite type (e.g., a table row type) */
    domain     = 'd', /**< Domain over another type */
    enumerated = 'e', /**< Enumerated type */
    pseudo     = 'p', /**< Pseudo-type (e.g., void, anyarray) */
    range      = 'r'  /**< Range type */
};

/**
 * @brief Category codes for PostgreSQL types
 *
 * These codes group PostgreSQL types into broader categories
 * as defined in pg_type.typcategory.
 */
enum class code_category : char {
    invalid        = 0,   /**< Invalid or unknown category */
    array          = 'A', /**< Array types */
    boolean        = 'B', /**< Boolean types */
    composite      = 'C', /**< Composite types */
    datetime       = 'D', /**< Date/time types */
    enumeration    = 'E', /**< Enum types */
    geometric      = 'G', /**< Geometric types */
    network        = 'I', /**< Network address types */
    numeric        = 'N', /**< Numeric types */
    pseudotype     = 'P', /**< Pseudo-types */
    range_category = 'R', /**< Range types */
    string         = 'S', /**< String types */
    timespan       = 'T', /**< Timespan types */
    user           = 'U', /**< User-defined types */
    bitstring      = 'V', /**< Bit-string types */
    unknown        = 'X'  /**< Unknown type */
};

/**
 * @brief Data format constants for protocol communication
 *
 * Defines whether data is transmitted in text or binary format
 * in the PostgreSQL wire protocol. These formats determine how
 * data is encoded and decoded during client-server communication.
 */
enum class protocol_data_format : smallint {
    Text   = 0, /**< Text format: data represented as ASCII strings */
    Binary = 1  /**< Binary format: compact representation of data */
};

} // namespace pg
} // namespace qb
