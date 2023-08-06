#
# qb - C++ Actor Framework
# Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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

project(qbm-pgsql)

set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

qb_register_module(
        NAME pgsql
        VERSION 1.0.0
        DEPENDENCIES
        ${Boost_LIBRARIES}
        SOURCES
        not-qb/sqlstates.cpp
        not-qb/error.cpp
        not-qb/pg_types.cpp
        not-qb/common.cpp
        not-qb/protocol_io_traits.cpp
        not-qb/protocol_parsers.cpp
        not-qb/protocol.cpp
        not-qb/result_impl.cpp
        not-qb/resultset.cpp
        detail/transaction.cpp
        pgsql.cpp
)

if (QB_BUILD_TEST)
    add_subdirectory(tests)
endif ()