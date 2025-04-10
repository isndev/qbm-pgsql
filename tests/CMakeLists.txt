#
# qb - C++ Actor Framework
# Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
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
        connection
        data-types
        data-types-integration
        prepared-statements
        transaction
        transaction-advanced
        operations
        queries
        params
        error-handling
)

# Register each test
foreach (TEST ${QB_PGSQL_TESTS})
    qb_register_module_gtest(
            NAME pgsql
            TESTNAME ${TEST}
            SOURCES test-${TEST}.cpp
    )
endforeach ()