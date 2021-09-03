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

#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include "partition_reversing_data_source.hh"
#include "reader_permit.hh"
#include "sstables/consumer.hh"
#include "sstables/shared_sstable.hh"
#include "sstables/sstables.hh"
#include "sstables/types.hh"
#include "vint-serialization.hh"

namespace sstables {

namespace mx {

// Parser for the partition header and the static row, if present.
//
// After consuming the input stream, allows reading the file offset after the consumed segment
// using header_end_pos()
// Parsing copied from the sstable reader, with verification removed.
//
class partition_header_context : public data_consumer::continuous_data_consumer<partition_header_context> {
    enum class state {
        PARTITION_START,
        PARTITION_KEY_AND_DELETION_TIME,
        FLAGS,
        FLAGS_2,
        EXTENDED_FLAGS,
        STATIC_ROW_SIZE,
        FINISHED
    } _state = state::PARTITION_START;

    uint64_t _header_end_pos;
    uint64_t current_position(temporary_buffer<char>& data) {
        return position() - data.size();
    }
public:
    bool non_consuming() const {
        return _state == state::FLAGS_2 || _state == state::STATIC_ROW_SIZE || _state == state::FINISHED;
    }
    void verify_end_state() const {
        if (_state != state::FINISHED) {
            throw std::runtime_error("partition_header_context - no more data but parsing is incomplete");
        }
    }
    uint64_t header_end_pos() {
        return _header_end_pos;
    }
    data_consumer::processing_result process_state(temporary_buffer<char>& data) {
        switch (_state) {
        case state::PARTITION_START:
            if (read_16(data) != read_status::ready) {
                // length of the partition key
                _state = state::PARTITION_KEY_AND_DELETION_TIME;
                break;
            }
        case state::PARTITION_KEY_AND_DELETION_TIME:
            _state = state::FLAGS;
            return skip(data,
                // skip partition key
                uint32_t{_u16}
                // skip deletion_time::local_deletion_time
                + sizeof(uint32_t)
                // skip deletion_time::marked_for_delete_at
                + sizeof(uint64_t));
        case state::FLAGS:
            if (read_8(data) != read_status::ready) {
                _state = state::EXTENDED_FLAGS;
                break;
            }
        case state::FLAGS_2: {
            // Peek the first row or tombstone. If it's a static row, determine where it ends,
            // i.e. where the sequence of clustering rows starts.

            auto flags = unfiltered_flags_m(_u8);
            if (flags.is_end_of_partition() || flags.is_range_tombstone() || !flags.has_extended_flags()) {
                _header_end_pos = current_position(data) - 1;
                _state = state::FINISHED;
                return data_consumer::proceed::no;
            }
            if (read_8(data) != read_status::ready) {
                _state = state::EXTENDED_FLAGS;
                break;
            }
        }
        case state::EXTENDED_FLAGS: {
            auto extended_flags = unfiltered_extended_flags_m(_u8);
            if (!extended_flags.is_static()) {
                _header_end_pos = current_position(data) - 2;
                _state = state::FINISHED;
                return data_consumer::proceed::no;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                // A static row is present.
                // There are no clustering blocks. Read the row body size:
                _state = state::STATIC_ROW_SIZE;
                break;
            }
        }
        case state::STATIC_ROW_SIZE:
            // skip the row body
            _header_end_pos = current_position(data) + _u64;
            _state = state::FINISHED;
        case state::FINISHED:
            // _header_end_pos is where the clustering rows start
            return data_consumer::proceed::no;
        }
        return data_consumer::proceed::yes;
    }

