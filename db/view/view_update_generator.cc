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

#include <boost/range/adaptor/map.hpp>
#include "view_update_generator.hh"
#include "service/priority_manager.hh"
#include "utils/error_injection.hh"

static logging::logger vug_logger("view_update_generator");

static inline void inject_failure(std::string_view operation) {
    utils::get_local_injector().inject(operation,
            [operation] { throw std::runtime_error(std::string(operation)); });
}

namespace db::view {

future<> view_update_generator::start() {
    thread_attributes attr;
    attr.sched_group = _db.get_streaming_scheduling_group();
    _started = seastar::async(std::move(attr), [this]() mutable {
        auto drop_sstable_references = defer([&] {
            // Clear sstable references so sstables_manager::stop() doesn't hang.
            vug_logger.info("leaving {} unstaged sstables unprocessed",
                    _sstables_to_move.size(), _sstables_with_tables.size());
            _sstables_to_move.clear();
            _sstables_with_tables.clear();
        });
        while (!_as.abort_requested()) {
            if (_sstables_with_tables.empty()) {
                _pending_sstables.wait().get();
            }

            // To ensure we don't race with updates, move the entire content
            // into a local variable.
            auto sstables_with_tables = std::exchange(_sstables_with_tables, {});

            // If we got here, we will process all tables we know about so far eventually so there
            // is no starvation
            for (auto table_it = sstables_with_tables.begin(); table_it != sstables_with_tables.end(); table_it = sstables_with_tables.erase(table_it)) {
                auto& [t, sstables] = *table_it;
                schema_ptr s = t->schema();

                vug_logger.trace("Processing {}.{}: {} sstables", s->ks_name(), s->cf_name(), sstables.size());

                const auto num_sstables = sstables.size();

                try {
                    // Exploit the fact that sstables in the staging directory
                    // are usually non-overlapping and use a partitioned set for
                    // the read.
                    auto ssts = make_lw_shared<sstables::sstable_set>(sstables::make_partitioned_sstable_set(s, false));
                    for (auto& sst : sstables) {
                        ssts->insert(sst);
                    }

                    auto permit = _db.get_reader_concurrency_semaphore().make_permit(s.get(), "view_update_generator");
                    auto ms = mutation_source([this, ssts] (
                                schema_ptr s,
                                reader_permit permit,
                                const dht::partition_range& pr,
                                const query::partition_slice& ps,
                                const io_priority_class& pc,
                                tracing::trace_state_ptr ts,
                                streamed_mutation::forwarding fwd_ms,
                                mutation_reader::forwarding fwd_mr) {
                        return make_restricted_range_sstable_reader(std::move(ssts), s, std::move(permit), pr, ps, pc, std::move(ts), fwd_ms, fwd_mr);
                    });
                    auto [staging_sstable_reader, staging_sstable_reader_handle] = make_manually_paused_evictable_reader(
                            std::move(ms),
                            s,
                            permit,
                            query::full_partition_range,
                            s->full_slice(),
                            service::get_local_streaming_priority(),
                            nullptr,
                            ::mutation_reader::forwarding::no);

                    inject_failure("view_update_generator_consume_staging_sstable");
                    auto result = staging_sstable_reader.consume_in_thread(view_updating_consumer(s, std::move(permit), *t, sstables, _as, staging_sstable_reader_handle), db::no_timeout);
                    if (result == stop_iteration::yes) {
                        break;
                    }
                } catch (...) {
                    vug_logger.warn("Processing {} failed for table {}:{}. Will retry...", s->ks_name(), s->cf_name(), std::current_exception());
                    // Need to add sstables back to the set so we can retry later. By now it may
                    // have had other updates.
                    std::move(sstables.begin(), sstables.end(), std::back_inserter(_sstables_with_tables[t]));
                    break;
                }
                try {
                    inject_failure("view_update_generator_collect_consumed_sstables");
                    // collect all staging sstables to move in a map, grouped by table.
                    std::move(sstables.begin(), sstables.end(), std::back_inserter(_sstables_to_move[t]));
                } catch (...) {
                    // Move from staging will be retried upon restart.
                    vug_logger.warn("Moving {} from staging failed: {}:{}. Ignoring...", s->ks_name(), s->cf_name(), std::current_exception());
                }
                _registration_sem.signal(num_sstables);
            }
            // For each table, move the processed staging sstables into the table's base dir.
            for (auto it = _sstables_to_move.begin(); it != _sstables_to_move.end(); ) {
                auto& [t, sstables] = *it;
                try {
                    inject_failure("view_update_generator_move_staging_sstable");
                    t->move_sstables_from_staging(sstables).get();
                } catch (...) {
                    // Move from staging will be retried upon restart.
                    vug_logger.warn("Moving some sstable from staging failed: {}. Ignoring...", std::current_exception());
                }
                it = _sstables_to_move.erase(it);
            }
        }
    });
    return make_ready_future<>();
}

future<> view_update_generator::stop() {
    _as.request_abort();
    _pending_sstables.signal();
    return std::move(_started).then([this] {
        _registration_sem.broken();
    });
}

bool view_update_generator::should_throttle() const {
    return !_started.available();
}

future<> view_update_generator::register_staging_sstable(sstables::shared_sstable sst, lw_shared_ptr<table> table) {
    if (_as.abort_requested()) {
        return make_ready_future<>();
    }
    inject_failure("view_update_generator_registering_staging_sstable");
    _sstables_with_tables[table].push_back(std::move(sst));

    _pending_sstables.signal();
    if (should_throttle()) {
        return _registration_sem.wait(1);
    } else {
        _registration_sem.consume(1);
        return make_ready_future<>();
    }
}

void view_update_generator::setup_metrics() {
    namespace sm = seastar::metrics;

    _metrics.add_group("view_update_generator", {
        sm::make_gauge("pending_registrations", sm::description("Number of tasks waiting to register staging sstables"),
                [this] { return _registration_sem.waiters(); }),

        sm::make_gauge("queued_batches_count", 
                sm::description("Number of sets of sstables queued for view update generation"),
                [this] { return _sstables_with_tables.size(); }),

        sm::make_gauge("sstables_to_move_count",
                sm::description("Number of sets of sstables which are already processed and wait to be moved from their staging directory"),
                [this] { return _sstables_to_move.size(); })
    });
}

}
