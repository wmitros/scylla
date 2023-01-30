# This file was translated from the original Java test from the Apache
# Cassandra source repository, as of Cassandra 4.1.1 (commit 8d91b469afd3fcafef7ef85c10c8acc11703ba2d)
#
# The original Apache Cassandra license:
#
# SPDX-License-Identifier: Apache-2.0

import datetime
import uuid
from cassandra_tests.porting import *

from cassandra.protocol import InvalidRequest
from cassandra.util import Date
from util import new_function, unique_name
import os

def read_function_from_file(file_name, orig_name=None, rename=None):
    wat_path = os.path.realpath(os.path.join(__file__, f'../../../../../../build/wasm/{file_name}.wat'))
    orig_name = orig_name or file_name
    rename = rename or orig_name
    try:
      with open(wat_path, "r") as f:
          return f.read().replace("'", "''").replace(f'export "{orig_name}"', f'export "{rename}"')
    except:
        print(f"Can't open {wat_path}.\nPlease build Wasm examples.")
        exit(1)

def test_complex_null_values(cql, test_keyspace):
    with create_type(cql, test_keyspace, "(txt text, i int)") as type:
        schema = f"(key int primary key, lst list<double>, st set<text>, mp map<int, boolean>, tup frozen<tuple<double, text, int, boolean>>, udt frozen<{type}>)"
        with create_table(cql, test_keyspace, schema) as table:
            flist_name = unique_name()
            flist_wasm_src = read_function_from_file("test_complex_null_values", "return_input_flist", flist_name)
            flist_src = f"(input list<double>) CALLED ON NULL INPUT RETURNS list<double> LANGUAGE wasm AS '{flist_wasm_src}'"
            fset_name = unique_name()
            fset_wasm_src = read_function_from_file("test_complex_null_values", "return_input_fset", fset_name)
            fset_src = f"(input set<text>) CALLED ON NULL INPUT RETURNS set<text> LANGUAGE wasm AS '{fset_wasm_src}'"
            fmap_name = unique_name()
            fmap_wasm_src = read_function_from_file("test_complex_null_values", "return_input_fmap", fmap_name)
            fmap_src = f"(input map<int, boolean>) CALLED ON NULL INPUT RETURNS map<int, boolean> LANGUAGE wasm AS '{fmap_wasm_src}'"
            ftup_name = unique_name()
            ftup_wasm_src = read_function_from_file("test_complex_null_values", "return_input_ftup", ftup_name)
            ftup_src = f"(input tuple<double, text, int, boolean>) CALLED ON NULL INPUT RETURNS tuple<double, text, int, boolean> LANGUAGE wasm AS '{ftup_wasm_src}'"
            fudt_name = unique_name()
            fudt_wasm_src = read_function_from_file("test_complex_null_values", "return_input_fudt", fudt_name)
            fudt_src = f"(input {type}) CALLED ON NULL INPUT RETURNS {type} LANGUAGE wasm AS '{fudt_wasm_src}'"
            cql.execute(f"INSERT INTO {table} (key, lst, st, mp, tup, udt) VALUES (1, [1.0, 2.0, 3.0], {{'one', 'three', 'two'}}, {{1:true, 2:false, 3:true}}, (1.0, 'one', 42, false), {{txt:'one', i:1}})")
            cql.execute(f"INSERT INTO {table} (key, lst, st, mp, tup, udt) VALUES (2, null, null, null, null, null)")
            with new_function(cql, test_keyspace, flist_src, flist_name), new_function(cql, test_keyspace, fset_src, fset_name), \
            new_function(cql, test_keyspace, fmap_src, fmap_name), new_function(cql, test_keyspace, ftup_src, ftup_name), \
            new_function(cql, test_keyspace, fudt_src, fudt_name):
                cql.execute(f"SELECT {flist_name}(lst) FROM {table} WHERE key = 1")
                cql.execute(f"SELECT {fset_name}(st) FROM {table} WHERE key = 1")
                cql.execute(f"SELECT {fmap_name}(mp) FROM {table} WHERE key = 1")
                cql.execute(f"SELECT {ftup_name}(tup) FROM {table} WHERE key = 1")
                cql.execute(f"SELECT {fudt_name}(udt) FROM {table} WHERE key = 1")
                row = cql.execute(f"SELECT {flist_name}(lst) as l, {fset_name}(st) as s, {fmap_name}(mp) as m, {ftup_name}(tup) as t, {fudt_name}(udt) as u FROM {table} WHERE key = 1").one()
                assert row.l
                assert row.s
                assert row.m
                assert row.t
                assert row.u
                row = cql.execute(f"SELECT {flist_name}(lst) as l, {fset_name}(st) as s, {fmap_name}(mp) as m, {ftup_name}(tup) as t, {fudt_name}(udt) as u FROM {table} WHERE key = 2").one()
                assert not row.l
                assert not row.s
                assert not row.m
                assert not row.t
                assert not row.u
                # Cassandra tests query execution both using internal commands and using the API for all protocol versions.
                # We only test the API here, but if we add more ABI versions we should test them as well.

