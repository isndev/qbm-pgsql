/**
 * @file pgsql.h
 * @brief PostgreSQL client for the QB Actor Framework
 *
 * This file implements an asynchronous PostgreSQL client integrated with the QB Actor
 * Framework. It provides a non-blocking interface for database operations such as:
 *
 * - Connection management to PostgreSQL databases
 * - Transaction management (begin, commit, rollback)
 * - Support for savepoints within transactions
 * - Simple and prepared statement execution with parameter binding
 * - Efficient query result retrieval and processing
 * - Support for multiple authentication methods (MD5, SCRAM-SHA-256, etc.)
 *
 * The implementation is designed to work with the actor model, allowing
 * database operations to be performed without blocking actor threads. The client
 * fully implements the PostgreSQL wire protocol for efficient communication.
 *
 * Key features:
 * - Asynchronous I/O using the QB Actor Framework
 * - Support for both plain TCP and SSL/TLS connections
 * - Comprehensive transaction management
 * - Prepared statement caching for performance
 * - Detailed error reporting and handling
 *
 * @see qb::pg::detail::Database
 * @see qb::pg::detail::Transaction
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

#include <memory>
#include <qb/io/async.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/crypto.h>
#include <qb/system/allocator/pipe.h>

#include "./src/commands.h"
#include "./src/transaction.h"

/**
 * @brief Maximum length for attribute names in PostgreSQL protocol
 *
 * Defines the maximum length in bytes for attribute names when parsing
 * PostgreSQL protocol messages. This limit helps prevent buffer overflow
 * attacks and ensures efficient memory usage.
 */
constexpr const uint32_t ATTRIBUTE_NAME_MAX = 1024; // 1 KB

/**
 * @brief Maximum length for attribute values in PostgreSQL protocol
 *
 * Defines the maximum length in bytes for attribute values when parsing
 * PostgreSQL protocol messages. This larger limit accommodates typical
 * PostgreSQL data values while preventing excessively large allocations.
 */
constexpr const uint32_t ATTRIBUTE_VALUE_MAX = 1024 * 1024; // 1 MB

/**
 * @brief Checks if a character is a control character
 *
 * Used during attribute parsing to validate input and ensure security.
 * Control characters are generally not allowed in attribute names or values
 * as they may indicate malformed or malicious input.
 *
 * @param c Character to check
 * @return true if the character is a control character (ASCII 0-31 or 127)
 */
inline bool
is_control(int c) {
    return ((c >= 0 && c <= 31) || c == 127);
}

/**
 * @brief Parses header attributes from a PostgreSQL protocol message
 *
 * Parses a buffer of header attributes into a case-insensitive map.
 * This function is primarily used during SCRAM authentication to process
 * challenge-response data between client and server.
 *
 * Features:
 * - Supports both quoted and unquoted attribute values
 * - Handles attribute separators (comma and semicolon)
 * - Enforces size limits to prevent buffer overflows
 * - Validates input to reject control characters
 * - Properly handles whitespace according to the PostgreSQL protocol
 *
 * @param ptr Pointer to the buffer containing attributes
 * @param len Length of the buffer
 * @return Case-insensitive map of attribute names to values
 * @throws std::runtime_error If parsing fails due to control characters or exceeding
 * size limits
 */
