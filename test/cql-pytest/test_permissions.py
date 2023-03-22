# Copyright 2021-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later

#############################################################################
# Tests for managing permissions
#############################################################################

import pytest
import time
from cassandra.protocol import SyntaxException, InvalidRequest, Unauthorized
from util import new_test_table, new_function, new_user, new_session, new_test_keyspace, unique_name, new_type

# Test that granting permissions to various resources works for the default user.
# This case does not include functions, because due to differences in implementation
# the tests will diverge between Scylla and Cassandra (e.g. there's no common language)
# to create a user-defined function in.
# Marked cassandra_bug, because Cassandra allows granting a DESCRIBE permission
# to a specific ROLE, in contradiction to its own documentation:
# https://cassandra.apache.org/doc/latest/cassandra/cql/security.html#cql-permissions
def test_grant_applicable_data_and_role_permissions(cql, test_keyspace, cassandra_bug):
    schema = "a int primary key"
    user = "cassandra"
    with new_test_table(cql, test_keyspace, schema) as table:
        # EXECUTE is not listed, as it only applies to functions, which aren't covered in this test case
        all_permissions = set(['create', 'alter', 'drop', 'select', 'modify', 'authorize', 'describe'])
        applicable_permissions = {
            'all keyspaces': ['create', 'alter', 'drop', 'select', 'modify', 'authorize'],
            f'keyspace {test_keyspace}': ['create', 'alter', 'drop', 'select', 'modify', 'authorize'],
            f'table {table}': ['alter', 'drop', 'select', 'modify', 'authorize'],
            'all roles': ['create', 'alter', 'drop', 'authorize', 'describe'],
            f'role {user}': ['alter', 'drop', 'authorize'],
        }
        for resource, permissions in applicable_permissions.items():
            # Applicable permissions can be legally granted
            for permission in permissions:
                cql.execute(f"GRANT {permission} ON {resource} TO {user}")
            # Only applicable permissions can be granted - nonsensical combinations
            # are refused with an error
            for permission in all_permissions.difference(set(permissions)):
                with pytest.raises((InvalidRequest, SyntaxException), match="support.*permissions"):
                    cql.execute(f"GRANT {permission} ON {resource} TO {user}")


def eventually_authorized(fun, timeout_s=10):
    for i in range(timeout_s * 10):
        try:
            return fun()
        except Unauthorized as e:
            time.sleep(0.1)
    return fun()

def eventually_unauthorized(fun, timeout_s=10):
    for i in range(timeout_s * 10):
        try:
            fun()
            time.sleep(0.1)
        except Unauthorized as e:
            return
    try:
        fun()
        pytest.fail(f"Function {fun} was not refused as unauthorized")
    except Unauthorized as e:
        return

def grant(cql, permission, resource, username):
    cql.execute(f"GRANT {permission} ON {resource} TO {username}")

def revoke(cql, permission, resource, username):
    cql.execute(f"REVOKE {permission} ON {resource} FROM {username}")

# Helper function for checking that given `function` can only be executed
# with given `permission` granted, and returns an unauthorized error
# with the `permission` revoked.
def check_enforced(cql, username, permission, resource, function):
    eventually_unauthorized(function)
    grant(cql, permission, resource, username)
    eventually_authorized(function)
    revoke(cql, permission, resource, username)
    eventually_unauthorized(function)

