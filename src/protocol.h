/**
 * @file protocol.h
 * @brief PostgreSQL wire protocol implementation
 *
 * This file implements the PostgreSQL wire protocol (version 3.0) used for
 * client-server communication. It defines message formats, tags, and data
 * structures used in the protocol.
 *
 * The implementation includes:
 * - Message tag definitions for all protocol messages
 * - Authentication method constants
 * - Message parsing and construction utilities
 * - Row and field data handling
 *
 * @see https://www.postgresql.org/docs/current/protocol.html
 * @see https://www.postgresql.org/docs/current/protocol-message-formats.html
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

#include <functional>
#include <iosfwd>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include "common.h"

namespace qb {
namespace pg {
namespace detail {

/**
 * @brief PostgreSQL message tags
 *
 * These single-byte codes identify message types in the PostgreSQL protocol.
 * Each message begins with a tag byte followed by message length and payload.
 *
 * Tags are marked as:
 * - (F) Frontend (client to server)
 * - (B) Backend (server to client)
 * - (B&F) Both directions
 *
 * @see https://www.postgresql.org/docs/current/protocol-message-formats.html
 */
enum message_tag {
    empty_tag             = '\0', /** (F) Startup message, SSL request, cancel request */
    authentication_tag    = 'R',  /**< (B) All authentication requests begin with 'R' */
    backend_key_data_tag  = 'K',  /**< (B) Backend key data (cancellation key data) */
    bind_tag              = 'B',  /**< (F) Bind parameters command */
    bind_complete_tag     = '2',  /**< (B) Server's reply to bind complete command */
    close_tag             = 'C',  /**< (F) Close prepared statement or a portal */
    close_complete_tag    = '3',  /**< (B) Server's acknowledgment of close command */
    command_complete_tag  = 'C',  /**< (B) Command completed normally */
    copy_data_tag         = 'd',  /**< (B&F) Data for COPY operation */
    copy_done_tag         = 'c',  /**< (B&F) COPY data transfer completed */
    copy_fail_tag         = 'f',  /**< (F) COPY failed */
    copy_in_response_tag  = 'G',  /**< (B) Server ready for COPY from client */
    copy_out_response_tag = 'H',  /**< (B) Server ready for COPY to client */
    copy_both_response_tag = 'W', /**< (B) Server ready for bidirectional COPY */
    data_row_tag           = 'D', /**< (B) Data row in a query result */
    describe_tag = 'D', /**< (F) Request detail about a prepared statement or portal */
    empty_query_response_tag  = 'I', /**< (B) Response to an empty query string */
    error_response_tag        = 'E', /**< (B) Error report */
    execute_tag               = 'E', /**< (F) Execute a prepared statement */
    flush_tag                 = 'H', /**< (F) Force server to deliver pending output */
    function_call_tag         = 'F', /**< (F) Function call request */
    function_call_resp_tag    = 'V', /**< (B) Function call result */
    no_data_tag               = 'n', /**< (B) No data to return */
    notice_response_tag       = 'N', /**< (B) Notice report */
    notification_resp_tag     = 'A', /**< (B) Asynchronous notification */
    parameter_description_tag = 't', /**< (B) Statement parameter description */
    parameter_status_tag      = 'S', /**< (B) Server parameter status report */
    parse_tag                 = 'P', /**< (F) Parse a prepared statement */
    parse_complete_tag        = '1', /**< (B) Parse complete */
    password_message_tag      = 'p', /**< (F) Password response */
    portal_suspended_tag = 's', /**< (B) Portal execution suspended (partial result) */
    query_tag            = 'Q', /**< (F) Simple query */
    ready_for_query_tag  = 'Z', /**< (B) Server is ready for a new query */
    row_description_tag  = 'T', /**< (B) Row description for query results */
    sync_tag             = 'S', /**< (F) Synchronize command sequence */
    terminate_tag        = 'X', /**< (F) Terminate session */
};
typedef std::set<message_tag> tag_set_type;

