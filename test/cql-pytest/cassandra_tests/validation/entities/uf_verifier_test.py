# This file was translated from the original Java test from the Apache
# Cassandra source repository, as of commit 235d2df0eea4a2c70bbdcb1b9f4d2e121080f4c3
#
# The original Apache Cassandra license:
#
# SPDX-License-Identifier: Apache-2.0

from cassandra_tests.porting import *

# /**
#  * Test the Java UDF byte code verifier.
#  */
# public class UFVerifierTest extends CQLTester
# {
#     @Test
#     public void testByteCodeVerifier()
#     {
#         verify(GoodClass.class);
#     }

#     @Test
#     public void testClassWithField()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("field declared: field")),
#                      verify(ClassWithField.class));
#     }

#     @Test
#     public void testClassWithInitializer()
#     {
#         assertEquals(new HashSet<>(Arrays.asList("field declared: field",
#                                                  "initializer declared")),
#                      verify(ClassWithInitializer.class));
#     }

#     @Test
#     public void testClassWithInitializer2()
#     {
#         assertEquals(new HashSet<>(Arrays.asList("field declared: field",
#                                                  "initializer declared")),
#                      verify(ClassWithInitializer2.class));
#     }

#     @Test
#     public void testClassWithInitializer3()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("initializer declared")),
#                      verify(ClassWithInitializer3.class));
#     }

#     @Test
#     public void testClassWithStaticInitializer()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("static initializer declared")),
#                      verify(ClassWithStaticInitializer.class));
#     }

#     @Test
#     public void testUseOfSynchronized()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("use of synchronized")),
#                      verify(UseOfSynchronized.class));
#     }

#     @Test
#     public void testUseOfSynchronizedWithNotify()
#     {
#         assertEquals(new HashSet<>(Arrays.asList("use of synchronized", "call to java.lang.Object.notify()")),
#                      verify(UseOfSynchronizedWithNotify.class));
#     }

#     @Test
#     public void testUseOfSynchronizedWithNotifyAll()
#     {
#         assertEquals(new HashSet<>(Arrays.asList("use of synchronized", "call to java.lang.Object.notifyAll()")),
#                      verify(UseOfSynchronizedWithNotifyAll.class));
#     }

#     @Test
#     public void testUseOfSynchronizedWithWait()
#     {
#         assertEquals(new HashSet<>(Arrays.asList("use of synchronized", "call to java.lang.Object.wait()")),
#                      verify(UseOfSynchronizedWithWait.class));
#     }

#     @Test
#     public void testUseOfSynchronizedWithWaitL()
#     {
#         assertEquals(new HashSet<>(Arrays.asList("use of synchronized", "call to java.lang.Object.wait()")),
#                      verify(UseOfSynchronizedWithWaitL.class));
#     }

#     @Test
#     public void testUseOfSynchronizedWithWaitI()
#     {
#         assertEquals(new HashSet<>(Arrays.asList("use of synchronized", "call to java.lang.Object.wait()")),
#                      verify(UseOfSynchronizedWithWaitLI.class));
#     }

#     @Test
#     public void testCallClone()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("call to java.lang.Object.clone()")),
#                      verify(CallClone.class));
#     }

#     @Test
#     public void testCallFinalize()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("call to java.lang.Object.finalize()")),
#                      verify(CallFinalize.class));
#     }

#     @Test
#     public void testCallOrgApache()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("call to org.apache.cassandra.config.DatabaseDescriptor.getClusterName()")),
#                      verify("org/", CallOrgApache.class));
#     }

#     @Test
#     public void testClassStaticInnerClass()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("class declared as inner class")),
#                      verify(ClassWithStaticInnerClass.class));
#     }

#     @Test
#     public void testUsingMapEntry()
#     {
#         assertEquals(Collections.emptySet(),
#                      verify(UsingMapEntry.class));
#     }

#     @Test
#     public void testClassInnerClass()
#     {
#         assertEquals(new HashSet<>(Collections.singletonList("class declared as inner class")),
#                      verify(ClassWithInnerClass.class));
#     }

