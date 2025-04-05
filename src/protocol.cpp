/**
 * @file protocol.cpp
 * @brief Implementation of PostgreSQL wire protocol
 *
 * This file implements the PostgreSQL wire protocol functionality defined in protocol.h,
 * including message parsing, construction, and data handling for client-server communication.
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
 *
 * @note Original author: zmij from project: https://github.com/zmij/pg_async.git
 */

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <cassert>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

#include "./protocol.h"
#include "./protocol_io_traits.h"

namespace qb {
namespace pg {
namespace detail {

/**
 * @brief Frontend and backend message tags
 *
 * These sets define the valid message tags for frontend (client) and backend (server)
 * communications according to the PostgreSQL protocol.
 */
namespace {
/** @brief Set of allowed message tags for frontend (client) */
tag_set_type FRONTEND_COMMANDS{empty_tag,     bind_tag,          close_tag,    copy_data_tag,
                               copy_done_tag, copy_fail_tag,     describe_tag, execute_tag,
                               flush_tag,     function_call_tag, parse_tag,    password_message_tag,
                               query_tag,     sync_tag,          terminate_tag};
/** @brief Set of allowed message tags for backend (server) */
tag_set_type BACKEND_COMMANDS{
    authentication_tag,     backend_key_data_tag,   bind_complete_tag,
    close_complete_tag,     command_complete_tag,   copy_data_tag,
    copy_done_tag,          copy_in_response_tag,   copy_out_response_tag,
    copy_both_response_tag, data_row_tag,           empty_query_response_tag,
    error_response_tag,     function_call_resp_tag, no_data_tag,
    notice_response_tag,    notification_resp_tag,  parameter_description_tag,
    parameter_status_tag,   parse_complete_tag,     portal_suspended_tag,
    ready_for_query_tag,    row_description_tag};
} // namespace

/**
 * @brief Get the set of allowed message tags for frontend
 *
 * @return tag_set_type const& Reference to set of frontend message tags
 */
tag_set_type const &
message::frontend_tags() {
    return FRONTEND_COMMANDS;
}

/**
 * @brief Get the set of allowed message tags for backend
 *
 * @return tag_set_type const& Reference to set of backend message tags
 */
tag_set_type const &
message::backend_tags() {
    return BACKEND_COMMANDS;
}

/**
 * @brief Construct message for reading from the stream
 *
 * Initializes an empty message with reserved buffer capacity.
 */
message::message()
    : payload()
    , packed_(false) {
    payload.reserve(256);
}

/**
 * @brief Construct message for sending to the backend
 *
 * Initializes a message with a specific tag and space for length field.
 *
 * @param tag Message type tag
 */
message::message(message_tag tag)
    : payload(5, 0)
    , packed_(false) {
    // TODO Check the tag
    payload[0] = (char)tag;
}

/**
 * @brief Move constructor
 *
 * @param rhs Message to move from
 */
message::message(message &&rhs)
    : payload{::std::move(rhs.payload)}
    , curr_{rhs.curr_}
    , packed_{rhs.packed_} {}

/**
 * @brief Get the message tag
 *
 * @return message_tag PostgreSQL message tag
 */
message_tag
message::tag() const {
    if (!payload.empty()) {
        message_tag t = static_cast<message_tag>(payload.front());
        return t;
    }
    return empty_tag;
}

/**
 * @brief Get the message length encoded in payload
 *
 * Decodes and returns the 4-byte length field from the message header.
 *
 * @return size_type Length in bytes
 */
message::size_type
message::length() const {
    const size_t header_size = sizeof(integer) + sizeof(byte);
    size_type len(0);
    if (payload.size() >= header_size) {
        // Decode length of message
        unsigned char *p = reinterpret_cast<unsigned char *>(&len);
        auto q = payload.begin() + 1;
        std::copy(q, q + sizeof(size_type), p);
        len = boost::endian::big_to_native(len);
    }

    return len;
}

/**
 * @brief Get iterators for the full message buffer
 *
 * Returns iterators to the complete message buffer for sending to the network.
 * Updates the length field if not already packed.
 *
 * @return const_range Iterators for the full buffer
 */
message::const_range
message::buffer() const {
    if (!packed_) {
        // Encode length of message
        integer len = size();
        // Manual conversion instead of using protocol_write
        unsigned char *p = reinterpret_cast<unsigned char *>(&len);
        len = boost::endian::native_to_big(len);
        for (size_t i = 0; i < sizeof(len); ++i) {
            payload[i + 1] = p[i];
        }
    }

    if (payload.front() == 0)
        return std::make_pair(payload.begin() + 1, payload.end());
    return std::make_pair(payload.begin(), payload.end());
}

/**
 * @brief Get size of payload, minus 1 (for the tag)
 *
 * @return size_t Payload size in bytes
 */
size_t
message::size() const {
    return payload.empty() ? 0 : payload.size() - 1;
}

/**
 * @brief Get full size of buffer including the tag
 *
 * @return size_t Total buffer size in bytes
 */
size_t
message::buffer_size() const {
    return payload.size();
}

/**
 * @brief Get iterator to current read position
 *
 * @return const_iterator Iterator to current read position
 */
message::const_iterator
message::input() const {
    return curr_;
}

/**
 * @brief Get an iterator for writing into the buffer
 *
 * @return output_iterator Output iterator for the buffer
 */
message::output_iterator
message::output() {
    return std::back_inserter(payload);
}

/**
 * @brief Move the read iterator to the beginning of actual payload
 *
 * Positions the read cursor after the message header.
 */
void
message::reset_read() {
    if (payload.size() <= 5) {
        curr_ = payload.end();
    } else {
        curr_ = payload.begin() + 5;
    }
}

/**
 * @brief Read a byte from the message buffer
 *
 * @param c Reference to store the read character
 * @return true if the operation was successful
 */
bool
message::read(char &c) {
    if (curr_ != payload.end()) {
        c = *curr_++;
        return true;
    }
    return false;
}

/**
 * @brief Helper function to write integer values with proper endianness
 *
 * @tparam T Integer type
 * @param payload Buffer to write to
 * @param val Value to write
 */
template <typename T>
void
write_int(message::buffer_type &payload, T val) {
    // Manual conversion instead of using protocol_write
    T converted = boost::endian::native_to_big(val);
    unsigned char *p = reinterpret_cast<unsigned char *>(&converted);
    for (size_t i = 0; i < sizeof(T); ++i) {
        payload.push_back(p[i]);
    }
}

/**
 * @brief Read a two-byte integer from the message buffer
 *
 * @param val Reference to store the read value
 * @return true if the operation was successful
 */
bool
message::read(smallint &val) {
    const_iterator c =
        io::protocol_read<pg::protocol_data_format::Binary>(curr_, payload.cend(), val);
    if (curr_ == c)
        return false;
    curr_ = c;
    return true;
}

/**
 * @brief Read a 4-byte integer from the message buffer
 *
 * @param val Reference to store the read value
 * @return true if the operation was successful
 */
bool
message::read(integer &val) {
    const_iterator c =
        io::protocol_read<pg::protocol_data_format::Binary>(curr_, payload.cend(), val);
    if (curr_ == c)
        return false;
    curr_ = c;
    return true;
}

/**
 * @brief Read a null-terminated string from the message buffer
 *
 * @param val Reference to store the read string
 * @return true if the operation was successful
 */
bool
message::read(std::string &val) {
    const_iterator c =
        io::protocol_read<pg::protocol_data_format::Text>(curr_, payload.cend(), val);
    if (curr_ == c)
        return false;
    curr_ = c;
    return true;
}

/**
 * @brief Read n bytes from message to the string
 *
 * @param val Reference to store the read string
 * @param n Number of bytes to read
 * @return true if the operation was successful
 */
bool
message::read(std::string &val, size_t n) {
    if (payload.end() - curr_ >= ::std::make_signed<size_t>::type(n)) {
        for (size_t i = 0; i < n; ++i) {
            val.push_back(*curr_++);
        }
        return true;
    }
    return false;
}

/**
 * @brief Read field description from the message buffer
 *
 * Reads a complete field description structure from the message.
 *
 * @param fd Reference to field description to store the result
 * @return true if the operation was successful
 */
bool
message::read(field_description &fd) {
    field_description tmp;
    tmp.max_size = 0;
    integer type_oid;
    smallint fmt;
    if (read(tmp.name) && read(tmp.table_oid) && read(tmp.attribute_number) && read(type_oid) &&
        read(tmp.type_size) && read(tmp.type_mod) && read(fmt)) {
        tmp.type_oid = static_cast<oid>(type_oid);
        tmp.format_code = static_cast<protocol_data_format>(fmt);
        fd = tmp;
        return true;
    }
    return false;
}

/**
 * @brief Read data row from the message buffer
 *
 * Parses a data row message and populates the row_data structure.
 *
 * @param row Reference to row_data to store the result
 * @return true if the operation was successful
 */
bool
message::read(row_data &row) {
    size_t len = length();
    assert(len == size() && "Invalid message length");
    if (len < sizeof(integer) + sizeof(smallint)) {
        std::cerr << "Size of invalid data row message is " << len << "\n";
        assert(len >= sizeof(integer) + sizeof(smallint) && "Invalid data row message");
    }
    smallint col_count(0);
    if (read(col_count)) {
        row_data tmp;
        tmp.offsets.reserve(col_count);
        size_t expected_sz = len - sizeof(integer) * (col_count + 1) - sizeof(int16_t);
        tmp.data.reserve(expected_sz);
        for (int16_t i = 0; i < col_count; ++i) {
            tmp.offsets.push_back(tmp.data.size());
            integer col_size(0);
            if (!read(col_size))
                return false;
            if (col_size == -1) {
                tmp.null_map.insert(i);
            } else if (col_size > 0) {
                const_iterator in = curr_;
                auto out = std::back_inserter(tmp.data);
                for (; in != curr_ + col_size; ++in) {
                    *out++ = *in;
                }
                curr_ = in;
            }
        }
        row.swap(tmp);
        return true;
    }
    return false;
}

/**
 * @brief Read a notice or error message from the message buffer
 *
 * Parses a notice or error message with field codes and values.
 *
 * @param notice Reference to notice_message to store the result
 * @return true if the operation was successful
 */
bool
message::read(notice_message &notice) {
    char code(0);
    while (read(code) && code) {
        if (notice.has_field(code)) {
            read(notice.field(code));
        } else {
            std::string fval;
            read(fval);
        }
    }
    return true;
}

/**
 * @brief Write char to the message buffer
 *
 * @param c The char to write
 */
void
message::write(char c) {
    payload.push_back(c);
}

/**
 * @brief Write a 2-byte integer to the message buffer
 *
 * @param v The integer to write
 */
void
message::write(smallint v) {
    write_int(payload, v);
}

/**
 * @brief Write a 4-byte integer to the message buffer
 *
 * @param v The integer to write
 */
void
message::write(integer v) {
    write_int(payload, v);
}

/**
 * @brief Write a string to the message buffer
 *
 * Writes the string including terminating '\0'.
 *
 * @param s The string to write
 */
void
message::write(std::string const &s) {
    // Copy string directly instead of using protocol_write
    std::copy(s.begin(), s.end(), std::back_inserter(payload));
    payload.push_back(0);
}

/**
 * @brief Write a string view to the message buffer
 *
 * Note: Does not add null terminator.
 *
 * @param s The string view to write
 */
void
message::write_sv(std::string_view const &s) {
    // Copy string view directly instead of using protocol_write
    std::copy(s.begin(), s.end(), std::back_inserter(payload));
}

/**
 * @brief Pack another message into this one
 *
 * Copies the buffer of another message, appending it to this one.
 *
 * @param m Message to pack into this one
 */
void
message::pack(message const &m) {
    buffer(); // to write the length, if hasn't been packed already
    packed_ = true;
    payload.reserve(payload.size() + m.payload.size());
    const_range r = m.buffer();
    std::copy(r.first, r.second, std::back_inserter(payload));
}

//----------------------------------------------------------------------------
// row_data implementation
//----------------------------------------------------------------------------

/**
 * @brief Get the number of fields in the row
 *
 * @return size_type Number of fields
 */
row_data::size_type
row_data::size() const {
    return offsets.size();
}

/**
 * @brief Check if the row is empty (has no fields)
 *
 * @return bool True if the row is empty
 */
bool
row_data::empty() const {
    return offsets.empty();
}

/**
 * @brief Check if field index is valid
 *
 * @param index Field index to check
 * @throw std::out_of_range if the index is out of range
 */
void
row_data::check_index(size_type index) const {
    if (index >= offsets.size()) {
        std::ostringstream out;
        out << "Field index " << index << " is out of range [0.." << size() << ")";
        throw std::out_of_range(out.str().c_str());
    }
}

/**
 * @brief Check if a field contains NULL
 *
 * @param index Field index (0-based)
 * @return true if the value in the field is null
 * @throw std::out_of_range if the index is out of range
 */
bool
row_data::is_null(size_type index) const {
    check_index(index);
    return null_map.count(index);
}

/**
 * @brief Get iterators to the raw data for a field
 *
 * @param index Field index (0-based)
 * @return data_buffer_bounds Pair of iterators to the field data
 * @throw std::out_of_range if the index is out of range
 * @throw value_is_null if the field is null
 */
row_data::data_buffer_bounds
row_data::field_buffer_bounds(size_type index) const {
    check_index(index);
    const_data_iterator s = data.begin() + offsets[index];
    if (index == offsets.size() - 1) {
        return std::make_pair(s, data.end());
    }
    return std::make_pair(s, data.begin() + offsets[index + 1]);
}

/**
 * @brief Get a field_buffer object for a field
 *
 * The field_buffer can be used with type conversion functions.
 *
 * @param index Field index (0-based)
 * @return field_buffer Field buffer for value conversion
 * @throw std::out_of_range if the index is out of range
 * @throw value_is_null if the field is null
 */
field_buffer
row_data::field_data(size_type index) const {
    data_buffer_bounds bounds = field_buffer_bounds(index);
    return field_buffer(bounds.first, bounds.second);
}

//----------------------------------------------------------------------------
// notice_message implementation
//----------------------------------------------------------------------------

/**
 * @brief Mapping between field codes and notice_message members
 *
 * This map associates PostgreSQL error/notice field codes with
 * corresponding member pointers in the notice_message structure.
 */
namespace {

const std::map<char, std::string notice_message::*> notice_fields{
    {'S', &notice_message::severity},
    {'C', &notice_message::sqlstate},
    {'M', &notice_message::message},
    {'D', &notice_message::detail},
    {'H', &notice_message::hint},
    {'P', &notice_message::position},
    {'p', &notice_message::internal_position},
    {'q', &notice_message::internal_query},
    {'W', &notice_message::where},
    {'s', &notice_message::schema_name},
    {'t', &notice_message::table_name},
    {'c', &notice_message::column_name},
    {'d', &notice_message::data_type_name},
    {'n', &notice_message::constraint_name},
    {'F', &notice_message::file_name},
    {'L', &notice_message::line},
    {'R', &notice_message::routine}};

} // namespace

/**
 * @brief Check if the notice contains a field
 *
 * @param code Field code character (see PostgreSQL protocol docs)
 * @return bool True if the field exists and has a value
 */
bool
notice_message::has_field(char code) const {
    return notice_fields.count(code);
}

/**
 * @brief Get reference to a field by its code
 *
 * @param code Field code character
 * @return std::string& Reference to the field value
 * @throw std::runtime_error if code is invalid
 */
std::string &
notice_message::field(char code) {
    auto f = notice_fields.find(code);
    if (f == notice_fields.end()) {
        throw std::runtime_error("Invalid message field code");
    }
    std::string notice_message::*pf = f->second;
    return this->*pf;
}

/**
 * @brief Output stream operator for notice_message
 *
 * Formats a notice_message for output to a stream with relevant fields.
 *
 * @param out Output stream
 * @param msg Notice message to output
 * @return std::ostream& Reference to the output stream
 */
std::ostream &
operator<<(std::ostream &out, notice_message const &msg) {
    std::ostream::sentry s(out);
    if (s) {
        out << "severity: " << msg.severity << " SQL code: " << msg.sqlstate << " message: '"
            << msg.message << "'";
        if (!msg.detail.empty()) {
            out << " detail: '" << msg.detail << "'";
        }
    }
    return out;
}

} // namespace detail
} // namespace pg
} // namespace qb
