// RUN: %swift -target x86_64-apple-darwin %s -emit-ir -g -o - | FileCheck %s
struct __CurrentErrno {}
struct CErrorOr<T>
 {
  var value : T?
  init(x : __CurrentErrno) {
    // CHECK: define void @_TFV20generic_enum_closure8CErrorOrCU__fMGS0_Q__FT1xVS_14__CurrentErrno_GS0_Q__
    // CHECK-NOT: define
    // This is a SIL-level debug_value_addr instruction.
    // CHECK: call void @llvm.dbg.value({{.*}}, metadata ![[SELF:.*]])
    // CHECK-DAG: ![[SELFTY:.*]] = {{.*}}_TtGV20generic_enum_closure8CErrorOrQq_S0__
    // CHECK-DAG: ![[SELF]] = {{.*}}_TtGV20generic_enum_closure8CErrorOrQq_S0__{{.*}} ; [ DW_TAG_auto_variable ] [self]
    value = .None
  }
  func isError() -> Bool {
    assert(value, "the object should not contain an error")
    return false
  }
}
