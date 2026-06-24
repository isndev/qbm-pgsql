/**
 * @file pgsql.cpp
 * @brief PostgreSQL client for the QB Actor Framework
 *
 * This file implements the core functionality of the PostgreSQL client:
 *
 * - Module initialization for PostgreSQL type mappings and configurations
 * - Implementation of pipe allocator specialization for PostgreSQL messages
 * - Explicit template instantiations for different transport types
 *
 * The implementation works in conjunction with pgsql.h to provide a complete
 * asynchronous PostgreSQL client solution for the QB Actor Framework.
 *
 * @see qb::pg::detail::Database
 * @see qb::pg::detail::Transaction
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include "./pgsql.h"

qb::icase_unordered_map<std::string>
parse_header_attributes(const char *ptr, const size_t len) {
    qb::icase_unordered_map<std::string> dict;

    enum AttributeParseState { ATTRIBUTE_PARSE_NAME, ATTRIBUTE_PARSE_VALUE, ATTRIBUTE_PARSE_IGNORE } parse_state = ATTRIBUTE_PARSE_NAME;

    // misc other variables used for parsing
    const char *const end = ptr + len;
    std::string       attribute_name;
    std::string       attribute_value;
    char              value_quote_character = '\0';

    // iterate through each character
    while (ptr < end) {
        switch (parse_state) {
            case ATTRIBUTE_PARSE_NAME:
                // parsing attribute name
                if (*ptr == '=') {
                    // end of name found (OK if empty)
                    value_quote_character = '\0';
                    parse_state           = ATTRIBUTE_PARSE_VALUE;
                } else if (*ptr == ';' || *ptr == ',') {
                    // ignore empty attribute names since this may occur naturally
                    // when quoted values are encountered
                    if (!attribute_name.empty()) {
                        // value is empty (OK)
                        dict.emplace(attribute_name, attribute_value);
                        attribute_name.erase();
                    }
                } else if (*ptr != ' ') { // ignore whitespace
                    // check if control character detected, or max sized exceeded
                    if (is_control(*ptr) || attribute_name.size() >= ATTRIBUTE_NAME_MAX)
                        throw std::runtime_error("ctrl in name found or max attribute name length");
                    // character is part of the name
                    attribute_name.push_back(*ptr);
                }
                break;

            case ATTRIBUTE_PARSE_VALUE:
                // parsing attribute value
                if (value_quote_character == '\0') {
                    // value is not (yet) quoted
                    if (*ptr == ';' || *ptr == ',') {
                        // end of value found (OK if empty)
                        dict.emplace(attribute_name, attribute_value);
                        attribute_name.erase();
                        attribute_value.erase();
                        parse_state = ATTRIBUTE_PARSE_NAME;
                    } else if (*ptr == '\'' || *ptr == '"') {
                        if (attribute_value.empty()) {
                            // begin quoted value
                            value_quote_character = *ptr;
                        } else if (attribute_value.size() >= ATTRIBUTE_VALUE_MAX) {
                            // max size exceeded
                            throw std::runtime_error("max attribute size");
                        } else {
                            // assume character is part of the (unquoted) value
                            attribute_value.push_back(*ptr);
                        }
                    } else if (*ptr != ' ' || !attribute_value.empty()) { // ignore leading unquoted whitespace
                        // check if control character detected, or max sized exceeded
                        if (is_control(*ptr) || attribute_value.size() >= ATTRIBUTE_VALUE_MAX)
                            throw std::runtime_error("ctrl in value found or max attribute value length");
                        // character is part of the (unquoted) value
                        attribute_value.push_back(*ptr);
                    }
                } else {
                    // value is quoted
                    if (*ptr == value_quote_character) {
                        // end of value found (OK if empty)
                        dict.emplace(attribute_name, attribute_value);
                        attribute_name.erase();
                        attribute_value.erase();
                        parse_state = ATTRIBUTE_PARSE_IGNORE;
                    } else if (attribute_value.size() >= ATTRIBUTE_VALUE_MAX) {
                        // max size exceeded
                        throw std::runtime_error("max attribute value length");
                    } else {
                        // character is part of the (quoted) value
                        attribute_value.push_back(*ptr);
                    }
                }
                break;

            case ATTRIBUTE_PARSE_IGNORE:
                // ignore everything until we reach a comma "," or semicolon ";"
                if (*ptr == ';' || *ptr == ',')
                    parse_state = ATTRIBUTE_PARSE_NAME;
                break;
        }

        ++ptr;
    }

    // handle last attribute in string
    dict.emplace(attribute_name, attribute_value);

    return dict;
}

namespace qb::pg::detail {

std::string
scram_escape_saslname(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '=')
            out += "=3D"; // must precede the ',' substitution
        else if (c == ',')
            out += "=2C";
        else
            out += c;
    }
    return out;
}

std::array<std::uint8_t, 8>
postgres_ssl_negotiator::make_request() noexcept {
    std::array<std::uint8_t, 8> r{};
    const std::uint32_t         len  = htonl(8u);
    const std::uint32_t         code = htonl(0x04D2162Fu); // 80877103
    std::memcpy(r.data(), &len, 4);
    std::memcpy(r.data() + 4, &code, 4);
    return r;
}

qb::io::async::tcp::starttls_action
postgres_ssl_negotiator::advance(qb::io::tcp::socket &sock, int /*revents*/) noexcept {
    using action = qb::io::async::tcp::starttls_action;
    // Phase 1: write the full SSLRequest (cleartext).
    while (written_ < request_.size()) {
        const int n = sock.write(request_.data() + written_, request_.size() - written_);
        if (n > 0) {
            written_ += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && qb::io::socket::not_send_error(qb::io::socket::get_last_errno()))
            return action::want_write;
        return action::fail;
    }
    // Phase 2: read the single-byte verdict.
    if (!got_verdict_) {
        const int n = sock.read(&verdict_, 1);
        if (n == 1)
            got_verdict_ = true;
        else if (n < 0 && qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
            return action::want_read;
        else
            return action::fail; // EOF or hard error before the verdict
    }
    // Phase 3: decide. 'S' => TLS; everything else => require-TLS failure.
    return verdict_ == 'S' ? action::upgrade : action::fail;
}

} // namespace qb::pg::detail