/**
 * @brief Authentication methods used by PostgreSQL
 *
 * These values are used in authentication request messages to indicate
 * what type of authentication the server requires from the client.
 *
 * @see https://www.postgresql.org/docs/current/protocol-flow.html#PROTOCOL-FLOW-AUTH
 */
enum auth_states {
    OK         = 0, /**< Specifies that the authentication was successful. */
    KerberosV5 = 2, /**< Specifies that Kerberos V5 authentication is required. */
    Cleartext  = 3, /**< Specifies that a clear-text password is required. */
    /**
     * Specifies that an MD5-encrypted password is required.
     * Message contains additional 4 bytes of salt
     */
    MD5Password   = 5,
    SCMCredential = 6, /**< Specifies that an SCM credentials message is required. */
    GSS           = 7, /**< Specifies that GSSAPI authentication is required. */
    /**
     * Specifies that this message contains GSSAPI or SSPI data.
     * Message contains additional bytes with GSSAPI or SSPI authentication data.
     */
    GSSContinue  = 8,
    SSPI         = 9,  /**< Specifies that SSPI authentication is required. */
    SCRAM_SHA256 = 10, /**< Specifies that SCRAM-SHA-256 authentication is required. */
    SCRAM_SHA256_CLIENT_PROOF = 11, /**< Message contains SCRAM-SHA-256 client proof. */
    SCRAM_SHA256_SERVER_CHECK =
        12 /**< Message contains SCRAM-SHA-256 server signature. */
};

struct row_data;
struct notice_message;

/**
 * @brief On-the-wire message of PostgreSQL protocol v3
 *
 * This class represents a message in the PostgreSQL wire protocol,
 * providing methods to read from and write to the message buffer.
 * It handles all the message encoding/decoding according to the protocol.
 *
 * @see https://www.postgresql.org/docs/current/protocol-message-formats.html
 */
class message {
public:
    /** Buffer type for the message */
    typedef std::vector<char> buffer_type;

    /** Input iterator for the message buffer */
    typedef buffer_type::const_iterator const_iterator;
    /** Output iterator for the message buffer */
    typedef std::back_insert_iterator<buffer_type> output_iterator;

    /** A range of iterators for input */
    typedef std::pair<const_iterator, const_iterator> const_range;

    /** Length type for the message */
    typedef uinteger size_type;

public:
    /**
     * @brief Construct message for reading from the stream
     */
    message();

    /**
     * @brief Construct message for sending to the backend
     *
     * The tag must be one of allowed for the frontend.
     *
     * @param tag Message type tag
     */
    explicit message(message_tag tag);

    /**
     * @brief Message is noncopyable
     */
    message(message const &) = delete;

    /**
     * @brief Message is move-only
     *
     * @param msg Message to move from
     */
    message(message &&msg);

    /**
     * @brief Get the message tag
     *
     * @return message_tag PostgreSQL message tag
     */
    message_tag tag() const;

    /**
     * @brief Get the message length encoded in payload
     *
     * @return size_type Length in bytes (bytes 1-4)
     */
    size_type length() const;

    /**
     * @brief Get size of payload, minus 1 (for the tag)
     *
     * When reading from stream, it must match the length
     *
     * @return size_t Payload size in bytes
     */
    size_t size() const;

    /**
     * @brief Get full size of buffer including the tag
     *
     * @return size_t Total buffer size in bytes
     */
    size_t buffer_size() const;

    /**
     * @brief Get a pair of iterators for constructing buffer
     *
     * Used for writing the message into the network stream
     *
     * @return const_range Iterators for the full buffer
     */
    const_range buffer() const;

    /**
     * @brief Get iterator to current read position
     *
     * @return const_iterator Iterator to current read position
     */
    const_iterator input() const;

    /**
     * @brief Get an iterator for writing into the buffer
     *
     * @return output_iterator Output iterator for the buffer
     */
    output_iterator output();

