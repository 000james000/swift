// Check that the sdk and resource dirs end up in the debug info.
// RUN: %swiftc_driver %s -emit-ir -g -o - | FileCheck %s
// RUN: %swiftc_driver %s -emit-ir -sdk "/Weird Location/SDK" -g -o - | FileCheck --check-prefix CHECK-EXPLICIT %s
// CHECK:          \00Swift version {{.*}}\000{{.*}} -resource-dir {{.*}}"
// CHECK-EXPLICIT: \00Swift version {{.*}}\000{{.*}} -sdk \22/Weird Location/SDK\22{{.*}} -resource-dir {{.*}}"
