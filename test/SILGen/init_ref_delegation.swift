// RUN: %swift -emit-silgen %s | FileCheck %s

struct X { }

// Initializer delegation within a struct.
struct S {
  // CHECK-LABEL: sil @_TFV19init_ref_delegation1SCfMS0_FT_S0_ : $@thin (@thin S.Type) -> S {
  init() {
    // CHECK: bb0([[SELF_META:%[0-9]+]] : $@thin S.Type):
    // CHECK-NEXT:   [[SELF_BOX:%[0-9]+]] = alloc_box $S
    // CHECK-NEXT:   [[SELF:%[0-9]+]] = mark_uninitialized [delegatingself] [[SELF_BOX]]#1 : $*S
    // CHECK-NEXT:   [[SELF_META:%[0-9]+]] = metatype $@thin S.Type
    
    // CHECK:   [[S_DELEG_INIT:%[0-9]+]] = function_ref @_TFV19init_ref_delegation1SCfMS0_FT1xVS_1X_S0_ : $@thin (X, @thin S.Type) -> S
    
    // CHECK:   [[X_CTOR:%[0-9]+]] = function_ref @_TFV19init_ref_delegation1XCfMS0_FT_S0_ : $@thin (@thin X.Type) -> X
    // CHECK-NEXT:   [[X_META:%[0-9]+]] = metatype $@thin X.Type
    // CHECK-NEXT:   [[X:%[0-9]+]] = apply [[X_CTOR]]([[X_META]]) : $@thin (@thin X.Type) -> X
    // CHECK-NEXT:   [[REPLACEMENT_SELF:%[0-9]+]] = apply [[S_DELEG_INIT]]([[X]], [[SELF_META]]) : $@thin (X, @thin S.Type) -> S
    self.init(x: X())
    // CHECK-NEXT:   assign [[REPLACEMENT_SELF]] to [[SELF]] : $*S
    // CHECK-NEXT:   [[SELF_BOX1:%[0-9]+]] = load [[SELF]] : $*S
    // CHECK-NEXT:   strong_release [[SELF_BOX]]#0 : $Builtin.NativeObject
    // CHECK-NEXT:   return [[SELF_BOX1]] : $S
  }

  init(x: X) { }
}

// Initializer delegation within an enum
enum E {
  // CHECK-LABEL: sil @_TFO19init_ref_delegation1ECfMS0_FT_S0_ : $@thin (@thin E.Type) -> E
  init() {
    // CHECK: bb0([[E_META:%[0-9]+]] : $@thin E.Type):
    // CHECK:   [[E_BOX:%[0-9]+]] = alloc_box $E
    // CHECK:   [[E_SELF:%[0-9]+]] = mark_uninitialized [delegatingself] [[E_BOX]]#1 : $*E
    // CHECK:   [[E_META:%[0-9]+]] = metatype $@thin E.Type

    // CHECK:   [[X_INIT:%[0-9]+]] = function_ref @_TFO19init_ref_delegation1ECfMS0_FT1xVS_1X_S0_ : $@thin (X, @thin E.Type) -> E

    // CHECK:   [[E_DELEG_INIT:%[0-9]+]] = function_ref @_TFV19init_ref_delegation1XCfMS0_FT_S0_ : $@thin (@thin X.Type) -> X
    // CHECK:   [[X_META:%[0-9]+]] = metatype $@thin X.Type
    // CHECK:   [[X:%[0-9]+]] = apply [[E_DELEG_INIT]]([[X_META]]) : $@thin (@thin X.Type) -> X
    // CHECK:   [[S:%[0-9]+]] = apply [[X_INIT]]([[X]], [[E_META]]) : $@thin (X, @thin E.Type) -> E
    // CHECK:   assign [[S:%[0-9]+]] to [[E_SELF]] : $*E
    // CHECK:   [[E_BOX1:%[0-9]+]] = load [[E_SELF]] : $*E
    self.init(x: X())
    // CHECK:   strong_release [[E_BOX:%[0-9]+]]#0 : $Builtin.NativeObject
    // CHECK:   return [[E_BOX1:%[0-9]+]] : $E
  }

