#
# Copyright (C) 2024-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#
import asyncio
import pytest
import time
from test.topology.conftest import skip_mode
from test.pylib.manager_client import ManagerClient

from cassandra.cluster import ConsistencyLevel  # type: ignore
from cassandra.query import SimpleStatement  # type: ignore


async def wait_for_view(cql, name, node_count):
    deadline = time.time() + 120
    while time.time() < deadline:
        done = await cql.run_async(f"SELECT COUNT(*) FROM system_distributed.view_build_status WHERE status = 'SUCCESS' AND view_name = '{name}' ALLOW FILTERING")
        if done[0][0] == node_count:
            return
        else:
            time.sleep(0.2)
    raise Exception("Timeout waiting for views to build")


# This test makes sure that even if the view building encounter errors, the view building is eventually finished
# and the view is consistent with the base table.
# Reproduces the scenario in #19261
@pytest.mark.asyncio
@skip_mode('release', "error injections aren't enabled in release mode")
async def test_mv_fail_building(manager: ManagerClient) -> None:
    node_count = 4
    servers = await manager.servers_add(node_count, config={'hinted_handoff_enabled': 'false'})#'error_injections_at_startup': ['view_update_limit']})
    cql = manager.get_cql()

    await cql.run_async(f"CREATE KEYSPACE ks WITH replication = {{'class': 'SimpleStrategy', 'replication_factor': 3}}")
    await cql.run_async(f"CREATE TABLE ks.tab (key int, c int, value text, PRIMARY KEY (key, c))")

    await cql.run_async(f"CREATE MATERIALIZED VIEW ks.mv_cf_view AS SELECT * FROM ks.tab "
                    "WHERE c IS NOT NULL and key IS NOT NULL PRIMARY KEY (c, key) ")
    await wait_for_view(cql, "mv_cf_view", node_count)

    for i in range(1000):
        await cql.run_async(f" INSERT INTO ks.tab (key, c, value) VALUES ({i}, {i+1}, '{'a'}')")
    await manager.server_stop(servers[-1].server_id)
    cql, hosts = await manager.get_ready_cql(servers[:-1])
    await manager.server_update_cmdline(servers[-1].server_id, ['--smp=3'])
    fut = manager.server_start(servers[-1].server_id)

    for i in range(1001,2000):
        await cql.run_async(f"INSERT INTO ks.tab (key, c, value) VALUES ({i}, {i+1}, 'a') ")
        time.sleep(0.1)
    await fut

    await cql.run_async(f"DROP KEYSPACE ks")