    partition_header_context(input_stream<char>&& input, uint64_t start, uint64_t maxlen, reader_permit permit)
                : continuous_data_consumer(std::move(permit), std::move(input), start, maxlen)
    {}
};

// Parser of rows/tombstones that skips their bodies.
//
// Reads rows in their file order, pausing consumption after each row.
// To read rows in reverse order, use the prev_len() value to find
// the start position of the previous row, and create a new context
// to read that row.
// After reading the end_of_partition flag, end_of_partition() returns
// true.
// After reading a tombstone, current_tombstone_reversing_info() returns
// information about the tombstone kind, as well as the offsets of its
// members, which is useful for reversing the tombstone.
//
// `row_body_skipping_context` does not handle the static row (if there is one in the partition),
// only `unfiltered`s (clustering rows and tombstones).
class row_body_skipping_context : public data_consumer::continuous_data_consumer<row_body_skipping_context> {
    enum class state {
        FLAGS,
        FLAGS_2,
        EXTENDED_FLAGS,
        RANGE_TOMBSTONE_KIND,
        RANGE_TOMBSTONE_SIZE,
        CK_BLOCK_HEADER,
        CK_BLOCK_END,
        BODY_SIZE,
        PREV_UNFILTERED_SIZE,
        RANGE_TOMBSTONE_BODY_TIMESTAMP,
        RANGE_TOMBSTONE_BODY_MARKED_FOR_DELETE_AT,
        FINISHED_ROW
    } _state = state::FLAGS;
    bool _end_of_partition = false;

public:
    struct tombstone_reversing_info {
        uint64_t kind_offset;
        bound_kind_m range_tombstone_kind;

        // Range tombstone markers in the sstable data file come in two kinds: bound markers and boundary markers.
        // Bound markers happen when a range tombstone opens or ends.
        // Boundary markers happen when one range tombstone ends but another opens at the same position.
        //
        // Bound markers have one `delta_deletion_time` structs (tombstone timestamp + local deletion time) at the end.
        // Boundary markers have two.
        //
        // `first_deletion_time_offset` gives the position of the first `delta_deletion_time` (which is present for both kinds),
        // after_first_deletion_time gives its end position (i.e. position of last byte plus one), which in case of boundary
        // markers is the start postiion of the second `delta_deletion_time` (in case of bound markers its the end of the whole marker).
        uint64_t first_deletion_time_offset;
        uint64_t after_first_deletion_time_offset;
    };
private:
    unfiltered_flags_m _flags{0};
    std::optional<tombstone_reversing_info> _current_tombstone_reversing_info = std::nullopt;
    uint64_t _next_row_offset;
    uint64_t _prev_unfiltered_size;

    // for calculating the clustering blocks
    boost::iterator_range<std::vector<std::optional<uint32_t>>::const_iterator> _ck_column_value_fix_lengths;
    uint64_t _ck_blocks_header;
    uint32_t _ck_blocks_header_offset;
    uint16_t _ck_size;
    column_translation _column_translation;
    fragmented_temporary_buffer _column_value;

