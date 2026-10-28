/** @file unit/metrics/metrics.cpp
 * @brief Pure snapshot-to-Prometheus contracts; no database or event loop.
 */
#include <gtest/gtest.h>
#include <qbm/pgsql/metrics.h>

namespace pgsql_metrics_test {
TEST(PgsqlMetrics, CountersRemainExactAndDurationsUseSeconds) {
    qb::pg::stats           stats;
    constexpr std::uint64_t first = 9007199254740993ULL;
    stats.queries                 = first;
    stats.errors                  = first + 1;
    stats.bytes_sent              = first + 2;
    stats.bytes_received          = first + 3;
    stats.notifications_received  = first + 4;
    stats.notifications_dropped   = first + 5;
    stats.prepared_hits           = first + 6;
    stats.prepared_misses         = first + 7;
    stats.prepared_evicted        = first + 8;
    stats.prepared_replans        = first + 9;
    stats.reconnects              = first + 10;
    stats.gaps                    = first + 11;
    stats.last_query_duration     = std::chrono::milliseconds(125);
    qb::metrics::snapshot snapshot;
    qb::metrics::describe(snapshot, stats, {{"connection", "catalog\"\\\n"}, {"core", "1"}});
    const char *names[] = {
        "queries",
        "errors",
        "bytes_sent",
        "bytes_received",
        "notifications_received",
        "notifications_dropped",
        "prepared_hits",
        "prepared_misses",
        "prepared_evicted",
        "prepared_replans",
        "reconnects",
        "gaps"
    };
    for (std::size_t i = 0; i < std::size(names); ++i) {
        const auto name = "qb_pgsql_" + std::string(names[i]) + "_total";
        SCOPED_TRACE(name);
        const auto *family = snapshot.find(name);
        ASSERT_NE(family, nullptr);
        EXPECT_EQ(family->type, qb::metrics::metric_type::counter);
        ASSERT_EQ(family->samples.size(), 1u);
        EXPECT_TRUE(family->samples[0].number.is_integer());
        EXPECT_EQ(family->samples[0].number.integer(), first + i);
        ASSERT_EQ(family->samples[0].labels.size(), 2u);
        EXPECT_EQ(family->samples[0].labels[0].value, "catalog\"\\\n");
    }
    const auto *duration = snapshot.find("qb_pgsql_last_query_duration_seconds");
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(duration->type, qb::metrics::metric_type::gauge);
    EXPECT_DOUBLE_EQ(duration->samples[0].number.as_double(), 0.125);
    EXPECT_EQ(snapshot.families().size(), 13u);
    const auto text = qb::metrics::write_text(snapshot);
    EXPECT_NE(text.find("qb_pgsql_queries_total{connection=\"catalog\\\"\\\\\\n\",core=\"1\"} 9007199254740993\n"), std::string::npos);
}
} // namespace pgsql_metrics_test