class TypesTestDef:
    def __init__(self, udf_type, table_type, column_name, reference_value):
        self.udf_type = udf_type
        self.table_type = table_type
        self.column_name = column_name
        self.reference_value = reference_value

def test_types_with_and_without_nulls(cql, test_keyspace):
    with create_type(cql, test_keyspace, "(txt text, i int)") as type:
        now = datetime.datetime.now()
        # Cassandra only supports milliseconds in timestamps
        micros = now.microsecond // 1000 * 1000
        type_defs = [
            # udf type, table type, column, reference value
            TypesTestDef("timestamp", "timestamp", "ts", now.replace(microsecond=micros)),
            TypesTestDef("date", "date", "dt", Date(12345)),
            TypesTestDef("time", "time", "tim", 12345),
            TypesTestDef("uuid", "uuid", "uu", uuid.uuid4()),
            TypesTestDef("timeuuid", "timeuuid", "tu", uuid.uuid1()),
            TypesTestDef("tinyint", "tinyint", "ti", 42),
            TypesTestDef("smallint", "smallint", "si", 43),
            TypesTestDef("int", "int", "i", 44),
            TypesTestDef("bigint", "bigint", "bi", 45),
            TypesTestDef("float", "float", "f", 46.0),
            TypesTestDef("double", "double", "d", 47.0),
            TypesTestDef("boolean", "boolean", "x", True),
            TypesTestDef("ascii", "ascii", "a", "tqbfjutld"),
            TypesTestDef("text", "text", "txt", "k\u00f6lsche jung"),
            # TypesTestDef(type, f"frozen<{type}>", "u", null),
            TypesTestDef("tuple<int, text>", "frozen<tuple<int, text>>", "tup", (1, "foo")),
        ]

        schema = "(key int PRIMARY KEY"
        values = [1]
        for type_def in type_defs:
            schema += ", " + type_def.column_name + ' ' + type_def.table_type
            values.append(type_def.reference_value)
        schema += ")"

        with create_table(cql, test_keyspace, schema) as table:
            insert_str = f"INSERT INTO {table} (key"
            for type_def in type_defs:
                insert_str += ", " + type_def.column_name
            insert_str += ") VALUES (?"
            for type_def in type_defs:
                insert_str += ", ?"
            insert_str += ")"
            insert_stmt = cql.prepare(insert_str)
            cql.execute(insert_stmt, values)

            for i in range(len(values)):
                values[i] = None
            values[0] = 2
            cql.execute(insert_stmt, values)

            for type_def in type_defs:
                fun = unique_name()
                fun_wasm_src = read_function_from_file("test_types_with_and_without_nulls", f"check_arg_and_return_{type_def.column_name}", fun)
                fun_src = f"(input {type_def.udf_type}) CALLED ON NULL INPUT RETURNS {type_def.udf_type} LANGUAGE wasm AS '{fun_wasm_src}'"
                with new_function(cql, test_keyspace, fun_src, fun) as fun:
                    assert_rows(cql.execute(f"SELECT {fun}({type_def.column_name}) FROM {table} WHERE key = 1"), [type_def.reference_value])

                fun = unique_name()
                fun_wasm_src = read_function_from_file("test_types_with_and_without_nulls", f"called_on_null_{type_def.column_name}", fun)
                fun_src = f"(input {type_def.udf_type}) CALLED ON NULL INPUT RETURNS text LANGUAGE wasm AS '{fun_wasm_src}'"
                with new_function(cql, test_keyspace, fun_src, fun) as fun:
                    assert_rows(cql.execute(f"SELECT {fun}({type_def.column_name}) FROM {table} WHERE key = 1"), ["called"])
                    assert_rows(cql.execute(f"SELECT {fun}({type_def.column_name}) FROM {table} WHERE key = 2"), ["called"])

                fun = unique_name()
                fun_wasm_src = read_function_from_file("test_types_with_and_without_nulls", f"returns_null_on_null_{type_def.column_name}", fun)
                fun_src = f"(input {type_def.udf_type}) RETURNS NULL ON NULL INPUT RETURNS text LANGUAGE wasm AS '{fun_wasm_src}'"
                with new_function(cql, test_keyspace, fun_src, fun) as fun:
                    assert_rows(cql.execute(f"SELECT {fun}({type_def.column_name}) FROM {table} WHERE key = 1"), ["called"])
                    assert_rows(cql.execute(f"SELECT {fun}({type_def.column_name}) FROM {table} WHERE key = 2"), [None])

