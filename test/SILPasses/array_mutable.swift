// RUN: %swift -O -emit-sil -primary-file %s | FileCheck %s
//
// Test Array "make_mutable" hoisting.  It's hard for FileCheck to
// recognize the hoisting because we don't know which block is the
// loop header. If this becomes to fragile, we could check debug-only
// output instead.

// CHECK-LABEL: sil hidden @_TF13array_mutable8inoutarrFRGSaSi_T_
// CHECK: %[[FR:[0-9]+]] = function_ref @_swift_isUniquelyReferenced
// CHECK-NOT: {{^bb}}
// CHECK: apply %[[FR]]
// CHECK: {{^bb}}
// CHECK-NOT: _swift_isUniquelyReferenced
// CHECK: [[VOID:%[^ ]+]] = tuple ()
// CHECK: return [[VOID]]
func inoutarr(inout a: [Int]) {
  for i in 0..<a.count {
    a[i] = 0
  }
}

struct S {
  var a: [Int]
}

// CHECK-LABEL: sil hidden @_TF13array_mutable6arreltFRVS_1ST_
// CHECK: %[[FR:[0-9]+]] = function_ref @_swift_isUniquelyReferenced
// CHECK-NOT: {{^bb}}
// CHECK: apply %[[FR]]
// CHECK: {{^bb}}
// CHECK-NOT: _swift_isUniquelyReferenced
// CHECK: {{^[}]}}
func arrelt(inout s: S) {
  for i in 0..<s.a.count {
    s.a[i] = 0
  }
}

// Check that we have an explicit retain before calling isUniquelyReferenced.
// <rdar:18109082> ARC: make _isUniquelyReferenced a barrier
// CHECK-LABEL: sil hidden @_TF13array_mutable7arrcopyFRGSaSi_Si
// CHECK: %[[FR:[0-9]+]] = function_ref @_swift_isUniquelyReferenced
// CHECK: retain_value
// CHECK: apply %[[FR]]
// CHECK: {{^bb1}}
func arrcopy(inout a: [Int]) -> Int {
  var b = a
  b[0] = 3
  return a[0] + b[0]
}