# Test that data permissions can be granted and revoked, and that they're effective
def test_grant_revoke_data_permissions(cql, test_keyspace):
    with new_user(cql) as username:
        with new_session(cql, username) as user_session:
            ks = unique_name()
            # Permissions on all keyspaces
            def create_keyspace_idempotent():
                user_session.execute(f"CREATE KEYSPACE IF NOT EXISTS {ks} WITH REPLICATION = {{ 'class' : 'SimpleStrategy', 'replication_factor' : 1 }}")
                user_session.execute(f"DROP KEYSPACE IF EXISTS {ks}")
            check_enforced(cql, username, permission='CREATE', resource='ALL KEYSPACES', function=create_keyspace_idempotent)
            # Permissions for a specific keyspace
            with new_test_keyspace(cql, "WITH REPLICATION = { 'class' : 'SimpleStrategy', 'replication_factor' : 1 }") as keyspace:
                t = unique_name()
                def create_table_idempotent():
                    user_session.execute(f"CREATE TABLE IF NOT EXISTS {keyspace}.{t}(id int primary key)")
                    cql.execute(f"DROP TABLE IF EXISTS {keyspace}.{t}")
                eventually_unauthorized(create_table_idempotent)
                grant(cql, 'CREATE', f'KEYSPACE {keyspace}', username)
                eventually_authorized(create_table_idempotent)
                cql.execute(f"CREATE TABLE {keyspace}.{t}(id int primary key)")
                # Permissions for a specific table
                check_enforced(cql, username, permission='ALTER', resource=f'{keyspace}.{t}',
                        function=lambda: user_session.execute(f"ALTER TABLE {keyspace}.{t} WITH comment = 'hey'"))
                check_enforced(cql, username, permission='SELECT', resource=f'{keyspace}.{t}',
                        function=lambda: user_session.execute(f"SELECT * FROM {keyspace}.{t}"))
                check_enforced(cql, username, permission='MODIFY', resource=f'{keyspace}.{t}',
                        function=lambda: user_session.execute(f"INSERT INTO {keyspace}.{t}(id) VALUES (42)"))
                cql.execute(f"DROP TABLE {keyspace}.{t}")
                revoke(cql, 'CREATE', f'KEYSPACE {keyspace}', username)
                eventually_unauthorized(create_table_idempotent)

                def drop_table_idempotent():
                    user_session.execute(f"DROP TABLE IF EXISTS {keyspace}.{t}")
                    cql.execute(f"CREATE TABLE IF NOT EXISTS {keyspace}.{t}(id int primary key)")

                cql.execute(f"CREATE TABLE {keyspace}.{t}(id int primary key)")
                check_enforced(cql, username, permission='DROP', resource=f'KEYSPACE {keyspace}', function=drop_table_idempotent)
                cql.execute(f"DROP TABLE {keyspace}.{t}")

                # CREATE permission on all keyspaces also implies creating any tables in any keyspace
                check_enforced(cql, username, permission='CREATE', resource='ALL KEYSPACES', function=create_table_idempotent)
                # Same for DROP
                cql.execute(f"CREATE TABLE IF NOT EXISTS {keyspace}.{t}(id int primary key)")
                check_enforced(cql, username, permission='DROP', resource='ALL KEYSPACES', function=drop_table_idempotent)

# Test that permissions for user-defined functions are serialized in a Cassandra-compatible way
def test_udf_permissions_serialization(cql):
    schema = "a int primary key"
    user = "cassandra"
    with new_test_keyspace(cql, "WITH REPLICATION = { 'class': 'SimpleStrategy', 'replication_factor': 1 }") as keyspace:
        with new_test_table(cql, keyspace, schema) as table:
            # Creating a bilingual function makes this test case work for both Scylla and Cassandra
            div_body_lua = "(b bigint, i int) CALLED ON NULL INPUT RETURNS bigint LANGUAGE lua AS 'return b//i'"
            div_body_java = "(b bigint, i int) CALLED ON NULL INPUT RETURNS bigint LANGUAGE java AS 'return b/i;'"
            div_body = div_body_lua
            try:
                with new_function(cql, keyspace, div_body) as div_fun:
                    pass
            except:
                div_body = div_body_java
            with new_function(cql, keyspace, div_body) as div_fun:
                applicable_permissions = {
                    'all functions': ['create', 'alter', 'drop', 'authorize', 'execute'],
                    f'all functions in keyspace {keyspace}': ['create', 'alter', 'drop', 'authorize', 'execute'],
                    f'function {keyspace}.{div_fun}(bigint, int)': ['alter', 'drop', 'authorize', 'execute'],
                }
                for resource, permissions in applicable_permissions.items():
                    # Applicable permissions can be legally granted
                    for permission in permissions:
                        cql.execute(f"GRANT {permission} ON {resource} TO {user}")

                permissions = {row.resource: row.permissions for row in cql.execute(f"SELECT * FROM system_auth.role_permissions")}
                assert permissions['functions'] == set(['ALTER', 'AUTHORIZE', 'CREATE', 'DROP', 'EXECUTE'])
                assert permissions[f'functions/{keyspace}'] == set(['ALTER', 'AUTHORIZE', 'CREATE', 'DROP', 'EXECUTE'])
                assert permissions[f'functions/{keyspace}/{div_fun}[org.apache.cassandra.db.marshal.LongType^org.apache.cassandra.db.marshal.Int32Type]'] == set(['ALTER', 'AUTHORIZE', 'DROP', 'EXECUTE'])

                resources_with_execute = [row.resource for row in cql.execute(f"LIST EXECUTE OF {user}")]
                assert '<all functions>' in resources_with_execute
                assert f'<all functions in {keyspace}>' in resources_with_execute
                assert f'<function {keyspace}.{div_fun}(bigint, int)>' in resources_with_execute

