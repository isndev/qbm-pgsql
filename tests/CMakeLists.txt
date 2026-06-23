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

set(QB_PGSQL_TESTS
        param-serializer
        param-unserializer
        param-parsing
        protocol-message
        connection
        #        connection-ssl
        data-types
        data-types-integration
        prepared-statements
        transaction
        transaction-advanced
        operations
        queries
        error-handling
        protocol-integration
        pgsql-coro-api
        notify
)

# Register each test
foreach (TEST ${QB_PGSQL_TESTS})
    qb_register_module_test(
            MODULE_NAME pgsql
            TEST_NAME ${TEST}
            SOURCES test-${TEST}.cpp
    )
endforeach ()

# Integration tests share localhost:5432; parallel ctest would collide on the same DB.
set(_qbm_pgsql_ctest_names "")
foreach (TEST ${QB_PGSQL_TESTS})
    list(APPEND _qbm_pgsql_ctest_names "qbm-pgsql-test-${TEST}")
endforeach ()
set_tests_properties(${_qbm_pgsql_ctest_names} PROPERTIES RESOURCE_LOCK qb_pgsql_integration)

if (QB_HAS_SSL)
    qb_register_module_test(
            MODULE_NAME pgsql
            TEST_NAME connection-ssl
            SOURCES test-connection-ssl.cpp
    )
    set_tests_properties(qbm-pgsql-test-connection-ssl PROPERTIES RESOURCE_LOCK qb_pgsql_integration)
endif ()