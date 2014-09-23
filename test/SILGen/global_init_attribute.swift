// RUN: rm -rf %t
// RUN: mkdir %t
// RUN: %swift -emit-module -o %t %S/Inputs/def_global.swift
// RUN: %swift -parse-as-library -emit-silgen -I=%t %s | FileCheck %s
//
// Test that SILGen uses the "global_init" attribute for all global
// variable addressors.

import def_global

let InternalConst = 42
// CHECK-NOT: [global_init]
// CHECK: // global_init_attribute.InternalConst.addressor : Swift.Int
// CHECK-NEXT: sil hidden [global_init] @_TF21global_init_attributea13InternalConstSi

func foo() -> Int {
  return ExportedVar
}

func bar(i: Int) {
  ExportedVar = i
}

// CHECK-NOT: [global_init]
// CHECK: // def_global.ExportedVar.addressor : Swift.Int
// CHECK-NEXT: sil [global_init] @_TF10def_globala11ExportedVarSi

var InternalFoo = foo()

// CHECK-NOT: [global_init]
// CHECK: // global_init_attribute.InternalFoo.addressor : Swift.Int
// CHECK-NEXT: sil hidden [global_init] @_TF21global_init_attributea11InternalFooSi