# Test that names that require quoting (e.g. due to having nonorthodox characters)
# are properly handled, with right permissions granted.
# Cassandra doesn't quote names properly, so the test fails
def test_udf_permissions_quoted_names(cassandra_bug, cql):
    udt_name = unique_name() + '"weird_udt[t^t]a^b^[]"'
    schema = f"a frozen<{udt_name}> primary key"
    with new_test_keyspace(cql, "WITH REPLICATION = { 'class': 'SimpleStrategy', 'replication_factor': 1 }") as keyspace:
        with new_type(cql, keyspace, "(a text, b int)", udt_name) as udt, new_test_table(cql, keyspace, schema) as table:
            fun_body_lua = f"(i {udt}) CALLED ON NULL INPUT RETURNS bigint LANGUAGE lua AS 'return 42;'"
            fun_body_java = f"(i {udt}) CALLED ON NULL INPUT RETURNS bigint LANGUAGE java AS 'return 42;'"
            fun_body = fun_body_lua
            try:
                with new_function(cql, keyspace, fun_body):
                    pass
            except:
                fun_body = fun_body_java
            with new_function(cql, keyspace, fun_body, unique_name() + '"weird[name1^name2]x^y"') as weird_fun:
                with new_user(cql) as username:
                    with new_session(cql, username) as user_session:
                        grant(cql, 'EXECUTE', f'FUNCTION {keyspace}.{weird_fun}({udt})', username)
                        grant(cql, 'SELECT', table, username)
                        cql.execute(f"INSERT INTO {table}(a) VALUES ({{a:'hello', b:42}})")

                        assert list([r[0] for r in user_session.execute(f"SELECT {keyspace}.{weird_fun}(a) FROM {table}")]) == [42]

                        resources_with_execute = [row.resource for row in cql.execute(f"LIST EXECUTE OF {username}")]
                        assert f'<function {keyspace}.{weird_fun}(frozen<{udt_name}>)>' in resources_with_execute

                        revoke(cql, 'EXECUTE', f'FUNCTION {keyspace}.{weird_fun}({udt})', username)
                        check_enforced(cql, username, 'EXECUTE', f'FUNCTION {keyspace}.{weird_fun}({udt})',
                                lambda: user_session.execute(f"SELECT {keyspace}.{weird_fun}(a) FROM {table}"))