  init(x: X) { }
}

// Initializer delegation to a generic initializer
struct S2 {
  // CHECK-LABEL: sil @_TFV19init_ref_delegation2S2CfMS0_FT_S0_ : $@thin (@thin S2.Type) -> S2
  init() {
    // CHECK: bb0([[S2_META:%[0-9]+]] : $@thin S2.Type):
    // CHECK:   [[SELF_BOX:%[0-9]+]] = alloc_box $S2
    // CHECK:   [[SELF:%[0-9]+]] = mark_uninitialized [delegatingself] [[SELF_BOX]]#1 : $*S2
    // CHECK:   [[S2_META:%[0-9]+]] = metatype $@thin S2.Type

    // CHECK:   [[S2_DELEG_INIT:%[0-9]+]] = function_ref @_TFV19init_ref_delegation2S2CfMS0_U__FT1tQ__S0_ : $@thin <τ_0_0> (@in τ_0_0, @thin S2.Type) -> S2

    // CHECK:   [[X_BOX:%[0-9]+]] = alloc_stack $X
    // CHECK:   [[X_INIT:%[0-9]+]] = function_ref @_TFV19init_ref_delegation1XCfMS0_FT_S0_ : $@thin (@thin X.Type) -> X
    // CHECK:   [[X_META:%[0-9]+]] = metatype $@thin X.Type
    // CHECK:   [[X:%[0-9]+]] = apply [[X_INIT]]([[X_META]]) : $@thin (@thin X.Type) -> X
    // CHECK:   store [[X]] to [[X_BOX]]#1 : $*X
    // CHECK:   [[SELF_BOX1:%[0-9]+]] = apply [[S2_DELEG_INIT]]<X>([[X_BOX]]#1, [[S2_META]]) : $@thin <τ_0_0> (@in τ_0_0, @thin S2.Type) -> S2
    // CHECK:   assign [[SELF_BOX1]] to [[SELF]] : $*S2
    // CHECK:   dealloc_stack [[X_BOX]]#0 : $*@local_storage X
    // CHECK:   [[SELF_BOX4:%[0-9]+]] = load [[SELF]] : $*S2
    self.init(t: X())
    // CHECK:   strong_release [[SELF_BOX]]#0 : $Builtin.NativeObject
    // CHECK:   return [[SELF_BOX4]] : $S2
  }

  init<T>(t: T) { }
}



class C1 {
  var ivar: X

 // CHECK-LABEL: sil @_TFC19init_ref_delegation2C1cfMS0_FT1xVS_1X_S0_ : $@cc(method) @thin (X, @owned C1) -> @owned C1
  convenience init(x: X) {
    // CHECK: bb0([[X:%[0-9]+]] : $X, [[ORIG_SELF:%[0-9]+]] : $C1):
    // CHECK:   [[SELF_BOX:%[0-9]+]] = alloc_box $C1
    // CHECK:   [[SELF:%[0-9]+]] = mark_uninitialized [delegatingself] [[ORIG_SELF]] : $C1
    // CHECK:   store [[SELF]] to [[SELF_BOX]]#1 : $*C1
    // CHECK:   [[SELF_FROM_BOX:%[0-9]+]] = load [[SELF_BOX]]#1 : $*C1

    // CHECK:   [[DELEG_INIT:%[0-9]+]] = class_method [[SELF_FROM_BOX]] : $C1, #C1.init!initializer.1 : C1.Type -> (x1: X, x2: X) -> C1 , $@cc(method) @thin (X, X, @owned C1) -> @owned C1
    // CHECK:   [[SELF:%[0-9]+]] = apply [[DELEG_INIT]]([[X]], [[X]], [[SELF_FROM_BOX]]) : $@cc(method) @thin (X, X, @owned C1) -> @owned C1
    // CHECK:   store [[SELF]] to [[SELF_BOX]]#1 : $*C1
    // CHECK:   [[SELF:%[0-9]+]] = load [[SELF_BOX]]#1 : $*C1
    // CHECK:   strong_retain [[SELF]] : $C1
    // CHECK:   strong_release [[SELF_BOX]]#0 : $Builtin.NativeObject
    // CHECK:   return [[SELF]] : $C1
    self.init(x1: x, x2: x)
  }

