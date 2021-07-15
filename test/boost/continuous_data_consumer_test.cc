/*
 * Copyright (C) 2018-present ScyllaDB
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

#include "vint-serialization.hh"
#include "sstables/consumer.hh"

#include "bytes.hh"
#include "utils/buffer_input_stream.hh"
#include "test/lib/reader_permit.hh"
#include "test/lib/random_utils.hh"

#include <boost/test/unit_test.hpp>
#include <seastar/core/iostream.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <random>

namespace {

class test_consumer final : public data_consumer::continuous_data_consumer<test_consumer> {
    static const int MULTIPLIER = 10;
    uint64_t _tested_value;
    int _state = 0;
    int _count = 0;

    void check(uint64_t got) {
        BOOST_REQUIRE_EQUAL(_tested_value, got);
    }

    static uint64_t calculate_length(uint64_t tested_value) {
        return MULTIPLIER * unsigned_vint::serialized_size(tested_value);
    }

    static input_stream<char> prepare_stream(uint64_t tested_value) {
        temporary_buffer<char> buf(calculate_length(tested_value));
        int pos = 0;
        bytes::value_type* out = reinterpret_cast<bytes::value_type*>(buf.get_write());
        for (int i = 0; i < MULTIPLIER; ++i) {
            pos += unsigned_vint::serialize(tested_value, out + pos);
        }
        return make_buffer_input_stream(std::move(buf), [] {return 1;});
    }

public:
    test_consumer(uint64_t tested_value)
        : continuous_data_consumer(tests::make_permit(), prepare_stream(tested_value), 0, calculate_length(tested_value))
        , _tested_value(tested_value)
    { }

    bool non_consuming() { return false; }

    void verify_end_state() {}

    data_consumer::processing_result process_state(temporary_buffer<char>& data) {
        switch (_state) {
        case 0:
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = 1;
                break;
            }
            // fall-through
        case 1:
            check(_u64);
            ++_count;
            _state = _count < MULTIPLIER ? 0 : 2;
            break;
        default:
            BOOST_FAIL("wrong consumer state");
        }
        return _state == 2 ? data_consumer::proceed::no : data_consumer::proceed::yes;
    }

    void run() {
        consume_input().get();
    }
};

}

SEASTAR_THREAD_TEST_CASE(test_read_unsigned_vint) {
    auto nr_tests =
#ifdef SEASTAR_DEBUG
            10
#else
            1000
#endif
            ;
    test_consumer(0).run();
    for (int highest_bit = 0; highest_bit < 64; ++highest_bit) {
        uint64_t tested_value = uint64_t{1} << highest_bit;
        for (int i = 0; i < nr_tests; ++i) {
            test_consumer(tested_value + tests::random::get_int<uint64_t>(tested_value - 1)).run();
        }
    }
}

class skipping_consumer final : public data_consumer::continuous_data_consumer<skipping_consumer> {
    int _initial_data_size;
    int _to_skip;
    int _next_data_size;

    enum class state {
        INITIAL_BYTES,
        INITIAL_BYTES_READ_FINISH,
        NEXT_BYTES,
        NEXT_BYTES_READ_FINISH
    } _state = state::INITIAL_BYTES;

    // stream starting with initial_data_size 'a's, followed by to_skip 'b's,
    // ending with next_data_size 'a's, returning one byte at a time
    static input_stream<char> prepare_stream(int initial_data_size, int to_skip, int next_data_size) {
        temporary_buffer<char> buf(initial_data_size + to_skip + next_data_size);
        std::memset(buf.get_write(), 'a', initial_data_size);
        std::memset(buf.get_write() + initial_data_size, 'b', to_skip);
        std::memset(buf.get_write() + initial_data_size + to_skip, 'a', next_data_size);
        return make_buffer_input_stream(std::move(buf), [] {return 1;});
    }

public:
    skipping_consumer(int initial_data_size, int to_skip, int next_data_size)
        : continuous_data_consumer(tests::make_permit(), prepare_stream(initial_data_size, to_skip, next_data_size),
                                    0, initial_data_size + 1 + tests::random::get_int<int>(to_skip - 1))
        , _initial_data_size(initial_data_size)
        , _to_skip(to_skip)
        , _next_data_size(next_data_size)
    { }

    bool non_consuming() { return false; }

    void verify_end_state() {}

    data_consumer::processing_result process_state(temporary_buffer<char>& data) {
        switch (_state) {
        case state::INITIAL_BYTES:
            if (read_8(data) != read_status::ready) {
                _state = state::INITIAL_BYTES_READ_FINISH;
                break;
            }
            [[fallthrough]];
        case state::INITIAL_BYTES_READ_FINISH:
            if (_u8 != 'a') {
                BOOST_FAIL("wrong data read");
            }
            if (--_initial_data_size == 0) {
                _state = state::NEXT_BYTES;
                assert(data.empty());
                return skip_bytes{_to_skip};
            }
            _state = state::INITIAL_BYTES;
            break;
        case state::NEXT_BYTES:
            if (read_8(data) != read_status::ready) {
                _state = state::NEXT_BYTES_READ_FINISH;
                break;
            }
            [[fallthrough]];
        case state::NEXT_BYTES_READ_FINISH:
            if (_u8 != 'a') {
                BOOST_FAIL("wrong data read");
            }
            if (--_next_data_size == 0) {
                return data_consumer::proceed::no;
            }
            _state = state::NEXT_BYTES;
            break;
        }
        return data_consumer::proceed::yes;
    }

    void run() {
        consume_input().get();
    }
};

// Make sure that we can correctly fast forward to the next position with useful data,
// while the previous consumer range was ending with bytes that we want to skip
SEASTAR_THREAD_TEST_CASE(test_skip_at_end) {
    for (int initial_data_size = 1; initial_data_size <= 10; initial_data_size++) {
        for (int to_skip = 1; to_skip <= 10; to_skip++) {
            for (int next_data_size = 1; next_data_size <= 10; next_data_size++) {
                skipping_consumer consumer(initial_data_size, to_skip, next_data_size);
                consumer.run();
                consumer.fast_forward_to(initial_data_size + to_skip, initial_data_size + to_skip + next_data_size).get();
                consumer.run();
            }
        }
    }
}