# In the following tests, we accept both frozen and non-frozen types as function arguments and return values, but the type in the table must match
# the type in the function signature. Cassandra requires non-frozen types in the signature, but allows both frozen and non-frozen types as actual arguments.
# FIXME: we should support both frozen and non-frozen arguments from the table, at least for functions with non-frozen types in the signature.
# Cassandra also tests the functions used in a WHERE clause, but we don't support that yet.

def test_function_with_frozen_set_type(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(a int PRIMARY KEY, b set<int>, c frozen<set<int>>)") as table:
        stmt = cql.prepare(f"INSERT INTO {table} (a, b, c) VALUES (?, ?, ?)")
        cql.execute(stmt, [0, set(), set()])
        cql.execute(stmt, [1, set([1, 2, 3]), set([1, 2, 3])])
        cql.execute(stmt, [2, set([4, 5, 6]), set([4, 5, 6])])
        cql.execute(stmt, [3, set([7, 8, 9]), set([7, 8, 9])])

        fun_name = unique_name()
        return_set_src = read_function_from_file("test_functions_with_frozen_types", "return_set", fun_name)
        fun_src = f"(values set<int>) CALLED ON NULL INPUT RETURNS set<int> LANGUAGE wasm AS '{return_set_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [None])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [set([1, 2, 3])])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [set([4, 5, 6])])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [set([7, 8, 9])])
        fun_src = f"(values frozen<set<int>>) CALLED ON NULL INPUT RETURNS frozen<set<int>> LANGUAGE wasm AS '{return_set_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 0"), [set()])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 1"), [set([1, 2, 3])])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 2"), [set([4, 5, 6])])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 3"), [set([7, 8, 9])])

        fun_name = unique_name()
        sum_set_src = read_function_from_file("test_functions_with_frozen_types", "sum_set", fun_name)
        fun_src = f"(values set<int>) CALLED ON NULL INPUT RETURNS int LANGUAGE wasm AS '{sum_set_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [0])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [6])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [15])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [24])

        fun_src = f"(values frozen<set<int>>) CALLED ON NULL INPUT RETURNS int LANGUAGE wasm AS '{sum_set_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 0"), [0])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 1"), [6])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 2"), [15])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 3"), [24])

