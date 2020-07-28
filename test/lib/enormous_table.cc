#include <boost/range/adaptor/indirected.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/find_if.hpp>

#include "clustering_bounds_comparator.hh"
#include "database_fwd.hh"
#include "dht/i_partitioner.hh"
#include "partition_range_compat.hh"
#include "range.hh"
#include "service/storage_proxy.hh"
#include "mutation_fragment.hh"
#include "sstables/sstables.hh"
#include "db/timeout_clock.hh"
#include "database.hh"
#include "enormous_table.hh"

namespace enormous_table {

// logging::logger elogger("enormous_logger");

enormous_table_reader::enormous_table_reader(schema_ptr schema, const dht::partition_range& prange, const query::partition_slice& slice)
        : impl(schema)
        , _schema(std::move(schema))
        , _slice(slice)
{
    // elogger.info("create enormous_table_reader {} {}", prange, slice);
    do_fast_forward_to(prange);
}

enormous_table_reader::~enormous_table_reader() {
    // elogger.info("destroy enormous_table_reader");
}

void enormous_table_reader::next_partition() {
    clear_buffer();
    _end_of_stream = true;
}

future<> enormous_table_reader::fill_buffer(db::timeout_clock::time_point timeout) {
    // elogger.info("fill_buffer");
    if (!_partition_in_range) {
        return make_ready_future<>();
    }
    return do_until([this] { return is_end_of_stream() || is_buffer_full(); }, [this] {
        auto int_to_ck = [this] (int64_t i) -> clustering_key {
            auto ck_data = data_value(i).serialize_nonnull();
            return clustering_key::from_single_value(*_schema, std::move(ck_data));
        };

        auto ck_to_int = [this] (const clustering_key& ck) -> int64_t {
            auto exploded = ck.explode();
            assert(exploded.size() == 1);
            return value_cast<int64_t>(long_type->deserialize(exploded[0]));
        };

        auto dk = get_dk();
        if (_pps == partition_production_state::before_partition_start) {
            push_mutation_fragment(partition_start(std::move(dk), tombstone()));
            _pps = partition_production_state::after_partition_start;

        } else if (_pps == partition_production_state::after_partition_start) {
            auto cmp = clustering_key::tri_compare(*_schema);

            auto ck = int_to_ck(_clustering_row_idx);
            for (const auto& range : _slice.row_ranges(*_schema, dk.key())) {
                if (range.before(ck, cmp)) {
                    _clustering_row_idx = ck_to_int(range.start()->value());
                    if (!range.start()->is_inclusive()) {
                        ++_clustering_row_idx;
                    }
                    ck = int_to_ck(_clustering_row_idx);
                    break;
                }
                if (!range.after(ck, cmp)) {
                    break;
                }
            }

            if (_clustering_row_idx >= CLUSTERING_ROW_COUNT) {
                _pps = partition_production_state::before_partition_end;
                return make_ready_future<>();
            }

            ++_clustering_row_idx;
            auto crow = clustering_row(std::move(ck));
            // crow.set_cell(_cdef, atomic_cell::make_live(*_cdef.type, ));
            crow.marker() = row_marker(api::new_timestamp());
            push_mutation_fragment(std::move(crow));

        } else if (_pps == partition_production_state::before_partition_end) {
            push_mutation_fragment(partition_end());
            _pps = partition_production_state::after_partition_end;
            _end_of_stream = true;
        }
        return make_ready_future<>();
    });
}

future<> enormous_table_reader::fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) {
    do_fast_forward_to(pr);
    return make_ready_future<>();
}

partition_key enormous_table_reader::get_pk() {
    auto pk_data = data_value(int64_t(0)).serialize_nonnull();
    return partition_key::from_single_value(*_schema, std::move(pk_data));
}

void enormous_table_reader::do_fast_forward_to(const dht::partition_range& pr) {
    // elogger.info("do_fast_forward_to {}", pr);
    clear_buffer();
    auto pos = dht::ring_position(get_dk());
    _partition_in_range = pr.contains(pos, dht::ring_position_comparator(*_schema));
    _end_of_stream = !_partition_in_range;
    if (_partition_in_range) {
        _pps = partition_production_state::before_partition_start;
    }
}

future<> enormous_table_reader::fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) {
    throw runtime_exception("not forwardable");
    return make_ready_future<>();
}

void enormous_table_reader::get_next_partition() {
    // elogger.info("get next partition");
    if (_pps != partition_production_state::not_started) {
        _end_of_stream = true;
    }
}

}
