/**
 * @file metrics.h
 * @brief Explicit, snapshot-only pgsql metrics adaptation.
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */
#pragma once

#include <qb/core/metrics.h>
#include "stats.h"

namespace qb::metrics {

/**
 * @brief Adds the counters and gauges of one pgsql statistics snapshot.
 * @details No hooks, clocks or client access. Use stable connection/core labels to distinguish sources;
 *          canonicalize() sums samples with identical labels, including last-duration gauges.
 *          Do not use SQL, keys, credentials or user-supplied paths as labels.
 */
void describe(snapshot &snap, qb::pg::stats const &stats, label_set labels);

} // namespace qb::metrics