def test_function_with_frozen_list_type(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(a int PRIMARY KEY, b list<int>, c frozen<list<int>>)") as table:
        stmt = cql.prepare(f"INSERT INTO {table} (a, b, c) VALUES (?, ?, ?)")
        cql.execute(stmt, [0, list(), list()])
        cql.execute(stmt, [1, list([1, 2, 3]), list([1, 2, 3])])
        cql.execute(stmt, [2, list([4, 5, 6]), list([4, 5, 6])])
        cql.execute(stmt, [3, list([7, 8, 9]), list([7, 8, 9])])

        fun_name = unique_name()
        return_list_src = read_function_from_file("test_functions_with_frozen_types", "return_list", fun_name)
        fun_src = f"(values list<int>) CALLED ON NULL INPUT RETURNS list<int> LANGUAGE wasm AS '{return_list_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [None])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [list([1, 2, 3])])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [list([4, 5, 6])])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [list([7, 8, 9])])
        fun_src = f"(values frozen<list<int>>) CALLED ON NULL INPUT RETURNS frozen<list<int>> LANGUAGE wasm AS '{return_list_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 0"), [list()])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 1"), [list([1, 2, 3])])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 2"), [list([4, 5, 6])])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 3"), [list([7, 8, 9])])

        fun_name = unique_name()
        sum_list_src = read_function_from_file("test_functions_with_frozen_types", "sum_list", fun_name)
        fun_src = f"(values list<int>) CALLED ON NULL INPUT RETURNS int LANGUAGE wasm AS '{sum_list_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [0])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [6])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [15])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [24])

        fun_src = f"(values frozen<list<int>>) CALLED ON NULL INPUT RETURNS int LANGUAGE wasm AS '{sum_list_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 0"), [0])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 1"), [6])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 2"), [15])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 3"), [24])

def test_function_with_frozen_map_type(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(a int PRIMARY KEY, b map<int, int>, c frozen<map<int, int>>)") as table:
        stmt = cql.prepare(f"INSERT INTO {table} (a, b,c ) VALUES (?, ?, ?)")
        cql.execute(stmt, [0, {}, {}])
        cql.execute(stmt, [1, {1: 1, 2: 2, 3: 3}, {1: 1, 2: 2, 3: 3}])
        cql.execute(stmt, [2, {4: 4, 5: 5, 6: 6}, {4: 4, 5: 5, 6: 6}])
        cql.execute(stmt, [3, {7: 7, 8: 8, 9: 9}, {7: 7, 8: 8, 9: 9}])

        fun_name = unique_name()
        fun_wasm_src = read_function_from_file("test_functions_with_frozen_types", "return_map", fun_name)
        fun_src = f"(values map<int, int>) CALLED ON NULL INPUT RETURNS map<int, int> LANGUAGE wasm AS '{fun_wasm_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [None])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [{1: 1, 2: 2, 3: 3}])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [{4: 4, 5: 5, 6: 6}])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [{7: 7, 8: 8, 9: 9}])

        fun_src = f"(values frozen<map<int, int>>) CALLED ON NULL INPUT RETURNS frozen<map<int, int>> LANGUAGE wasm AS '{fun_wasm_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 0"), [{}])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 1"), [{1: 1, 2: 2, 3: 3}])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 2"), [{4: 4, 5: 5, 6: 6}])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 3"), [{7: 7, 8: 8, 9: 9}])

        fun_name = unique_name()
        sum_map_src = read_function_from_file("test_functions_with_frozen_types", "sum_map", fun_name)
        fun_src = f"(values map<int, int>) CALLED ON NULL INPUT RETURNS int LANGUAGE wasm AS '{sum_map_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [0])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [6])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [15])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [24])

        fun_src = f"(values frozen<map<int, int>>) CALLED ON NULL INPUT RETURNS int LANGUAGE wasm AS '{sum_map_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 0"), [0])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 1"), [6])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 2"), [15])
            assert_rows(cql.execute(f"SELECT {fun}(c) AS result FROM {table} WHERE a = 3"), [24])