# Test that dropping a function without specifying its signature works with the DROP permission if there's only
# one function with the given name, and that it fails if there are multiple functions with the same name,
# regardless of the permissions of the user.
# If the signature is specified, test that the permission check is performed as usual.
def test_drop_udf_with_same_name(cql):
    schema = "a int primary key"
    with new_test_keyspace(cql, "WITH REPLICATION = { 'class': 'SimpleStrategy', 'replication_factor': 1 }") as keyspace:
        body1_lua = "(i int) CALLED ON NULL INPUT RETURNS bigint LANGUAGE lua AS 'return 42;'"
        body1_java = "(i int) CALLED ON NULL INPUT RETURNS bigint LANGUAGE java AS 'return 42;'"
        body2_lua = "(i int, j int) CALLED ON NULL INPUT RETURNS bigint LANGUAGE lua AS 'return 42;'"
        body2_java = "(i int, j int) CALLED ON NULL INPUT RETURNS bigint LANGUAGE java AS 'return 42;'"
        body1 = body1_lua
        body2 = body2_lua
        try:
            with new_function(cql, keyspace, body1):
                pass
        except:
            body1 = body1_java
            body2 = body2_java
        fun = "fun43"
        cql.execute(f"CREATE FUNCTION {keyspace}.{fun}{body1}")
        cql.execute(f"CREATE FUNCTION {keyspace}.{fun}{body2}")
        with new_user(cql) as username:
            with new_session(cql, username) as user_session:
                grant(cql, 'DROP', f'FUNCTION {keyspace}.{fun}(int)', username)
                with pytest.raises(InvalidRequest):
                    user_session.execute(f"DROP FUNCTION {keyspace}.{fun}")
                eventually_unauthorized(lambda: user_session.execute(f"DROP FUNCTION {keyspace}.{fun}(int, int)"))
                grant(cql, 'DROP', f'FUNCTION {keyspace}.{fun}(int, int)', username)
                with pytest.raises(InvalidRequest):
                    user_session.execute(f"DROP FUNCTION {keyspace}.{fun}")
                eventually_authorized(lambda: user_session.execute(f"DROP FUNCTION {keyspace}.{fun}(int)"))
                eventually_authorized(lambda: user_session.execute(f"DROP FUNCTION {keyspace}.{fun}"))

# Test that permissions set for user-defined functions are enforced
# Tests for ALTER are separate, because they are qualified as cassandra_bug
def test_grant_revoke_udf_permissions(cql):
    schema = "a int primary key, b list<int>"
    user = "cassandra"
    with new_test_keyspace(cql, "WITH REPLICATION = { 'class': 'SimpleStrategy', 'replication_factor': 1 }") as keyspace:
        with new_test_table(cql, keyspace, schema) as table:
            fun_body_lua = "(i int, l list<int>) CALLED ON NULL INPUT RETURNS int LANGUAGE lua AS 'return 42;'"
            fun_body_java = "(i int, l list<int>) CALLED ON NULL INPUT RETURNS int LANGUAGE java AS 'return 42;'"
            fun_body = fun_body_lua
            try:
                with new_function(cql, keyspace, fun_body) as fun:
                    pass
            except:
                fun_body = fun_body_java
            with new_user(cql) as username:
                with new_session(cql, username) as user_session:
                    fun = "fun42"

                    def create_function_idempotent():
                        user_session.execute(f"CREATE FUNCTION IF NOT EXISTS {keyspace}.{fun} {fun_body}")
                        cql.execute(f"DROP FUNCTION IF EXISTS {keyspace}.{fun}(int, list<int>)")
                    check_enforced(cql, username, permission='CREATE', resource=f'all functions in keyspace {keyspace}',
                            function=create_function_idempotent)
                    check_enforced(cql, username, permission='CREATE', resource='all functions',
                            function=create_function_idempotent)

                    def drop_function_idempotent():
                        user_session.execute(f"DROP FUNCTION IF EXISTS {keyspace}.{fun}(int, list<int>)")
                        cql.execute(f"CREATE FUNCTION IF NOT EXISTS {keyspace}.{fun} {fun_body}")
                    for resource in [f'function {keyspace}.{fun}(int, list<int>)', f'all functions in keyspace {keyspace}', 'all functions']:
                        check_enforced(cql, username, permission='DROP', resource=resource, function=drop_function_idempotent)

                    grant(cql, 'SELECT', table, username)
                    for resource in [f'function {keyspace}.{fun}(int, list<int>)', f'all functions in keyspace {keyspace}', 'all functions']:
                        check_enforced(cql, username, permission='EXECUTE', resource=resource,
                                function=lambda: user_session.execute(f"SELECT {keyspace}.{fun}(a, b) FROM {table}"))

                    grant(cql, 'EXECUTE', 'ALL FUNCTIONS', username)
                    def grant_idempotent():
                        grant(user_session, 'EXECUTE', f'function {keyspace}.{fun}(int, list<int>)', 'cassandra')
                        revoke(cql, 'EXECUTE', f'function {keyspace}.{fun}(int, list<int>)', 'cassandra')
                    for resource in [f'function {keyspace}.{fun}(int, list<int>)', f'all functions in keyspace {keyspace}', 'all functions']:
                        check_enforced(cql, username, permission='AUTHORIZE', resource=resource, function=grant_idempotent)