    void setup_ck(const std::vector<std::optional<uint32_t>>& column_value_fix_lengths) {
        if (column_value_fix_lengths.empty()) {
            _ck_column_value_fix_lengths = boost::make_iterator_range(column_value_fix_lengths);
        } else {
            _ck_column_value_fix_lengths = boost::make_iterator_range(std::begin(column_value_fix_lengths),
                                                                      std::begin(column_value_fix_lengths) + _ck_size);
        }
        _ck_blocks_header_offset = 0u;
    }
    bool no_more_ck_blocks() const { return _ck_column_value_fix_lengths.empty(); }
    void move_to_next_ck_block() {
        _ck_column_value_fix_lengths.advance_begin(1);
        ++_ck_blocks_header_offset;
        if (_ck_blocks_header_offset == 32u) {
            _ck_blocks_header_offset = 0u;
        }
    }
    std::optional<uint32_t> get_ck_block_value_length() const {
        return _ck_column_value_fix_lengths.front();
    }
    bool is_block_empty() const {
        return (_ck_blocks_header & (uint64_t(1) << (2 * _ck_blocks_header_offset))) != 0;
    }
    bool is_block_null() const {
        return (_ck_blocks_header & (uint64_t(1) << (2 * _ck_blocks_header_offset + 1))) != 0;
    }
    bool should_read_block_header() const {
        return _ck_blocks_header_offset == 0u;
    }

public:
    bool non_consuming() const {
        return _state == state::FINISHED_ROW;
    }
    void verify_end_state() const {
        if (_state == state::FLAGS) {
            throw std::runtime_error("row_body_skipping_context - no more data but parsing is incomplete");
        }
    }
    bool end_of_partition() const {
        return _end_of_partition;
    }
    uint64_t prev_len() {
        return _prev_unfiltered_size;
    }
    std::optional<tombstone_reversing_info> current_tombstone_reversing_info() {
        // std::nullopt if the last consumed unfiltered was not a tombstone
        return _current_tombstone_reversing_info;
    }
private:
    uint64_t current_position(temporary_buffer<char>& data) {
        return position() - data.size();
    }
public:
    data_consumer::processing_result process_state(temporary_buffer<char>& data) {
        switch (_state) {
        case state::FLAGS:
            if (read_8(data) != read_status::ready) {
                _state = state::FLAGS_2;
                break;
            }
        case state::FLAGS_2: {
            auto flags = unfiltered_flags_m(_u8);
            _current_tombstone_reversing_info.reset();
            if (flags.is_end_of_partition()) {
                _end_of_partition = true;
                _state = state::FLAGS;
                return data_consumer::proceed::no;
            } else if (flags.is_range_tombstone()) {
                _current_tombstone_reversing_info.emplace();
                _current_tombstone_reversing_info->kind_offset = current_position(data);
                if (read_8(data) != read_status::ready) {
                    _state = state::RANGE_TOMBSTONE_KIND;
                    break;
                }
                goto range_tombstone_kind_label;
            } else if (!flags.has_extended_flags()) {
                _ck_size = _column_translation.clustering_column_value_fix_legths().size();
                goto clustering_row_label;
            }
            if (read_8(data) != read_status::ready) {
                _state = state::EXTENDED_FLAGS;
                break;
            }
        }
        case state::EXTENDED_FLAGS: {
            auto extended_flags = unfiltered_extended_flags_m(_u8);
            // `row_body_skipping_context` should not be constructed on static rows
            assert(!extended_flags.is_static());
            _ck_size = _column_translation.clustering_column_value_fix_legths().size();
            goto clustering_row_label;
        }
        case state::RANGE_TOMBSTONE_KIND:
        range_tombstone_kind_label:
            _current_tombstone_reversing_info->range_tombstone_kind = bound_kind_m(_u8);
            if (read_16(data) != read_status::ready) {
                _state = state::RANGE_TOMBSTONE_SIZE;
                break;
            }
        case state::RANGE_TOMBSTONE_SIZE:
            _ck_size = _u16;
            if (_ck_size == 0) {
                goto body_label;
            }
        clustering_row_label:
            setup_ck(_column_translation.clustering_column_value_fix_legths());
        ck_block_label:
            if (no_more_ck_blocks()) {
                goto body_label;
            }
            if (!should_read_block_header()) {
                goto ck_block2_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::CK_BLOCK_HEADER;
                break;
            }
        case state::CK_BLOCK_HEADER:
            _ck_blocks_header = _u64;
        ck_block2_label: {
            if (is_block_null()) {
                move_to_next_ck_block();
                goto ck_block_label;
            }
            if (is_block_empty()) {
                move_to_next_ck_block();
                goto ck_block_label;
            }
            read_status status = read_status::waiting;
            if (auto len = get_ck_block_value_length()) {
                status = read_bytes(data, *len, _column_value);
            } else {
                status = read_unsigned_vint_length_bytes(data, _column_value);
            }
            if (status != read_status::ready) {
                _state = state::CK_BLOCK_END;
                break;
            }
        }
        case state::CK_BLOCK_END:
            move_to_next_ck_block();
            goto ck_block_label;
        body_label:
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::BODY_SIZE;
                break;
            }
        case state::BODY_SIZE:
            _next_row_offset = current_position(data) + _u64;
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::PREV_UNFILTERED_SIZE;
                break;
            }
        case state::PREV_UNFILTERED_SIZE:
            _prev_unfiltered_size = _u64;
            if (!_current_tombstone_reversing_info) {
                _state = state::FINISHED_ROW;
                // skip until the next row, allowing to read consecutive rows in disk order
                return skip(data, _next_row_offset - current_position(data));
            }
            _current_tombstone_reversing_info->first_deletion_time_offset = current_position(data);
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::RANGE_TOMBSTONE_BODY_TIMESTAMP;
                break;
            }

        case state::RANGE_TOMBSTONE_BODY_TIMESTAMP:
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::RANGE_TOMBSTONE_BODY_MARKED_FOR_DELETE_AT;
                break;
            }
        case state::RANGE_TOMBSTONE_BODY_MARKED_FOR_DELETE_AT:
            _current_tombstone_reversing_info->after_first_deletion_time_offset = current_position(data);
            _state = state::FINISHED_ROW;
            // skip until the next row, allowing to read consecutive rows in disk order
            return skip(data, _next_row_offset - current_position(data));
        case state::FINISHED_ROW:
            // extra state for stopping the reader after reading a row (we don't stop when skipping)
            _state = state::FLAGS;
            return data_consumer::proceed::no;
        }
        return data_consumer::proceed::yes;
    }
    row_body_skipping_context(input_stream<char>&& input, uint64_t start, uint64_t maxlen, reader_permit permit, column_translation ct)
                : continuous_data_consumer(std::move(permit), std::move(input), start, maxlen)
                , _column_translation(std::move(ct))
    {}
};

