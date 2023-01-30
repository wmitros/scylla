# This file was translated from the original Java test from the Apache
# Cassandra source repository, as of commit 235d2df0eea4a2c70bbdcb1b9f4d2e121080f4c3
#
# The original Apache Cassandra license:
#
# SPDX-License-Identifier: Apache-2.0

from cassandra_tests.porting import *

# public class UFPureScriptTupleCollectionTest extends CQLTester
# {
#     // Just JavaScript UDFs to check how UDF - especially security/class-loading/sandboxing stuff -
#     // behaves, if no Java UDF has been executed before.

#     // Do not add any other test here!
#     // See CASSANDRA-10141

#     @Test
#     public void testJavascriptTupleTypeCollection() throws Throwable
#     {
#         String tupleTypeDef = "tuple<double, list<double>, set<text>, map<int, boolean>>";
#         createTable("CREATE TABLE %s (key int primary key, tup frozen<" + tupleTypeDef + ">)");

#         String fTup1 = createFunction(KEYSPACE_PER_TEST, tupleTypeDef,
#                                       "CREATE FUNCTION %s( tup " + tupleTypeDef + " ) " +
#                                       "RETURNS NULL ON NULL INPUT " +
#                                       "RETURNS tuple<double, list<double>, set<text>, map<int, boolean>> " +
#                                       "LANGUAGE javascript\n" +
#                                       "AS $$" +
#                                       "       tup;$$;");
#         String fTup2 = createFunction(KEYSPACE_PER_TEST, tupleTypeDef,
#                                       "CREATE FUNCTION %s( tup " + tupleTypeDef + " ) " +
#                                       "RETURNS NULL ON NULL INPUT " +
#                                       "RETURNS double " +
#                                       "LANGUAGE javascript\n" +
#                                       "AS $$" +
#                                       "       tup.getDouble(0);$$;");
#         String fTup3 = createFunction(KEYSPACE_PER_TEST, tupleTypeDef,
#                                       "CREATE FUNCTION %s( tup " + tupleTypeDef + " ) " +
#                                       "RETURNS NULL ON NULL INPUT " +
#                                       "RETURNS list<double> " +
#                                       "LANGUAGE javascript\n" +
#                                       "AS $$" +
#                                       "       tup.getList(1, java.lang.Double.class);$$;");
#         String fTup4 = createFunction(KEYSPACE_PER_TEST, tupleTypeDef,
#                                       "CREATE FUNCTION %s( tup " + tupleTypeDef + " ) " +
#                                       "RETURNS NULL ON NULL INPUT " +
#                                       "RETURNS set<text> " +
#                                       "LANGUAGE javascript\n" +
#                                       "AS $$" +
#                                       "       tup.getSet(2, java.lang.String.class);$$;");
#         String fTup5 = createFunction(KEYSPACE_PER_TEST, tupleTypeDef,
#                                       "CREATE FUNCTION %s( tup " + tupleTypeDef + " ) " +
#                                       "RETURNS NULL ON NULL INPUT " +
#                                       "RETURNS map<int, boolean> " +
#                                       "LANGUAGE javascript\n" +
#                                       "AS $$" +
#                                       "       tup.getMap(3, java.lang.Integer.class, java.lang.Boolean.class);$$;");

#         List<Double> list = Arrays.asList(1d, 2d, 3d);
#         Set<String> set = new TreeSet<>(Arrays.asList("one", "three", "two"));
#         Map<Integer, Boolean> map = new TreeMap<>();
#         map.put(1, true);
#         map.put(2, false);
#         map.put(3, true);

#         Object t = tuple(1d, list, set, map);

#         execute("INSERT INTO %s (key, tup) VALUES (1, ?)", t);

#         assertRows(execute("SELECT " + fTup1 + "(tup) FROM %s WHERE key = 1"),
#                    row(t));
#         assertRows(execute("SELECT " + fTup2 + "(tup) FROM %s WHERE key = 1"),
#                    row(1d));
#         assertRows(execute("SELECT " + fTup3 + "(tup) FROM %s WHERE key = 1"),
#                    row(list));
#         assertRows(execute("SELECT " + fTup4 + "(tup) FROM %s WHERE key = 1"),
#                    row(set));
#         assertRows(execute("SELECT " + fTup5 + "(tup) FROM %s WHERE key = 1"),
#                    row(map));

#         // same test - but via native protocol
#         // we use protocol V3 here to encode the expected version because the server
#         // always serializes Collections using V3 - see CollectionSerializer's
#         // serialize and deserialize methods.
#         TupleType tType = tupleTypeOf(ProtocolVersion.V3,
#                                       DataType.cdouble(),
#                                       DataType.list(DataType.cdouble()),
#                                       DataType.set(DataType.text()),
#                                       DataType.map(DataType.cint(),
#                                                    DataType.cboolean()));
#         TupleValue tup = tType.newValue(1d, list, set, map);
#         for (ProtocolVersion version : PROTOCOL_VERSIONS)
#         {
#             assertRowsNet(version,
#                           executeNet(version, "SELECT " + fTup1 + "(tup) FROM %s WHERE key = 1"),
#                           row(tup));
#             assertRowsNet(version,
#                           executeNet(version, "SELECT " + fTup2 + "(tup) FROM %s WHERE key = 1"),
#                           row(1d));
#             assertRowsNet(version,
#                           executeNet(version, "SELECT " + fTup3 + "(tup) FROM %s WHERE key = 1"),
#                           row(list));
#             assertRowsNet(version,
#                           executeNet(version, "SELECT " + fTup4 + "(tup) FROM %s WHERE key = 1"),
#                           row(set));
#             assertRowsNet(version,
#                           executeNet(version, "SELECT " + fTup5 + "(tup) FROM %s WHERE key = 1"),
#                           row(map));
#         }
#     }
# }