def test_function_with_frozen_tuple_type(cql, test_keyspace):
    with create_table(cql, test_keyspace, "(a int PRIMARY KEY, b frozen<tuple<int, int>>)") as table:
        stmt = cql.prepare(f"INSERT INTO {table} (a, b) VALUES (?, ?)")
        cql.execute(stmt, [0, ()])
        cql.execute(stmt, [1, (1, 2)])
        cql.execute(stmt, [2, (4, 5)])
        cql.execute(stmt, [3, (7, 8)])

        # Tuples are always frozen. Both 'tuple' and 'frozen tuple' have the same effect.
        # So allows to create function with explicit frozen tuples as argument and return types.
        fun_name = unique_name()
        return_tuple_src = read_function_from_file("test_functions_with_frozen_types", "return_tuple", fun_name)
        fun_src = f"(values frozen<tuple<int, int>>) CALLED ON NULL INPUT RETURNS frozen<tuple<int, int>> LANGUAGE wasm AS '{return_tuple_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [None])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [(1, 2)])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [(4, 5)])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [(7, 8)])

        fun_src = f"(values tuple<int, int>) CALLED ON NULL INPUT RETURNS tuple<int, int> LANGUAGE wasm AS '{return_tuple_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [None])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [(1, 2)])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [(4, 5)])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [(7, 8)])

        fun_name = unique_name()
        tostring_tuple_src = read_function_from_file("test_functions_with_frozen_types", "tostring_tuple", fun_name)
        fun_src = f"(values tuple<int, int>) CALLED ON NULL INPUT RETURNS text LANGUAGE wasm AS '{tostring_tuple_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), ["None"])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), ["Some((1, 2))"])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), ["Some((4, 5))"])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), ["Some((7, 8))"])
        fun_src = f"(values frozen<tuple<int, int>>) CALLED ON NULL INPUT RETURNS text LANGUAGE wasm AS '{tostring_tuple_src}'"
        with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), ["None"])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), ["Some((1, 2))"])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), ["Some((4, 5))"])
            assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), ["Some((7, 8))"])

def test_function_with_frozen_udt_type(cql, test_keyspace):
    with create_type(cql, test_keyspace, "(f int)") as type:
        with create_table(cql, test_keyspace, f"(a int PRIMARY KEY, b frozen<{type}>)") as table:
            stmt = cql.prepare(f"INSERT INTO {table} (a, b) VALUES (?, {{f : ?}})")
            cql.execute(stmt, [0, 0])
            cql.execute(stmt, [1, 1])
            cql.execute(stmt, [2, 4])
            cql.execute(stmt, [3, 7])

            fun_name = unique_name()
            return_udt_src = read_function_from_file("test_functions_with_frozen_types", "return_udt", fun_name)
            fun_src = f"(values {type}) CALLED ON NULL INPUT RETURNS {type} LANGUAGE wasm AS '{return_udt_src}'"
            with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
                assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), [(0,)])
                assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), [(1,)])
                assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), [(4,)])
                assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), [(7,)])
            fun_src = f"(values {type}) CALLED ON NULL INPUT RETURNS frozen<{type}> LANGUAGE wasm AS '{return_udt_src}'"
            with pytest.raises(InvalidRequest, match="should not be frozen"):
                cql.execute(f"CREATE FUNCTION {test_keyspace}.{fun_name} {fun_src}")

            fun_name = unique_name()
            tostring_udt_src = read_function_from_file("test_functions_with_frozen_types", "tostring_udt", fun_name)
            fun_src = f"(values {type}) CALLED ON NULL INPUT RETURNS text LANGUAGE wasm AS '{tostring_udt_src}'"
            with new_function(cql, test_keyspace, fun_src, fun_name) as fun:
                assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 0"), ["Some(Udt { f: Some(0) })"])
                assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 1"), ["Some(Udt { f: Some(1) })"])
                assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 2"), ["Some(Udt { f: Some(4) })"])
                assert_rows(cql.execute(f"SELECT {fun}(b) AS result FROM {table} WHERE a = 3"), ["Some(Udt { f: Some(7) })"])
            fun_src = f"(values frozen<{type}>) CALLED ON NULL INPUT RETURNS text LANGUAGE wasm AS '{tostring_udt_src}'"
            with pytest.raises(InvalidRequest, match="should not be frozen"):
                cql.execute(f"CREATE FUNCTION {test_keyspace}.{fun_name} {fun_src}")
