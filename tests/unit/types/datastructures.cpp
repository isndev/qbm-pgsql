/**
 * @file datastructures.cpp
 * @brief Unit tests for the daemon-free pgsql data structures that the legacy
 *        "data-types" monolith mis-filed: resultset move/dtor semantics,
 *        row_data::null_map (vector<bool> bitmap), the ParamSerializer batch buffer,
 *        connection_options keepalive defaults, and the Reply<T>/Reply<void> monadic
 *        combinators. None of these are TypeConverter codecs.
 *
 * Pure logic, no daemon, no event loop. Split out of the legacy monolith
 * `test-data-types.cpp`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;

// ----------------------------------------------------------------------------
// Populated-resultset fixture (daemon-free): hand-build a detail::result_impl
// with text-format columns, then wrap it in a *borrowing* resultset
// (resultset(result_impl_ptr)) so every row/field/iterator access path in
// src/resultset.cpp runs without a live connection. The protocol layer strips
// the per-field length prefix, so each field's bytes ARE the text value.
// ----------------------------------------------------------------------------
namespace {

// Build a result_impl owning `ncols` text columns named col0..colN and the
// supplied rows. A field whose string is the sentinel "\0NULL" is encoded as
// SQL NULL via the null_map; otherwise its raw bytes are the value.
struct PopulatedResult {
    detail::result_impl impl;

    explicit PopulatedResult(std::vector<std::string>                     names,
                             std::vector<std::vector<std::string>>        rows,
                             std::vector<bool>                            nulls_flat = {}) {
        auto &desc = impl.row_description();
        for (std::size_t i = 0; i < names.size(); ++i) {
            field_description fd{};
            fd.name             = names[i];
            fd.table_oid        = 0;
            fd.attribute_number = static_cast<smallint>(i);
            fd.type_oid         = static_cast<oid>(25); // TEXT
            fd.type_size        = -1;
            fd.type_mod         = -1;
            fd.format_code      = protocol_data_format::Text;
            desc.push_back(fd);
        }
        std::size_t flat = 0;
        for (auto const &cells : rows) {
            row_data rd;
            integer  off = 0;
            for (std::size_t c = 0; c < cells.size(); ++c) {
                rd.offsets.push_back(off);
                bool is_null = !nulls_flat.empty() && flat < nulls_flat.size() && nulls_flat[flat];
                rd.null_map.push_back(is_null);
                if (!is_null) {
                    for (char ch : cells[c])
                        rd.data.push_back(static_cast<byte>(ch));
                    off += static_cast<integer>(cells[c].size());
                }
                ++flat;
            }
            impl.rows().push_back(std::move(rd));
        }
    }

    // Borrowing wrapper — the resultset observes impl without owning it.
    resultset rs() {
        return resultset(&impl);
    }
};

} // namespace

// 2x2 text result: iterate rows + fields, by-index and by-name access.
TEST(ResultsetPopulated, RowAndFieldAccessByIndexAndName) {
    PopulatedResult pr({"id", "name"}, {{"1", "alice"}, {"2", "bob"}});
    resultset       rs = pr.rs();

    EXPECT_FALSE(rs.empty());
    EXPECT_EQ(rs.size(), 2u);
    EXPECT_EQ(rs.columns_size(), 2u);

    // operator[] by index, field by index and by name.
    EXPECT_EQ(rs[0][0].as<std::string>(), "1");
    EXPECT_EQ(rs[0][1].as<std::string>(), "alice");
    EXPECT_EQ(rs[1]["id"].as<std::string>(), "2");
    EXPECT_EQ(rs[1]["name"].as<std::string>(), "bob");

    // front()/back().
    EXPECT_EQ(rs.front()[1].as<std::string>(), "alice");
    EXPECT_EQ(rs.back()[1].as<std::string>(), "bob");

    // row::empty() is always false for a row in a valid result set.
    EXPECT_FALSE(rs[0].empty());
    EXPECT_EQ(rs[0].size(), 2u);

    // field metadata.
    EXPECT_EQ(rs[0][0].name(), "id");
    EXPECT_EQ(rs[0][1].name(), "name");
    EXPECT_FALSE(rs[0][0].is_null());

    // index_of_name found + npos for missing.
    EXPECT_EQ(rs.index_of_name("name"), 1u);
    EXPECT_EQ(rs.index_of_name("missing"), resultset::npos);
}

// at() range-checking: valid index returns the row, out-of-range throws.
TEST(ResultsetPopulated, AtThrowsOnOutOfRange) {
    PopulatedResult pr({"id"}, {{"7"}});
    resultset       rs = pr.rs();

    EXPECT_EQ(rs.at(0)[0].as<std::string>(), "7");
    EXPECT_THROW(rs.at(1), std::out_of_range);
    EXPECT_THROW(rs.at(99), std::out_of_range);
}

// row::operator[] bounds check (the P0-12 fix) throws on a too-large column.
TEST(ResultsetPopulated, RowColumnOutOfRangeThrows) {
    PopulatedResult pr({"a", "b"}, {{"x", "y"}});
    resultset       rs = pr.rs();

    EXPECT_NO_THROW((void) rs[0][1]);
    EXPECT_THROW(rs[0][2], std::out_of_range);
    EXPECT_THROW(rs[0][100], std::out_of_range);
}

// NULL handling: is_null true/false, and as<optional> yields nullopt for NULL.
TEST(ResultsetPopulated, NullFieldSemantics) {
    // Row 0: ("v", NULL). nulls_flat is per-cell, row-major.
    PopulatedResult pr({"a", "b"}, {{"v", ""}}, {false, true});
    resultset       rs = pr.rs();

    EXPECT_FALSE(rs[0][0].is_null());
    EXPECT_TRUE(rs[0][1].is_null());

    EXPECT_EQ(rs[0][0].as<std::optional<std::string>>().value(), "v");
    EXPECT_FALSE(rs[0][1].as<std::optional<std::string>>().has_value());

    // as<T>() on a NULL non-nullable target throws value_is_null.
    EXPECT_THROW(rs[0][1].as<std::string>(), qb::pg::error::value_is_null);

    // field::to(optional&) writes nullopt for a NULL field, value otherwise.
    std::optional<std::string> o_null, o_val;
    EXPECT_TRUE(rs[0][1].to(o_null));
    EXPECT_FALSE(o_null.has_value());
    EXPECT_TRUE(rs[0][0].to(o_val));
    ASSERT_TRUE(o_val.has_value());
    EXPECT_EQ(*o_val, "v");
}

// field::view()/text() zero-copy accessors: bytes for a value, empty for NULL.
TEST(ResultsetPopulated, FieldViewAndText) {
    PopulatedResult pr({"a", "b"}, {{"hello", ""}}, {false, true});
    resultset       rs = pr.rs();

    EXPECT_EQ(rs[0][0].text(), "hello");
    EXPECT_EQ(rs[0][0].view().size(), 5u);

    // NULL field -> empty view/text (sz == 0 branch).
    EXPECT_TRUE(rs[0][1].text().empty());
    EXPECT_TRUE(rs[0][1].view().empty());
}

// row::to(targets...) multi-target write, and row::as<std::tuple<...>>.
TEST(ResultsetPopulated, RowToMultiTargetAndAsTuple) {
    PopulatedResult pr({"id", "name"}, {{"42", "zoe"}});
    resultset       rs = pr.rs();

    // Variadic to(T&...).
    std::string id, name;
    rs[0].to(id, name);
    EXPECT_EQ(id, "42");
    EXPECT_EQ(name, "zoe");

    // as<std::tuple<...>> -> structured bindings.
    auto [tid, tname] = rs[0].as<std::tuple<std::string, std::string>>();
    EXPECT_EQ(tid, "42");
    EXPECT_EQ(tname, "zoe");

    // to(initializer_list<names>, targets...) by-name multi-target.
    std::string only_name;
    rs[0].to({"name"}, only_name);
    EXPECT_EQ(only_name, "zoe");
}

// Forward row iteration drives const_row_iterator::advance/compare/operator*.
TEST(ResultsetPopulated, RowIterationForward) {
    PopulatedResult pr({"id"}, {{"a"}, {"b"}, {"c"}});
    resultset       rs = pr.rs();

    std::string fwd;
    for (auto const &row : rs)
        fwd += row[0].as<std::string>();
    EXPECT_EQ(fwd, "abc");

    // begin()+distance / iterator comparison + post/pre-increment.
    auto b = rs.begin();
    auto e = rs.end();
    EXPECT_EQ(std::distance(b, e), 3);
    EXPECT_TRUE(b != e);
    auto b2 = rs.begin();
    EXPECT_TRUE(b == b2);
    auto b3 = b++;
    EXPECT_TRUE(b3 == b2);
    EXPECT_TRUE(b != b2);
}

// Forward field iteration drives const_field_iterator::advance/compare/operator*.
TEST(ResultsetPopulated, FieldIterationForward) {
    PopulatedResult pr({"a", "b", "c"}, {{"1", "2", "3"}});
    resultset       rs  = pr.rs();
    resultset::row  row = rs[0];

    std::string fwd;
    for (auto const &f : row)
        fwd += f.as<std::string>();
    EXPECT_EQ(fwd, "123");

    EXPECT_EQ(std::distance(row.begin(), row.end()), 3);
    EXPECT_TRUE(row.begin() != row.end());
}

// resultset::field(name) linear search: found returns the description, missing throws.
TEST(ResultsetPopulated, FieldDescriptionByNameAndMissingThrows) {
    PopulatedResult pr({"alpha", "beta"}, {{"1", "2"}});
    resultset       rs = pr.rs();

    EXPECT_EQ(rs.field("beta").name, "beta");
    EXPECT_EQ(rs.field(0).name, "alpha");
    EXPECT_EQ(rs.field_name(1), "beta");
    EXPECT_THROW(rs.field("nope"), std::runtime_error);
}

// rows_affected + command tag parsing, and deep_snapshot of a populated set.
TEST(ResultsetPopulated, RowsAffectedAndDeepSnapshot) {
    PopulatedResult pr({"id"}, {{"1"}, {"2"}});
    pr.impl.set_command_tag("SELECT 2");
    resultset rs = pr.rs();
    EXPECT_EQ(rs.rows_affected(), 2);

    // deep_snapshot owns a fresh copy that survives independently.
    resultset snap = rs.deep_snapshot();
    EXPECT_EQ(snap.size(), 2u);
    EXPECT_EQ(snap[0][0].as<std::string>(), "1");
    EXPECT_EQ(snap[1][0].as<std::string>(), "2");
    EXPECT_EQ(snap.rows_affected(), 2);
}

// Lightweight accessor / iterator-default surface that the higher-level tests above
// never touch directly: resultset operator bool / operator!, row::row_index,
// row::index_of_name, field::row_index / field::field_index, operator-(row,row), and
// the default-constructed const_row_iterator / const_field_iterator valid() == false.
TEST(ResultsetPopulated, AccessorAndIteratorDefaultSurface) {
    PopulatedResult pr({"id", "name"}, {{"1", "a"}, {"2", "b"}, {"3", "c"}});
    resultset       rs = pr.rs();

    // operator bool / operator! on a non-empty set.
    EXPECT_TRUE(static_cast<bool>(rs));
    EXPECT_FALSE(!rs);

    // row::row_index reflects each row's ordinal; operator-(row,row) is the index delta.
    resultset::row r0 = rs[0];
    resultset::row r2 = rs[2];
    EXPECT_EQ(r0.row_index(), 0u);
    EXPECT_EQ(r2.row_index(), 2u);
    EXPECT_EQ(r2 - r0, 2);

    // row::index_of_name delegates to the resultset (found + npos).
    EXPECT_EQ(r0.index_of_name("name"), 1u);
    EXPECT_EQ(r0.index_of_name("nope"), resultset::npos);

    // field::row_index / field::field_index carry the field's coordinates.
    resultset::row::value_type f = r2[1]; // row 2 == {"3","c"}, column 1 == "c"
    EXPECT_EQ(f.row_index(), 2u);
    EXPECT_EQ(f.field_index(), 1u);
    EXPECT_EQ(f.as<std::string>(), "c");

    // Default-constructed iterators are invalid (no backing resultset).
    resultset::const_row_iterator   dead_row{};
    resultset::const_field_iterator dead_field{};
    EXPECT_FALSE(dead_row.valid());
    EXPECT_FALSE(dead_field.valid());
}

// resultset::json() materializes rows as an array of {name: value-or-null}.
TEST(ResultsetPopulated, JsonSerialization) {
    PopulatedResult pr({"id", "name"}, {{"1", ""}}, {false, true});
    resultset       rs = pr.rs();

    qb::json j = rs.json();
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["id"], "1");
    EXPECT_TRUE(j[0]["name"].is_null());
}

// ----------------------------------------------------------------------------
// resultset move semantics / dtor (regression for the P0-17 memory leak)
// ----------------------------------------------------------------------------

TEST(ResultsetDataStructure, MoveSemanticsAndDtor) {
    // Create+destroy many resultsets — ASan/valgrind catch a leak here.
    {
        std::vector<qb::pg::resultset> resultsets;
        resultsets.reserve(100);
        for (int i = 0; i < 100; ++i) {
            qb::pg::resultset rs;
            resultsets.push_back(std::move(rs));
        }
    }

    // Move-construct: the moved-to object is usable, the moved-from does not crash.
    {
        qb::pg::resultset original;
        qb::pg::resultset moved(std::move(original));
        EXPECT_NO_THROW({ (void) moved.empty(); });
    }

    // Move-assign: ownership transfers cleanly.
    {
        qb::pg::resultset rs1;
        qb::pg::resultset rs2;
        rs2 = std::move(rs1);
        EXPECT_NO_THROW({ (void) rs2.empty(); });
    }
}

// An empty resultset reports zero columns and is empty (honest name — the legacy
// "OutOfRangeThrows" never triggered the throw it claimed, so it is dropped in favor
// of the invariants that are actually observable without a live connection).
TEST(ResultsetDataStructure, EmptyInvariants) {
    qb::pg::resultset rs;
    EXPECT_EQ(rs.columns_size(), 0u);
    EXPECT_TRUE(rs.empty());
    EXPECT_EQ(rs.size(), 0u);
}

// ----------------------------------------------------------------------------
// row_data::null_map (vector<bool> bitmap — P0-4)
// ----------------------------------------------------------------------------

TEST(NullBitmapDataStructure, VectorBoolBitmap) {
    row_data row;
    row.null_map.resize(5, false); // 5 columns, all non-NULL initially
    row.null_map[1] = true;        // column 1 is NULL
    row.null_map[3] = true;        // column 3 is NULL

    EXPECT_FALSE(row.null_map[0]);
    EXPECT_TRUE(row.null_map[1]);
    EXPECT_FALSE(row.null_map[2]);
    EXPECT_TRUE(row.null_map[3]);
    EXPECT_FALSE(row.null_map[4]);
    EXPECT_EQ(row.null_map.size(), 5u);
}

// ----------------------------------------------------------------------------
// ParamSerializer batch buffer (P0-3) — assert the encoded bytes, not just a count
// ----------------------------------------------------------------------------

TEST(ParamSerializerBatch, StringVectorEncodesExactBytes) {
    ParamSerializer serializer;

    std::vector<std::string> values;
    for (int i = 0; i < 100; ++i)
        values.push_back("test_value_" + std::to_string(i));

    serializer.add_string_vector(values);

    // One param per string.
    EXPECT_EQ(serializer.param_count(), 100);

    // add_string_vector emits, per string, [int32 big-endian length][raw bytes] with
    // no leading count. Compute the exact total and verify it.
    std::size_t expected_bytes = 0;
    for (const auto &v : values)
        expected_bytes += sizeof(integer) + v.size();
    const auto &buffer = serializer.params_buffer();
    EXPECT_EQ(buffer.size(), expected_bytes);

    // reserve() (P0-3) means the capacity is at least the final size — no shrink.
    EXPECT_GE(buffer.capacity(), buffer.size());

    // Decode the first and last param straight out of the buffer to prove the layout.
    auto read_len = [&](std::size_t off) {
        integer be;
        std::memcpy(&be, buffer.data() + off, sizeof(integer));
        return static_cast<integer>(ntohl(be));
    };
    // First param.
    integer first_len = read_len(0);
    ASSERT_EQ(first_len, static_cast<integer>(values.front().size()));
    EXPECT_EQ(std::string(buffer.data() + sizeof(integer), buffer.data() + sizeof(integer) + first_len), values.front());

    // Last param (walk to its offset).
    std::size_t off = 0;
    for (std::size_t i = 0; i + 1 < values.size(); ++i)
        off += sizeof(integer) + values[i].size();
    integer last_len = read_len(off);
    ASSERT_EQ(last_len, static_cast<integer>(values.back().size()));
    EXPECT_EQ(std::string(buffer.data() + off + sizeof(integer), buffer.data() + off + sizeof(integer) + last_len), values.back());
}

// ----------------------------------------------------------------------------
// std::chrono::seconds INTERVAL conversion — assert the decoded VALUE, not size()
// ----------------------------------------------------------------------------

TEST(IntervalConversionDataStructure, ChronoDurationValueRoundTrip) {
    using namespace std::chrono;
    const auto duration = seconds(3600); // 1 hour

    std::vector<byte> buffer;
    TypeConverter<seconds>::to_binary(duration, buffer);
    // to_binary emits [int32 length == 16][int64 micros][int32 days][int32 months].
    ASSERT_EQ(buffer.size(), sizeof(integer) + 16u);
    integer len;
    std::memcpy(&len, buffer.data(), sizeof(integer));
    EXPECT_EQ(ntohl(len), 16);

    // OID is INTERVAL (1186).
    EXPECT_EQ(TypeConverter<seconds>::get_oid(), 1186);

    // The value round-trips through from_binary (strip the 4-byte length prefix).
    std::vector<byte> body(buffer.begin() + sizeof(integer), buffer.end());
    EXPECT_EQ(TypeConverter<seconds>::from_binary(body).count(), 3600);

    // Text spelling.
    EXPECT_EQ(TypeConverter<seconds>::to_text(duration), "3600 seconds");
}

// ----------------------------------------------------------------------------
// connection_options keepalive defaults (P1-1)
// ----------------------------------------------------------------------------

TEST(ConnectionOptionsDataStructure, KeepaliveDefaultsAndMutation) {
    qb::pg::connection_options opts;
    EXPECT_EQ(opts.keepalive_interval, 0); // disabled by default
    EXPECT_EQ(opts.keepalive_probes, 3);
    EXPECT_EQ(opts.keepalive_idle, 60);

    opts.keepalive_interval = 30;
    opts.keepalive_probes   = 5;
    opts.keepalive_idle     = 120;
    EXPECT_EQ(opts.keepalive_interval, 30);
    EXPECT_EQ(opts.keepalive_probes, 5);
    EXPECT_EQ(opts.keepalive_idle, 120);
}

// ----------------------------------------------------------------------------
// Reply<T> / Reply<void> std::expected-style monadic combinators
// ----------------------------------------------------------------------------

TEST(ReplyMonadicDataStructure, TransformAndThenOrElseValueOr) {
    using qb::pg::Reply;
    using qb::pg::error::db_error;

    const auto ok = Reply<int>::success(21);
    EXPECT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 21);
    EXPECT_EQ(ok.value_or(99), 21);
    EXPECT_EQ(ok.transform([](int x) { return x * 2; }).result(), 42);               // int -> int
    EXPECT_EQ(ok.transform([](int x) { return std::to_string(x); }).result(), "21"); // int -> string
    auto chained = ok.and_then([](int x) { return Reply<std::string>::success("v" + std::to_string(x)); });
    EXPECT_TRUE(chained.ok());
    EXPECT_EQ(chained.result(), "v21");

    const auto bad = Reply<int>::failure(db_error{"boom"});
    EXPECT_FALSE(bad.has_value());
    EXPECT_EQ(bad.value_or(7), 7);
    EXPECT_FALSE(bad.transform([](int x) { return x * 2; }).ok()); // f not called, error propagates
    EXPECT_FALSE(bad.and_then([](int) { return Reply<int>::success(1); }).ok());
    auto recovered = bad.or_else([](db_error const &) { return Reply<int>::success(123); });
    EXPECT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.result(), 123);

    // Reply<void>
    EXPECT_TRUE(Reply<void>::success().and_then([] { return Reply<int>::success(5); }).ok());
    EXPECT_FALSE(Reply<void>::failure(db_error{"x"}).and_then([] { return Reply<int>::success(5); }).ok());
    EXPECT_TRUE(Reply<void>::failure(db_error{"x"}).or_else([](db_error const &) { return Reply<void>::success(); }).ok());
}
