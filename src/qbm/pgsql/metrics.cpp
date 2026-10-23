/**
 * @file metrics.cpp
 * @brief Explicit, snapshot-only pgsql metrics adaptation.
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */
#include "metrics.h"

#include <utility>

namespace qb::metrics {

void
describe(snapshot &snap, qb::pg::stats const &stats, label_set labels) {
    snap.counter("qb_pgsql_queries_total", "Completed PostgreSQL queue round trips, successful or not.").add(labels, stats.queries);
    snap.counter("qb_pgsql_errors_total", "Completed round trips that failed, a subset of queries.").add(labels, stats.errors);
    snap.counter("qb_pgsql_bytes_sent_total", "PostgreSQL protocol bytes queued for the server, TLS framing excluded.")
        .add(labels, stats.bytes_sent);
    snap.counter("qb_pgsql_bytes_received_total", "PostgreSQL protocol bytes received, TLS framing excluded.")
        .add(labels, stats.bytes_received);
    snap.counter("qb_pgsql_notifications_received_total", "PostgreSQL notifications received.").add(labels, stats.notifications_received);
    snap.counter("qb_pgsql_notifications_dropped_total", "Notifications dropped by a full bounded consumer queue.")
        .add(labels, stats.notifications_dropped);
    snap.counter("qb_pgsql_prepared_hits_total", "Named executions found in the prepared statement cache.").add(labels, stats.prepared_hits);
    snap.counter("qb_pgsql_prepared_misses_total", "Named executions refused because the statement was not cached.")
        .add(labels, stats.prepared_misses);
    snap.counter("qb_pgsql_prepared_evicted_total", "Prepared statements evicted and closed on the server.")
        .add(labels, stats.prepared_evicted);
    snap.counter("qb_pgsql_reconnects_total", "Ready connections reached after a lost connection.").add(labels, stats.reconnects);
    snap.counter("qb_pgsql_gaps_total", "Notification stream gap markers emitted.").add(labels, stats.gaps);
    snap.gauge("qb_pgsql_last_query_duration_seconds", "Send-to-completion time of the last sent query; zero before any measurement.")
        .add(std::move(labels), std::chrono::duration<double>(stats.last_query_duration).count());
}

} // namespace qb::metrics
