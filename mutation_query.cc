/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mutation_query.hh"
#include "gc_clock.hh"
#include "mutation_partition_serializer.hh"
#include "service/priority_manager.hh"
#include "query-result-writer.hh"

reconcilable_result::~reconcilable_result() {}

reconcilable_result::reconcilable_result()
    : _row_count_low_bits(0)
    , _row_count_high_bits(std::nullopt)
{ }

reconcilable_result::reconcilable_result(uint32_t row_count_low_bits, utils::chunked_vector<partition> p, query::short_read short_read,
                                         std::optional<uint32_t> row_count_high_bits, query::result_memory_tracker memory_tracker)
    : _row_count_low_bits(row_count_low_bits)
    , _short_read(short_read)
    , _memory_tracker(std::move(memory_tracker))
    , _partitions(std::move(p))
    , _row_count_high_bits(row_count_high_bits)
{ }

reconcilable_result::reconcilable_result(uint64_t row_count, utils::chunked_vector<partition> p, query::short_read short_read,
                                         query::result_memory_tracker memory_tracker)
    : reconcilable_result(static_cast<uint32_t>(row_count), std::move(p), short_read, static_cast<uint32_t>(row_count >> 32), std::move(memory_tracker))
{ }

const utils::chunked_vector<partition>& reconcilable_result::partitions() const {
    return _partitions;
}

utils::chunked_vector<partition>& reconcilable_result::partitions() {
    return _partitions;
}

bool
reconcilable_result::operator==(const reconcilable_result& other) const {
    return boost::equal(_partitions, other._partitions);
}

bool reconcilable_result::operator!=(const reconcilable_result& other) const {
    return !(*this == other);
}

query::result
to_data_query_result(const reconcilable_result& r, schema_ptr s, const query::partition_slice& slice, uint64_t max_rows, uint32_t max_partitions, query::result_options opts) {
    // This result was already built with a limit, don't apply another one.
    query::result::builder builder(slice, opts, query::result_memory_accounter{ query::result_memory_limiter::unlimited_result_size });
    for (const partition& p : r.partitions()) {
        if (builder.row_count() >= max_rows || builder.partition_count() >= max_partitions) {
            break;
        }
        // Also enforces the per-partition limit.
        p.mut().unfreeze(s).query(builder, slice, gc_clock::time_point::min(), max_rows - builder.row_count());
    }
    if (r.is_short_read()) {
        builder.mark_as_short_read();
    }
    return builder.build();
}

std::ostream& operator<<(std::ostream& out, const reconcilable_result::printer& pr) {
    out << "{rows=" << pr.self.row_count() << ", short_read="
        << pr.self.is_short_read() << ", [";
    bool first = true;
    for (const partition& p : pr.self.partitions()) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << "{rows=" << p.row_count() << ", ";
        out << p._m.pretty_printer(pr.schema);
        out << "}";
    }
    out << "]}";
    return out;
}

reconcilable_result::printer reconcilable_result::pretty_printer(schema_ptr s) const {
    return { *this, std::move(s) };
}
