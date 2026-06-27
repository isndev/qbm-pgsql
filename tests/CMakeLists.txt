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
# -----------------------------------------------------------------------------
# qbm-pgsql tiered test suite (unit / system / integration / benchmark).
#   unit/        pure logic, daemon-free, parallel-safe (no RESOURCE_LOCK)
#   system/      in-process event loop, no daemon (connect-to-dead-host timeout)
#   integration/ needs live postgres:5432 — REQUIRES live (skip-not-fail), shares
#                the qb_pgsql_integration RESOURCE_LOCK (serialized against the DB)
#   benchmark/   google-benchmark; the wire benches are daemon-free
# Includes resolve "../pgsql.h" / "../../shared/*.hpp" via INCLUDES=tests dir.
# -----------------------------------------------------------------------------

# unit / system: tier + module labels, INCLUDES tests dir, optional REQUIRES, no lock.
function(qpg_test TIER NAME RELPATH)
    cmake_parse_arguments(T "" "" "REQUIRES;EXTRA_LABELS" ${ARGN})
    qb_register_module_test(
        MODULE_NAME pgsql TIER ${TIER} TEST_NAME ${NAME}
        SOURCES ${RELPATH}
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
        LABELS module:qbm-pgsql tier:${TIER} ${T_EXTRA_LABELS}
        REQUIRES ${T_REQUIRES})
endfunction()

# integration: as qpg_test + RESOURCE_LOCK qb_pgsql_integration + REQUIRES live.
function(qpg_itest NAME RELPATH)
    cmake_parse_arguments(T "" "" "REQUIRES;EXTRA_LABELS" ${ARGN})
    qb_register_module_test(
        MODULE_NAME pgsql TIER integration TEST_NAME ${NAME}
        SOURCES ${RELPATH}
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
        LABELS module:qbm-pgsql tier:integration ${T_EXTRA_LABELS}
        REQUIRES live ${T_REQUIRES}
        RESOURCE_LOCK qb_pgsql_integration)
endfunction()

function(qpg_bench NAME RELPATH)
    cmake_parse_arguments(B "" "" "REQUIRES;EXTRA_LABELS" ${ARGN})
    qb_register_module_benchmark(
        MODULE_NAME pgsql BENCH_NAME ${NAME}
        SOURCES ${RELPATH}
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
        LABELS module:qbm-pgsql bench ${B_EXTRA_LABELS}
        REQUIRES ${B_REQUIRES})
endfunction()

# -----------------------------------------------------------------------------
# UNIT — pure logic, parallel-safe
# -----------------------------------------------------------------------------
qpg_test(unit unserializer-primitives  unit/wire/unserializer-primitives.cpp   EXTRA_LABELS wire)
qpg_test(unit typeconverter-codecs     unit/wire/typeconverter-codecs.cpp      EXTRA_LABELS wire)
qpg_test(unit param-serializer-encode  unit/serialization/param-serializer-encode.cpp EXTRA_LABELS wire)
qpg_test(unit typeconverter-scalar     unit/types/typeconverter-scalar.cpp     EXTRA_LABELS types)
qpg_test(unit typeconverter-numeric    unit/types/typeconverter-numeric.cpp    EXTRA_LABELS types)
qpg_test(unit typeconverter-temporal   unit/types/typeconverter-temporal.cpp   EXTRA_LABELS types)
qpg_test(unit typeconverter-array      unit/types/typeconverter-array.cpp      EXTRA_LABELS types)
qpg_test(unit typeconverter-json       unit/types/typeconverter-json.cpp       EXTRA_LABELS types)
qpg_test(unit datastructures           unit/types/datastructures.cpp           EXTRA_LABELS types)
qpg_test(unit oid-stream               unit/types/oid-stream.cpp               EXTRA_LABELS types)
qpg_test(unit protocol-message-codec   unit/protocol/protocol-message-codec.cpp EXTRA_LABELS protocol)
qpg_test(unit dsn-parse                unit/dsn/dsn-parse.cpp                   EXTRA_LABELS dsn)
qpg_test(unit scram-and-cancel         unit/auth/scram-and-cancel.cpp          EXTRA_LABELS auth)
qpg_test(unit prepared-storage-lru     unit/prepared/prepared-storage-lru.cpp  EXTRA_LABELS prepared)

# -----------------------------------------------------------------------------
# SYSTEM — no daemon (network-timing dependent → slow)
# -----------------------------------------------------------------------------
qpg_test(system connect-timeout        system/connection/connect-timeout.cpp   EXTRA_LABELS slow)

# -----------------------------------------------------------------------------
# INTEGRATION — live postgres:5432 (REQUIRES live, RESOURCE_LOCK)
# -----------------------------------------------------------------------------
qpg_itest(connection-lifecycle  integration/connection/connection-lifecycle.cpp)
qpg_itest(queries               integration/query/queries-execution.cpp        REQUIRES coroutine)
qpg_itest(prepared-statements   integration/prepared/prepared-statements.cpp   REQUIRES coroutine)
qpg_itest(transaction-basic     integration/transaction/transaction-basic.cpp  REQUIRES coroutine)
qpg_itest(transaction-advanced  integration/transaction/transaction-advanced.cpp)
qpg_itest(datatypes-roundtrip   integration/datatypes/datatypes-roundtrip.cpp  REQUIRES coroutine)
qpg_itest(wire-formats          integration/protocol/wire-formats.cpp)
qpg_itest(listen-notify         integration/notify/listen-notify.cpp           REQUIRES coroutine)
qpg_itest(coro-api              integration/api/coro-api.cpp                    REQUIRES coroutine)
qpg_itest(errors-sqlstate       integration/errors/errors-sqlstate.cpp         REQUIRES coroutine)

if(QB_HAS_SSL)
    qpg_itest(connection-ssl    integration/connection/connection-ssl.cpp      REQUIRES ssl coroutine EXTRA_LABELS ssl)
endif()

# -----------------------------------------------------------------------------
# BENCHMARK — wire benches daemon-free; prepared-throughput is live
# -----------------------------------------------------------------------------
qpg_bench(codec-decode    benchmark/wire/codec-decode-bench.cpp   EXTRA_LABELS wire)
qpg_bench(param-encode    benchmark/wire/param-encode-bench.cpp   EXTRA_LABELS wire)
qpg_bench(prepared-throughput benchmark/wire/prepared-throughput-bench.cpp REQUIRES live EXTRA_LABELS wire)
