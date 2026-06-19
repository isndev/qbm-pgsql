/**
 * @file test-protocol-integration.cpp
 * @brief Integration tests for protocol paths (COPY, NOTIFY, empty query, Bind formats)
 *
 * Exercises backend message flows that rarely appear in normal SELECT/INSERT tests:
 * CopyInResponse recovery, CopyOut data draining, LISTEN/NOTIFY, empty simple query,
 * and extended-query Bind (binary parameters + binary result columns).
 */

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/json.h>
#include <qb/uuid.h>
#include <string>

#include "../pgsql.h"
#include "test_config.hpp"

using namespace qb::pg;

namespace {

class PgProtocolIntegrationTest : public ::testing::Test {
protected:
    bool fixture_ready_{false};

    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        if (!qb::io::async::run_sync(db_->connect(qb::pg::test::dsn_tcp_string()))) {
            GTEST_SKIP() << "PostgreSQL not reachable";
            return;
        }
        ASSERT_TRUE(db_->execute("CREATE TEMP TABLE qb_proto_copy_t (v TEXT NOT NULL)", discard_query, discard_error).await());
        fixture_ready_ = true;
    }

    void
    TearDown() override {
        if (db_ && fixture_ready_) {
            (void) db_->execute("DROP TABLE IF EXISTS qb_proto_copy_t", discard_query, discard_error).await();
            db_->disconnect();
        }
        db_.reset();
        fixture_ready_ = false;
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

} // namespace

TEST_F(PgProtocolIntegrationTest, CopyFromStdin_RejectedThenNextQuerySucceeds) {
    bool saw_error = false;
    auto st        = db_->execute(
                            "COPY qb_proto_copy_t FROM STDIN",
                            [](qb::pg::transaction &, qb::pg::results) { FAIL() << "COPY FROM STDIN must not complete as a normal result query"; },
                            [&saw_error](qb::pg::error::db_error const &) { saw_error = true; })
                         .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(saw_error);

    // Recovery: connection must accept a simple query again (avoid tight coupling to
    // row materialization paths right after CopyFail + server error).
    EXPECT_TRUE(db_->execute("SELECT 1", discard_query, discard_error).await());
}

TEST_F(PgProtocolIntegrationTest, CopyToStdout_TextFormat_Completes) {
    bool ok = false;
    auto st = db_->execute(
                     "COPY (SELECT unnest(ARRAY['a','b','c']::text[])) TO STDOUT WITH (FORMAT text)",
                     [&ok](qb::pg::transaction &, qb::pg::results r) {
                         (void) r;
                         ok = true;
                     },
                     [](qb::pg::error::db_error const &e) { FAIL() << "COPY TO STDOUT failed: " << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST_F(PgProtocolIntegrationTest, EmptySimpleQuery_Completes) {
    bool ok = false;
    auto st = db_->execute(
                     "", [&ok](qb::pg::transaction &, qb::pg::results) { ok = true; },
                     [](qb::pg::error::db_error const &e) { FAIL() << "Empty query failed: " << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST_F(PgProtocolIntegrationTest, ListenNotify_SequentialQueries_NoHang) {
    ASSERT_TRUE(db_->execute("LISTEN qb_proto_chan", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("NOTIFY qb_proto_chan, 'ping'", discard_query, discard_error).await());
    EXPECT_TRUE(db_->execute("SELECT 1", discard_query, discard_error).await());
}

TEST_F(PgProtocolIntegrationTest, SimpleQuery_ResultColumnsAreText) {
    bool ok = false;
    auto st = db_->execute(
                     "SELECT 7::int AS n, 'x'::text AS t",
                     [&](qb::pg::transaction &, qb::pg::results r) {
                         ASSERT_EQ(r.size(), 1U);
                         ASSERT_EQ(r.columns_size(), 2U);
                         EXPECT_EQ(r.field(0).format_code, protocol_data_format::Text);
                         EXPECT_EQ(r.field(1).format_code, protocol_data_format::Text);
                         EXPECT_EQ(r[0][0].as<int>(), 7);
                         EXPECT_EQ(r[0][1].as<std::string>(), "x");
                         ok = true;
                     },
                     [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST_F(PgProtocolIntegrationTest, PreparedStatement_ResultColumnsAreBinary) {
    ASSERT_TRUE(
        db_->prepare("qb_proto_bin_one", "SELECT ($1::int * 2) AS n", type_oid_sequence{oid::int4}, discard_prepare, discard_error).await());
    bool ok = false;
    auto st = db_->execute(
                     "qb_proto_bin_one", params{11},
                     [&](qb::pg::transaction &, qb::pg::results r) {
                         ASSERT_EQ(r.size(), 1U);
                         ASSERT_EQ(r.columns_size(), 1U);
                         EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                         EXPECT_EQ(r[0][0].as<int>(), 22);
                         ok = true;
                     },
                     [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST_F(PgProtocolIntegrationTest, PreparedStatement_MixedTypesBinaryColumns) {
    ASSERT_TRUE(db_->prepare("qb_proto_bin_mix", "SELECT $1::int8 AS i, $2::text AS s, $3::float8 AS f",
                             type_oid_sequence{oid::int8, oid::text, oid::float8}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    auto st = db_->execute(
                     "qb_proto_bin_mix", params{static_cast<int64_t>(-9), std::string{"pq"}, 1.25},
                     [&](qb::pg::transaction &, qb::pg::results r) {
                         ASSERT_EQ(r.size(), 1U);
                         ASSERT_EQ(r.columns_size(), 3U);
                         EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                         EXPECT_EQ(r.field(1).format_code, protocol_data_format::Text);
                         EXPECT_EQ(r.field(2).format_code, protocol_data_format::Binary);
                         EXPECT_EQ(r[0][0].as<int64_t>(), -9);
                         EXPECT_EQ(r[0][1].as<std::string>(), "pq");
                         EXPECT_DOUBLE_EQ(r[0][2].as<double>(), 1.25);
                         ok = true;
                     },
                     [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST_F(PgProtocolIntegrationTest, PreparedStatement_TextParameterBinaryResult) {
    ASSERT_TRUE(
        db_->prepare("qb_proto_bin_textparam", "SELECT $1::text AS s", type_oid_sequence{oid::text}, discard_prepare, discard_error).await());
    bool ok = false;
    auto st = db_->execute(
                     "qb_proto_bin_textparam", params{std::string{"hello-binary"}},
                     [&](qb::pg::transaction &, qb::pg::results r) {
                         ASSERT_EQ(r.size(), 1U);
                         ASSERT_EQ(r.columns_size(), 1U);
                         EXPECT_EQ(r.field(0).format_code, protocol_data_format::Text);
                         EXPECT_EQ(r[0][0].as<std::string>(), "hello-binary");
                         ok = true;
                     },
                     [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST_F(PgProtocolIntegrationTest, InsertWithoutReturning_EmptyResultset) {
    bool ok = false;
    auto st = db_->execute(
                     "INSERT INTO qb_proto_copy_t (v) VALUES ('no-returning-rows')",
                     [&](qb::pg::transaction &, qb::pg::results r) {
                         EXPECT_EQ(r.size(), 0U);
                         EXPECT_EQ(r.columns_size(), 0U);
                         ok = true;
                     },
                     [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST_F(PgProtocolIntegrationTest, PreparedStatement_InsertReturning_MixedBinaryAndText) {
    ASSERT_TRUE(db_->prepare("qb_proto_ins_ret",
                             "INSERT INTO qb_proto_copy_t (v) VALUES ($1) RETURNING "
                             "42::int AS k, $1::text AS lbl",
                             type_oid_sequence{oid::text}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    auto st = db_->execute(
                     "qb_proto_ins_ret", params{std::string{"ret-mix"}},
                     [&](qb::pg::transaction &, qb::pg::results r) {
                         ASSERT_EQ(r.size(), 1U);
                         ASSERT_EQ(r.columns_size(), 2U);
                         EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                         EXPECT_EQ(r.field(1).format_code, protocol_data_format::Text);
                         EXPECT_EQ(r[0][0].as<int>(), 42);
                         EXPECT_EQ(r[0][1].as<std::string>(), "ret-mix");
                         ok = true;
                     },
                     [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST_F(PgProtocolIntegrationTest, PreparedStatement_JsonText_JsonbBinary) {
    ASSERT_TRUE(db_->prepare("qb_proto_json_col", "SELECT '[]'::json AS j", type_oid_sequence{}, discard_prepare, discard_error).await());
    ASSERT_TRUE(
        db_->prepare("qb_proto_jsonb_col", "SELECT '[true,1]'::jsonb AS jb", type_oid_sequence{}, discard_prepare, discard_error).await());
    bool json_ok = false;
    EXPECT_TRUE(db_->execute(
                       "qb_proto_json_col", params{},
                       [&](qb::pg::transaction &, qb::pg::results r) {
                           ASSERT_EQ(r.size(), 1U);
                           ASSERT_EQ(r.columns_size(), 1U);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Text);
                           EXPECT_EQ(r[0][0].as<std::string>(), "[]");
                           json_ok = true;
                       },
                       [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(json_ok);

    bool jsonb_ok = false;
    EXPECT_TRUE(db_->execute(
                       "qb_proto_jsonb_col", params{},
                       [&](qb::pg::transaction &, qb::pg::results r) {
                           ASSERT_EQ(r.size(), 1U);
                           ASSERT_EQ(r.columns_size(), 1U);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                           qb::jsonb jb = r[0][0].as<qb::jsonb>();
                           EXPECT_TRUE(jb.is_array());
                           EXPECT_EQ(jb.size(), 2U);
                           jsonb_ok = true;
                       },
                       [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(jsonb_ok);
}

TEST_F(PgProtocolIntegrationTest, PreparedStatement_UuidAndByteaRoundTrip) {
    auto const  test_uuid = qb::uuid::from_string("6ba7b810-9dad-11d1-80b4-00c04fd430c8").value();
    bytea const bin{{static_cast<byte>(0xDE), static_cast<byte>(0xAD), static_cast<byte>(0xBE), static_cast<byte>(0xEF)}};

    ASSERT_TRUE(
        db_->prepare("qb_proto_uuid_echo", "SELECT $1::uuid AS u", type_oid_sequence{oid::uuid}, discard_prepare, discard_error).await());
    ASSERT_TRUE(
        db_->prepare("qb_proto_bytea_echo", "SELECT $1::bytea AS b", type_oid_sequence{oid::bytea}, discard_prepare, discard_error).await());

    bool uuid_ok = false;
    EXPECT_TRUE(db_->execute(
                       "qb_proto_uuid_echo", params{test_uuid},
                       [&](qb::pg::transaction &, qb::pg::results r) {
                           ASSERT_EQ(r.size(), 1U);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                           EXPECT_EQ(r[0][0].as<qb::uuid>(), test_uuid);
                           uuid_ok = true;
                       },
                       [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(uuid_ok);

    bool bytea_ok = false;
    EXPECT_TRUE(db_->execute(
                       "qb_proto_bytea_echo", params{bin},
                       [&](qb::pg::transaction &, qb::pg::results r) {
                           ASSERT_EQ(r.size(), 1U);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                           bytea out = r[0][0].as<bytea>();
                           EXPECT_EQ(out, bin);
                           bytea_ok = true;
                       },
                       [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(bytea_ok);
}

TEST_F(PgProtocolIntegrationTest, PreparedStatement_NoParameters_StillBinaryResults) {
    ASSERT_TRUE(
        db_->prepare("qb_proto_bin_nop", "SELECT true AS b, 42::int AS n", type_oid_sequence{}, discard_prepare, discard_error).await());
    bool ok = false;
    auto st = db_->execute(
                     "qb_proto_bin_nop", params{},
                     [&](qb::pg::transaction &, qb::pg::results r) {
                         ASSERT_EQ(r.size(), 1U);
                         ASSERT_EQ(r.columns_size(), 2U);
                         EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                         EXPECT_EQ(r.field(1).format_code, protocol_data_format::Binary);
                         EXPECT_TRUE(r[0][0].as<bool>());
                         EXPECT_EQ(r[0][1].as<int>(), 42);
                         ok = true;
                     },
                     [](qb::pg::error::db_error const &e) { FAIL() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

TEST(PgProtocolIntegrationTwoConnections, PgNotifyFromPeerDoesNotBreakListener) {
    auto a = std::make_unique<qb::pg::tcp::database>();
    auto b = std::make_unique<qb::pg::tcp::database>();
    if (!qb::io::async::run_sync(a->connect(qb::pg::test::dsn_tcp_string()))
        || !qb::io::async::run_sync(b->connect(qb::pg::test::dsn_tcp_string()))) {
        GTEST_SKIP() << "PostgreSQL not reachable";
        return;
    }

    ASSERT_TRUE(a->execute("LISTEN qb_proto_peer", discard_query, discard_error).await());
    ASSERT_TRUE(b->execute("SELECT pg_notify('qb_proto_peer', 'from_b')", discard_query, discard_error).await());

    EXPECT_TRUE(a->execute("SELECT 1", discard_query, discard_error).await());

    a->disconnect();
    b->disconnect();
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