# This test case is artificially extracted from the one above,
# because it's qualified as cassandra_bug - the documentation quotes that ALTER is needed on
# functions if the definition is replaced (CREATE OR REPLACE FUNCTION (...)),
# and yet it's not enforced
def test_grant_revoke_alter_udf_permissions(cassandra_bug, cql):
    schema = "a int primary key"
    user = "cassandra"
    with new_test_keyspace(cql, "WITH REPLICATION = { 'class': 'SimpleStrategy', 'replication_factor': 1 }") as keyspace:
        with new_test_table(cql, keyspace, schema) as table:
            fun_body_lua = "(i int) CALLED ON NULL INPUT RETURNS int LANGUAGE lua AS 'return 42;'"
            fun_body_java = "(i int) CALLED ON NULL INPUT RETURNS int LANGUAGE java AS 'return 42;'"
            fun_body = fun_body_lua
            try:
                with new_function(cql, keyspace, fun_body) as fun:
                    pass
            except:
                fun_body = fun_body_java
            with new_user(cql) as username:
                with new_session(cql, username) as user_session:
                    fun = "fun42"

                    grant(cql, 'ALTER', 'ALL FUNCTIONS', username)
                    check_enforced(cql, username, permission='CREATE', resource=f'all functions in keyspace {keyspace}',
                            function=lambda: user_session.execute(f"CREATE OR REPLACE FUNCTION {keyspace}.{fun} {fun_body}"))
                    check_enforced(cql, username, permission='CREATE', resource='all functions',
                            function=lambda: user_session.execute(f"CREATE OR REPLACE FUNCTION {keyspace}.{fun} {fun_body}"))
                    revoke(cql, 'ALTER', 'ALL FUNCTIONS', username)

                    grant(cql, 'CREATE', 'ALL FUNCTIONS', username)
                    check_enforced(cql, username, permission='ALTER', resource=f'all functions in keyspace {keyspace}',
                            function=lambda: user_session.execute(f"CREATE OR REPLACE FUNCTION {keyspace}.{fun} {fun_body}"))
                    check_enforced(cql, username, permission='ALTER', resource='all functions',
                            function=lambda: user_session.execute(f"CREATE OR REPLACE FUNCTION {keyspace}.{fun} {fun_body}")) 
                    check_enforced(cql, username, permission='ALTER', resource=f'FUNCTION {keyspace}.{fun}(int)',
                            function=lambda: user_session.execute(f"CREATE OR REPLACE FUNCTION {keyspace}.{fun} {fun_body}"))