    //@{
    /** @name Stream read interface */
    /**
     * Move the read iterator to the beginning of actual payload
     */
    void reset_read();

    /**
     * Read a byte from the message buffer
     * @param c the char
     * @return true if the operation was successful
     */
    bool read(char &);

    /**
     * Read a two-byte integer from the message buffer
     * @param v the integer
     * @return true if the operation was successful
     */
    bool read(smallint &);

    /**
     * Read a 4-byte integer from the message buffer
     * @param v the integer
     * @return true if the operation was successful
     */
    bool read(integer &);

    /**
     * Read a null-terminated string from the message buffer
     * @param s the string
     * @return true if the operation was successful
     */
    bool read(std::string &);

    /**
     * Read n bytes from message to the string.
     * @param s the string
     * @param n nubmer of bytes
     * @return true if the operation was successful
     */
    bool read(std::string &, size_t n);

    /**
     * Read field description from the message buffer
     * @param fd field description
     * @return true if the operation was successful
     */
    bool read(field_description &fd);

    /**
     * @brief Read data row from the message buffer
     * @param row data row
     * @return true if the operation was successful
     */
    bool read(row_data &row);

    /**
     * @brief Read a notice or error message form the message buffer
     * @param notice
     * @return true if the operation was successful
     */
    bool read(notice_message &notice);
    //@}

    //@{
    /** @name Stream write interface */
    /**
     * Write char to the message buffer
     * @param c the char
     */
    void write(char);

    /**
     * Write a 2-byte integer to the message buffer
     * @param v the integer
     */
    void write(smallint);

    /**
     * Write a 4-byte integer to the message buffer
     * @param v the integer
     */
    void write(integer);

    /**
     * Write a string to the message buffer.
     * Includes terminating '\0'
     * @param s the string
     */
    void write(std::string const &);

    /**
     * Write a string view to the message buffer.
     * Includes terminating '\0'
     * @param s the string
     */
    void write_sv(std::string_view const &);

    /**
     * Pack another message into this one.
     * Copies the buffer, doesn't preserve the other message.
     * @param m
     */
    void pack(message const &);
    //@}

    /**
     * Get the set of allowed message tags for frontend
     */
    static tag_set_type const &frontend_tags();

    /**
     * Get the set of allowed message tags for backend
     */
    static tag_set_type const &backend_tags();

private:
    mutable buffer_type payload;
    const_iterator      curr_;
    bool                packed_;
};

/** @brief Shared pointer to message */
typedef std::shared_ptr<message> message_ptr;

/**
 * @brief Data row from a query result
 *
 * Represents a row of data returned from a PostgreSQL query,
 * providing access to individual field values. The row data is stored
 * in binary format as received from the server, with support for
 * handling NULL values and converting field data to appropriate types.
 */
struct row_data {
    /** @brief Buffer type for raw field data */
    typedef std::vector<byte> data_buffer;
    /** @brief Iterator type for data buffer */
    typedef data_buffer::const_iterator const_data_iterator;
    /** @brief Range of iterators for a field's data */
    typedef std::pair<const_data_iterator, const_data_iterator> data_buffer_bounds;

    /** @brief Type for field index and count */
    typedef uint16_t size_type;
    /** @brief Type for field offsets within data buffer */
    typedef std::vector<integer> offsets_type;
    /** @brief Type for tracking which fields are NULL */
    typedef std::set<size_type> null_map_type;

    /** @brief Field offsets within the data buffer */
    offsets_type offsets;
    /** @brief Raw field data */
    data_buffer data;
    /** @brief Set of indices for NULL fields */
    null_map_type null_map;

    /** @brief Default constructor */
    row_data() = default;

    /** @brief Move constructor */
    row_data(row_data &&) = default;

    /** @brief Move assignment operator */
    row_data &operator=(row_data &&) = default;

    /**
     * @brief Get the number of fields in the row
     *
     * @return size_type Number of fields
     */
    size_type size() const;

