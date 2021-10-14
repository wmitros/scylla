/*
 * Copyright (C) 2021-present ScyllaDB
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

#include <seastar/core/iostream.hh>
#include <seastar/core/io_priority_class.hh>
#include "reader_permit.hh"
#include "sstables/index_reader.hh"
#include "sstables/shared_sstable.hh"
#include "sstables/sstables.hh"
#include "tracing/trace_state.hh"

namespace sstables {
namespace mx {

struct partition_reversing_data_source {
    seastar::data_source the_source;

    // Underneath, the data source is iterating over the sstable file in reverse order.
    // This points to the current position of the source over the underlying sstable file;
    // either the end of partition or the beginning of some row (never in the middle of a row).
    // The reference is valid as long as the data source is alive.
    const uint64_t& current_position_in_sstable;
};

// Returns a single partition retrieved from an sstable data file as a sequence of buffers
// but with the clustering order of rows reversed.
//
// `pos` is where the partition starts.
// `len` is the length of the partition.
// `ir` provides access to an index over the sstable.
//
// `ir.data_file_positions().end` may decrease below `current_position_in_sstable`,
// informing us that the user wants us to skip the sequence of rows between `ir.data_file_positions().end` and `current_position_in_sstable`.
// `ir.data_file_positions().end`, if engaged, must always point at the end of partition (pos + len) or the beginning of some row.
// We ignore the value of `ir.data_file_positions().start`.
//
// We assume that `ir.current_clustered_cursor()`, if engaged, is of type `sstables::mc::bsearch_clustered_cursor*`.
partition_reversing_data_source make_partition_reversing_data_source(
    const schema& s, shared_sstable sst, index_reader& ir, uint64_t pos, size_t len,
    reader_permit permit, const io_priority_class& io_priority, tracing::trace_state_ptr trace_state);

}
}
