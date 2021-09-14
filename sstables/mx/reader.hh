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

#include "flat_mutation_reader_v2.hh"
#include "sstables/progress_monitor.hh"

namespace sstables {
namespace mx {

// Precondition: if the slice is reversed, the schema must be reversed as well.
// Reversed slices must be provided in the 'half-reversed' format (the order of ranges
// being reversed, but the ranges themselves are not).
//
// If the slice is reversed then:
// - if this is a single-partition read (range.is_singular()), each partition
//   in the returned fragment stream will be reversed - i.e. ordered according to
//   the reversed schema. The reader's schema will be the provided schema.
//   In this mode fast-forwarding is not supported (FIXME);
// - otherwise, the data will be returned in non-reversed order and the reader's
//   schema will be non-reversed - i.e. it will be the reverse of the provided schema
//   (since the provided schema is already reversed according to the precondition).
//   In this case the caller is responsible for reversing the fragment stream
//   themselves.
flat_mutation_reader_v2 make_reader(
        shared_sstable sstable,
        schema_ptr schema,
        reader_permit permit,
        const dht::partition_range& range,
        const query::partition_slice& slice,
        const io_priority_class& pc,
        tracing::trace_state_ptr trace_state,
        streamed_mutation::forwarding fwd,
        mutation_reader::forwarding fwd_mr,
        read_monitor& monitor);

// A reader which doesn't use the index at all. It reads everything from the
// sstable and it doesn't support skipping.
flat_mutation_reader_v2 make_crawling_reader(
        shared_sstable sstable,
        schema_ptr schema,
        reader_permit permit,
        const io_priority_class& pc,
        tracing::trace_state_ptr trace_state,
        read_monitor& monitor);

} // namespace mx
} // namespace sstables
