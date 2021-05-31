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
    struct promise_type {
        using handle_type = SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<promise_type>;
        proceed_generator get_return_object() {
            return proceed_generator{handle_type::from_promise(*this)};
        }
        // the coroutine doesn't start running before the first handle::resume() call
        static SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_always initial_suspend() noexcept {
            return {}; 
        }
        static SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_always final_suspend() noexcept { 
            return {}; 
        }
        SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_always yield_value(data_consumer::processing_result value) noexcept {
            current_value = std::move(value);
            return {};
        }
        // Disallow co_await in generator coroutines.
        void await_transform() = delete;
        [[noreturn]]
        static void unhandled_exception() {
            throw;
        }
        void return_void() noexcept {}

        std::optional<data_consumer::processing_result> current_value;
    };
private:
    SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<promise_type> m_coroutine;
public: 
    explicit proceed_generator(const SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<promise_type> coroutine) : 
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
        auto ret = *m_coroutine.promise().current_value;
        m_coroutine.promise().current_value = std::nullopt;
        return ret;
    }
};

template<typename... Args>
struct SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_traits<proceed_generator, Args...> {
    using promise_type = proceed_generator::promise_type;
};