/**
 * @file qbm/pgsql/tests/unit/wire/result-format-routing.cpp
 * @brief Unit tests for type_oid_prefers_binary_result_format (common.h).
 *
 * The extended/prepared query path requests the BINARY result format only for OIDs that
 * have a verified TypeConverter::from_binary decoder; everything else degrades to the
 * readable text representation (the inverse of a denylist). This pins that whitelist
 * directly — every binary OID returns true, and representative text-routed OIDs (and any
 * unmapped OID) return false — so a future edit that drops or mis-adds an OID is caught
 * without needing a live server. Pure logic: no daemon, no event loop, parallel-safe.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include <gtest/gtest.h>

#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;

// Every OID with a real binary decoder requests the binary result format.
TEST(ResultFormatRouting, BinaryDecodableOidsPreferBinary) {
    const oid binary_oids[] = {
        oid::boolean,
        oid::int2,
        oid::int4,
        oid::int8,
        oid::float4,
        oid::float8,
        oid::numeric,
        oid::bytea,
        oid::uuid,
        oid::jsonb,
        oid::timestamp,
        oid::timestamptz,
        oid::date,
        oid::time,
        oid::timetz,
        oid::interval,
        // 1-D arrays that have a std::vector<T> decoder.
        oid::boolean_array,
        oid::int2_array,
        oid::int4_array,
        oid::int8_array,
        oid::float4_array,
        oid::float8_array,
        oid::text_array,
    };
    for (oid t : binary_oids)
        EXPECT_TRUE(type_oid_prefers_binary_result_format(t))
            << "OID " << static_cast<int>(t) << " has a binary decoder and must prefer binary";
}

// Text-only / not-yet-binary-decodable types degrade to the text representation.
TEST(ResultFormatRouting, TextOnlyOidsPreferText) {
    const oid text_oids[] = {
        // varlena text (json has no version byte; varchar/text/name/xml are plain text)
        oid::json,
        oid::varchar,
        oid::text,
        oid::name,
        oid::xml,
        oid::char_,
        // network / bit / geometric / money have no binary decoder -> text
        oid::macaddr,
        oid::bit,
        oid::varbit,
        oid::point,
        oid::box,
        oid::circle,
        oid::line,
        oid::cash,
    };
    for (oid t : text_oids)
        EXPECT_FALSE(type_oid_prefers_binary_result_format(t))
            << "OID " << static_cast<int>(t) << " has no binary decoder and must route as text";
}

// An unknown / unmapped OID falls through the switch default to text (no garbage path).
TEST(ResultFormatRouting, UnknownOidDefaultsToText) {
    EXPECT_FALSE(type_oid_prefers_binary_result_format(static_cast<oid>(999999)));
    EXPECT_FALSE(type_oid_prefers_binary_result_format(static_cast<oid>(4242)));
}

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
