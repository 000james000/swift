// RUN: rm -rf %t
// RUN: mkdir %t
// RUN: %swift -emit-module -parse-as-library -sil-serialize-all -o %t %s
// RUN: llvm-bcanalyzer %t/global_init.swiftmodule | FileCheck %s -check-prefix=BCANALYZER
// RUN: %sil-opt %t/global_init.swiftmodule | FileCheck %s

// BCANALYZER-NOT: UnknownCode

// Swift globals are not currently serialized. However, addressor
// declarations are serialized when all these three flags are present:
// -emit-module -parse-as-library -sil-serialize-all
//
// The only way to inspect the serialized module is sil-opt. The swift
// driver will only output the SIL that it deserializes.

let MyConst = 42
var MyVar = 3

// CHECK: let MyConst: Int { get }
// CHECK: var MyVar: Int

// CHECK-DAG: sil public [global_init] @_TF11global_inita7MyConstSi : $@thin () -> Builtin.RawPointer
// CHECK-DAG: sil public [global_init] @_TF11global_inita5MyVarSi : $@thin () -> Builtin.RawPointer

func getGlobals() -> Int {
  return MyVar + MyConst
}
