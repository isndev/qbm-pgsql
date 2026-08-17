/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file integration/serialization/param-roundtrip.cpp
 * @brief End-to-end parameter-serializer coverage: bind through a REAL prepared statement
 *        and read the value the server decoded back.
 *
 * The unit suite (`unit/serialization/param-serializer-encode.cpp`) pins the *wire bytes*
 * `ParamSerializer` emits. This file complements it by binding those bytes to a live
 * PostgreSQL via the unnamed / named prepared-statement path and asserting the SERVER round-
 * trips them to the expected value — the part a byte-level unit test cannot prove. It targets
 * the serializer branches that only matter once they reach the backend:
 *
 *  - `add_param(std::nullptr_t)` -> `add_null` (the -1 length sentinel) decodes as SQL NULL.
 *  - `std::optional<T>` with / without a value (the optional branch in `add_param`).
 *  - An EMPTY `std::vector<int>` binds an EMPTY ARRAY ('{}'), NOT NULL — `array_length` is
 *    NULL but `cardinality` is 0 and `$1 = '{}'` holds (the add_vector empty-array body).
 *  - A populated `std::vector<int>` binds an int4[] usable with `= ANY($1)`.
 *  - A `std::vector<std::string>` expands to N separate text params (the documented
 *    add_string_vector batch asymmetry) and fills a multi-row VALUES insert.
 *  - Mixed scalar + array + scalar in one bind (param-count + per-param OID routing).
 *  - The two loud throw paths: > MAX_PARAMS bind params -> std::length_error
 *    (`ensure_param_count_fits`); a vector whose element type has no PostgreSQL array
 *    companion -> std::invalid_argument (`add_vector`). These run without the daemon.
 *
 * Tier: integration (REQUIRES live postgres) for the round-trips; the throw cases need no
 * server. Shared skip-not-fail fixture: daemon down -> Skipped, never Failed.
 *
 * @ingroup Pgsql
 */
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

constexpr std::string_view kBatchTable = "qb_pgsql_param_batch_t";

} // namespace

/**
 * @brief Connect-or-skip fixture for the serializer round-trip cases.
 */
class PgsqlParamRoundTrip : public qb::pg::test::PgIntegrationTest {
protected:
    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
    }
};

// --------------------------------------------------------------------------------------
// NULL / optional binding (add_param nullptr_t + optional branch)
// --------------------------------------------------------------------------------------

// params{nullptr} binds a SQL NULL: the server sees $1 IS NULL. A non-null optional binds
// its value; a nullopt optional binds NULL.
TEST_F(PgsqlParamRoundTrip, NullAndOptionalBindings) {
    bool null_is_null = false, opt_value = false, opt_null = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        // nullptr -> NULL. $1 has no inferred type, so cast in SQL.
        auto p1 = co_await db_->prepare("", "SELECT ($1::int) IS NULL AS is_null", type_oid_sequence{oid::int4});
        if (!p1)
            co_return;
        auto r1      = co_await db_->execute("", params{nullptr});
        null_is_null = r1.ok() && r1.result().size() == 1 && r1.result()[0][0].as<bool>();

        // std::optional<int> with a value -> 5; without -> NULL.
        auto p2 = co_await db_->prepare("", "SELECT $1::int AS v", type_oid_sequence{oid::int4});
        if (!p2)
            co_return;
        auto r2   = co_await db_->execute("", params{std::optional<int>{5}});
        opt_value = r2.ok() && r2.result().size() == 1 && r2.result()[0][0].as<int>() == 5;

        auto r3  = co_await db_->execute("", params{std::optional<int>{}});
        opt_null = r3.ok() && r3.result().size() == 1 && r3.result()[0][0].is_null();
        co_return;
    }());
    EXPECT_TRUE(null_is_null) << "params{nullptr} must bind a SQL NULL";
    EXPECT_TRUE(opt_value) << "a populated std::optional must bind its value";
    EXPECT_TRUE(opt_null) << "an empty std::optional must bind NULL";
}