# Test that granting permissions on non-existent UDFs fails
def test_grant_perms_on_nonexistent_udf(cql):
    keyspace = "ks"
    fun_name = "fun42"
    with new_user(cql) as username:
        grant(cql, 'EXECUTE', 'ALL FUNCTIONS', username)
        revoke(cql, 'EXECUTE', 'ALL FUNCTIONS', username)
        with pytest.raises(InvalidRequest):
            grant(cql, 'EXECUTE', f'ALL FUNCTIONS IN KEYSPACE {keyspace}', username)
        with pytest.raises(InvalidRequest):
            grant(cql, 'EXECUTE', f'FUNCTION {keyspace}.{fun_name}(int)', username)
        cql.execute(f"CREATE KEYSPACE IF NOT EXISTS {keyspace} WITH REPLICATION = {{ 'class' : 'SimpleStrategy', 'replication_factor' : 1 }}")
        grant(cql, 'EXECUTE', f'ALL FUNCTIONS IN KEYSPACE {keyspace}', username)
        revoke(cql, 'EXECUTE', f'ALL FUNCTIONS IN KEYSPACE {keyspace}', username)
        with pytest.raises(InvalidRequest):
            grant(cql, 'EXECUTE', f'FUNCTION {keyspace}.{fun_name}(int)', username)

        fun_body_lua = "(i int) CALLED ON NULL INPUT RETURNS int LANGUAGE lua AS 'return 42;'"
        fun_body_java = "(i int) CALLED ON NULL INPUT RETURNS int LANGUAGE java AS 'return 42;'"
        fun_body = fun_body_lua
        try:
            with new_function(cql, keyspace, fun_body) as fun:
                pass
        except:
            fun_body = fun_body_java
        with new_function(cql, keyspace, fun_body, fun_name):
            grant(cql, 'EXECUTE', f'FUNCTION {keyspace}.{fun_name}(int)', username)
        cql.execute(f"DROP KEYSPACE IF EXISTS {keyspace}")

# Test that permissions for user-defined aggregates are also enforced.
# scylla_only, because Lua is used as the target language
def test_grant_revoke_uda_permissions(scylla_only, cql):
    schema = 'id bigint primary key'
    with new_test_keyspace(cql, "WITH REPLICATION = { 'class': 'SimpleStrategy', 'replication_factor': 1 }") as keyspace:
        with new_test_table(cql, keyspace, schema) as table:
            for i in range(8):
                cql.execute(f"INSERT INTO {table} (id) VALUES ({10**i})")
            avg_partial_body = "(state tuple<bigint, bigint>, val bigint) CALLED ON NULL INPUT RETURNS tuple<bigint, bigint> LANGUAGE lua AS 'return {state[1] + val, state[2] + 1}'"
            div_body = "(state tuple<bigint, bigint>) CALLED ON NULL INPUT RETURNS bigint LANGUAGE lua AS 'return state[1]//state[2]'"
            with new_function(cql, keyspace, avg_partial_body) as avg_partial, new_function(cql, keyspace, div_body) as div_fun:
                custom_avg_body = f"(bigint) SFUNC {avg_partial} STYPE tuple<bigint, bigint> FINALFUNC {div_fun} INITCOND (0,0)"
                with new_user(cql) as username:
                    with new_session(cql, username) as user_session:
                        custom_avg = "custom_avg"
                        def create_aggr_idempotent():
                            user_session.execute(f"CREATE AGGREGATE IF NOT EXISTS {keyspace}.{custom_avg} {custom_avg_body}")
                            cql.execute(f"DROP AGGREGATE IF EXISTS {keyspace}.{custom_avg}(bigint)")
                        check_enforced(cql, username, permission='CREATE', resource=f'all functions in keyspace {keyspace}',
                                function=create_aggr_idempotent)
                        check_enforced(cql, username, permission='CREATE', resource='all functions',
                                function=create_aggr_idempotent)

                        grant(cql, 'CREATE', 'ALL FUNCTIONS', username)
                        check_enforced(cql, username, permission='ALTER', resource=f'all functions in keyspace {keyspace}',
                                function=lambda: user_session.execute(f"CREATE OR REPLACE AGGREGATE {keyspace}.{custom_avg} {custom_avg_body}"))
                        check_enforced(cql, username, permission='ALTER', resource='all functions',
                                function=lambda: user_session.execute(f"CREATE OR REPLACE AGGREGATE {keyspace}.{custom_avg} {custom_avg_body}"))

                        grant(cql, 'SELECT', table, username)
                        for resource in [f'function {keyspace}.{custom_avg}(bigint)', f'all functions in keyspace {keyspace}', 'all functions']:
                            check_enforced(cql, username, permission='EXECUTE', resource=resource,
                                    function=lambda: user_session.execute(f"SELECT {keyspace}.{custom_avg}(id) FROM {table}"))

                        def drop_aggr_idempotent():
                            user_session.execute(f"DROP AGGREGATE IF EXISTS {keyspace}.{custom_avg}(bigint)")
                            cql.execute(f"CREATE AGGREGATE IF NOT EXISTS {keyspace}.{custom_avg} {custom_avg_body}")
                        for resource in [f'function {keyspace}.{custom_avg}(bigint)', f'all functions in keyspace {keyspace}', 'all functions']:
                            check_enforced(cql, username, permission='DROP', resource=resource, function=drop_aggr_idempotent)

                        grant(cql, 'EXECUTE', 'ALL FUNCTIONS', username)
                        def grant_idempotent():
                            grant(user_session, 'EXECUTE', f'function {keyspace}.{custom_avg}(bigint)', 'cassandra')
                            revoke(cql, 'EXECUTE', f'function {keyspace}.{custom_avg}(bigint)', 'cassandra')
                        for resource in [f'function {keyspace}.{custom_avg}(bigint)', f'all functions in keyspace {keyspace}', 'all functions']:
                            check_enforced(cql, username, permission='AUTHORIZE', resource=resource, function=grant_idempotent)

                        cql.execute(f"DROP AGGREGATE IF EXISTS {keyspace}.{custom_avg}(bigint)")

