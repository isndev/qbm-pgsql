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

cmake_minimum_required(VERSION 3.24)

project(qbm-pgsql VERSION 3.0.0)

# This module is NOT standalone-configurable, and it says so here rather than dying on whichever
# qb_* command it happens to reach first.
#
# qbm modules are built as part of the qb-dev superproject, which include()s qb's cmake/ helpers
# before add_subdirectory()ing them: this file calls qb_status_message(), qb_register_module() and
# qb_add_test(), and an INSTALLED qb ships none of those (they are development-time helpers, not
# package content -- lib/cmake/qb/ carries only qbConfig, qbConfigVersion, qbTargets and the
# Find modules). Pointing CMAKE_PREFIX_PATH at an installed qb therefore does not help, which is
# exactly the mistake this guard is here to name: without it the first symptom is
#     CMake Error at CMakeLists.txt:22 (qb_status_message):
#       Unknown CMake command "qb_status_message".
# which reads like a missing include, not like "wrong entry point".
#
# To build this module: configure the qb-dev superproject root. To get an INSTALLABLE one:
#     cmake --preset package && cmake --build --preset package
#     cmake --install build/presets/package --prefix <prefix>
# then consume it with find_package(qbm-pgsql CONFIG REQUIRED) -> qbm::pgsql.
if(NOT COMMAND qb_status_message)
    message(FATAL_ERROR
        "[qbm-pgsql] this module cannot be configured on its own.\n"
        "     It is built from the qb-dev superproject, which loads qb's CMake helpers "
        "(qb_status_message / qb_register_module / qb_add_test) first; an installed qb does not "
        "ship them, so CMAKE_PREFIX_PATH will not fix this.\n"
        "     Configure the superproject root instead, or use its `package` preset to produce an "
        "installable qbm-pgsql and consume that with find_package(qbm-pgsql CONFIG REQUIRED).")
endif()

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
        # The one file under src/qbm/pgsql/ that is NOT part of this module's public surface.
        #
        # field_handler.h is dead and broken, and it was shipped to every consumer anyway.
        # Dead: no TU in this module, in the superproject, in the examples or in any test
        # includes it -- the live decode path is resultset.h's merged tail (field::as<T> at
        # resultset.h:552, resultset::row::to at resultset.h:975). Broken: compiled alone it
        # produces 11 errors under clang 17 (`typename specifier refers to non-type member
        # 'field' in 'qb::pg::resultset'` x6, `no member named 'deserialize' in
        # ParamUnserializer`, `inline declaration of 'to' follows non-inline definition` x2,
        # and an unqualified field_is_null). The last of those is the substance: it redefines
        # members resultset.h already defines, so it is not a missing #include -- reconciling
        # it means deciding whether that feature comes back, which is a maintainer call.
        #
        # Until that call is made the file stays in the tree (it is still swept by
        # check-header-linkage.py, check-namespace-scoped-includes.py and
        # check-abi-macro-split.py, all of which read src/ rather than a prefix) but it must
        # not reach a prefix. It had been handled the other way round -- installed, and named
        # as a permanent exclusion in qb/scripts/check-installed-headers.sh -- which is the
        # worse half of the trade: it left the only broken file in the shipped surface as the
        # only file the self-containment gate could not see.
        HEADER_EXCLUDE "/qbm/pgsql/field_handler\\.h$"
)

# No CONFIG_DEPENDENCIES / INSTALL_CMAKE_FILES: this module's only consume-time dependency is
# qb::core, which the generated qbm-pgsqlConfig.cmake already resolves with find_dependency(qb).
# There is no libpq here -- the PostgreSQL wire protocol (src/qbm/pgsql/protocol.cpp) and SCRAM are
# implemented in-tree.

# Add tests if enabled
if (QB_BUILD_TESTS)
    add_subdirectory(tests)
endif ()