    /**
     * @brief Check if the row is empty (has no fields)
     *
     * @return bool True if the row is empty
     */
    bool empty() const;

    /**
     * @brief Check if a field contains NULL
     *
     * @param index Field index (0-based)
     * @return true if the value in the field is null
     * @throw std::out_of_range if the index is out of range
     */
    bool is_null(size_type index) const;

    /**
     * @brief Get iterators to the raw data for a field
     *
     * @param index Field index (0-based)
     * @return data_buffer_bounds Pair of iterators to the field data
     * @throw std::out_of_range if the index is out of range
     * @throw value_is_null if the field is null
     */
    data_buffer_bounds field_buffer_bounds(size_type index) const;

    /**
     * @brief Get a field_buffer object for a field
     *
     * The field_buffer can be used with type conversion functions
     *
     * @param index Field index (0-based)
     * @return field_buffer Field buffer for value conversion
     * @throw std::out_of_range if the index is out of range
     * @throw value_is_null if the field is null
     */
    field_buffer field_data(size_type index) const;

    /**
     * @brief Swap contents with another row_data
     *
     * @param rhs Other row_data to swap with
     */
    void
    swap(row_data &rhs) {
        offsets.swap(rhs.offsets);
        data.swap(rhs.data);
        null_map.swap(rhs.null_map);
    }

private:
    /**
     * @brief Check if field index is valid
     *
     * @param index Field index to check
     * @throw std::out_of_range if the index is out of range
     */
    void check_index(size_type index) const;
};

/**
 * @brief Notice or error message from PostgreSQL server
 *
 * Contains various fields describing a notice or error message from
 * the PostgreSQL server, structured according to the protocol.
 * Each field is identified by a single-byte field code.
 *
 * @see https://www.postgresql.org/docs/current/protocol-error-fields.html
 */
struct notice_message {
    std::string
        severity; /**< Error severity (S): ERROR, FATAL, PANIC, WARNING, NOTICE, etc. */
    std::string sqlstate;          /**< SQL state code (C): SQLSTATE code */
    std::string message;           /**< Primary error message (M) */
    std::string detail;            /**< Detailed error message (D) */
    std::string hint;              /**< Suggestion for resolving the problem (H) */
    std::string position;          /**< Error position in original query (P) */
    std::string internal_position; /**< Error position in internal query (p) */
    std::string internal_query;    /**< Text of internal query (q) */
    std::string where;             /**< Context of error occurrence (W) */
    std::string schema_name;       /**< Schema name (s) */
    std::string table_name;        /**< Table name (t) */
    std::string column_name;       /**< Column name (c) */
    std::string data_type_name;    /**< Data type name (d) */
    std::string constraint_name;   /**< Constraint name (n) */
    std::string file_name;         /**< Source file name (F) */
    std::string line;              /**< Source file line number (L) */
    std::string routine;           /**< Source routine name (R) */

    /**
     * @brief Check if the notice contains a field
     *
     * @param code Field code character (see PostgreSQL protocol docs)
     * @return bool True if the field exists and has a value
     */
    bool has_field(char code) const;

    /**
     * @brief Get reference to a field by its code
     *
     * @param code Field code character
     * @return std::string& Reference to the field value
     */
    std::string &field(char code);
};

/**
 * @brief Output stream operator for notice_message
 *
 * @param os Output stream
 * @param notice Notice message to output
 * @return std::ostream& Reference to the output stream
 */
std::ostream &operator<<(std::ostream &, notice_message const &);

/**
 * @brief Command completion message
 *
 * Contains information about a completed SQL command,
 * including the command tag (e.g., "SELECT", "INSERT", etc.)
 * and the number of rows affected or returned.
 */
struct command_complete {
    std::string command_tag; /**< Command tag with completion info (e.g., "SELECT 10") */
};

} // namespace detail
} // namespace pg
} // namespace qb
