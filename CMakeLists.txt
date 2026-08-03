#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
#         limitations under the License.
#

project(qbm-pgsql VERSION 3.0.0)

# Check if qb framework is available
if (NOT QB_FOUND)
    qb_status_message("qb framework not found, skipping PostgreSQL module")
    return()
endif ()

# TCP/cleartext builds without OpenSSL; qb::pg::tcp::ssl::database requires QB_HAS_SSL.
# Two things are lost, not one, and the second is easy to miss: besides the TLS transport
# alias, the AUTHENTICATION methods that need real crypto are gone. MD5 needs qb::crypto::md5
# and SCRAM-SHA-256 needs a CSPRNG nonce + PBKDF2-HMAC-SHA256 + HMAC, none of which exist
# without OpenSSL, so pgsql.h compiles a refusal in their place (see refuse_auth_without_ssl:
# LOG_CRIT + terminal connect failure). SCRAM is PostgreSQL's default password_encryption
# since 14, so a stock server will refuse this client unless the role uses trust/password.
if (NOT QB_HAS_SSL)
    qb_status_message("PostgreSQL: building cleartext TCP only (QB_HAS_SSL off — no TLS transport alias;")
    qb_status_message("            auth limited to 'trust' and 'password' — MD5 and SCRAM-SHA-256 unavailable)")
endif ()

# Register the PostgreSQL module using the new framework functions
qb_register_module(
        NAME pgsql
        VERSION ${PROJECT_VERSION}
        DEPENDS qb-core
        SOURCES
        src/qbm/pgsql/sqlstates.cpp
        src/qbm/pgsql/error.cpp
        src/qbm/pgsql/pg_types.cpp
        src/qbm/pgsql/common.cpp
        src/qbm/pgsql/protocol.cpp
        src/qbm/pgsql/protocol_io_traits.cpp
        src/qbm/pgsql/result_impl.cpp
        src/qbm/pgsql/resultset.cpp
        src/qbm/pgsql/transaction.cpp
        src/qbm/pgsql/field_reader_integration.cpp
        src/qbm/pgsql/param_unserializer.cpp
        src/qbm/pgsql/type_converter.cpp
        src/qbm/pgsql/param_serializer.cpp
        src/qbm/pgsql/field_reader.cpp
        src/qbm/pgsql/queries.cpp
        src/qbm/pgsql/pg_notify_sql.cpp
        src/qbm/pgsql/pgsql.cpp
        DEFINES
        QBM_PGSQL_VERSION="${PROJECT_VERSION}"
)

# No CONFIG_DEPENDENCIES / INSTALL_CMAKE_FILES: this module's only consume-time dependency is
# qb::core, which the generated qbm-pgsqlConfig.cmake already resolves with find_dependency(qb).
# There is no libpq here -- the PostgreSQL wire protocol (src/qbm/pgsql/protocol.cpp) and SCRAM are
# implemented in-tree.

# Add tests if enabled
if (QB_BUILD_TESTS)
    add_subdirectory(tests)
endif ()