#     @Test
#     public void testClassInnerClass2()
#     {
#         assertEquals(Collections.emptySet(),
#                      verify(ClassWithInnerClass2.class));
#     }

#     private Set<String> verify(Class cls)
#     {
#         return new UDFByteCodeVerifier().verify(cls.getName(), readClass(cls));
#     }

#     private Set<String> verify(String disallowedPkg, Class cls)
#     {
#         return new UDFByteCodeVerifier().addDisallowedPackage(disallowedPkg).verify(cls.getName(), readClass(cls));
#     }

#     @SuppressWarnings("resource")
#     private static byte[] readClass(Class<?> clazz)
#     {
#         ByteArrayOutputStream out = new ByteArrayOutputStream();
#         URL res = clazz.getClassLoader().getResource(clazz.getName().replace('.', '/') + ".class");
#         assert res != null;
#         try (InputStream input = res.openConnection().getInputStream())
#         {
#             int i;
#             while ((i = input.read()) != -1)
#                 out.write(i);
#             return out.toByteArray();
#         }
#         catch (IOException e)
#         {
#             throw new RuntimeException(e);
#         }
#     }

#     @Test
#     public void testInvalidByteCodeUDFs() throws Throwable
#     {
#         assertInvalidByteCode("try\n" +
#                               "{\n" +
#                               "    clone();\n" +
#                               "}\n" +
#                               "catch (CloneNotSupportedException e)\n" +
#                               "{\n" +
#                               "    throw new RuntimeException(e);\n" +
#                               "}\n" +
#                               "return 0d;", "Java UDF validation failed: [call to java.lang.Object.clone()]");
#         assertInvalidByteCode("try\n" +
#                               "{\n" +
#                               "    finalize();\n" +
#                               "}\n" +
#                               "catch (Throwable e)\n" +
#                               "{\n" +
#                               "    throw new RuntimeException(e);\n" +
#                               "}\n" +
#                               "return 0d;", "Java UDF validation failed: [call to java.lang.Object.finalize()]");
#         assertInvalidByteCode('\n' +
#                               "return 0d;\n" +
#                               "    }\n" +
#                               '\n' +
#                               "    Object field;\n" +
#                               '\n' +
#                               "    {", "Java UDF validation failed: [field declared: field]");
#         assertInvalidByteCode('\n' +
#                               "return 0d;\n" +
#                               "    }\n" +
#                               '\n' +
#                               "    final Object field;\n" +
#                               '\n' +
#                               "    {\n" +
#                               "field = new Object();", "Java UDF validation failed: [field declared: field, initializer declared]");
#         assertInvalidByteCode('\n' +
#                               "return 0d;\n" +
#                               "    }\n" +
#                               '\n' +
#                               "    Object field = new Object();\n" +
#                               '\n' +
#                               "    {\n" +
#                               "Math.sin(1d);", "Java UDF validation failed: [field declared: field, initializer declared]");
#         assertInvalidByteCode('\n' +
#                               "return 0d;\n" +
#                               "    }\n" +
#                               '\n' +
#                               "    {\n" +
#                               "Math.sin(1d);", "Java UDF validation failed: [initializer declared]");
#         assertInvalidByteCode('\n' +
#                               "return 0d;\n" +
#                               "    }\n" +
#                               '\n' +
#                               "    static\n" +
#                               "    {\n" +
#                               "Math.sin(1d);", "Java UDF validation failed: [static initializer declared]");
#         assertInvalidByteCode("synchronized (this)\n" +
#                               "{\n" +
#                               "    Math.sin(1d);\n" +
#                               "}\n" +
#                               "return 0d;", "Java UDF validation failed: [use of synchronized]");
#         assertInvalidByteCode("synchronized (this)\n" +
#                               "{\n" +
#                               "    notify();\n" +
#                               "}\n" +
#                               "return 0d;", "Java UDF validation failed: [call to java.lang.Object.notify(), use of synchronized]");
#         assertInvalidByteCode("synchronized (this)\n" +
#                               "{\n" +
#                               "    notifyAll();\n" +
#                               "}\n" +
#                               "return 0d;", "Java UDF validation failed: [call to java.lang.Object.notifyAll(), use of synchronized]");
#         assertInvalidByteCode("synchronized (this)\n" +
#                               "{\n" +
#                               "    try\n" +
#                               "    {\n" +
#                               "        wait();\n" +
#                               "    }\n" +
#                               "    catch (InterruptedException e)\n" +
#                               "    {\n" +
#                               "        throw new RuntimeException(e);\n" +
#                               "    }\n" +
#                               "}\n" +
#                               "return 0d;", "Java UDF validation failed: [call to java.lang.Object.wait(), use of synchronized]");
#         assertInvalidByteCode("synchronized (this)\n" +
#                               "{\n" +
#                               "    try\n" +
#                               "    {\n" +
#                               "        wait(1000L);\n" +
#                               "    }\n" +
#                               "    catch (InterruptedException e)\n" +
#                               "    {\n" +
#                               "        throw new RuntimeException(e);\n" +
#                               "    }\n" +
#                               "}\n" +
#                               "return 0d;", "Java UDF validation failed: [call to java.lang.Object.wait(), use of synchronized]");
#         assertInvalidByteCode("synchronized (this)\n" +
#                               "{\n" +
#                               "    try\n" +
#                               "    {\n" +
#                               "        wait(1000L, 100);\n" +
#                               "    }\n" +
#                               "    catch (InterruptedException e)\n" +
#                               "    {\n" +
#                               "        throw new RuntimeException(e);\n" +
#                               "    }\n" +
#                               "}\n" +
#                               "return 0d;", "Java UDF validation failed: [call to java.lang.Object.wait(), use of synchronized]");
#         assertInvalidByteCode("try {" +
#                               "     java.nio.ByteBuffer.allocateDirect(123); return 0d;" +
#                               "} catch (Exception t) {" +
#                               "     throw new RuntimeException(t);" +
#                               '}', "Java UDF validation failed: [call to java.nio.ByteBuffer.allocateDirect()]");
#         assertInvalidByteCode("try {" +
#                               "     java.net.InetAddress.getLocalHost(); return 0d;" +
#                               "} catch (Exception t) {" +
#                               "     throw new RuntimeException(t);" +
#                               '}', "Java UDF validation failed: [call to java.net.InetAddress.getLocalHost()]");
#         assertInvalidByteCode("try {" +
#                               "     java.net.InetAddress.getAllByName(\"localhost\"); return 0d;" +
#                               "} catch (Exception t) {" +
#                               "     throw new RuntimeException(t);" +
#                               '}', "Java UDF validation failed: [call to java.net.InetAddress.getAllByName()]");
#         assertInvalidByteCode("try {" +
#                               "     java.net.Inet4Address.getByName(\"127.0.0.1\"); return 0d;" +
#                               "} catch (Exception t) {" +
#                               "     throw new RuntimeException(t);" +
#                               '}', "Java UDF validation failed: [call to java.net.Inet4Address.getByName()]");
#         assertInvalidByteCode("try {" +
#                               "     java.net.Inet6Address.getByAddress(new byte[]{127,0,0,1}); return 0d;" +
#                               "} catch (Exception t) {" +
#                               "     throw new RuntimeException(t);" +
#                               '}', "Java UDF validation failed: [call to java.net.Inet6Address.getByAddress()]");
#         assertInvalidByteCode("try {" +
#                               "     java.net.NetworkInterface.getNetworkInterfaces(); return 0d;" +
#                               "} catch (Exception t) {" +
#                               "     throw new RuntimeException(t);" +
#                               '}', "Java UDF validation failed: [call to java.net.NetworkInterface.getNetworkInterfaces()]");
#     }

#     private void assertInvalidByteCode(String body, String error) throws Throwable
#     {
#         assertInvalidMessage(error,
#                              "CREATE FUNCTION " + KEYSPACE + ".mustBeInvalid ( input double ) " +
#                              "CALLED ON NULL INPUT " +
#                              "RETURNS double " +
#                              "LANGUAGE java AS $$" + body + "$$");
#     }
# }