// --------------------------------------------------------------------------------------
// Array binding: empty vs populated int4[] (add_vector)
// --------------------------------------------------------------------------------------

// An empty std::vector<int> binds an EMPTY ARRAY (not NULL): array_length is NULL,
// cardinality is 0, and it equals the '{}' literal. A NULL bind would make all three differ.
TEST_F(PgsqlParamRoundTrip, EmptyIntArrayBindsEmptyArrayNotNull) {
    bool ok = false, is_empty_not_null = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto p = co_await db_->prepare("",
                                       "SELECT $1::int[] = '{}'::int[]      AS eq_empty, "
                                       "       cardinality($1::int[])       AS card, "
                                       "       ($1::int[]) IS NULL          AS is_null",
                                       type_oid_sequence{oid::int4_array});
        if (!p)
            co_return;
        auto r = co_await db_->execute("", params{std::vector<int>{}});
        ok     = r.ok() && r.result().size() == 1;
        if (ok) {
            const auto &row   = r.result()[0];
            is_empty_not_null = row[0].as<bool>()        // = '{}'
                                && row[1].as<int>() == 0 // cardinality 0
                                && !row[2].as<bool>();   // NOT NULL
        }
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_TRUE(is_empty_not_null) << "an empty vector must bind '{}'::int[], not NULL";
}

// A populated std::vector<int> binds int4[] usable with = ANY($1): the matched and the
// unmatched lookup both decode correctly.
TEST_F(PgsqlParamRoundTrip, PopulatedIntArrayWorksWithAnyOperator) {
    bool hit = false, miss = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto p = co_await db_->prepare("", "SELECT 3 = ANY($1::int[]) AS m", type_oid_sequence{oid::int4_array});
        if (!p)
            co_return;
        auto in  = co_await db_->execute("", params{std::vector<int>{1, 2, 3, 4}});
        hit      = in.ok() && in.result()[0][0].as<bool>();
        auto out = co_await db_->execute("", params{std::vector<int>{7, 8, 9}});
        miss     = out.ok() && !out.result()[0][0].as<bool>();
        co_return;
    }());
    EXPECT_TRUE(hit) << "3 must be ANY of {1,2,3,4}";
    EXPECT_TRUE(miss) << "3 must NOT be ANY of {7,8,9}";
}

// --------------------------------------------------------------------------------------
// std::vector<std::string> expands to N text params (add_string_vector batch path)
// --------------------------------------------------------------------------------------

// A vector<string> binds as N separate text params (NOT a text[] array). Driven through a
// 3-row VALUES INSERT with $1,$2,$3 — all three rows must land verbatim.
TEST_F(PgsqlParamRoundTrip, StringVectorExpandsToMultiRowInsert) {
    bool ok = false;
    int  n  = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto drop = co_await db_->execute(std::string("DROP TABLE IF EXISTS ") + std::string(kBatchTable));
        if (!drop)
            co_return;
        auto create = co_await db_->execute(std::string("CREATE TEMP TABLE ") + std::string(kBatchTable) + " (v TEXT NOT NULL)");
        if (!create)
            co_return;

        // Three text params expanded from one vector<string>.
        auto p =
            co_await db_->prepare("qb_str_vec_batch", std::string("INSERT INTO ") + std::string(kBatchTable) + " (v) VALUES ($1),($2),($3)",
                                  type_oid_sequence{oid::text, oid::text, oid::text});
        if (!p)
            co_return;
        // Named: a temporary built inside the co_await operand has to be promoted into the
        // coroutine frame to outlive the suspension.
        const std::vector<std::string> batch{"sv_a", "sv_b", "sv_c"};
        auto                           ins = co_await db_->execute("qb_str_vec_batch", params{batch});
        if (!ins)
            co_return;
        auto cnt =
            co_await db_->query(std::string("SELECT COUNT(*)::int FROM ") + std::string(kBatchTable) + " WHERE v IN ('sv_a','sv_b','sv_c')");
        ok = cnt.ok() && cnt.result().size() == 1;
        if (ok)
            n = cnt.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_EQ(n, 3) << "vector<string> must expand to 3 distinct text params, one per row";
}

