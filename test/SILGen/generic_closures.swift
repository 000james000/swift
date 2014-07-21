// RUN: %swift -parse-stdlib -emit-silgen %s | FileCheck %s

typealias Int = Builtin.Int64
typealias Char = Builtin.Int32
typealias Bool = Builtin.Int1
var zero: Int

// CHECK-LABEL: sil  @_TF16generic_closures28generic_nondependent_context{{.*}}
func generic_nondependent_context<T>(x: T, var y: Int) -> Int {
  func foo() -> Int { return y }
  // CHECK: [[FOO:%.*]] = function_ref @_TFF16generic_closures28generic_nondependent_context{{.*}} : $@thin <τ_0_0> (@owned Builtin.NativeObject, @inout Builtin.Int64) -> Builtin.Int64
  // CHECK: [[FOO_CLOSURE:%.*]] = partial_apply [[FOO]]<T>
  return foo()
}

// CHECK-LABEL: sil  @_TF16generic_closures25generic_dependent_context{{.*}}
func generic_dependent_context<T>(x: T, y: Int) -> T {
  func foo() -> T { return x }
  // CHECK: [[FOO:%.*]] = function_ref @_TFF16generic_closures25generic_dependent_context{{.*}} : $@thin <τ_0_0> (@out τ_0_0, @owned Builtin.NativeObject, @inout τ_0_0) -> ()
  // CHECK: [[FOO_CLOSURE:%.*]] = partial_apply [[FOO]]<T>
  return foo()
}

enum Optionable<T> {
  case Some(T)
  case None
}

class NestedGeneric<U> {
  class func generic_nondependent_context<T>(x: T, y: Int, z: U) -> Int {
    func foo() -> Int { return y }
    return foo()
  }

  class func generic_dependent_inner_context<T>(x: T, y: Int, z: U) -> T {
    func foo() -> T { return x }
    return foo()
  }

  class func generic_dependent_outer_context<T>(x: T, y: Int, z: U) -> U {
    func foo() -> U { return z }
    return foo()
  }

  class func generic_dependent_both_contexts<T>(x: T, y: Int, z: U) -> (T, U) {
    func foo() -> (T, U) { return (x, z) }
    return foo()
  }

  // CHECK-LABEL: sil @_TFC16generic_closures13NestedGeneric20nested_reabstraction{{.*}}
  //   CHECK:       [[REABSTRACT:%.*]] = function_ref @_TTRG0_0_R_XFo__dT__XFo_iT__iT__
  //   CHECK:       partial_apply [[REABSTRACT]]<U, T>
  func nested_reabstraction<T>(x: T) -> Optionable<() -> ()> {
    return .Some({})
  }
}

// CHECK-LABEL: sil  @_TF16generic_closures24generic_curried_function{{.*}} : $@thin <T, U> (@in U, @in T) -> () {
// CHECK-LABEL: sil shared @_TF16generic_closures24generic_curried_function{{.*}}
func generic_curried_function<T, U>(x: T)(y: U) { }

var f: (Char) -> () = generic_curried_function(zero)

  // <rdar://problem/15417773>
  // Ensure that nested closures capture the generic parameters of their nested
  // context.

  // CHECK: sil @_TF16generic_closures25nested_closure_in_generic{{.*}} : $@thin <T> (@out T, @in T) -> ()
  // CHECK:   function_ref [[OUTER_CLOSURE:@_TFF16generic_closures25nested_closure_in_genericU__FQ_Q_U_FT_Q_]]
  // CHECK: sil shared [[OUTER_CLOSURE]] : $@thin <T> (@out T, @owned Builtin.NativeObject, @inout T) -> ()
  // CHECK:   function_ref [[INNER_CLOSURE:@_TFFF16generic_closures25nested_closure_in_genericU__FQ_Q_U_FT_Q_U_FT_Q_]]
  // CHECK: sil shared [[INNER_CLOSURE]] : $@thin <T> (@out T, @owned Builtin.NativeObject, @inout T) -> () {
  func nested_closure_in_generic<T>(x:T) -> T {
    return { { x }() }()
  }

// CHECK-LABEL: sil @_TF16generic_closures16local_properties
func local_properties<T>(inout t: T) {
  // CHECK: [[TBOX:%[0-9]+]] = alloc_box $T
  var prop: T {
    get {
      return t
    }
    set {
      t = newValue
    }
  }

  // CHECK: [[GETTER_REF:%[0-9]+]] = function_ref [[GETTER_CLOSURE:@_TFF16generic_closures16local_properties.*]] : $@thin <τ_0_0> (@out τ_0_0, @owned Builtin.NativeObject, @inout τ_0_0) -> ()
  // CHECK: [[GETTER:%[0-9]+]] = partial_apply [[GETTER_REF]]<T>([[TBOX]]#0, [[TBOX]]#1) : $@thin <τ_0_0> (@out τ_0_0, @owned Builtin.NativeObject, @inout τ_0_0) -> ()
  // CHECK: [[SETTER_REF:%[0-9]+]] = function_ref [[SETTER_CLOSURE:@_TFF16generic_closures16local_properties.*]] : $@thin <τ_0_0> (@in τ_0_0, @owned Builtin.NativeObject, @inout τ_0_0) -> ()
  // CHECK: [[SETTER:%[0-9]+]] = partial_apply [[SETTER_REF]]<T>(%1#0, %1#1) : $@thin <τ_0_0> (@in τ_0_0, @owned Builtin.NativeObject, @inout τ_0_0) -> ()
  
  // CHECK: apply [[GETTER]]
  t = prop

  // CHECK: apply [[SETTER]]
  prop = t

  var prop2: T {
    get {
      return t
    }
    set {
      // doesn't capture anything
    }
  }

  // CHECK: [[GETTER2_REF:%[0-9]+]] = function_ref [[GETTER2_CLOSURE:@_TFF16generic_closures16local_properties.*]] : $@thin <τ_0_0> (@out τ_0_0, @owned Builtin.NativeObject, @inout τ_0_0) -> ()
  // CHECK: [[GETTER2:%[0-9]+]] = partial_apply [[GETTER2_REF]]<T>([[TBOX]]#0, [[TBOX]]#1) : $@thin <τ_0_0> (@out τ_0_0, @owned Builtin.NativeObject, @inout τ_0_0) -> ()
  // CHECK: [[SETTER2_REF:%[0-9]+]] = function_ref [[SETTER2_CLOSURE:@_TFF16generic_closures16local_properties.*]] : $@thin <τ_0_0> (@in τ_0_0) -> ()
  // CHECK: [[SETTER2:%[0-9]+]] = partial_apply [[SETTER2_REF]]<T>()

  // CHECK: apply [[GETTER2]]
  t = prop2

  // CHECK: apply [[SETTER2]]
  prop2 = t
}

protocol Fooable {
  class func foo() -> Bool
}

// <rdar://problem/16399018>
func shmassert(f: @autoclosure () -> Bool) {}

// CHECK-LABEL: sil @_TF16generic_closures21capture_generic_param
func capture_generic_param<A: Fooable>(x: A) {
  shmassert(A.foo())
}
