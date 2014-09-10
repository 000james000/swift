// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -target x86_64-apple-macosx10.9 -module-cache-path %t/clang-module-cache -sdk %S/Inputs -I=%S/Inputs -enable-source-import %s -emit-ir | FileCheck %s

import Foundation
import gizmo

// CHECK: [[TYPE:%swift.type]] = type
// CHECK: [[BLAMMO:%C4objc6Blammo]] = type
// CHECK: [[MYBLAMMO:%C4objc8MyBlammo]] = type
// CHECK: [[TEST2:%C4objc5Test2]] = type
// CHECK: [[OBJC:%objc_object]] = type
// CHECK: [[GIZMO:%CSo5Gizmo]] = type
// CHECK: [[ID:%V4objc2id]] = type <{ %PSs9AnyObject_ }>
// CHECK: [[RECT:%VSC4Rect]] = type
// CHECK: [[FLOAT:%Sf]] = type

// CHECK: @"\01L_selector_data(bar)" = internal constant [4 x i8] c"bar\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK: @"\01L_selector(bar)" = internal global i8* getelementptr inbounds ([4 x i8]* @"\01L_selector_data(bar)", i64 0, i64 0), section "__DATA,__objc_selrefs,literal_pointers,no_dead_strip", align 8

// CHECK: @_TMnVSC4Rect = linkonce_odr hidden constant
// CHECK: @_TMdVSC4Rect = linkonce_odr hidden constant

struct id {
  var data : AnyObject
}

// Exporting something as [objc] doesn't make it an ObjC class.
@objc class Blammo {
}
// Class and methods are [objc] by inheritance.
class MyBlammo : Blammo {
  func foo() {}
// CHECK:  define void @_TFC4objc8MyBlammo3foofS0_FT_T_([[MYBLAMMO]]*) {
// CHECK:    call {{.*}} @swift_release
// CHECK:    ret void
}

// Class and methods are [objc] by inheritance.
class Test2 : Gizmo {
  func foo() {}
// CHECK:  define void @_TFC4objc5Test23foofS0_FT_T_([[TEST2]]*) {
// CHECK:    call {{.*}} @objc_release
// CHECK:    ret void

  dynamic func bar() {}
}

// CHECK:    define %objc_object* @_TF4objc5test0{{.*}}(%objc_object*)
// CHECK-NOT:  call {{.*}} @swift_retain
// CHECK:      call {{.*}} @objc_retain
// CHECK-NOT:  call {{.*}} @swift_release
// CHECK:      call {{.*}} @objc_release
// CHECK:      ret %objc_object*
func test0(arg: id) -> id {
  var x : id
  x = arg
  var y = x
  return y
}

func test1(cell: Blammo) {}
// CHECK:  define void @_TF4objc5test1{{.*}}([[BLAMMO]]*) {
// CHECK:    call {{.*}} @swift_release
// CHECK:    ret void


// FIXME: These ownership convention tests should become SILGen tests.
func test2(v: Test2) { v.bar() }
func test3() -> NSObject {
  return Gizmo()
}
// Normal message send with argument, no transfers.
func test5(g: Gizmo) {
  Gizmo.inspect(g)
}
// The argument to consume: is __attribute__((ns_consumed)).
func test6(g: Gizmo) {
  Gizmo.consume(g)
}
// fork is __attribute__((ns_consumes_self)).
func test7(g: Gizmo) {
  g.fork()
}
// clone is __attribute__((ns_returns_retained)).
func test8(g: Gizmo) {
  g.clone()
}
// duplicate has an object returned at +0.
func test9(g: Gizmo) {
  g.duplicate()
}

func test10(g: Gizmo, r: Rect) {
  Gizmo.runWithRect(r, andGizmo:g);
}

// Force the emission of the Rect metadata.
func test11_helper<T>(t: T) {}
func test11(r: Rect) { test11_helper(r) }

class WeakObjC {
  weak var obj: NSObject?
  weak var id: AnyObject?

  init() {
    var foo = obj
    var bar: AnyObject? = id
  }
}

// rdar://17528908
// CHECK: metadata !{i32 1, metadata !"Objective-C Version", i32 2}
// CHECK: metadata !{i32 1, metadata !"Swift Version", i32 2}
// CHECK: metadata !{i32 1, metadata !"Objective-C Image Info Version", i32 0}
// CHECK: metadata !{i32 1, metadata !"Objective-C Image Info Section", metadata !"__DATA, __objc_imageinfo, regular, no_dead_strip"}
//   512 == (2 << 8).  1 is the Swift ABI version.
// CHECK: metadata !{i32 4, metadata !"Objective-C Garbage Collection", i32 512}