// --------------------------------------------------------------------------------------
// Mixed scalar + array + scalar in one bind (per-param OID routing + count)
// --------------------------------------------------------------------------------------

// One bind mixing text, int4[] and int4 decodes each param to the right value and shape.
TEST_F(PgsqlParamRoundTrip, MixedScalarArrayScalarBind) {
    bool ok = false, correct = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto p = co_await db_->prepare("", "SELECT $1::text AS t, cardinality($2::int[]) AS card, $3::int AS n",
                                       type_oid_sequence{oid::text, oid::int4_array, oid::int4});
        if (!p)
            co_return;
        auto r = co_await db_->execute("", params{std::string("mix"), std::vector<int>{10, 20, 30}, 42});
        ok     = r.ok() && r.result().size() == 1;
        if (ok) {
            const auto &row = r.result()[0];
            correct         = row[0].as<std::string>() == "mix" && row[1].as<int>() == 3 && row[2].as<int>() == 42;
        }
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_TRUE(correct) << "mixed text/int4[]/int4 bind must decode each param correctly";
}

// --------------------------------------------------------------------------------------
// Loud serializer throw paths (no daemon needed)
// --------------------------------------------------------------------------------------

// Binding more than the 16-bit wire limit (MAX_PARAMS = 32767) must throw std::length_error
// from ensure_param_count_fits rather than silently wrap the int16 count. Driven directly
// through ParamSerializer::serialize_params with 32768 params.
TEST(PgsqlParamSerializerThrows, TooManyParamsThrowsLengthError) {
    ParamSerializer s;
    // serialize_params over a single std::vector<int> binds ONE array param, so build the
    // overflow as a string-vector: each element expands to its own text param via
    // add_string_vector, giving param_count() = 32768 > MAX_PARAMS (32767).
    std::vector<std::string> overflow(ParamSerializer::MAX_PARAMS + 1, "x");
    EXPECT_THROW(s.serialize_params(overflow), std::length_error) << "binding > MAX_PARAMS params must throw, not wrap the int16 count";
}

// finalize_params_buffer() also enforces the cap (the callback-side twin of the check):
// after manually adding > MAX_PARAMS params it must throw.
TEST(PgsqlParamSerializerThrows, FinalizeEnforcesMaxParams) {
    ParamSerializer s;
    s.serialize_params(); // reset to a valid count-prefixed buffer (0 params)
    for (std::size_t i = 0; i <= ParamSerializer::MAX_PARAMS; ++i)
        s.add_integer(0); // pushes one OID + value each
    EXPECT_THROW(s.finalize_params_buffer(), std::length_error);
}

// A std::vector whose element type maps to a scalar OID with NO PostgreSQL array companion
// (here: geometric `point`, OID 600, absent from array_oid_for_element) must throw
// std::invalid_argument from add_vector rather than emit a wire-invalid anyarray Bind.
//
// We can't easily construct a C++ vector<point> with a TypeConverter, so assert the policy
// at the source of truth: array_oid_for_element(point) == invalid, the exact precondition
// add_vector turns into the throw.
TEST(PgsqlParamSerializerThrows, UnmappableElementHasNoArrayOid) {
    EXPECT_EQ(array_oid_for_element(oid::point), oid::invalid)
        << "an element type with no array companion must map to invalid so add_vector throws";
    EXPECT_EQ(array_oid_for_element(oid::any_array), oid::invalid);
    // A mapped element does have a concrete (non-invalid) array OID — the success side.
    EXPECT_NE(array_oid_for_element(oid::int4), oid::invalid);
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
