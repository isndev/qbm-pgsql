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

#ifndef QBM_PGSQL_DETAIL_QUERIES_H
#define QBM_PGSQL_DETAIL_QUERIES_H
#include "../not-qb/common.h"
#include "../not-qb/error.h"
#include "../not-qb/protocol.h"
#include "../not-qb/write_format.h"
#include <qb/io.h>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/branch_hints.h>
#include <type_traits>

namespace qb::pg::detail {
using namespace qb::pg;

struct PreparedQuery {
    std::string name;
    std::string expression;
    type_oid_sequence param_types;
    row_description_type row_description;
};

class PreparedQueryStorage {
    qb::unordered_map<std::string, PreparedQuery> _prepared_queries;

public:
    PreparedQueryStorage() = default;

    bool
    has(std::string const &name) const {
        return _prepared_queries.find(name) != _prepared_queries.cend();
    }

    const PreparedQuery &
    push(PreparedQuery &&query) {
        return _prepared_queries.emplace(query.name, std::move(query)).first->second;
    }

    PreparedQuery const &
    get(std::string const &name) const {
        return _prepared_queries.at(name);
    }
};

class QueryParams {
    std::vector<byte> _params;

public:
    template <typename... T>
    QueryParams(T &&...args) {
        if constexpr (static_cast<bool>(sizeof...(T))) {
            std::vector<oids::type::oid_type> param_types;
            write_params(param_types, _params, std::forward<T>(args)...);
        }
    }
    std::vector<byte> &
    get() {
        return _params;
    }
};

class ISqlQuery {
public:
    ISqlQuery() = default;
    virtual ~ISqlQuery() = default;

    virtual bool
    is_valid() const {
        return true;
    }
    virtual message get() const = 0;
    virtual void on_success() const = 0;
    virtual void on_error(error::db_error const &err) const = 0;
};

template <typename CB_SUCCESS, typename CB_ERROR>
class SqlQuery : public ISqlQuery {
    CB_SUCCESS _on_success;
    CB_ERROR _on_error;

public:
    SqlQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : _on_success(std::forward<CB_SUCCESS>(success))
        , _on_error(std::forward<CB_ERROR>(error)) {}
    virtual ~SqlQuery() = default;

    void
    on_success() const final {
        _on_success();
    }

    void
    on_error(error::db_error const &err) const final {
        _on_error(err);
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class BeginQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    transaction_mode _mode;

public:
    BeginQuery(transaction_mode mode, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _mode(mode) {}

    message
    get() const final {
        LOG_DEBUG("[pgsql] Send BEGIN");
        message m(query_tag);
        ::std::ostringstream cmd{"begin"};
        cmd << _mode;
        m.write(cmd.str());
        return m;
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class CommitQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {

public:
    CommitQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error)) {}

    message
    get() const final {
        LOG_DEBUG("[pgsql] Send COMMIT");
        message m(query_tag);
        m.write("commit");
        return m;
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class RollbackQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {

public:
    RollbackQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error)) {}

    message
    get() const final {
        LOG_DEBUG("[pgsql] Send ROLLBACK");
        message m(query_tag);
        m.write("rollback");
        return m;
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class SavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string const &_name;

public:
    SavePointQuery(std::string const &name, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _name(name) {}

    message
    get() const final {
        LOG_DEBUG("[pgsql] Send SAVEPOINT " << _name);
        message m(query_tag);
        m.write("savepoint " + _name);
        return m;
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class ReleaseSavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string const &_name;

public:
    ReleaseSavePointQuery(std::string const &name, CB_SUCCESS &&success,
                          CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _name(name) {}

    message
    get() const final {
        LOG_DEBUG("[pgsql] Send RELEASE SAVEPOINT " << _name);
        message m(query_tag);
        m.write("release savepoint " + _name);
        return m;
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class RollbackSavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string const &_name;

public:
    RollbackSavePointQuery(std::string const &name, CB_SUCCESS &&success,
                           CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _name(name) {}

    message
    get() const final {
        LOG_DEBUG("[pgsql] Send ROLLBACK TO SAVEPOINT " << _name);
        message m(query_tag);
        m.write("rollback to savepoint " + _name);
        return m;
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class SimpleQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    const std::string _expression;

public:
    SimpleQuery(std::string &&expr, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _expression(std::move(expr)) {}

    message
    get() const final {
        LOG_DEBUG("[pgsql] Send QUERY \"" << _expression << "\"");
        message m(query_tag);
        m.write(_expression);

        return m;
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class ParseQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    PreparedQuery const &_query;

public:
    ParseQuery(PreparedQuery const &query, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success),
                                         std::forward<CB_ERROR>(error))
        , _query(query) {}

    message
    get() const final {
        LOG_DEBUG("[pgsql] Send PARSE QUERY \"" << _query.expression << "\"");
        message cmd(parse_tag);
        cmd.write(_query.name);
        cmd.write(_query.expression);
        cmd.write((smallint)_query.param_types.size());
        for (oids::type::oid_type oid : _query.param_types) {
            cmd.write((integer)oid);
        }

        message describe(describe_tag);
        describe.write('S');
        describe.write(_query.name);
        cmd.pack(describe);
        cmd.pack(message(sync_tag));

        return cmd;
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class BindExecQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    PreparedQueryStorage const &_storage;
    std::string const &_query_name;
    std::vector<byte> _params;

public:
    BindExecQuery(PreparedQueryStorage const &storage, std::string const &query_name,
                  std::vector<byte> &&params, CB_SUCCESS &&success, CB_ERROR &&error)
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
        message cmd(bind_tag);
        cmd.write(""); // portal_name
        cmd.write(query.name);
        if (!_params.empty()) {
            auto out = cmd.output();
            std::copy(_params.begin(), _params.end(), out);
        } else {
            cmd.write((smallint)0); // parameter format codes
            cmd.write((smallint)0); // number of parameters
        }

        auto const &fields = query.row_description;
        cmd.write((smallint)fields.size());
        LOG_DEBUG("[pgsql] Write " << fields.size() << " field formats");
        for (auto const &fd : fields) {
            cmd.write((smallint)fd.format_code);
        }
        LOG_DEBUG("[pgsql] Execute prepared [" << query.name << "] \""
                                               << query.expression << "\"");
        // issue logger does not accept binary
        // << " params : " << std::string_view(_params.data(), _params.size()));

        message execute(execute_tag);
        execute.write(""); // portal name
        execute.write(0);
        cmd.pack(execute);
        cmd.pack(message(sync_tag));

        return cmd;
    }
};

} // namespace qb::pg::detail

#endif // QBM_PGSQL_DETAIL_QUERIES_H
