#
# Copyright (C) 2022-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#
"""
Test UDAs across restart.
"""
import sys
import pytest


# Import fixtures from topology
sys.path.insert(1, sys.path[0] + '/..')
from topology.conftest import manager, pytest_addoption

# Use the util.py library from ../cql-pytest:
sys.path.insert(1, sys.path[0] + '/../cql-pytest')
from util import new_test_keyspace, new_test_table, new_function, new_aggregate, unique_name


@pytest.mark.asyncio
async def test_uda_restart(manager):
    """Test UDA survives server restart"""
    servers = await manager.servers()
    schema = 'id bigint primary key'
    keyspace = unique_name()
    manager.cql.execute("CREATE KEYSPACE " + keyspace + " " + "WITH REPLICATION = { 'class' : 'SimpleStrategy', 'replication_factor' : 1 }")

    table = keyspace + "." + unique_name()
    manager.cql.execute("CREATE TABLE " + table + "(" + schema + ")")
    for i in range(8):
        manager.cql.execute(f"INSERT INTO {table} (id) VALUES ({10**i})")

    avg_partial = unique_name()
    manager.cql.execute(f"CREATE FUNCTION {keyspace}.{avg_partial} (state tuple<bigint, bigint>, val bigint) CALLED ON NULL INPUT RETURNS tuple<bigint, bigint> LANGUAGE lua AS 'return {{state[1] + val, state[2] + 1}}'")

    div_fun = unique_name()
    manager.cql.execute(f"CREATE FUNCTION {keyspace}.{div_fun} (state tuple<bigint, bigint>) CALLED ON NULL INPUT RETURNS bigint LANGUAGE lua AS 'return state[1]//state[2]'")

    custom_avg = unique_name()
    manager.cql.execute(f"CREATE AGGREGATE {keyspace}.{custom_avg} (bigint) SFUNC {avg_partial} STYPE tuple<bigint, bigint> FINALFUNC {div_fun} INITCOND (0,0)")

    manager.driver_close()
    await manager.server_restart(servers[0]) # Restart server before using aggregate
    await manager.driver_connect()

    custom_res = [row for row in manager.cql.execute(f"SELECT {keyspace}.{custom_avg}(id) AS result FROM {table}")]
    avg_res = [row for row in manager.cql.execute(f"SELECT avg(id) AS result FROM {table}")]
    assert custom_res == avg_res

    manager.cql.execute(f"DROP AGGREGATE {keyspace}.{custom_avg}")
    manager.cql.execute(f"DROP FUNCTION {keyspace}.{div_fun}")
    manager.cql.execute(f"DROP FUNCTION {keyspace}.{avg_partial}")
    manager.cql.execute("DROP TABLE " + table)
    manager.cql.execute("DROP KEYSPACE " + keyspace)