inline qb::icase_unordered_map<std::string>
parse_header_attributes(const char *ptr, const size_t len) {
    qb::icase_unordered_map<std::string> dict;

    enum AttributeParseState {
        ATTRIBUTE_PARSE_NAME,
        ATTRIBUTE_PARSE_VALUE,
        ATTRIBUTE_PARSE_IGNORE
    } parse_state = ATTRIBUTE_PARSE_NAME;

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
                        throw std::runtime_error(
                            "ctrl in name found or max attribute name length");
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
                    } else if (*ptr != ' ' ||
                               !attribute_value
                                    .empty()) { // ignore leading unquoted whitespace
                        // check if control character detected, or max sized exceeded
                        if (is_control(*ptr) ||
                            attribute_value.size() >= ATTRIBUTE_VALUE_MAX)
                            throw std::runtime_error(
                                "ctrl in value found or max attribute value length");
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

namespace qb::protocol {

/**
 * @brief PostgreSQL protocol implementation for the QB actor framework
 *
 * Handles the message framing and parsing according to the PostgreSQL
 * wire protocol specification. This class is responsible for:
 *
 * - Extracting complete messages from the input stream
 * - Managing protocol state between messages
 * - Forwarding complete messages to the appropriate handlers
 * - Implementing the PostgreSQL message format requirements
 *
 * The protocol handler processes the incoming byte stream and constructs
 * well-formed PostgreSQL protocol messages. It maintains internal state
 * to handle partial messages that arrive in multiple network packets.
 *
 * @tparam IO_ I/O handler type that provides input/output stream access
 */
template <typename IO_>
class pgsql final : public qb::io::async::AProtocol<IO_> {
public:
    /**
     * @brief PostgreSQL protocol message type
     *
     * Represents a complete PostgreSQL protocol message including
     * message type, length, and payload data.
     */
    using message = std::unique_ptr<pg::detail::message>;

private:
    message     message_;    ///< Current message being processed
    std::size_t offset_ = 0; ///< Current offset in the input buffer

public:
    pgsql() = delete;

    /**
     * @brief Constructs a PostgreSQL protocol handler
     *
     * Initializes the protocol handler with a reference to the I/O
     * subsystem that provides access to input and output streams.
     *
     * @param io Reference to the I/O handler
     */
    explicit pgsql(IO_ &io) noexcept
        : qb::io::async::AProtocol<IO_>(io) {}

    /**
     * @brief Copy data from input iterator to output iterator
     *
     * Helper method to copy data between iterators with a maximum limit.
     * Used internally for buffer management and message construction.
     *
     * @tparam InputIter Input iterator type
     * @tparam OutputIter Output iterator type
     * @param in Start of input range
     * @param end End of input range
     * @param max Maximum number of items to copy
     * @param out Output iterator
     * @return InputIter Iterator after the last copied element
     */
    template <typename InputIter, typename OutputIter>
    InputIter
    copy(InputIter in, InputIter end, size_t max, OutputIter out) {
        for (size_t i = 0; i < max && in != end; ++i) {
            *out++ = *in++;
        }
        return in;
    }

    /**
     * @brief Calculate the size of a complete PostgreSQL message
     *
     * Inspects the input buffer to determine if a complete message is available.
     * This method implements the PostgreSQL message framing protocol by:
     *
     * 1. Reading the message type byte and length field
     * 2. Creating a new message object if needed
     * 3. Reading message payload data up to the expected length
     * 4. Determining if the message is complete
     *
     * If a message is complete, returns its size in bytes. If incomplete,
     * returns 0 to indicate more data is needed from the network.
     *
     * @return std::size_t Size of the complete message, or 0 if incomplete
     */
    std::size_t
    getMessageSize() noexcept final {
        constexpr const size_t header_size =
            sizeof(qb::pg::integer) + sizeof(qb::pg::byte);

        if (this->_io.in().size() < header_size)
            return 0; // read more

        auto        max_bytes = this->_io.in().size() - offset_;
        const auto &in        = this->_io.in();

        if (!message_) {
            message_ = std::make_unique<pg::detail::message>();

            // copy header
            auto out = message_->output();
            for (auto i = 0u; i < header_size; ++i)
                *out++ = *(in.begin() + i);
            offset_ += header_size;
            max_bytes -= header_size;
        }

        if (message_->length() > message_->size()) {
            // Read the message body
            auto              out = message_->output();
            const std::size_t to_copy =
                std::min(message_->length() - message_->size(), max_bytes);
            for (auto i = 0u; i < to_copy; ++i)
                *out++ = *(in.begin() + offset_ + i);

            offset_ += to_copy;
            // max_bytes -= to_copy;
        }

        if (message_->length() == message_->size()) {
            return message_->buffer_size();
        }

        return 0;
    }

    /**
     * @brief Handle a complete PostgreSQL message
     *
     * Called by the protocol framework when a complete message has been
     * received and parsed according to the PostgreSQL protocol rules.
     *
     * This method:
     * 1. Validates the connection state
     * 2. Resets the message read pointer
     * 3. Forwards the complete message to the I/O handler for processing
     * 4. Resets the protocol state to prepare for the next message
     *
     * @param size Size of the message (unused in this implementation)
     */
    void
    onMessage(std::size_t) noexcept final {
        if (!this->ok())
            return;

        message_->reset_read();
        this->_io.on(std::move(message_));
        reset();
    }

    /**
     * @brief Reset the protocol state
     *
     * Prepares the protocol handler for the next message by resetting
     * internal state variables. This ensures that each new message is
     * processed from a clean initial state.
     */
    void
    reset() noexcept final {
        offset_ = 0;
    }
};

} // namespace qb::protocol

namespace qb::pg {
namespace detail {
using namespace qb::io;
using namespace qb::pg;

/**
 * @brief PostgreSQL database client implementation
 *
 * Core implementation of the PostgreSQL client that handles connection
 * establishment, authentication, and query execution. This class provides
 * the foundation for asynchronous database operations with PostgreSQL.
 *
 * Key features:
 * - Asynchronous TCP/IP connection management
 * - Multiple authentication methods support (Cleartext, MD5, SCRAM-SHA-256)
 * - Transaction management (inherited from Transaction class)
 * - Query execution and result processing
 * - Prepared statement caching and execution
 * - Event-driven message handling
 *
 * The Database class inherits from both the TCP client base class for network
 * connectivity and the Transaction class for query and transaction management.
 *
 * @tparam QB_IO_ I/O handler type that provides networking capabilities
 */
template <typename QB_IO_>
class Database
    : public qb::io::async::tcp::client<Database<QB_IO_>, QB_IO_, void>
    , public Transaction {
public:
    /**
     * @brief PostgreSQL protocol handler type
     *
     * Type alias for the protocol handler used by this database client.
     */
    using pg_protocol = qb::protocol::pgsql<Database<QB_IO_>>;

private:
    connection_options   conn_opts_;      ///< Database connection options
    client_options_type  client_opts_;    ///< Client options
    integer              serverPid_{};    ///< Server process ID
    integer              serverSecret_{}; ///< Server secret for protocol operations
    PreparedQueryStorage storage_;        ///< Storage for prepared statements
    bool is_connected_ = false; ///< Flag indicating if the connection is established

    /**
     * @brief Creates a startup message for PostgreSQL connection
     *
     * Builds the startup message according to the PostgreSQL protocol specification.
     * The message includes:
     * - Protocol version
     * - User authentication information
     * - Target database name
     * - Client parameters and options
     *
     * @param m Message object to populate with startup information
     */
    void
    create_startup_message(message &m) {
        m.write(PROTOCOL_VERSION);
        // Create startup packet
        m.write(options::USER);
        m.write(conn_opts_.user);
        m.write(options::DATABASE);
        m.write(conn_opts_.database);

        for (auto &opt : client_opts_) {
            m.write(opt.first);
            m.write(opt.second);
        }
        // trailing terminator
        m.write('\0');
    }

    /**
     * @brief Sends the startup message to the PostgreSQL server
     */
    void
    send_startup_message() {
        message m(empty_tag);
        create_startup_message(m);
        *this << m;
    }

    /**
     * @brief Handles new command events in the transaction
     */
    void
    on_new_command() final {
        process_if_query_ready();
    }

    /**
     * @brief Handles sub-command status updates
     *
     * @param status Status of the sub-command
     */
    void
    on_sub_command_status(bool) final {}

    Transaction *_current_command = this;    ///< Current transaction being processed
    ISqlQuery   *_current_query   = nullptr; ///< Current query being executed
    bool         _ready_for_query = false;   ///< Flag indicating if ready for next query

    /**
     * @brief Finds the next transaction to execute
     *
     * Recursively traverses the transaction tree to find the
     * deepest (leaf) transaction that should be executed next.
     *
     * @param cmd Current transaction
     * @return Transaction* Next transaction to execute
     */
    static Transaction *
    next_transaction(Transaction *cmd) {
        if (!cmd)
            return nullptr;

        auto sub = cmd->next_transaction();
        if (!sub)
            return cmd;
        else
            return next_transaction(sub);
    }

    /**
     * @brief Processes a query in the transaction
     *
     * Fetches and executes the next query from the given transaction.
     * If no more queries are in the current transaction, moves to parent.
     *
     * @param cmd Transaction containing the query
     * @return bool true if a query was processed, false if no queries remain
     */
    bool
    process_query(Transaction *cmd) {
        _ready_for_query = false;
        _current_command = next_transaction(cmd);
        _current_query   = _current_command->next_query();

        if (_current_query) {
            if (qb::likely(_current_query->is_valid())) {
                *this << _current_query->get();
                return true;
            } else {
                LOG_DEBUG("[pgsql] error processing query not valid");
                _error = error::client_error{
                    "query couldn't be processed check logs for more infos"};
                on_error_query(error());
                return process_query(_current_command) || (_ready_for_query = true);
            }
        } else if (_current_command->parent()) {
            auto next_cmd = _current_command->parent();
            do {
                next_cmd->pop_transaction();
            } while (!next_cmd->result() && (next_cmd = next_cmd->parent()));

            return process_query(next_cmd);
        }
        return false;
    }

    /**
     * @brief Processes queries if the client is ready
     */
    void
    process_if_query_ready() {
        if (_ready_for_query) {
            process_query(_current_command);
        }
    }

    /**
     * @brief Handles successful query completion
     */
    void
    on_success_query() {
        if (_current_query) {
            auto query = _current_command->pop_query();
            query->on_success();
            _current_query = nullptr;
        }
    }

    /**
     * @brief Handles query error
     *
     * @param err Error information
     */
    void
    on_error_query(error::db_error const &err) {
        _error = err;
        if (_current_query) {
            _current_command->result(false);
            auto query = _current_command->pop_query();
            query->on_error(err);
            _current_query = nullptr;
        }
    }

private:
    std::string          _nonce;         ///< Client nonce for SCRAM authentication
    std::vector<uint8_t> _password_salt; ///< Salted password for SCRAM authentication
    std::string          _auth_message;  ///< Authentication message for SCRAM protocol

public:
    /**
     * @brief Handles authentication messages from the server
     *
     * Processes various authentication methods requested by the PostgreSQL server
     * and responds appropriately. Supports multiple authentication mechanisms:
     *
     * - OK: Authentication already successful
     * - Cleartext: Simple plaintext password authentication
     * - MD5: MD5 hash-based password authentication
     * - SCRAM-SHA-256: Modern challenge-response authentication
     *
     * For each authentication type, this method constructs and sends the
     * appropriate response message according to the PostgreSQL protocol and
     * authentication specifications.
     *
     * @param msg Authentication message from the server
     */
    void
    on_authentication(message &msg) {
        integer auth_state(-1);
        msg.read(auth_state);

        LOG_DEBUG("[pgsql] Handle auth_event");
        switch (auth_state) {
            case OK: {
                LOG_INFO("[pgsql] Authenticated with server");
                is_connected_ = true;
            } break;
            case Cleartext: {
                LOG_INFO("[pgsql] Clear text authentication requested");
                message pm(password_message_tag);
                pm.write(conn_opts_.password);

                *this << pm;
            } break;
            case MD5Password: {
                LOG_INFO("[pgsql] MD5 authentication requested");
                // Read salt
                std::string salt;
                msg.read(salt, 4);
                // Calculate hash
                std::string pwdhash = qb::crypto::to_hex_string(
                    qb::crypto::md5(conn_opts_.password + conn_opts_.user),
                    qb::crypto::range_hex_lower);
                std::string md5digest =
                    std::string("md5") +
                    qb::crypto::to_hex_string(qb::crypto::md5(pwdhash + salt),
                                              qb::crypto::range_hex_lower);
                // Construct and send message
                message pm(password_message_tag);
                pm.write(md5digest);

                *this << pm;
            } break;
            case SCRAM_SHA256: {
                LOG_INFO("[pgsql] SCRAM-SHA-256 authentication requested");
                message pm(password_message_tag);
                // Set new nonce
                _nonce =
                    qb::crypto::generate_random_string(32, qb::crypto::range_hex_lower);
                const auto data = "n,,n=" + conn_opts_.user + ",r=" + _nonce;
                // Add mechanism
                pm.write("SCRAM-SHA-256");
                // Add sasl data
                pm.write(static_cast<qb::pg::integer>(data.size()));
                pm.write_sv(data);
                *this << pm;
            } break;
            case SCRAM_SHA256_CLIENT_PROOF: {
                LOG_INFO("[pgsql] SCRAM-SHA-256 authentication client proof check");
                std::string data;
                msg.read(data);
                auto params = parse_header_attributes(data.c_str(), data.size());

                // SCRAM inputs
                const std::string clientNonce = _nonce; // Nonce generated by client
                const std::string username    = conn_opts_.user;
                const std::string password    = conn_opts_.password;
                const std::string serverNonce =
                    std::move(params["r"]); // Combined nonce (client + server)
                const std::string salt_base64 = std::move(params["s"]); // Salt (base64)
                const int         iteration =
                    std::stoi(params["i"]); // Number of iterations received from server

                // Client-first-message-bare
                std::string client_first_message_bare =
                    "n=" + username + ",r=" + clientNonce;
                std::string server_first_message = "r=" + serverNonce +
                                                   ",s=" + salt_base64 +
                                                   ",i=" + std::to_string(iteration);
                std::string client_final_message_without_proof =
                    "c=biws,r=" + serverNonce; // "biws" is the base64 encoding of "n,,"
                _auth_message = client_first_message_bare + "," + server_first_message +
                                "," + client_final_message_without_proof;
                // Compute SaltedPassword using PBKDF2-HMAC-SHA256
                std::vector<unsigned char> salt = qb::crypto::base64_decode(salt_base64);
                std::vector<unsigned char> saltedPassword(32); // 32 bytes for SHA256
                if (PKCS5_PBKDF2_HMAC(password.c_str(),
                                      static_cast<int>(password.size()), salt.data(),
                                      static_cast<int>(salt.size()), iteration,
                                      EVP_sha256(), 32, saltedPassword.data()) != 1) {
                    throw std::runtime_error("error during PBKDF2 computing");
                }
                // Compute clientKey: HMAC(saltedPassword, "Client Key")
                std::vector<unsigned char> clientKey =
                    qb::crypto::hmac_sha256(saltedPassword, "Client Key");
                // Compute storedKey: SHA256(clientKey)
                std::vector<unsigned char> storedKey = qb::crypto::sha256(clientKey);
                // Compute clientSignature: HMAC(storedKey, authMessage)
                std::vector<unsigned char> clientSignature =
                    qb::crypto::hmac_sha256(storedKey, _auth_message);
                // Compute clientProof: XOR(clientKey, clientSignature)
                std::vector<unsigned char> clientProof =
                    qb::crypto::xor_bytes(clientKey, clientSignature);
                // Encode clientProof in base64
                std::string clientProofBase64 =
                    qb::crypto::base64_encode(clientProof.data(), clientProof.size());
                // Construction of final message to send
                std::string client_final_message =
                    "c=biws,r=" + serverNonce + ",p=" + clientProofBase64;

                message pm(password_message_tag);
                pm.write_sv(client_final_message);
                *this << pm;
                _password_salt = std::move(saltedPassword);
            } break;
            case SCRAM_SHA256_SERVER_CHECK: {
                try {
                    std::string serverFinalMessage;

                    msg.read(serverFinalMessage);
                    // Extract the server signature from the final message
                    const std::string prefix = "v=";
                    size_t            pos    = serverFinalMessage.find(prefix);
                    if (pos == std::string::npos) {
                        throw std::runtime_error(
                            "server final message does not contain a signature");
                    }
                    std::string receivedServerSignatureBase64 =
                        serverFinalMessage.substr(pos + prefix.size());
                    // Compute the ServerKey: HMAC(saltedPassword, "Server Key")
                    std::vector<unsigned char> serverKey =
                        qb::crypto::hmac_sha256(_password_salt, "Server Key");
                    // Compute the ServerSignature: HMAC(serverKey, authMessage)
                    std::vector<unsigned char> computedServerSignature =
                        qb::crypto::hmac_sha256(serverKey, _auth_message);
                    // Encode the computed server signature in Base64
                    std::string computedServerSignatureBase64 =
                        qb::crypto::base64_encode(computedServerSignature.data(),
                                                  computedServerSignature.size());
                    // Compare the computed server signature with the received one
                    if (computedServerSignatureBase64 != receivedServerSignatureBase64) {
                        throw std::runtime_error(
                            "server signature does not match. Authentication failed");
                    }
                    LOG_INFO("[pgsql] SCRAM-SHA-256 Authentication successful: server "
                             "signature verified");
                    break;
                } catch (std::exception &ex) {
                    LOG_CRIT("[pgsql] SCRAM-SHA-256 Failed verifying server signature: "
                             << ex.what());
                }
            } break;
            default: {
                LOG_CRIT("[pgsql] Unsupported authentication scheme "
                         << auth_state << "requested by server");
                throw std::runtime_error("[pgsql] fatal error: check logs");
            }
        }
    }

    /**
     * @brief Handles command complete messages
     *
     * @param msg Command complete message
     */
    void
    on_command_complete(message &msg) {
        command_complete cmpl;
        msg.read(cmpl.command_tag);
        LOG_DEBUG("[pgsql] Command complete (" << cmpl.command_tag << ")");
    }

    /**
     * @brief Handles backend key data messages
     *
     * @param msg Backend key data message
     */
    void
    on_backend_key_data(message &msg) {
        msg.read(serverPid_);
        msg.read(serverSecret_);
        LOG_DEBUG("[pgsql] Received backend key data");
    }

    /**
     * @brief Handles error response messages
     *
     * @param msg Error response message
     */
    void
    on_error_response(message &msg) {
        notice_message notice;
        msg.read(notice);

        LOG_WARN("[pgsql] Error " << notice);
        error::query_error err(notice.message, notice.severity, notice.sqlstate,
                               notice.detail);

        on_error_query(err);
    }

    /**
     * @brief Handles parameter status messages
     *
     * @param msg Parameter status message
     */
    void
    on_parameter_status(message &msg) {
        std::string key;
        std::string value;

        msg.read(key);
        msg.read(value);

        LOG_DEBUG("[pgsql] Received parameter " << key << "=" << value);

        client_opts_[key] = value;
    }

    /**
     * @brief Handles notice response messages
     *
     * @param msg Notice response message
     */
    void
    on_notice_response(message &msg) {
        notice_message notice;
        msg.read(notice);

        LOG_INFO("[pgsql] Received notice" << notice);
    }

    /**
     * @brief Handles ready for query messages
     *
     * @param msg Ready for query message
     */
    void
    on_ready_for_query(message &msg) {
        on_success_query();
        char stat(0);
        msg.read(stat);

        if (!process_query(_current_command)) {
            _ready_for_query = true;
            LOG_DEBUG("[pgsql] Database " << conn_opts_.uri << "[" << conn_opts_.database
                                          << "]"
                                          << " is ready for query (" << stat << ")");
        }
    }

    /**
     * @brief Handles row description messages
     *
     * @param msg Row description message
     */
    void
    on_row_description(message &msg) {
        row_description_type fields;
        smallint             col_cnt;
        msg.read(col_cnt);
        fields.reserve(col_cnt);
        for (int i = 0; i < col_cnt; ++i) {
            field_description fd;
            if (msg.read(fd)) {
                fields.push_back(fd);
            } else {
                LOG_WARN("[pgsql] Failed to read field description " << i);
                _current_command->result(false);
                break;
            }
        }
        _current_command->on_new_row_description(std::move(fields));
    }

    /**
     * @brief Handles data row messages
     *
     * @param msg Data row message
     */
    void
    on_data_row(message &msg) {
        row_data row;
        if (msg.read(row))
            _current_command->on_new_data_row(std::move(row));
        else {
            LOG_WARN("[pgsql] Failed to read data row");
            _current_command->result(false);
        }
    }

    /**
     * @brief Handles parse complete messages
     *
     * @param msg Parse complete message
     */
    void
    on_parse_complete(message &) {
        LOG_DEBUG("[pgsql] Parse complete");
    }

    /**
     * @brief Handles parameter description messages
     *
     * @param msg Parameter description message
     */
    void
    on_parameter_description(message &) {
        LOG_DEBUG("[pgsql] Parameter descriptions");
    }

    /**
     * @brief Handles bind complete messages
     *
     * @param msg Bind complete message
     */
    void
    on_bind_complete(message &) {
        LOG_DEBUG("[pgsql] Bind complete");
    }

    /**
     * @brief Handles no data messages
     *
     * @param msg No data message
     */
    void
    on_no_data(message &) {
        LOG_DEBUG("[pgsql] No data");
    }

    /**
     * @brief Handles portal suspended messages
     *
     * @param msg Portal suspended message
     */
    void
    on_portal_suspended(message &) {
        LOG_DEBUG("[pgsql] Portal suspended");
    }

    /**
     * @brief Handles unrecognized messages
     *
     * @param msg Unhandled message
     */
    void
    on_unhandled_message(message &msg) {
        LOG_DEBUG("[pgsql] Unhandled message tag " << (char) msg.tag());
    }

    /**
     * @brief Message routing table
     *
     * Maps PostgreSQL protocol message tags to their handler methods.
     */
    inline static const qb::unordered_flat_map<int, void (Database::*)(message &)>
        routes_ = {{authentication_tag, &Database::on_authentication},
                   {command_complete_tag, &Database::on_command_complete},
                   {backend_key_data_tag, &Database::on_backend_key_data},
                   {error_response_tag, &Database::on_error_response},
                   {parameter_status_tag, &Database::on_parameter_status},
                   {notice_response_tag, &Database::on_notice_response},
                   {ready_for_query_tag, &Database::on_ready_for_query},
                   {row_description_tag, &Database::on_row_description},
                   {data_row_tag, &Database::on_data_row},
                   {parse_complete_tag, &Database::on_parse_complete},
                   {parameter_description_tag, &Database::on_parameter_description},
                   {bind_complete_tag, &Database::on_bind_complete},
                   {no_data_tag, &Database::on_no_data},
                   {portal_suspended_tag, &Database::on_portal_suspended}};

public:
    /**
     * @brief Default constructor
     *
     * Creates a database client without connection information.
     * The client is initialized but not connected to any database.
     * Use the connect() method with connection options to establish a connection.
     */
    Database()
        : Transaction(storage_) {}

    /**
     * @brief Constructs a database client with connection options
     *
     * Initializes the client with the given connection options but does not
     * establish a connection immediately. Call connect() to initiate the connection.
     *
     * @param opts Connection string in the format
     * "postgresql://user:password@host:port/database"
     */
    explicit Database(std::string const &opts)
        : Transaction(storage_)
        , conn_opts_(connection_options::parse(opts)) {}

    /**
     * @brief Destructor
     *
     * Ensures the connected flag is reset. The actual connection
     * cleanup is handled by the TCP client base class.
     */
    ~Database() {
        is_connected_ = false;
    }

    /**
     * @brief Initiates a connection to the database
     *
     * Establishes a TCP connection to the PostgreSQL server, configures
     * the protocol handler, and performs the authentication process.
     * This method blocks until the connection is established or an error occurs.
     *
     * @return bool True if connection was successful, false on failure
     */
    bool
    connect() {
        if (is_connected_)
            return true;

        _error = error::db_error{"unknown error"};

        if (!static_cast<qb::io::tcp::socket &>(this->transport()).connect(
                qb::io::uri{conn_opts_.schema + "://" + conn_opts_.uri})) {
            if (this->protocol())
                this->clear_protocols();
            using tpt = std::decay_t<decltype(this->transport())>;
            if constexpr (tpt::is_secure()) {
                uint32_t len = htonl(8);
                uint32_t code = htonl(0x04D2162F);
                std::array<uint8_t, 8> ssl_request{};
                std::memcpy(ssl_request.data(), &len, 4);
                std::memcpy(ssl_request.data() + 4, &code, 4);

                if (send(this->transport().native_handle(), reinterpret_cast<const char *>(ssl_request.data()), ssl_request.size(), 0) != 8) {
                    LOG_CRIT("[pgsql] Failed to send SSL request");
                    return false;
                }

                uint8_t response;
                auto    n = recv(this->transport().native_handle(),
                                 reinterpret_cast<char *>(&response), 1, 0);
                if (n != 1) {
                    LOG_CRIT("[pgsql] Failed to receive SSL response");
                    return false;
                }

                if (response == 'S') {
                    LOG_INFO("[pgsql] Server supports SSL");
                    if (!this->transport().connect(
                            qb::io::uri{conn_opts_.schema + "://" + conn_opts_.uri})) {
                        LOG_CRIT("[pgsql] Failed to connect to SSL server");
                        return false;
                    }
                } else if (response == 'N') {
                    LOG_CRIT("[pgsql] Server does NOT support SSL");
                    return false;
                }
            }
            this->template switch_protocol<pg_protocol>(*this);
            this->start();
            send_startup_message();

            while (!is_connected_ && !has_error())
                qb::io::async::run_once();

            return is_connected_;
        }
        return false;
    }

    /**
     * @brief Connects to a database using connection options
     *
     * Parses the connection string and initiates a connection to the
     * specified PostgreSQL database.
     *
     * @param conn_opts Connection string in format
     * "postgresql://user:password@host:port/database"
     * @return bool True if connection was successful, false on failure
     */
    bool
    connect(std::string const &conn_opts) {
        conn_opts_ = connection_options::parse(conn_opts);
        return connect();
    }

    /**
     * @brief Connects to a database using an existing I/O channel
     *
     * Uses an existing transport I/O channel (e.g., from a connection pool)
     * to establish a database connection. This allows for connection reuse
     * and efficient resource management.
     *
     * @param conn_opts Connection string with database credentials
     * @param raw_io Existing I/O channel to use for communication
     * @return bool True if connection was successful, false on failure
     */
    bool
    connect(std::string const &conn_opts, typename QB_IO_::transport_io_type &&raw_io) {
        conn_opts_        = connection_options::parse(conn_opts);
        this->transport() = std::move(raw_io);

        return connect();
    }

    /**
     * @brief Message handler callback
     *
     * Called by the protocol handler when a complete message is received.
     * Routes the message to the appropriate handler method based on its tag.
     * This is a key part of the event-driven architecture of the client.
     *
     * @param msg Protocol message to be processed
     */
    void
    on(typename pg_protocol::message msg) {
        const auto it = routes_.find(msg->tag());
        if (qb::likely(it != routes_.end()))
            (this->*(it->second))(*msg);
        else
            on_unhandled_message(*msg);
    }

    /**
     * @brief Disconnection handler
     *
     * Called when the connection to the database server is lost.
     * Updates the connection state and raises an error for any pending queries.
     *
     * @param Disconnection event (unused)
     */
    void
    on(qb::io::async::event::disconnected const &) {
        if (is_connected_) {
            is_connected_ = false;
            on_error_query(error::client_error("database disconnected"));
        }
    }

    /**
     * @brief Explicitly disconnects from the database
     *
     * Closes the connection to the PostgreSQL server and runs the event loop once
     * to process any pending disconnection events. This ensures a clean shutdown
     * of the database connection.
     */
    void
    disconnect() {
        static_cast<qb::io::async::tcp::client<Database<QB_IO_>, QB_IO_, void> &>(*this)
            .disconnect();
        qb::io::async::run(EVRUN_NOWAIT);
    }
};

} // namespace detail

/**
 * @brief Type alias for database with custom I/O handler
 *
 * Provides a convenient alias for creating database clients with
 * different I/O handlers. This allows for flexibility in choosing
 * the networking implementation while maintaining a consistent API.
 *
 * @tparam QB_IO_ I/O handler type that provides networking capabilities
 */
template <typename QB_IO_>
using database = detail::Database<QB_IO_>;

/**
 * @brief Type alias for transaction base class
 *
 * Provides a convenient alias for the transaction base class.
 * Use this type to work with database transactions including
 * begin, commit, rollback, and savepoint operations.
 */
using transaction = detail::Transaction;

/**
 * @brief Type alias for query result set
 *
 * Provides a convenient alias for working with query results.
 * The result set contains the rows and columns returned by a query.
 */
using results = detail::resultset;

/**
 * @brief Type alias for query parameters
 *
 * Provides a convenient alias for binding parameters to prepared statements.
 * Parameters can be bound by position or name, depending on the query style.
 */
using params = detail::QueryParams;

/**
 * @brief TCP transport namespace
 *
 * Contains database clients that use TCP transport for PostgreSQL communication.
 */
struct tcp {
    /**
     * @brief Database client with plain TCP transport
     *
     * Database client implementation using unencrypted TCP connections.
     * Suitable for local networks or when using an external encryption layer.
     */
    using database = detail::Database<qb::io::transport::tcp>;
#ifdef QB_IO_WITH_SSL
    /**
     * @brief SSL transport namespace
     *
     * Contains database clients that use SSL/TLS encrypted transport
     * for secure communication with PostgreSQL servers.
     */
    struct ssl {
        /**
         * @brief Database client with SSL transport
         *
         * Database client implementation using SSL/TLS encrypted connections.
         * Recommended for production environments and connections over
         * public networks for enhanced security.
         */
        using database = detail::Database<qb::io::transport::stcp>;
    };
#endif
};

} // namespace qb::pg

namespace qb::allocator {

/**
 * @brief Specialization for pipe allocation with PostgreSQL messages
 *
 * Allows PostgreSQL messages to be allocated with the pipe allocator.
 * This specialization enables efficient memory management for message processing.
 *
 * @param message PostgreSQL message to allocate
 * @return pipe<char>& Reference to the pipe containing the allocated message
 */
template <>
pipe<char> &pipe<char>::put<qb::pg::detail::message>(const qb::pg::detail::message &);

} // namespace qb::allocator
