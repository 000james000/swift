// RUN: %target-swift-frontend %s -emit-ir | FileCheck %s

// REQUIRES: CPU=x86_64

// FIXME: Not a SIL test because we can't parse dynamic Self in SIL.
// <rdar://problem/16931299>

// CHECK: [[TYPE:[%a-zA-Z0-9]+]] = type <{ [8 x i8] }>

class C {
  class func fromMetatype() -> Self? { return nil }
  // CHECK-LABEL: define hidden i64 @_TZFC21dynamic_self_metadata1C12fromMetatypefMS0_FT_GSqDS0__(%swift.type*)
  // CHECK: [[ALLOCA:%[a-zA-Z0-9]+]] = alloca [[TYPE]], align 8
  // CHECK: [[CAST1:%[a-zA-Z0-9]+]] = bitcast [[TYPE]]* [[ALLOCA]] to i64*
  // CHECK: store i64 0, i64* [[CAST1]], align 8
  // CHECK: [[CAST2:%[a-zA-Z0-9]+]] = bitcast [[TYPE]]* [[ALLOCA]] to i64*
  // CHECK: [[LOAD:%[a-zA-Z0-9]+]] = load i64, i64* [[CAST2]], align 8
  // CHECK: ret i64 [[LOAD]]

  func fromInstance() -> Self? { return nil }
  // CHECK: [[ALLOCA:%[a-zA-Z0-9]+]] = alloca [[TYPE]], align 8
  // CHECK: [[CAST1:%[a-zA-Z0-9]+]] = bitcast [[TYPE]]* [[ALLOCA]] to i64*
  // CHECK: store i64 0, i64* [[CAST1]], align 8
  // CHECK: [[CAST2:%[a-zA-Z0-9]+]] = bitcast [[TYPE]]* [[ALLOCA]] to i64*
  // CHECK: [[LOAD:%[a-zA-Z0-9]+]] = load i64, i64* [[CAST2]], align 8
  // CHECK: ret i64 [[LOAD]]
}
