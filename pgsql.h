/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QBM_PGSQL_H
#define QBM_PGSQL_H
#include <qb/io/async.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/system/allocator/pipe.h>
#include <qb/io/crypto.h>

#include <memory>
#include "detail/transaction.h"
#include "detail/commands.h"

constexpr const uint32_t ATTRIBUTE_NAME_MAX = 1024;         // 1 KB
constexpr const uint32_t ATTRIBUTE_VALUE_MAX = 1024 * 1024; // 1 MB

inline bool
is_control(int c) {
    return ((c >= 0 && c <= 31) || c == 127);
}

inline qb::icase_unordered_map<std::string>
parse_header_attributes(const char *ptr, const size_t len) {
    qb::icase_unordered_map<std::string> dict;

    enum AttributeParseState {
        ATTRIBUTE_PARSE_NAME, ATTRIBUTE_PARSE_VALUE, ATTRIBUTE_PARSE_IGNORE
    } parse_state = ATTRIBUTE_PARSE_NAME;

    // misc other variables used for parsing
    const char * const end = ptr + len;
    std::string attribute_name;
    std::string attribute_value;
    char value_quote_character = '\0';

    // iterate through each character
    while (ptr < end) {
        switch (parse_state) {

            case ATTRIBUTE_PARSE_NAME:
                // parsing attribute name
                if (*ptr == '=') {
                    // end of name found (OK if empty)
                    value_quote_character = '\0';
                    parse_state = ATTRIBUTE_PARSE_VALUE;
                } else if (*ptr == ';' || *ptr == ',') {
                    // ignore empty attribute names since this may occur naturally
                    // when quoted values are encountered
                    if (! attribute_name.empty()) {
                        // value is empty (OK)
                        dict.emplace(attribute_name, attribute_value);
                        attribute_name.erase();
                    }
                } else if (*ptr != ' ') {   // ignore whitespace
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
                    } else if (*ptr != ' ' || !attribute_value.empty()) {  // ignore leading unquoted whitespace
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

namespace qb::protocol {

template <typename IO_>
class pgsql final : public qb::io::async::AProtocol<IO_> {
public:
    using message = std::unique_ptr<pg::detail::message>;

private:
    message message_;
    std::size_t offset_ = 0;

public:
    pgsql() = delete;
    explicit pgsql(IO_ &io) noexcept
        : qb::io::async::AProtocol<IO_>(io) {}

    template <typename InputIter, typename OutputIter>
    InputIter
    copy(InputIter in, InputIter end, size_t max, OutputIter out) {
        for (size_t i = 0; i < max && in != end; ++i) {
            *out++ = *in++;
        }
        return in;
    }

    std::size_t
    getMessageSize() noexcept final {
        constexpr const size_t header_size = sizeof(qb::pg::integer) + sizeof(qb::pg::byte);

        if (this->_io.in().size() < header_size)
            return 0; // read more

        auto max_bytes = this->_io.in().size() - offset_;
        const auto &in = this->_io.in();

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
            auto out = message_->output();
            const std::size_t to_copy = std::min(message_->length() - message_->size(), max_bytes);
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

    void
    onMessage(std::size_t) noexcept final {
        if (!this->ok())
            return;

        message_->reset_read();
        this->_io.on(std::move(message_));
        reset();
    }

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

template <typename QB_IO_>
class Database
    : public qb::io::async::tcp::client<Database<QB_IO_>, QB_IO_, void>
    , public Transaction {
public:
    using pg_protocol = qb::protocol::pgsql<Database<QB_IO_>>;

private:
    connection_options conn_opts_;
    client_options_type client_opts_;
    integer serverPid_{};
    integer serverSecret_{};
    PreparedQueryStorage storage_;

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
    void
    send_startup_message() {
        message m(empty_tag);
        create_startup_message(m);
        *this << m;
    }

    void
    on_new_command() final {
        process_if_query_ready();
    }
    void
    on_sub_command_status(bool) final {}

    Transaction *_current_command = this;
    ISqlQuery *_current_query = nullptr;
    bool _ready_for_query = false;

    // postgres database
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
    bool
    process_query(Transaction *cmd) {
        _ready_for_query = false;
        _current_command = next_transaction(cmd);
        _current_query = _current_command->next_query();

        if (_current_query) {
            if (qb::likely(_current_query->is_valid())) {
                *this << _current_query->get();
                return true;
            } else {
                LOG_DEBUG("[pgsql] error processing query not valid");
                on_error_query(error::client_error{"query couldn't be processed check logs for more infos"});
                return process_query(_current_command);
            }
        } else if (_current_command->parent()) {
            auto next_cmd = _current_command->parent();
            do {
                delete next_cmd->pop_transaction();
            } while (!next_cmd->result() && (next_cmd = next_cmd->parent()));

            return process_query(next_cmd);
        }
        return false;
    }
    void
    process_if_query_ready() {
        if (_ready_for_query) {
            process_query(_current_command);
        }
    }

    void
    on_success_query() {
        if (_current_query) {
            const auto query = _current_command->pop_query();
            query->on_success();
            delete query;
            _current_query = nullptr;
        }
    }
    void
    on_error_query(error::db_error const &err) {
        if (_current_query) {
            _current_command->result(false);
            const auto query = _current_command->pop_query();
            query->on_error(err);
            delete query;
            _current_query = nullptr;
        }
    }

    std::string _nonce;
    std::vector<uint8_t> _password_salt;
    std::string _auth_message;
    // postgres handle protocol message
    void
    on_authentication(message &msg) {
        integer auth_state(-1);
        msg.read(auth_state);

        LOG_DEBUG("[pgsql] Handle auth_event");
        switch (auth_state) {
            case OK: {
                LOG_INFO("[pgsql] Authenticated with server");
            }
                break;
            case Cleartext: {
                LOG_INFO("[pgsql] Clear text authentication requested");
                message pm(password_message_tag);
                pm.write(conn_opts_.password);

                *this << pm;
            }
                break;
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
                        qb::crypto::to_hex_string(qb::crypto::md5(pwdhash + salt), qb::crypto::range_hex_lower);
                // Construct and send message
                message pm(password_message_tag);
                pm.write(md5digest);

                *this << pm;
            }
                break;
            case SCRAM_SHA256: {
                LOG_INFO("[pgsql] SCRAM-SHA-256 authentication requested");
                message pm(password_message_tag);
                // set new nonce
                _nonce = qb::crypto::generate_random_string(32, qb::crypto::range_hex_lower);
                const auto data = "n,,n=" + conn_opts_.user + ",r=" + _nonce;
                // add mechanism
                pm.write("SCRAM-SHA-256");
                // add sasl data
                pm.write(static_cast<qb::pg::integer>(data.size()));
                pm.write_sv(data);
                *this << pm;
            }
                break;
            case SCRAM_SHA256_CLIENT_PROOF: {
                LOG_INFO("[pgsql] SCRAM-SHA-256 authentication client proof check");
                std::string data;
                msg.read(data);
                auto params = parse_header_attributes(data.c_str(), data.size());

                // SCRAM inputs
                const std::string clientNonce = _nonce;        // nonce generated by us
                const std::string username = conn_opts_.user;
                const std::string password = conn_opts_.password;
                const std::string serverNonce = std::move(params["r"]);    // combined nonce (client + server)
                const std::string salt_base64 = std::move(params["s"]);   // salt (base64)
                const int iteration = std::stoi(
                        params["i"]);                         // nombre d'itérations reçu du serveur

                // client-first-message-bare
                std::string client_first_message_bare = "n=" + username + ",r=" + clientNonce;
                std::string server_first_message =
                        "r=" + serverNonce + ",s=" + salt_base64 + ",i=" + std::to_string(iteration);
                std::string client_final_message_without_proof =
                        "c=biws,r=" + serverNonce; // "biws" est l'encodage base64 de "n,,"
                _auth_message = client_first_message_bare + "," + server_first_message + "," +
                                client_final_message_without_proof;
                // compute SaltedPassword using PBKDF2-HMAC-SHA256
                std::vector<unsigned char> salt = qb::crypto::base64_decode(salt_base64);
                std::vector<unsigned char> saltedPassword(32); // 32 octets pour SHA256
                if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                                      salt.data(), static_cast<int>(salt.size()),
                                      iteration, EVP_sha256(), 32, saltedPassword.data()) != 1) {
                    throw std::runtime_error("error during PBKDF2 computing");
                }
                // compute clientKey : HMAC(saltedPassword, "Client Key")
                std::vector<unsigned char> clientKey = qb::crypto::hmac_sha256(saltedPassword, "Client Key");
                // compute storedKey : SHA256(clientKey)
                std::vector<unsigned char> storedKey = qb::crypto::sha256(clientKey);
                // compute clientSignature : HMAC(storedKey, authMessage)
                std::vector<unsigned char> clientSignature = qb::crypto::hmac_sha256(storedKey, _auth_message);
                // compute clientProof : XOR(clientKey, clientSignature)
                std::vector<unsigned char> clientProof = qb::crypto::xor_bytes(clientKey, clientSignature);
                // encode clientProof in base64
                std::string clientProofBase64 = qb::crypto::base64_encode(clientProof.data(), clientProof.size());
                // construction of final message to send
                std::string client_final_message = "c=biws,r=" + serverNonce + ",p=" + clientProofBase64;

                message pm(password_message_tag);
                pm.write_sv(client_final_message);
                *this << pm;
                _password_salt = std::move(saltedPassword);
            }
                break;
            case SCRAM_SHA256_SERVER_CHECK: {
                try {
                    std::string serverFinalMessage;

                    msg.read(serverFinalMessage);
                    // extract the server signature from the final message.
                    const std::string prefix = "v=";
                    size_t pos = serverFinalMessage.find(prefix);
                    if (pos == std::string::npos) {
                        throw std::runtime_error("server final message does not contain a signature");
                    }
                    std::string receivedServerSignatureBase64 = serverFinalMessage.substr(pos + prefix.size());
                    // compute the ServerKey: HMAC(saltedPassword, "Server Key")
                    std::vector<unsigned char> serverKey = qb::crypto::hmac_sha256(_password_salt, "Server Key");
                    // compute the ServerSignature: HMAC(serverKey, authMessage)
                    std::vector<unsigned char> computedServerSignature = qb::crypto::hmac_sha256(serverKey,
                                                                                                 _auth_message);
                    // encode the computed server signature in Base64.
                    std::string computedServerSignatureBase64 = qb::crypto::base64_encode(
                            computedServerSignature.data(),
                            computedServerSignature.size());
                    // compare the computed server signature with the received one
                    if (computedServerSignatureBase64 != receivedServerSignatureBase64) {
                        throw std::runtime_error("server signature does not match. Authentication failed");
                    }
                    LOG_INFO("[pgsql] SCRAM-SHA-256 Authentication successful: server signature verified");
                    break;
                } catch (std::exception &ex) {
                    LOG_CRIT("[pgsql] SCRAM-SHA-256 Failed verifying server signature: " << ex.what());
                }
            }
                break;
            default: {
                LOG_CRIT("[pgsql] Unsupported authentication scheme " << auth_state << "requested by server");
                throw std::runtime_error("[pgsql] fatal error: check logs");
            }
        }
    }
    void
    on_command_complete(message &msg) {
        command_complete cmpl;
        msg.read(cmpl.command_tag);
        LOG_DEBUG("[pgsql] Command complete (" << cmpl.command_tag << ")");
    }
    void
    on_backend_key_data(message &msg) {
        msg.read(serverPid_);
        msg.read(serverSecret_);
        LOG_DEBUG("[pgsql] Received backend key data");
    }
    void
    on_error_response(message &msg) {
        notice_message notice;
        msg.read(notice);

        LOG_WARN("[pgsql] Error " << notice);
        error::query_error err(notice.message, notice.severity, notice.sqlstate, notice.detail);

        on_error_query(err);
    }
    void
    on_parameter_status(message &msg) {
        std::string key;
        std::string value;

        msg.read(key);
        msg.read(value);

        LOG_DEBUG("[pgsql] Received parameter " << key << "=" << value);

        client_opts_[key] = value;
    }
    void
    on_notice_response(message &msg) {
        notice_message notice;
        msg.read(notice);

        LOG_INFO("[pgsql] Received notice" << notice);
    }
    void
    on_ready_for_query(message &msg) {
        on_success_query();
        char stat(0);
        msg.read(stat);

        if (!process_query(_current_command)) {
            _ready_for_query = true;
            LOG_DEBUG(
                "[pgsql] Database " << conn_opts_.uri << "[" << conn_opts_.database << "]"
                                    << " is ready for query (" << stat << ")");
        }
    }
    void
    on_row_description(message &msg) {
        row_description_type fields;
        smallint col_cnt;
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
    void
    on_parse_complete(message &) {
        LOG_DEBUG("[pgsql] Parse complete");
    }
    void
    on_parameter_description(message &) {
        LOG_DEBUG("[pgsql] Parameter descriptions");
    }
    void
    on_bind_complete(message &) {
        LOG_DEBUG("[pgsql] Bind complete");
    }
    void
    on_no_data(message &) {
        LOG_DEBUG("[pgsql] No data");
    }
    void
    on_portal_suspended(message &) {
        LOG_DEBUG("[pgsql] Portal suspended");
    }
    void
    on_unhandled_message(message &msg) {
        LOG_DEBUG("[pgsql] Unhandled message tag " << (char)msg.tag());
    }

    inline static const qb::unordered_flat_map<int, void (Database::*)(message &)> routes_ = {
        {authentication_tag, &Database::on_authentication},
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
    Database()
        : Transaction(storage_) {}
    explicit Database(std::string const &opts)
        : Transaction(storage_)
        , conn_opts_(connection_options::parse(opts)) {}

    bool
    connect() {
        if (!this->transport().connect(qb::io::uri{conn_opts_.schema + "://" + conn_opts_.uri})) {

            if (this->protocol())
                this->clear_protocols();

            this->template switch_protocol<pg_protocol>(*this);
            this->start();
            send_startup_message();

            return true;
        }
        return false;
    }
    bool
    connect(std::string const &conn_opts) {
        conn_opts_ = connection_options::parse(conn_opts);
        return connect();
    }
    bool
    connect(std::string const &conn_opts, typename QB_IO_::transport_io_type &&raw_io) {
        conn_opts_ = connection_options::parse(conn_opts);
        this->transport() = std::move(raw_io);

        return connect();
    }

    void
    on(typename pg_protocol::message msg) {
        const auto it = routes_.find(msg->tag());
        if (qb::likely(it != routes_.end()))
            (this->*(it->second))(*msg);
        else
            on_unhandled_message(*msg);
    }

    void
    on(qb::io::async::event::disconnected const &) {
        on_error_query(error::client_error("database disconnected"));
    }
};

} // namespace detail

template <typename QB_IO_>
using database = detail::Database<QB_IO_>;
using transaction = detail::Transaction;
using results = detail::resultset;
using params = detail::QueryParams;

struct tcp {
    using database = detail::Database<qb::io::transport::tcp>;
#ifdef QB_IO_WITH_SSL
    struct ssl {
        using database = detail::Database<qb::io::transport::stcp>;
    };
#endif
};

} // namespace qb::pg

namespace qb::allocator {

template <>
pipe<char> &pipe<char>::put<qb::pg::detail::message>(const qb::pg::detail::message &);

} // namespace qb::allocator

#endif // QBM_PGSQL_H
