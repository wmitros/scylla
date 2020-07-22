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

#pragma once

#include "query-request.hh"
#include "query-result.hh"
#include "mutation_reader.hh"
#include "frozen_mutation.hh"
#include "db/timeout_clock.hh"
#include "querier.hh"
#include "utils/chunked_vector.hh"
#include "query_class_config.hh"
#include <seastar/core/execution_stage.hh>

class reconcilable_result;
class frozen_reconcilable_result;

// Can be read by other cores after publishing.
struct partition {
    uint32_t _row_count_low_bits;
    frozen_mutation _m; // FIXME: We don't need cf UUID, which frozen_mutation includes.
    std::optional<uint32_t> _row_count_high_bits;
    partition(uint32_t row_count_low_bits, frozen_mutation m, std::optional<uint32_t> row_count_high_bits)
        : _row_count_low_bits(row_count_low_bits)
        , _m(std::move(m))
        , _row_count_high_bits(row_count_high_bits)
    { }

    partition(uint64_t row_count, frozen_mutation m)
        : _row_count_low_bits(static_cast<uint32_t>(row_count))
        , _m(std::move(m))
        , _row_count_high_bits(static_cast<uint32_t>(row_count >> 32))
    { }

    uint32_t row_count_low_bits() const {
        return _row_count_low_bits;
    }

    std::optional<uint32_t> row_count_high_bits() const {
        return _row_count_high_bits;
    }
    
    uint64_t row_count() const {
        return (static_cast<uint64_t>(_row_count_high_bits.value_or(0)) << 32) | _row_count_low_bits;
    }

    const frozen_mutation& mut() const {
        return _m;
    }

    frozen_mutation& mut() {
        return _m;
    }


    bool operator==(const partition& other) const {
        return row_count() == other.row_count() && _m.representation() == other._m.representation();
    }

    bool operator!=(const partition& other) const {
        return !(*this == other);
    }
};

// The partitions held by this object are ordered according to dht::decorated_key ordering and non-overlapping.
// Each mutation must have different key.
//
// Can be read by other cores after publishing.
class reconcilable_result {
    uint32_t _row_count_low_bits;
    query::short_read _short_read;
    query::result_memory_tracker _memory_tracker;
    utils::chunked_vector<partition> _partitions;
    std::optional<uint32_t> _row_count_high_bits;
public:
    ~reconcilable_result();
    reconcilable_result();
    reconcilable_result(reconcilable_result&&) = default;
    reconcilable_result& operator=(reconcilable_result&&) = default;
    reconcilable_result(uint32_t row_count, utils::chunked_vector<partition> partitions, query::short_read short_read,
                        std::optional<uint32_t> row_count_high_bits, query::result_memory_tracker memory_tracker = { });
    reconcilable_result(uint64_t row_count, utils::chunked_vector<partition> partitions, query::short_read short_read,
                        query::result_memory_tracker memory_tracker = { });

    const utils::chunked_vector<partition>& partitions() const;
    utils::chunked_vector<partition>& partitions();

    uint32_t row_count_low_bits() const {
        return _row_count_low_bits;
    }

    std::optional<uint32_t> row_count_high_bits() const {
        return _row_count_high_bits;
    }

    uint64_t row_count() const {
        return (static_cast<uint64_t>(_row_count_high_bits.value_or(0)) << 32) | _row_count_low_bits;
    }

    query::short_read is_short_read() const {
        return _short_read;
    }

    size_t memory_usage() const {
        return _memory_tracker.used_memory();
    }

    bool operator==(const reconcilable_result& other) const;
    bool operator!=(const reconcilable_result& other) const;

    struct printer {
        const reconcilable_result& self;
        schema_ptr schema;
        friend std::ostream& operator<<(std::ostream&, const printer&);
    };

    printer pretty_printer(schema_ptr) const;
};

class reconcilable_result_builder {
    const schema& _schema;
    const query::partition_slice& _slice;

    utils::chunked_vector<partition> _result;
    uint64_t _live_rows{};

    bool _return_static_content_on_partition_with_no_rows{};
    bool _static_row_is_alive{};
    uint64_t _total_live_rows = 0;
    query::result_memory_accounter _memory_accounter;
    stop_iteration _stop;
    bool _short_read_allowed;
    std::optional<streamed_mutation_freezer> _mutation_consumer;
public:
    reconcilable_result_builder(const schema& s, const query::partition_slice& slice,
                                query::result_memory_accounter&& accounter)
        : _schema(s), _slice(slice)
        , _memory_accounter(std::move(accounter))
        , _short_read_allowed(slice.options.contains<query::partition_slice::option::allow_short_read>())
    { }

    void consume_new_partition(const dht::decorated_key& dk);
    void consume(tombstone t);
    stop_iteration consume(static_row&& sr, tombstone, bool is_alive);
    stop_iteration consume(clustering_row&& cr, row_tombstone, bool is_alive);
    stop_iteration consume(range_tombstone&& rt);
    stop_iteration consume_end_of_partition();
    reconcilable_result consume_end_of_stream();
};

query::result to_data_query_result(const reconcilable_result&, schema_ptr, const query::partition_slice&, uint64_t row_limit, uint32_t partition_limit, query::result_options opts = query::result_options::only_result());

// Performs a query on given data source returning data in reconcilable form.
//
// Reads at most row_limit rows. If less rows are returned, the data source
// didn't have more live data satisfying the query.
//
// Any cells which have expired according to query_time are returned as
// deleted cells and do not count towards live data. The mutations are
// compact, meaning that any cell which is covered by higher-level tombstone
// is absent in the results.
//
// 'source' doesn't have to survive deferring.
future<reconcilable_result> mutation_query(
    schema_ptr,
    mutation_source source,
    const dht::partition_range& range,
    const query::partition_slice& slice,
    uint64_t row_limit,
    uint32_t partition_limit,
    gc_clock::time_point query_time,
    db::timeout_clock::time_point timeout,
    query_class_config class_config,
    query::result_memory_accounter&& accounter = { },
    tracing::trace_state_ptr trace_ptr = nullptr,
    query::querier_cache_context cache_ctx = { });

future<> data_query(
    schema_ptr s,
    const mutation_source& source,
    const dht::partition_range& range,
    const query::partition_slice& slice,
    uint64_t row_limit,
    uint32_t partition_limit,
    gc_clock::time_point query_time,
    query::result::builder& builder,
    db::timeout_clock::time_point timeout,
    query_class_config class_config,
    tracing::trace_state_ptr trace_ptr = nullptr,
    query::querier_cache_context cache_ctx = { });


class mutation_query_stage {
    inheriting_concrete_execution_stage<future<reconcilable_result>,
        schema_ptr,
        mutation_source,
        const dht::partition_range&,
        const query::partition_slice&,
        uint32_t,
        uint32_t,
        gc_clock::time_point,
        db::timeout_clock::time_point,
        query_class_config,
        query::result_memory_accounter&&,
        tracing::trace_state_ptr,
        query::querier_cache_context> _execution_stage;
public:
    explicit mutation_query_stage();
    template <typename... Args>
    future<reconcilable_result> operator()(Args&&... args) { return _execution_stage(std::forward<Args>(args)...); }
};

// Performs a query for counter updates.
future<mutation_opt> counter_write_query(schema_ptr, const mutation_source&, reader_permit permit,
                                         const dht::decorated_key& dk,
                                         const query::partition_slice& slice,
                                         tracing::trace_state_ptr trace_ptr,
                                         db::timeout_clock::time_point timeout);