// Precondition: `k` is not static_clustering or clustering
bound_kind_m reverse_tombstone_kind(bound_kind_m k) {
    switch (k) {
        case bound_kind_m::excl_end:
            return bound_kind_m::excl_start;
        case bound_kind_m::incl_start:
            return bound_kind_m::incl_end;
        case bound_kind_m::excl_end_incl_start:
            return bound_kind_m::incl_end_excl_start;
        case bound_kind_m::incl_end_excl_start:
            return bound_kind_m::excl_end_incl_start;
        case bound_kind_m::incl_end:
            return bound_kind_m::incl_start;
        case bound_kind_m::excl_start:
            return bound_kind_m::excl_end;
        default:
            assert(false);
    }
}

// A 'row' consisting of a single byte, representing the end of partition in sstable data file.
static temporary_buffer<char> end_of_partition() {
    temporary_buffer<char> tmp(1);
    *tmp.get_write() = 1;
    return tmp;
}

// The intermediary data source that reads from an sstable, and produces
// data buffers, as if the sstable had all rows written in a reversed order.
//
// The intermediary always starts by reading the partition header and the
// static row using partition_header_context. The offset after the parsed
// segment is the new actual "partition end" in reversed order - when
// reached, an unfiltered with a single flag "partition_end" is produced.
//
// After reading the partition header, the data source advances to the end
// of the clustering range. Afterwards, we may encounter 2 situations:
// there is another unfiltered after the clustering range, or there is
// partition end. In the former case, we read the following unfiltered, and
// deduce the position of the first row of our actual range using
// row_body_skipping_context::prev_len(). If it's the latter, we find the
// last row by iterating over the entire last promoted index block.
//
// After finding the last row, we produce rows in reversed order one by one,
// parsing current row using row_body_skipping_context, and finding file
// offsets of the previous one using the start of the current row as the end,
// and the end decreased by row_body_skipping_context::prev_len() as the start
//
// We skip between clustering ranges using the index_reader's data range.
// When we detect that the range end has been decreased, we return to the same
// state as after reading the partition header, and continue as if the new
// range was the original.
//
// Because vast majority of the data consumed in our parsers is later reused
// in the sstable reader, we cache the read buffer. The size of the buffer
// starts at 4KB and is doubled after each read up to 128KB. We set the
// range of our reads so that the current row that will be returned to the
// sstable reader is at the end of the buffer. After returning a row, we
// trim it off the end of the buffer, so that the next row is again at the
// end of the buffer.
//
// Because the range tombstones are read in reversed order, we need to swap
// the start tombstones with the ends. We achieve that by finding the file
// offsets of the row tombstone member variables using row_body_skipping_context,
// and modifying them in our cached read accordingly.
//
class partition_reversing_data_source_impl final : public data_source_impl {
    const schema& _schema;
    shared_sstable _sst;
    index_reader& _ir;
    const ::io_priority_class& _io_priority;
    reader_permit _permit;
    tracing::trace_state_ptr _trace_state;
    std::optional<partition_header_context> _partition_header_context;
    std::optional<row_body_skipping_context> _row_skipping_context;
    uint64_t _clustering_range_start;
    uint64_t _partition_start;
    uint64_t _partition_end;

    // _row_start denotes our current position in the input stream:
    // either _partition_end or the start of some row (_row_start never lands in the middle of a row).
    // We share this position with the user (they can only read it, not modify it)
    // so they can e.g. compare it with index positions.
    uint64_t _row_start;
    uint64_t _row_end;
    // Invariant: _row_start <= _row_end

    temporary_buffer<char> _cached_read;
    uint64_t _current_read_size = 4 * 1024;
    const uint64_t max_read_size = 128 * 1024;

    column_translation _cached_column_translation;

    enum class state {
        // Looking for the first row entry (last in original order) in the clustering range being read
        RANGE_END,

        // Returning a buffer containing a row entry
        ROWS,

        // Returning a partition end flag
        PARTITION_END,