namespace qb::pg {

/**
 * @brief Initialize the PostgreSQL module
 *
 * This function performs the following operations:
 * - Sets up default configurations for PostgreSQL connections
 * - Initializes the static database of PostgreSQL types and their mappings
 * - Configures field readers for different data types
 *
 * This function is called once during application startup to prepare
 * the PostgreSQL subsystem for use with the QB Actor Framework.
 */
void
init() {
    // Initialize default configuration
    // Load type databases
    // Static databases are loaded at startup

    // Initialize the field reader for PostgreSQL data types
    detail::initialize_field_reader();
}

} // namespace qb::pg

namespace qb::allocator {
/**
 * @brief Template specialization for pipe allocation with PostgreSQL messages
 *
 * This specialization handles the efficient allocation of PostgreSQL protocol messages
 * in the pipe allocator. It extracts the buffer data from the message and writes
 * it to the pipe.
 *
 * @param msg PostgreSQL message to write to the pipe
 * @return Reference to the pipe after data has been written
 */
template <>
pipe<char> &
pipe<char>::put<qb::pg::detail::message>(const qb::pg::detail::message &msg) {
    auto buffer_range = msg.buffer();
    // Write buffer data to the pipe
    if (buffer_range.first != buffer_range.second) {
        const char *data = &(*buffer_range.first);
        std::size_t size = std::distance(buffer_range.first, buffer_range.second);
        put(data, size);
    }
    return *this;
}
} // namespace qb::allocator

// Explicit template instantiations for Database class with different transport types
template class qb::pg::detail::Database<qb::io::transport::tcp, void>;

#ifdef QB_HAS_SSL

template class qb::pg::detail::Database<qb::io::transport::stcp, void>;

#endif