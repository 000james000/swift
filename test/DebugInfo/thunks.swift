// RUN: %target-swift-frontend -emit-ir -g %s -o - | FileCheck %s
// RUN: %target-swift-frontend -emit-sil -emit-verbose-sil -g %s -o - | FileCheck %s --check-prefix=SIL-CHECK
// REQUIRES: objc_interop
import Foundation

@objc class Foo {
  dynamic func foo(f: Int -> Int, x: Int) -> Int {
    return f(x)
  }
}

let foo = Foo()
let y = 3
let i = foo.foo(-, x: y)

// CHECK: define {{.*}}@_TTRXFdCb_dSi_dSi_XFo_dSi_dSi_
// CHECK-NOT: ret
// CHECK: call {{.*}}, !dbg ![[LOC:.*]]
// CHECK-DAG: ![[LOC]] = !MDLocation(line: 0, scope: ![[THUNK:.*]])
// CHECK-DAG: ![[THUNK]] = {{.*}}[ DW_TAG_subprogram ] [line 0]

// SIL-CHECK: sil shared {{.*}}@_TTRXFo_dSi_dSi_XFdCb_dSi_dSi_
// SIL-CHECK-NOT: return
// SIL-CHECK: apply {{.*}}auto_gen