        // Nothing more to return
        FINISHED
    } _state = state::RANGE_END;
private:
    input_stream<char> data_stream(size_t start, size_t end) {
        return _sst->data_stream(start, end - start, _io_priority, _permit, _trace_state, {});
    }
    future<temporary_buffer<char>> data_read(uint64_t start, uint64_t end) {
        return _sst->data_read(start, end - start, _io_priority, _permit);
    }
    future<input_stream<char>> last_row_stream(size_t row_size) {
        if (_cached_read.size() < row_size) {
            if (_clustering_range_start + _current_read_size < _row_end) {
                _cached_read = co_await data_read(std::min(_row_end - _current_read_size, _row_end - row_size), _row_end);
            } else {
                _cached_read = co_await data_read(_clustering_range_start, _row_end);
            }
            _current_read_size = std::min(max_read_size, _current_read_size * 2);
        }
        co_return make_buffer_input_stream(_cached_read.share(_cached_read.size() - row_size, row_size));
    }
    temporary_buffer<char> last_row(size_t row_size) {
        auto tmp = _cached_read.share(_cached_read.size() - row_size, row_size);
        _cached_read.trim(_cached_read.size() - row_size);
        return tmp;
    }

    void modify_cached_tombstone(const row_body_skipping_context::tombstone_reversing_info& info) {
        auto to_cache_offset = [this] (uint64_t file_offset) {
            return _cached_read.size() - (_row_end - file_offset);
        };
        char& out = _cached_read.get_write()[to_cache_offset(info.kind_offset)];
        // reverse the kind of the range tombstone bound/boundary
        out = (char)reverse_tombstone_kind(info.range_tombstone_kind);
        if (is_boundary_between_adjacent_intervals(info.range_tombstone_kind)) {
            // if the tombstone is a boundary, we need to swap the order of end/start deletion times
            // Need to clone part of the buffer containing first_del_time because we overwrite it with second_del_time before using first_del_time
            auto first_del_time = _cached_read.share(to_cache_offset(info.first_deletion_time_offset), info.after_first_deletion_time_offset - info.first_deletion_time_offset).clone();
            // We also need to clone the part containing second_del_time as we may overwrite a prefix of that part while writing second_del_time
            // (if second_del_time is longer than first_del_time - it may be as we're dealing with varints here)
            auto second_del_time = _cached_read.share(to_cache_offset(info.after_first_deletion_time_offset), _row_end - info.after_first_deletion_time_offset).clone();
            std::copy(second_del_time.begin(), second_del_time.end(), _cached_read.get_write() + to_cache_offset(info.first_deletion_time_offset));
            std::copy(first_del_time.begin(), first_del_time.end(), _cached_read.get_write() + to_cache_offset(info.first_deletion_time_offset) + second_del_time.size());
        }
    }
public:
    partition_reversing_data_source_impl(const schema& s,
            shared_sstable sst,
            index_reader& ir,
            uint64_t partition_start,
            size_t partition_len,
            reader_permit permit,
            const io_priority_class& io_priority,
            tracing::trace_state_ptr trace_state)
        : _schema(s)
        , _sst(std::move(sst))
        , _ir(ir)
        , _io_priority(io_priority)
        , _permit(std::move(permit))
        , _trace_state(std::move(trace_state))
        , _partition_start(partition_start)
        , _partition_end(partition_start + partition_len)
        , _row_start(_partition_end)
        , _row_end(_partition_end)
        , _cached_column_translation(_sst->get_column_translation(_schema, _sst->get_serialization_header(), _sst->features()))
    { }

    partition_reversing_data_source_impl(partition_reversing_data_source_impl&&) noexcept = default;
    partition_reversing_data_source_impl& operator=(partition_reversing_data_source_impl&&) noexcept = default;