  init(x1: X, x2: X) { ivar = x1 }
}

@objc class C2 {
  var ivar: X

  // CHECK-LABEL: sil @_TFC19init_ref_delegation2C2cfMS0_FT1xVS_1X_S0_ : $@cc(method) @thin (X, @owned C2) -> @owned C2
  convenience init(x: X) {
    // CHECK: bb0([[X:%[0-9]+]] : $X, [[ORIG_SELF:%[0-9]+]] : $C2):
    // CHECK:   [[SELF_BOX:%[0-9]+]] = alloc_box $C2
    // CHECK:   [[UNINIT_SELF:%[0-9]+]] = mark_uninitialized [delegatingself] [[ORIG_SELF]] : $C2
    // CHECK:   store [[UNINIT_SELF]] to [[SELF_BOX]]#1 : $*C2
    // CHECK:   [[SELF:%[0-9]+]] = load [[SELF_BOX]]#1 : $*C2

    // CHECK:   [[DELEG_INIT:%[0-9]+]] = class_method [[SELF]] : $C2, #C2.init!initializer.1 : C2.Type -> (x1: X, x2: X) -> C2 , $@cc(method) @thin (X, X, @owned C2) -> @owned C2
    // CHECK:   [[REPLACE_SELF:%[0-9]+]] = apply [[DELEG_INIT]]([[X]], [[X]], [[SELF]]) : $@cc(method) @thin (X, X, @owned C2) -> @owned C2
    // CHECK:   store [[REPLACE_SELF]] to [[SELF_BOX]]#1 : $*C2
    // CHECK:   [[VAR_15:%[0-9]+]] = load [[SELF_BOX]]#1 : $*C2
    // CHECK:   strong_retain [[VAR_15]] : $C2
    // CHECK:   strong_release [[SELF_BOX]]#0 : $Builtin.NativeObject
    // CHECK:   return [[VAR_15]] : $C2
    self.init(x1: x, x2: x)
    // CHECK-NOT: sil @_TToFC19init_ref_delegation2C2cfMS_FT1xV19init_ref_delegation1X_S_ : $@cc(objc_method) @thin (X, @owned C2) -> @owned C2 {
  }

  // CHECK-LABEL: sil @_TFC19init_ref_delegation2C2CfMS0_FT2x1VS_1X2x2S1__S0_ : $@thin (X, X, @thick C2.Type) -> @owned C2 {
  // CHECK-NOT:   sil @_TToFC19init_ref_delegation2C2cfMS_FT2x1V19init_ref_delegation1X2x2S1__S_ : $@cc(objc_method) @thin (X, X, @owned C2) -> @owned C2 {
  init(x1: X, x2: X) { ivar = x1 }
}

var x: X = X()

class C3 {
  var i: Int = 5

  // CHECK-LABEL: sil @_TFC19init_ref_delegation2C3cfMS0_FT_S0_ : $@cc(method) @thin (@owned C3) -> @owned C3
  convenience init() {
    // CHECK: mark_uninitialized [delegatingself]
    // CHECK-NOT: integer_literal
    // CHECK: class_method [[SELF:%[0-9]+]] : $C3, #C3.init!initializer.1 : C3.Type -> (x: X) -> C3 , $@cc(method) @thin (X, @owned C3) -> @owned C3
    // CHECK-NOT: integer_literal
    // CHECK: return
    self.init(x: x)
  }

  init(x: X) { }
}

class C4 { }

extension C4 {
  convenience init(x1: X) {
    self.init()
  }
  // CHECK: sil @_TFC19init_ref_delegation2C4cfMS0_FT2x2VS_1X_S0_
  // CHECK: [[PEER:%[0-9]+]] = function_ref @_TFC19init_ref_delegation2C4cfMS0_FT2x1VS_1X_S0_
  // CHECK: apply [[PEER]]([[X:%[0-9]+]], [[OBJ:%[0-9]+]])
  convenience init(x2: X) {
    self.init(x1: x2)
  }
}
