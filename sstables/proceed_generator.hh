/*
 * Copyright (C) 2021 ScyllaDB
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

#include <seastar/core/coroutine.hh>
#include "sstables/consumer.hh"

class proceed_generator {
public:
    struct promise_type;
    struct read_awaiter {
        data_consumer::read_status _rs;
        read_awaiter(data_consumer::read_status rs) : _rs(rs) {}
        constexpr bool await_ready() const noexcept { return _rs == data_consumer::read_status::ready; }
        constexpr void await_suspend(std::experimental::coroutine_handle<promise_type>) const noexcept {}
        constexpr void await_resume() const noexcept {}
    };
    struct promise_type {
        using handle_type = std::experimental::coroutine_handle<promise_type>;
        proceed_generator get_return_object() {
            return proceed_generator{handle_type::from_promise(*this)};
        }
        // the coroutine doesn't start running until the first handle::resume() call
        static std::experimental::suspend_always initial_suspend() noexcept {
            return {};
        }
        static std::experimental::suspend_always final_suspend() noexcept {
            return {};
        }
        std::experimental::suspend_always yield_value(data_consumer::processing_result value) noexcept {
            current_value = std::move(value);
            return {};
        }
        read_awaiter yield_value(data_consumer::read_status rs) noexcept {
            if (rs == data_consumer::read_status::waiting) {
                current_value = data_consumer::proceed::yes;
            }
            return read_awaiter(rs);
        }
        // Disallow co_await in generator coroutines.
        void await_transform() = delete;

        void unhandled_exception() {
            _ex = std::current_exception();
        }
        void return_void() noexcept {}

        std::optional<data_consumer::processing_result> current_value;
        std::exception_ptr _ex;
    };
private:
    std::experimental::coroutine_handle<promise_type> m_coroutine;
public:
    explicit proceed_generator(const std::experimental::coroutine_handle<promise_type> coroutine) :
        m_coroutine{coroutine}
    {}

    proceed_generator() = default;
    ~proceed_generator() {
        if (m_coroutine) {
            m_coroutine.destroy();
        }
    }

    proceed_generator(const proceed_generator&) = delete;
    proceed_generator& operator=(const proceed_generator&) = delete;

    proceed_generator(proceed_generator&& other) noexcept :
        m_coroutine{other.m_coroutine}
    {
        other.m_coroutine = {};
    }
    proceed_generator& operator=(proceed_generator&& other) noexcept {
        if (this != &other) {
            if (m_coroutine) {
                m_coroutine.destroy();
            }
            m_coroutine = other.m_coroutine;
            other.m_coroutine = {};
        }
        return *this;
    }
    data_consumer::processing_result generate() {
        m_coroutine();
        if (m_coroutine.promise()._ex) {
            std::rethrow_exception(m_coroutine.promise()._ex);
        }
        return *std::exchange(m_coroutine.promise().current_value, std::nullopt);
    }
};

template<typename... Args>
struct std::experimental::coroutine_traits<proceed_generator, Args...> {
    using promise_type = proceed_generator::promise_type;
};