    virtual future<temporary_buffer<char>> get() override {
        if (!_partition_header_context) {
            _partition_header_context.emplace(data_stream(_partition_start, _partition_end), _partition_start, _partition_end - _partition_start, _permit);
            co_await _partition_header_context->consume_input();
            _clustering_range_start = _partition_header_context->header_end_pos();
            co_return co_await data_read(_partition_start, _clustering_range_start);
        }
        if (_ir.data_file_positions().end && *_ir.data_file_positions().end < _row_start) {
            // we can skip at least one row
            _row_start = *_ir.data_file_positions().end;
            if (_cached_read.size() + *_ir.data_file_positions().end >= _row_end) {
                // we can reuse the cache for the new range
                _cached_read.trim(_cached_read.size() - (_row_end - *_ir.data_file_positions().end));
            } else {
                // we'll need to reset the cache
                _cached_read.trim(0);
            }
            _state = state::RANGE_END;
        }
        switch (_state) {
        case state::RANGE_END: {
            bool look_in_last_block = false;
            if (_row_start >= _row_end) {
                assert(_row_start == _row_end);
                assert(_row_start == _partition_end);
                look_in_last_block = true;
            } else {
                _row_skipping_context.emplace(data_stream(_row_start, _row_end), _row_start, _row_end - _row_start, _permit, _cached_column_translation);
                co_await _row_skipping_context->consume_input();
                if (_row_skipping_context->end_of_partition()) {
                    look_in_last_block = true;
                } else {
                    _row_end = _row_start;
                    _row_start -= _row_skipping_context->prev_len();
                }
            }
            if (look_in_last_block) {
                _cached_read.trim(0);
                if (auto offset = co_await _ir.last_block_offset()) {
                    // there was a promoted index block in the partition, read from its beginning to find the last row
                    _row_start = *offset;
                } else {
                    // no promoted index blocks in the partition, read from the beginning
                    _row_start = _clustering_range_start;
                }
                uint64_t last_row_start = _row_start;
                _row_skipping_context.emplace(data_stream(_row_start, _partition_end), _row_start, _partition_end - _row_start, _permit, _cached_column_translation);
                co_await _row_skipping_context->consume_input();
                while (!_row_skipping_context->end_of_partition()) {
                    last_row_start = _row_start;
                    _row_start = _row_skipping_context->position();
                    co_await _row_skipping_context->consume_input();
                }
                _row_end = _row_start;
                _row_start = last_row_start;
                if (_row_start == _row_end) {
                    // empty partition
                    _state = state::FINISHED;
                    co_return end_of_partition();
                }
            }

            if (_row_start < _clustering_range_start) {
                // The first index block starts after the range being read,
                // i.e. the range being read is empty.
                assert(_row_skipping_context->prev_len() == _clustering_range_start - _partition_start);
                _row_start = _clustering_range_start;
                _state = state::FINISHED;
                co_return end_of_partition();
            }

            _state = state::ROWS;
            [[fallthrough]];
        }
        case state::ROWS: {
            _row_skipping_context.emplace(co_await last_row_stream(_row_end - _row_start), _row_start, _row_end - _row_start, _permit, _cached_column_translation);
            co_await _row_skipping_context->consume_input();
            if (_row_skipping_context->current_tombstone_reversing_info()) {
                // TODO: modify `ret`, not `_cached_read`
                modify_cached_tombstone(*_row_skipping_context->current_tombstone_reversing_info());
            }
            auto ret = last_row(_row_end - _row_start);
            _row_end = _row_start;
            _row_start -= _row_skipping_context->prev_len();
            if (_row_end == _clustering_range_start) {
                _state = state::PARTITION_END;
            }
            co_return ret;
        }
        case state::PARTITION_END: {
            _state = state::FINISHED;
            co_return end_of_partition();
        }
        case state::FINISHED:
            co_return temporary_buffer<char>();
        }
    }

    virtual future<temporary_buffer<char>> skip(uint64_t n) override {
        // Skipping is implemented by checking the index.
        on_internal_error(sstlog, "partition_reversing_data_source does not support skipping");
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }

    // Points to the current position of the source over the sstable file, which
    // is either the end of partition or the beginning of some row.
    // Can only decrease.
    const uint64_t& current_position_in_sstable() const {
        return _row_start;
    }
};

partition_reversing_data_source make_partition_reversing_data_source(const schema& s, shared_sstable sst, index_reader& ir, uint64_t pos, size_t len,
                                                          reader_permit permit, const io_priority_class& io_priority, tracing::trace_state_ptr trace_state) {
    auto source_impl = std::make_unique<partition_reversing_data_source_impl>(
            s, std::move(sst), ir, pos, len, std::move(permit), io_priority, trace_state);
    auto& curr_pos = source_impl->current_position_in_sstable();
    return partition_reversing_data_source {
        .the_source = seastar::data_source{std::move(source_impl)},
        .current_position_in_sstable = curr_pos
    };
}

}

}
