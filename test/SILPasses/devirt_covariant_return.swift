// RUN: %swift -O3 %s -emit-sil -sil-inline-threshold 1000 -sil-verify-all | FileCheck %s

// Make sure that we can dig all the way through the class hierarchy and
// protocol conformances with covariant return types correctly. The verifier
// should trip if we do not handle things correctly.

// CHECK-LABEL: sil @_TF23devirt_covariant_return6driverFT_T_ : $@thin () -> () {
// CHECK: bb0
// CHECK: alloc_ref
// CHECK: alloc_ref
// CHECK: alloc_ref
// CHECK: function_ref @unknown1a : $@thin () -> ()
// CHECK: apply
// CHECK: function_ref @defrenestrate : $@thin () -> ()
// CHECK: apply
// CHECK: function_ref @unknown2a : $@thin () -> ()
// CHECK: apply
// CHECK: apply
// CHECK: function_ref @unknown3a : $@thin () -> ()
// CHECK: apply
// CHECK: apply
// CHECK: strong_release
// CHECK: strong_release
// CHECK: strong_release
// CHECK: tuple
// CHECK: return

@asmname("unknown1a")
func unknown1a() -> ()
@asmname("unknown1b")
func unknown1b() -> ()
@asmname("unknown2a")
func unknown2a() -> ()
@asmname("unknown2b")
func unknown2b() -> ()
@asmname("unknown3a")
func unknown3a() -> ()
@asmname("unknown3b")
func unknown3b() -> ()
@asmname("defrenestrate")
func defrenestrate() -> ()

class B<T> {
  // We do not specialize typealias's correctly now.
  //typealias X = B
  func doSomething() -> B<T> {
    unknown1a()
    return self
  }

  // See comment in protocol P
  //class func doSomethingMeta() {
  //  unknown1b()
  //}

  func doSomethingElse() {
    defrenestrate()
  }
}

class B2<T> : B<T> {
  // When we have covariance in protocols, change this to B2.
  // We do not specialize typealias correctly now.
  //typealias X = B
  override func doSomething() -> B2<T> {
    unknown2a()
    return self
  }

  // See comment in protocol P
  //override class func doSomethingMeta() {
  //  unknown2b()
  //}
}

class B3<T> : B2<T> {
  override func doSomething() -> B3<T> {
    unknown3a()
    return self
  }
}

func WhatShouldIDo<T>(b : B<T>) -> B<T> {
  b.doSomething().doSomethingElse()
  return b
}

func doSomethingWithB<T>(b : B<T>) {
  
}

struct S {}

func driver() -> () {
  var b = B<S>()
  var b2 = B2<S>()
  var b3 = B3<S>()

  WhatShouldIDo(b)
  WhatShouldIDo(b2)
  WhatShouldIDo(b3)
}

driver()
