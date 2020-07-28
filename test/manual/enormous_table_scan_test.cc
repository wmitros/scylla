/*
 * Copyright (C) 2020 ScyllaDB
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

#include "test/lib/cql_test_env.hh"
#include "test/lib/enormous_table.hh"
#include "test/lib/log.hh"
#include "test/lib/cql_assertions.hh"
#include "transport/messages/result_message.hh"

static lw_shared_ptr<service::pager::paging_state> extract_paging_state(::shared_ptr<cql_transport::messages::result_message> res) {
    auto rows = dynamic_pointer_cast<cql_transport::messages::result_message::rows>(res);
    auto paging_state = rows->rs().get_metadata().paging_state();
    if (!paging_state) {
        return nullptr;
    }
    return make_lw_shared<service::pager::paging_state>(*paging_state);
};

static size_t count_rows_fetched(::shared_ptr<cql_transport::messages::result_message> res) {
    auto rows = dynamic_pointer_cast<cql_transport::messages::result_message::rows>(res);
    return rows->rs().result_set().size();
};

static bool has_more_pages(::shared_ptr<cql_transport::messages::result_message> res) {
    auto rows = dynamic_pointer_cast<cql_transport::messages::result_message::rows>(res);
    return rows->rs().get_metadata().flags().contains(cql3::metadata::flag::HAS_MORE_PAGES);
};

SEASTAR_TEST_CASE(scan_enormous_table_test) {
    return do_with_cql_env_thread([] (cql_test_env& e) {
        e.create_table([](std::string_view ks_name) {
            return schema({}, ks_name, "enormous_table",
                {{"pk", long_type}}, {{"ck", long_type}},
                {}, {}, utf8_type, "a very big table (4.5 billion entries, one partition)");
        }).get();
        auto& db = e.local_db();
        db.find_column_family("ks", "enormous_table").set_virtual_reader(mutation_source(enormous_table::virtual_reader()));

        uint64_t rows_fetched;
        shared_ptr<cql_transport::messages::result_message> msg;
        lw_shared_ptr<service::pager::paging_state> paging_state;
        std::unique_ptr<cql3::query_options> qo;
        uint64_t fetched_rows_log_counter = 1e7;
        do {
            qo = std::make_unique<cql3::query_options>(db::consistency_level::LOCAL_ONE, infinite_timeout_config, std::vector<cql3::raw_value>{},
                    cql3::query_options::specific_options{10000, paging_state, {}, api::new_timestamp()});
            msg = e.execute_cql("select * from enormous_table;", std::move(qo)).get0();
            rows_fetched += count_rows_fetched(msg);
            paging_state = extract_paging_state(msg);
            if (rows_fetched >= fetched_rows_log_counter){
                testlog.info("Fetched {} rows", rows_fetched);
                fetched_rows_log_counter += 1e7;
            }
        } while(has_more_pages(msg));
        BOOST_REQUIRE_EQUAL(rows_fetched, enormous_table::enormous_table_reader::CLUSTERING_ROW_COUNT);
    });
}

SEASTAR_TEST_CASE(count_enormous_table_test) {
    return do_with_cql_env_thread([] (cql_test_env& e) {
        e.create_table([](std::string_view ks_name) {
            return schema({}, ks_name, "enormous_table",
                {{"pk", long_type}}, {{"ck", long_type}},
                {}, {}, utf8_type, "a very big table (4.5 billion entries, one partition)");
        }).get();
        auto& db = e.local_db();
        db.find_column_family("ks", "enormous_table").set_virtual_reader(mutation_source(enormous_table::virtual_reader()));

        auto msg = e.execute_cql("select count(*) from enormous_table").get0();
        assert_that(msg).is_rows().with_rows({{{long_type->decompose(int64_t(enormous_table::enormous_table_reader::CLUSTERING_ROW_COUNT))}}});
    });
}
