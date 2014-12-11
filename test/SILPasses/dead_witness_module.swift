// RUN: rm -rf %t && mkdir %t
// RUN: %swift -Onone -parse-stdlib -parse-as-library  -module-name TestModule -sil-serialize-all %S/Inputs/TestModule.swift -emit-module-path %t/TestModule.swiftmodule 
// RUN: %swift -O %s -I %t -emit-sil | FileCheck %s

// DeadFunctionElimination may not remove a method from a witness table which
// is imported from another module.

import TestModule

testit(MyStruct())

// CHECK: sil_witness_table private_external [fragile] MyStruct: Proto module TestModule
// CHECK-NEXT: method #Proto.confx!1: @_TTWV10TestModule{{.*}}confxUS1___fQPS1_FT_T_
