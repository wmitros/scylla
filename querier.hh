/*
 * Copyright (C) 2018 ScyllaDB
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

#include "mutation_compactor.hh"
#include "mutation_reader.hh"

#include <boost/intrusive/set.hpp>

#include <variant>

namespace query {

template <typename Consumer>
class clustering_position_tracker {
    Consumer _consumer;
    lw_shared_ptr<std::optional<clustering_key_prefix>> _last_ckey;

public:
    clustering_position_tracker(Consumer&& consumer, lw_shared_ptr<std::optional<clustering_key_prefix>> last_ckey)
        : _consumer(std::forward<Consumer>(consumer))
        , _last_ckey(std::move(last_ckey)) {
    }

    void consume_new_partition(const dht::decorated_key& dk) {
        _last_ckey->reset();
        _consumer.consume_new_partition(dk);
    }
    void consume(tombstone t) {
        _consumer.consume(t);
    }
    stop_iteration consume(static_row&& sr, tombstone t, bool is_live) {
        return _consumer.consume(std::move(sr), std::move(t), is_live);
    }
    stop_iteration consume(clustering_row&& cr, row_tombstone t, bool is_live) {
        *_last_ckey = cr.key();
        return _consumer.consume(std::move(cr), std::move(t), is_live);
    }
    stop_iteration consume(range_tombstone&& rt) {
        return _consumer.consume(std::move(rt));
    }
    stop_iteration consume_end_of_partition() {
        return _consumer.consume_end_of_partition();
    }
    auto consume_end_of_stream() {
        return _consumer.consume_end_of_stream();
    }
};

/// Consume a page worth of data from the reader.
///
/// Uses `compaction_state` for compacting the fragments and `consumer` for
/// building the results.
/// Returns a future containing a tuple with the last consumed clustering key,
/// or std::nullopt if the last row wasn't a clustering row, and whatever the
/// consumer's `consume_end_of_stream()` method returns.
template <emit_only_live_rows OnlyLive, typename Consumer>
requires CompactedFragmentsConsumer<Consumer>
auto consume_page(flat_mutation_reader& reader,
        lw_shared_ptr<compact_for_query_state<OnlyLive>> compaction_state,
        const query::partition_slice& slice,
        Consumer&& consumer,
        uint64_t row_limit,
        uint32_t partition_limit,
        gc_clock::time_point query_time,
        db::timeout_clock::time_point timeout,
        query::max_result_size max_size) {
    return reader.peek(timeout).then([=, &reader, consumer = std::move(consumer), &slice] (
                mutation_fragment* next_fragment) mutable {
        const auto next_fragment_kind = next_fragment ? next_fragment->mutation_fragment_kind() : mutation_fragment::kind::partition_end;
        compaction_state->start_new_page(row_limit, partition_limit, query_time, next_fragment_kind, consumer);

        auto last_ckey = make_lw_shared<std::optional<clustering_key_prefix>>();
        auto reader_consumer = make_stable_flattened_mutations_consumer<compact_for_query<OnlyLive, clustering_position_tracker<Consumer>>>(
                compaction_state,
                clustering_position_tracker(std::move(consumer), last_ckey));

        auto consume = [&reader, &slice, reader_consumer = std::move(reader_consumer), timeout, max_size] () mutable {
            if (slice.options.contains(query::partition_slice::option::reversed)) {
                return do_with(make_reversing_reader(reader, max_size),
                        [reader_consumer = std::move(reader_consumer), timeout] (flat_mutation_reader& reversing_reader) mutable {
                    return reversing_reader.consume(std::move(reader_consumer), timeout);
                });
            }
            return reader.consume(std::move(reader_consumer), timeout);
        };

        return consume().then([last_ckey] (auto&&... results) mutable {
            static_assert(sizeof...(results) <= 1);
            return make_ready_future<std::tuple<std::optional<clustering_key_prefix>, std::decay_t<decltype(results)>...>>(std::tuple(std::move(*last_ckey), std::move(results)...));
        });
    });
}

struct position_view {
    const dht::decorated_key* partition_key;
    const clustering_key_prefix* clustering_key;
};

class querier_base {
protected:
    schema_ptr _schema;
    reader_permit _permit;
    std::unique_ptr<const dht::partition_range> _range;
    std::unique_ptr<const query::partition_slice> _slice;
    flat_mutation_reader _reader;
    dht::partition_ranges_view _query_ranges;

public:
    querier_base(reader_permit permit, std::unique_ptr<const dht::partition_range> range,
            std::unique_ptr<const query::partition_slice> slice, flat_mutation_reader reader, dht::partition_ranges_view query_ranges)
        : _schema(reader.schema())
        , _permit(std::move(permit))
        , _range(std::move(range))
        , _slice(std::move(slice))
        , _reader(std::move(reader))
        , _query_ranges(query_ranges)
    { }

    querier_base(schema_ptr schema, reader_permit permit, dht::partition_range range,
            query::partition_slice slice, const mutation_source& ms, const io_priority_class& pc, tracing::trace_state_ptr trace_ptr)
        : _schema(std::move(schema))
        , _permit(std::move(permit))
        , _range(std::make_unique<const dht::partition_range>(std::move(range)))
        , _slice(std::make_unique<const query::partition_slice>(std::move(slice)))
        , _reader(ms.make_reader(_schema, _permit, *_range, *_slice, pc, std::move(trace_ptr), streamed_mutation::forwarding::no, mutation_reader::forwarding::no))
        , _query_ranges(*_range)
    { }

    querier_base(querier_base&&) = default;
    querier_base& operator=(querier_base&&) = default;

    virtual ~querier_base() = default;

    const ::schema& schema() const {
        return *_schema;
    }

    reader_permit& permit() {
        return _permit;
    }

    bool is_reversed() const {
        return _slice->options.contains(query::partition_slice::option::reversed);
    }

    virtual position_view current_position() const = 0;

    dht::partition_ranges_view ranges() const {
        return _query_ranges;
    }

    size_t memory_usage() const {
        return _reader.buffer_size();
    }
};

/// One-stop object for serving queries.
///
/// Encapsulates all state and logic for serving all pages for a given range
/// of a query on a given shard. Can be used with any CompactedMutationsConsumer
/// certified result-builder.
/// Intended to be created on the first page of a query then saved and reused on
/// subsequent pages.
/// (1) Create with the parameters of your query.
/// (2) Call consume_page() with your consumer to consume the contents of the
///     next page.
/// (3) At the end of the page save the querier if you expect more pages.
///     The are_limits_reached() method can be used to determine whether the
///     page was filled or not. Also check your result builder for short reads.
///     Most result builders have memory-accounters that will stop the read
///     once some memory limit was reached. This is called a short read as the
///     read stops before the row and/or partition limits are reached.
/// (4) At the beginning of the next page validate whether it can be used with
///     the page's schema and start position. In case a schema or position
///     mismatch is detected the querier shouldn't be used to produce the next
///     page. It should be dropped instead and a new one should be created
///     instead.
template <emit_only_live_rows OnlyLive>
class querier : public querier_base {
    lw_shared_ptr<compact_for_query_state<OnlyLive>> _compaction_state;
    std::optional<clustering_key_prefix> _last_ckey;

public:
    querier(const mutation_source& ms,
            schema_ptr schema,
            reader_permit permit,
            dht::partition_range range,
            query::partition_slice slice,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_ptr)
        : querier_base(schema, permit, std::move(range), std::move(slice), ms, pc, std::move(trace_ptr))
        , _compaction_state(make_lw_shared<compact_for_query_state<OnlyLive>>(*schema, gc_clock::time_point{}, *_slice, 0, 0)) {
    }

    bool are_limits_reached() const {
        return  _compaction_state->are_limits_reached();
    }

    template <typename Consumer>
    requires CompactedFragmentsConsumer<Consumer>
    auto consume_page(Consumer&& consumer,
            uint64_t row_limit,
            uint32_t partition_limit,
            gc_clock::time_point query_time,
            db::timeout_clock::time_point timeout,
            query::max_result_size max_size) {
        return ::query::consume_page(_reader, _compaction_state, *_slice, std::move(consumer), row_limit, partition_limit, query_time,
                timeout, max_size).then([this] (auto&& results) {
            _last_ckey = std::get<std::optional<clustering_key>>(std::move(results));
            constexpr auto size = std::tuple_size<std::decay_t<decltype(results)>>::value;
            static_assert(size <= 2);
            if constexpr (size == 1) {
                return make_ready_future<>();
            } else {
                auto result = std::get<1>(std::move(results));
                return make_ready_future<std::decay_t<decltype(result)>>(std::move(result));
            }
        });
    }

    virtual position_view current_position() const override {
        const dht::decorated_key* dk = _compaction_state->current_partition();
        const clustering_key_prefix* clustering_key = _last_ckey ? &*_last_ckey : nullptr;
        return {dk, clustering_key};
    }
};

using data_querier = querier<emit_only_live_rows::yes>;
using mutation_querier = querier<emit_only_live_rows::no>;

/// Local state of a multishard query.
///
/// This querier is not intended to be used directly to read pages. Instead it
/// is merely a shard local state of a suspended multishard query and is
/// intended to be used for storing the state of the query on each shard where
/// it executes. It stores the local reader and the referenced parameters it was
/// created with (similar to other queriers).
/// For position validation purposes (at lookup) the reader's position is
/// considered to be the same as that of the query.
class shard_mutation_querier : public querier_base {
    std::unique_ptr<const dht::partition_range_vector> _query_ranges;
    dht::decorated_key _nominal_pkey;
    std::optional<clustering_key_prefix> _nominal_ckey;

private:
    shard_mutation_querier(
            std::unique_ptr<const dht::partition_range_vector> query_ranges,
            std::unique_ptr<const dht::partition_range> reader_range,
            std::unique_ptr<const query::partition_slice> reader_slice,
            flat_mutation_reader reader,
            reader_permit permit,
            dht::decorated_key nominal_pkey,
            std::optional<clustering_key_prefix> nominal_ckey)
        : querier_base(permit, std::move(reader_range), std::move(reader_slice), std::move(reader), *query_ranges)
        , _query_ranges(std::move(query_ranges))
        , _nominal_pkey(std::move(nominal_pkey))
        , _nominal_ckey(std::move(nominal_ckey)) {
    }


public:
    shard_mutation_querier(
            const dht::partition_range_vector query_ranges,
            std::unique_ptr<const dht::partition_range> reader_range,
            std::unique_ptr<const query::partition_slice> reader_slice,
            flat_mutation_reader reader,
            reader_permit permit,
            dht::decorated_key nominal_pkey,
            std::optional<clustering_key_prefix> nominal_ckey)
        : shard_mutation_querier(std::make_unique<const dht::partition_range_vector>(std::move(query_ranges)), std::move(reader_range),
                std::move(reader_slice), std::move(reader), std::move(permit), std::move(nominal_pkey), std::move(nominal_ckey)) {
    }

    virtual position_view current_position() const override {
        return {&_nominal_pkey, _nominal_ckey ? &*_nominal_ckey : nullptr};
    }

    std::unique_ptr<const dht::partition_range> reader_range() && {
        return std::move(_range);
    }

    std::unique_ptr<const query::partition_slice> reader_slice() && {
        return std::move(_slice);
    }

    flat_mutation_reader reader() && {
        return std::move(_reader);
    }
};

/// Special-purpose cache for saving queriers between pages.
///
/// Queriers are saved at the end of the page and looked up at the beginning of
/// the next page. The lookup() always removes the querier from the cache, it
/// has to be inserted again at the end of the page.
/// Lookup provides the following extra logic, special to queriers:
/// * It accepts a factory function which is used to create a new querier if
///     the lookup fails (see below). This allows for simple call sites.
/// * It does range matching. A query sometimes will result in multiple querier
///     objects executing on the same node and shard paralelly. To identify the
///     appropriate querier lookup() will consider - in addition to the lookup
///     key - the read range.
/// * It does schema version and position checking. In some case a subsequent
///     page will have a different schema version or will start from a position
///     that is before the end position of the previous page. lookup() will
///     recognize these cases and drop the previous querier and create a new one.
///
/// Inserted queriers will have a TTL. When this expires the querier is
/// evicted. This is to avoid excess and unnecessary resource usage due to
/// abandoned queriers.
/// Registers cached readers with the reader concurrency semaphore, as inactive
/// readers, so the latter can evict them if needed.
/// Keeps the total memory consumption of cached queriers
/// below max_queriers_memory_usage by evicting older entries upon inserting
/// new ones if the the memory consupmtion would go above the limit.
class querier_cache {
public:
    static const std::chrono::seconds default_entry_ttl;

    struct stats {
        // The number of inserts into the cache.
        uint64_t inserts = 0;
        // The number of cache lookups.
        uint64_t lookups = 0;
        // The subset of lookups that missed.
        uint64_t misses = 0;
        // The subset of lookups that hit but the looked up querier had to be
        // dropped due to position mismatch.
        uint64_t drops = 0;
        // The number of queriers evicted due to their TTL expiring.
        uint64_t time_based_evictions = 0;
        // The number of queriers evicted to free up resources to be able to
        // create new readers.
        uint64_t resource_based_evictions = 0;
        // The number of queriers evicted to because the maximum memory usage
        // was reached.
        uint64_t memory_based_evictions = 0;
        // The number of queriers currently in the cache.
        uint64_t population = 0;
    };

    class entry : public boost::intrusive::set_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>> {
        // Self reference so that we can remove the entry given an `entry&`.
        std::list<entry>::iterator _pos;
        const utils::UUID _key;
        const lowres_clock::time_point _expires;
        std::unique_ptr<querier_base> _value;
        reader_concurrency_semaphore::inactive_read_handle _handle;

    public:
        template <typename Querier>
        entry(utils::UUID key, Querier q, lowres_clock::time_point expires)
            : _key(key)
            , _expires(expires)
            , _value(std::make_unique<Querier>(std::move(q))) {
        }

        std::list<entry>::iterator pos() const {
            return _pos;
        }

        void set_pos(std::list<entry>::iterator pos) {
            _pos = pos;
        }

        void set_inactive_handle(reader_concurrency_semaphore::inactive_read_handle handle) {
            _handle = std::move(handle);
        }

        reader_concurrency_semaphore::inactive_read_handle get_inactive_handle() && {
            return std::move(_handle);
        }

        const utils::UUID& key() const {
            return _key;
        }

        bool is_expired(const lowres_clock::time_point& now) const {
            return _expires <= now;
        }

        const querier_base& value() const {
            return *_value;
        }

        querier_base& value() {
            return *_value;
        }
    };

    struct key_of_entry {
        using type = utils::UUID;
        const type& operator()(const entry& e) { return e.key(); }
    };

    using entries = std::list<entry>;
    using index = boost::intrusive::multiset<entry, boost::intrusive::key_of_value<key_of_entry>,
          boost::intrusive::constant_time_size<false>>;

private:
    entries _entries;
    index _data_querier_index;
    index _mutation_querier_index;
    index _shard_mutation_querier_index;
    timer<lowres_clock> _expiry_timer;
    std::chrono::seconds _entry_ttl;
    stats _stats;
    size_t _max_queriers_memory_usage;

    void scan_cache_entries();

public:
    explicit querier_cache(size_t max_cache_size = 1'000'000, std::chrono::seconds entry_ttl = default_entry_ttl);

    querier_cache(const querier_cache&) = delete;
    querier_cache& operator=(const querier_cache&) = delete;

    // this is captured
    querier_cache(querier_cache&&) = delete;
    querier_cache& operator=(querier_cache&&) = delete;

    void insert(utils::UUID key, data_querier&& q, tracing::trace_state_ptr trace_state);

    void insert(utils::UUID key, mutation_querier&& q, tracing::trace_state_ptr trace_state);

    void insert(utils::UUID key, shard_mutation_querier&& q, tracing::trace_state_ptr trace_state);

    /// Lookup a data querier in the cache.
    ///
    /// Queriers are found based on `key` and `range`. There may be multiple
    /// queriers for the same `key` differentiated by their read range. Since
    /// each subsequent page may have a narrower read range then the one before
    /// it ranges cannot be simply matched based on equality. For matching we
    /// use the fact that the coordinator splits the query range into
    /// non-overlapping ranges. Thus both bounds of any range, or in case of
    /// singular ranges only the start bound are guaranteed to be unique.
    ///
    /// The found querier is checked for a matching position and schema version.
    /// The start position of the querier is checked against the start position
    /// of the page using the `range' and `slice'.
    std::optional<data_querier> lookup_data_querier(utils::UUID key,
            const schema& s,
            const dht::partition_range& range,
            const query::partition_slice& slice,
            tracing::trace_state_ptr trace_state);

    /// Lookup a mutation querier in the cache.
    ///
    /// See \ref lookup_data_querier().
    std::optional<mutation_querier> lookup_mutation_querier(utils::UUID key,
            const schema& s,
            const dht::partition_range& range,
            const query::partition_slice& slice,
            tracing::trace_state_ptr trace_state);

    /// Lookup a shard mutation querier in the cache.
    ///
    /// See \ref lookup_data_querier().
    std::optional<shard_mutation_querier> lookup_shard_mutation_querier(utils::UUID key,
            const schema& s,
            const dht::partition_range_vector& ranges,
            const query::partition_slice& slice,
            tracing::trace_state_ptr trace_state);

    void set_entry_ttl(std::chrono::seconds entry_ttl);

    /// Evict a querier.
    ///
    /// Return true if a querier was evicted and false otherwise (if the cache
    /// is empty).
    bool evict_one();

    /// Evict all queriers that belong to a table.
    ///
    /// Should be used when dropping a table.
    void evict_all_for_table(const utils::UUID& schema_id);

    const stats& get_stats() const {
        return _stats;
    }
};

class querier_cache_context {
    querier_cache* _cache{};
    utils::UUID _key;
    query::is_first_page _is_first_page;

public:
    querier_cache_context() = default;
    querier_cache_context(querier_cache& cache, utils::UUID key, query::is_first_page is_first_page);
    void insert(data_querier&& q, tracing::trace_state_ptr trace_state);
    void insert(mutation_querier&& q, tracing::trace_state_ptr trace_state);
    void insert(shard_mutation_querier&& q, tracing::trace_state_ptr trace_state);
    std::optional<data_querier> lookup_data_querier(const schema& s,
            const dht::partition_range& range,
            const query::partition_slice& slice,
            tracing::trace_state_ptr trace_state);
    std::optional<mutation_querier> lookup_mutation_querier(const schema& s,
            const dht::partition_range& range,
            const query::partition_slice& slice,
            tracing::trace_state_ptr trace_state);
    std::optional<shard_mutation_querier> lookup_shard_mutation_querier(const schema& s,
            const dht::partition_range_vector& ranges,
            const query::partition_slice& slice,
            tracing::trace_state_ptr trace_state);
};

} // namespace query
