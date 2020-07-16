/*
 * Copyright (C) 2017 ScyllaDB
 *
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

#include "table_helper.hh"
#include "cql3/query_processor.hh"
#include "cql3/statements/alter_table_statement.hh"
#include "cql3/statements/create_table_statement.hh"
#include "cql3/statements/modification_statement.hh"
#include "database.hh"

future<> table_helper::setup_table() const {
    auto& qp = cql3::get_local_query_processor();
    auto& db = qp.db();

    if (db.has_schema(_keyspace, _name)) {
        if (_alter_cql) {
            auto parsed = cql3::query_processor::parse_statement(_alter_cql.value());
            cql3::statements::raw::cf_statement* parsed_cf_stmt = static_cast<cql3::statements::raw::cf_statement*>(parsed.get());
            parsed_cf_stmt->prepare_keyspace(_keyspace);
            ::shared_ptr<cql3::statements::alter_table_statement> statement =
                            static_pointer_cast<cql3::statements::alter_table_statement>(
                                            parsed_cf_stmt->prepare(db, qp.get_cql_stats())->statement);

            // Instead of checking if the alteration is possible, we try it. If it fails, the change has already been done
            return do_with(statement, [this] (auto& stmt) {
                return stmt->announce_migration(service::get_storage_proxy().local(), false);
            }).discard_result().handle_exception([this] (auto ep) {
            });
        } else {
            return make_ready_future<>();
        }
    }

    auto parsed = cql3::query_processor::parse_statement(_create_cql);

    cql3::statements::raw::cf_statement* parsed_cf_stmt = static_cast<cql3::statements::raw::cf_statement*>(parsed.get());
    parsed_cf_stmt->prepare_keyspace(_keyspace);
    ::shared_ptr<cql3::statements::create_table_statement> statement =
                    static_pointer_cast<cql3::statements::create_table_statement>(
                                    parsed_cf_stmt->prepare(db, qp.get_cql_stats())->statement);
    auto schema = statement->get_cf_meta_data(db);

    // Generate the CF UUID based on its KF names. This is needed to ensure that
    // all Nodes that create it would create it with the same UUID and we don't
    // hit the #420 issue.
    auto uuid = generate_legacy_id(schema->ks_name(), schema->cf_name());

    schema_builder b(schema);
    b.set_uuid(uuid);

    // We don't care it it fails really - this may happen due to concurrent
    // "CREATE TABLE" invocation on different Nodes.
    // The important thing is that it will converge eventually (some traces may
    // be lost in a process but that's ok).
    return service::get_local_migration_manager().announce_new_column_family(b.build(), false).discard_result().handle_exception([this] (auto ep) {});;
}

future<> table_helper::cache_table_info(service::query_state& qs) {
    if (_prepared_stmt) {
        return now();
    } else {
        // if prepared statement has been invalidated - drop cached pointers
        _insert_stmt = nullptr;
    }

    return cql3::get_local_query_processor().prepare(_insert_cql, qs.get_client_state(), false).then([this] (shared_ptr<cql_transport::messages::result_message::prepared> msg_ptr) {
        _prepared_stmt = std::move(msg_ptr->get_prepared());
        shared_ptr<cql3::cql_statement> cql_stmt = _prepared_stmt->statement;
        _insert_stmt = dynamic_pointer_cast<cql3::statements::modification_statement>(cql_stmt);
    }).handle_exception([this] (auto eptr) {
        // One of the possible causes for an error here could be the table that doesn't exist.
        //FIXME: discarded future.
        (void)this->setup_table().discard_result();

        // We throw the bad_column_family exception because the caller
        // expects and accounts this type of errors.
        try {
            std::rethrow_exception(eptr);
        } catch (std::exception& e) {
            throw bad_column_family(_keyspace, _name, e);
        } catch (...) {
            throw bad_column_family(_keyspace, _name);
        }
    });
}

future<> table_helper::insert(service::query_state& qs, noncopyable_function<cql3::query_options ()> opt_maker) {
    return cache_table_info(qs).then([this, &qs, opt_maker = std::move(opt_maker)] () mutable {
        return do_with(opt_maker(), [this, &qs] (auto& opts) {
            return _insert_stmt->execute(service::get_storage_proxy().local(), qs, opts);
        });
    }).discard_result();
}

future<> table_helper::setup_keyspace(const sstring& keyspace_name, sstring replication_factor, service::query_state& qs, std::vector<table_helper*> tables) {
    if (this_shard_id() == 0) {
        size_t n = tables.size();
        for (size_t i = 0; i < n; ++i) {
            if (tables[i]->_keyspace != keyspace_name) {
                throw std::invalid_argument("setup_keyspace called with table_helper for different keyspace");
            }
        }
        return seastar::async([&keyspace_name, replication_factor, &qs, tables] {
            auto& db = cql3::get_local_query_processor().db();

            // Create a keyspace
            if (!db.has_keyspace(keyspace_name)) {
                std::map<sstring, sstring> opts;
                opts["replication_factor"] = replication_factor;
                auto ksm = keyspace_metadata::new_keyspace(keyspace_name, "org.apache.cassandra.locator.SimpleStrategy", std::move(opts), true);
                // We use min_timestamp so that default keyspace metadata will loose with any manual adjustments. See issue #2129.
                service::get_local_migration_manager().announce_new_keyspace(ksm, api::min_timestamp, false).get();
            }

            qs.get_client_state().set_keyspace(cql3::get_local_query_processor().db(), keyspace_name);


            // Create tables
            size_t n = tables.size();
            for (size_t i = 0; i < n; ++i) {
                tables[i]->setup_table().get();
            }
        });
    } else {
        return make_ready_future<>();
    }
}