# Test that permissions for user-defined functions created on top of user-defined types work
def test_udf_permissions_with_udt(cql):
    with new_test_keyspace(cql, "WITH REPLICATION = { 'class': 'SimpleStrategy', 'replication_factor': 1 }") as keyspace:
        with new_type(cql, keyspace, '(v int)') as udt:
            schema = f"a frozen<{udt}> primary key"
            with new_test_table(cql, keyspace, schema) as table:
                fun_body_lua = f"(i {udt}) CALLED ON NULL INPUT RETURNS int LANGUAGE lua AS 'return 42;'"
                fun_body_java = f"(i {udt}) CALLED ON NULL INPUT RETURNS int LANGUAGE java AS 'return 42;'"
                fun_body = fun_body_lua
                try:
                    with new_function(cql, keyspace, fun_body) as fun:
                        pass
                except:
                    fun_body = fun_body_java
                with new_user(cql) as username:
                    with new_session(cql, username) as user_session:
                        with new_function(cql, keyspace, fun_body) as fun:
                            cql.execute(f"INSERT INTO {table}(a) VALUES ((7))")
                            grant(cql, 'SELECT', table, username)
                            grant(cql, 'EXECUTE', f'FUNCTION {keyspace}.{fun}({udt})', username)
                            user_session.execute(f'SELECT {keyspace}.{fun}(a) FROM {table}')

# Test that permissions on user-defined functions with no arguments work
def test_udf_permissions_no_args(cql):
    with new_test_keyspace(cql, "WITH REPLICATION = { 'class': 'SimpleStrategy', 'replication_factor': 1 }") as keyspace:
        with new_test_table(cql, keyspace, schema="a int primary key") as table, new_user(cql) as username:
            with new_session(cql, username) as user_session:
                fun_body_lua = f"() CALLED ON NULL INPUT RETURNS int LANGUAGE lua AS 'return 42;'"
                fun_body_java = f"() CALLED ON NULL INPUT RETURNS int LANGUAGE java AS 'return 42;'"
                fun_body = fun_body_lua
                try:
                    with new_function(cql, keyspace, fun_body):
                        pass
                except:
                    fun_body = fun_body_java
                with new_function(cql, keyspace, fun_body) as fun:
                    grant(cql, 'SELECT', table, username)
                    check_enforced(cql, username, permission='EXECUTE', resource=f'function {keyspace}.{fun}()',
                            function=lambda: user_session.execute(f'SELECT {keyspace}.{fun}() FROM {table}'))
