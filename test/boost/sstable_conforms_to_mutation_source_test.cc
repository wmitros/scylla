/*
 * Copyright (C) 2015-present ScyllaDB
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


#include <boost/test/unit_test.hpp>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include "test/boost/sstable_test.hh"
#include <seastar/core/thread.hh>
#include "sstables/sstables.hh"
#include "test/lib/mutation_source_test.hh"
#include "test/lib/sstable_utils.hh"
#include "row_cache.hh"
#include "test/lib/simple_schema.hh"
#include "partition_slice_builder.hh"
#include "test/lib/flat_mutation_reader_assertions.hh"
#include "db/chained_delegating_reader.hh"
#include "flat_mutation_reader.hh"

using namespace sstables;
using namespace std::chrono_literals;

static
mutation_source make_sstable_mutation_source(sstables::test_env& env, schema_ptr s, sstring dir, std::vector<mutation> mutations,
        sstable_writer_config cfg, sstables::sstable::version_types version, gc_clock::time_point query_time = gc_clock::now()) {
    return as_mutation_source(make_sstable(env, s, dir, std::move(mutations), cfg, version, query_time));
}

// Create a mutation_source that performs a reversed query on an sstable from the given set of mutations.
// The data is returned in forward order (non-reversed). We achieve this by reversing the given set
// of mutations and creating the sstable from that. Performing a reversed query then gives us back the original order.
// WARNING: the readers produced by this source cannot be partition-forwarded (they can be position-forwarded).
static
mutation_source make_sstable_reversing_mutation_source(sstables::test_env& env, schema_ptr s, sstring dir, std::vector<mutation> mutations,
        sstable_writer_config cfg, sstables::sstable::version_types version, gc_clock::time_point query_time = gc_clock::now()) {
    for (auto& m: mutations) {
        m = reverse(std::move(m));
    }
    auto sst = make_sstable(env, s->make_reversed(), dir, std::move(mutations), cfg, version, query_time);

    return mutation_source([sst = std::move(sst)] (schema_ptr s,
            reader_permit permit,
            const dht::partition_range& range,
            const query::partition_slice& slice,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_state,
            streamed_mutation::forwarding fwd,
            mutation_reader::forwarding fwd_mr) mutable {
        // `make_reader_v1` expects a reversed schema and a half-reversed slice.
        //
        // Observe that `s` is indeed the reverse of the schema used to create the sstable
        // (assuming that `s` the mutation source lambda argument is the same as `s` the outer function argument - which it should be).
        //
        // `slice` is given in forward order for `s`. We need it in half-reversed order for `s->make_reversed()`.
        // We first obtain the forward order for `s->make_reversed()` - i.e. the native reversed order for `s` - and then half-reverse that.
        // Example: given a slice [1, 3], [5, 9] for `s`, the corresponding slice for `s->make_reversed()` would be [9, 5], [3, 1].
        // The half-reversed version of this for `s->make_reversed()` is [3, 1], [9, 5]; this is what the reader expects.
        auto rev_slice = make_lw_shared<query::partition_slice>(half_reverse_slice(*s, reverse_slice(*s, slice)));
        // We've flipped the option twice, flip it again...
        rev_slice->options.set(query::partition_slice::option::reversed);
        return make_flat_mutation_reader<chained_delegating_reader>(s, [=] () mutable -> future<flat_mutation_reader> {
            auto rd = sst->make_reader_v1(s, std::move(permit), range, *rev_slice, pc, std::move(trace_state),
                // The reader does not support forwarding in reverse mode. We'll use `make_forwardable`.
                streamed_mutation::forwarding::no,
                // FIXME: `fwd_mr` passed in by the caller may actually be `yes` - not intentionally, but because
                // this is the usual default; but we still expect the user not to forward us.
                mutation_reader::forwarding::no,
                default_read_monitor());
            if (fwd == streamed_mutation::forwarding::yes) {
                rd = make_forwardable(std::move(rd));
            }
            return make_ready_future<flat_mutation_reader>(std::move(rd));
        }, permit, [rev_slice] { });
    });
}

static void consume_all(flat_mutation_reader& rd) {
    while (auto mfopt = rd().get0()) {}
}

// It is assumed that src won't change.
static snapshot_source snapshot_source_from_snapshot(mutation_source src) {
    return snapshot_source([src = std::move(src)] {
        return src;
    });
}

static
void test_cache_population_with_range_tombstone_adjacent_to_population_range(populate_fn_ex populate) {
    simple_schema s;
    tests::reader_concurrency_semaphore_wrapper semaphore;
    auto cache_mt = make_lw_shared<memtable>(s.schema());

    auto pkey = s.make_pkey();

    // underlying should not be empty, otherwise cache will make the whole range continuous
    mutation m1(s.schema(), pkey);
    s.add_row(m1, s.make_ckey(0), "v1");
    s.add_row(m1, s.make_ckey(1), "v2");
    s.add_row(m1, s.make_ckey(2), "v3");
    s.delete_range(m1, s.make_ckey_range(2, 100));
    cache_mt->apply(m1);

    cache_tracker tracker;
    auto ms = populate(s.schema(), std::vector<mutation>({m1}), gc_clock::now());
    row_cache cache(s.schema(), snapshot_source_from_snapshot(std::move(ms)), tracker);

    auto pr = dht::partition_range::make_singular(pkey);

    auto populate_range = [&] (int start) {
        auto slice = partition_slice_builder(*s.schema())
                .with_range(query::clustering_range::make_singular(s.make_ckey(start)))
                .build();
        auto rd = cache.make_reader(s.schema(), semaphore.make_permit(), pr, slice);
        auto close_rd = deferred_close(rd);
        consume_all(rd);
    };

    populate_range(2);

    // The cache now has only row with ckey 2 populated and the rest is discontinuous.
    // Populating reader which stops populating at entry with ckey 2 should not forget
    // to emit range_tombstone which starts at before(2).

    assert_that(cache.make_reader(s.schema(), semaphore.make_permit()))
            .produces(m1)
            .produces_end_of_stream();
}

static future<> test_sstable_conforms_to_mutation_source(sstable_version_types version, int index_block_size) {
    return sstables::test_env::do_with_async([version, index_block_size] (sstables::test_env& env) {
        sstable_writer_config cfg = env.manager().configure_writer();
        cfg.promoted_index_block_size = index_block_size;

        std::vector<tmpdir> dirs;
        auto populate = [&env, &dirs, &cfg, version] (schema_ptr s, const std::vector<mutation>& partitions,
                                                      gc_clock::time_point query_time) -> mutation_source {
            dirs.emplace_back();
            return make_sstable_mutation_source(env, s, dirs.back().path().string(), partitions, cfg, version, query_time);
        };

        run_mutation_source_tests(populate);

        if (index_block_size == 1) {
            // The tests below are not sensitive to index bock size so run once.
            test_cache_population_with_range_tombstone_adjacent_to_population_range(populate);
        }
    });
}

static future<> test_sstable_reversing_conforms_to_mutation_source(sstable_version_types version, int index_block_size) {
    return sstables::test_env::do_with_async([version, index_block_size] (sstables::test_env& env) {
        sstable_writer_config cfg = env.manager().configure_writer();
        cfg.promoted_index_block_size = index_block_size;

        std::vector<tmpdir> dirs;
        auto populate = [&env, &dirs, &cfg, version] (schema_ptr s, const std::vector<mutation>& partitions,
                                                      gc_clock::time_point query_time) -> mutation_source {
            dirs.emplace_back();
            return make_sstable_reversing_mutation_source(env, s, dirs.back().path().string(), partitions, cfg, version, query_time);
        };

        run_mutation_source_tests(populate, false);
    });
}

static constexpr std::array<int, 3> block_sizes = { 1, 128, 64 * 1024 };

// Split for better parallelizm

SEASTAR_TEST_CASE(test_sstable_conforms_to_mutation_source_mc_tiny) {
    return test_sstable_conforms_to_mutation_source(writable_sstable_versions[0], block_sizes[0]);
}

SEASTAR_TEST_CASE(test_sstable_conforms_to_mutation_source_mc_medium) {
    return test_sstable_conforms_to_mutation_source(writable_sstable_versions[0], block_sizes[1]);
}

SEASTAR_TEST_CASE(test_sstable_conforms_to_mutation_source_mc_large) {
    return test_sstable_conforms_to_mutation_source(writable_sstable_versions[0], block_sizes[2]);
}

SEASTAR_TEST_CASE(test_sstable_conforms_to_mutation_source_md_tiny) {
    return test_sstable_conforms_to_mutation_source(writable_sstable_versions[1], block_sizes[0]);
}

SEASTAR_TEST_CASE(test_sstable_conforms_to_mutation_source_md_medium) {
    return test_sstable_conforms_to_mutation_source(writable_sstable_versions[1], block_sizes[1]);
}

SEASTAR_TEST_CASE(test_sstable_conforms_to_mutation_source_md_large) {
    return test_sstable_conforms_to_mutation_source(writable_sstable_versions[1], block_sizes[2]);
}

SEASTAR_TEST_CASE(test_sstable_reversing_conforms_to_mutation_source_mc_tiny) {
    return test_sstable_reversing_conforms_to_mutation_source(writable_sstable_versions[0], block_sizes[0]);
}

SEASTAR_TEST_CASE(test_sstable_reversing_conforms_to_mutation_source_mc_medium) {
    return test_sstable_reversing_conforms_to_mutation_source(writable_sstable_versions[0], block_sizes[1]);
}

SEASTAR_TEST_CASE(test_sstable_reversing_conforms_to_mutation_source_mc_large) {
    return test_sstable_reversing_conforms_to_mutation_source(writable_sstable_versions[0], block_sizes[2]);
}

SEASTAR_TEST_CASE(test_sstable_reversing_conforms_to_mutation_source_md_tiny) {
    return test_sstable_reversing_conforms_to_mutation_source(writable_sstable_versions[1], block_sizes[0]);
}

SEASTAR_TEST_CASE(test_sstable_reversing_conforms_to_mutation_source_md_medium) {
    return test_sstable_reversing_conforms_to_mutation_source(writable_sstable_versions[1], block_sizes[1]);
}

SEASTAR_TEST_CASE(test_sstable_reversing_conforms_to_mutation_source_md_large) {
    return test_sstable_reversing_conforms_to_mutation_source(writable_sstable_versions[1], block_sizes[2]);
}

// This assert makes sure we don't miss writable vertions
static_assert(writable_sstable_versions.size() == 2);
