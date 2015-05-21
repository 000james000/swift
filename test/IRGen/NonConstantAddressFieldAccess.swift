// RUN: %target-build-swift %S/Inputs/ObjcSuperClass.swift %s -module-name a -emit-ir 2>&1 | FileCheck %s

// REQUIRES: objc_interop

// CHECK: @_TWvdvC1a12ObjCSubclass5fieldSi = global i{{[0-9]+}}
// CHECK: @_TWvdvC1a12ObjCSubclass5fieldSi = external global i{{[0-9]+}}

func test(o: ObjCSubclass) {
  o.field = 10
}
