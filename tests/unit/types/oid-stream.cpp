/**
 * @file qbm/pgsql/tests/unit/types/oid-stream.cpp
 * @brief Unit tests for the qb::pg::oid stream operators (pg_types.cpp).
 *
 * `operator<<(ostream, oid)` prints the catalog name for a known OID and the
 * fallback `oid_<numeric>` for an unmapped value; `operator>>(istream, oid&)` parses
 * a catalog name back to the enum and sets failbit for an unknown name. Pure logic —
 * no daemon, no event loop, parallel-safe.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;

namespace {

std::string
to_str(oid v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

} // namespace

// operator<< prints the catalog name for mapped OIDs.
TEST(OidStream, WritesKnownNames) {
    EXPECT_EQ(to_str(oid::boolean), "boolean");
    EXPECT_EQ(to_str(oid::int2), "int2");
    EXPECT_EQ(to_str(oid::int4), "int4");
    EXPECT_EQ(to_str(oid::int8), "int8");
    EXPECT_EQ(to_str(oid::float4), "float4");
    EXPECT_EQ(to_str(oid::float8), "float8");
    EXPECT_EQ(to_str(oid::text), "text");
    EXPECT_EQ(to_str(oid::bytea), "bytea");
    EXPECT_EQ(to_str(oid::numeric), "numeric");
    EXPECT_EQ(to_str(oid::uuid), "uuid");
    EXPECT_EQ(to_str(oid::json), "json");
    EXPECT_EQ(to_str(oid::jsonb), "jsonb");
    EXPECT_EQ(to_str(oid::timestamp), "timestamp");
    EXPECT_EQ(to_str(oid::timestamptz), "timestamptz");
    EXPECT_EQ(to_str(oid::date), "date");
    EXPECT_EQ(to_str(oid::interval), "interval");
}

// An OID with no catalog-name mapping falls back to the numeric form `oid_<N>`.
TEST(OidStream, WritesNumericFallbackForUnknown) {
    const auto unknown = static_cast<oid>(987654);
    EXPECT_EQ(to_str(unknown), "oid_987654");
    // A real-but-unmapped low OID also takes the fallback (the map is intentionally partial).
    const auto unmapped = static_cast<oid>(4242);
    EXPECT_EQ(to_str(unmapped), "oid_4242");
}

// operator>> parses a known catalog name back into the enum value.
TEST(OidStream, ParsesKnownNames) {
    for (const auto &name : {"boolean", "int4", "int8", "text", "numeric", "uuid", "jsonb", "timestamptz", "date"}) {
        std::istringstream iss{name};
        oid                v{};
        iss >> v;
        EXPECT_FALSE(iss.fail()) << "parse failed for " << name;
        EXPECT_EQ(to_str(v), name) << "round-trip mismatch for " << name;
    }
}

// An unrecognised name leaves the stream in a failed state (failbit set).
TEST(OidStream, UnknownNameSetsFailbit) {
    std::istringstream iss{"definitely_not_a_pg_type"};
    oid                v = oid::int4; // sentinel — must stay unchanged on failure
    iss >> v;
    EXPECT_TRUE(iss.fail());
    EXPECT_EQ(v, oid::int4);
}

// Round-trip oid -> name -> oid is the identity for every mapped representative.
TEST(OidStream, RoundTripIdentity) {
    const oid samples[] = {oid::boolean, oid::int2,  oid::int4,      oid::int8,        oid::float4,   oid::float8,
                           oid::text,    oid::bytea, oid::numeric,   oid::uuid,        oid::json,     oid::jsonb,
                           oid::date,    oid::time,  oid::timestamp, oid::timestamptz, oid::interval, oid::varchar};
    for (oid o : samples) {
        std::istringstream iss{to_str(o)};
        oid                back{};
        iss >> back;
        EXPECT_FALSE(iss.fail());
        EXPECT_EQ(back, o);
    }
}

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
