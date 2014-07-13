// RUN: %swift -emit-silgen %s | FileCheck %s

protocol P {
  func f() -> Self
}

protocol CP : class {
  func f() -> Self
}

class X : P, CP {
  required init(int i: Int) { }

  // CHECK-LABEL: sil @_TFC12dynamic_self1X1ffDS0_FT_DS0_ : $@cc(method) @thin (@owned X) -> @owned
  func f() -> Self { return self }

  // CHECK-LABEL: sil @_TFC12dynamic_self1X7factory{{.*}} : $@thin (Int, @thick X.Type) -> @owned X
  // CHECK: bb0([[I:%[0-9]+]] : $Int, [[SELF:%[0-9]+]] : $@thick X.Type):
  // CHECK: [[CTOR:%[0-9]+]] = class_method [[SELF]] : $@thick X.Type, #X.init!allocator.1 : X.Type -> (int: Int) -> X , $@thin (Int, @thick X.Type) -> @owned X
  // CHECK: apply [[CTOR]]([[I]], [[SELF]]) : $@thin (Int, @thick X.Type) -> @owned X
  class func factory(i: Int) -> Self { return self(int: i) }
}

class Y : X { 
  required init(int i: Int) { }
}

class GX<T> {
  func f() -> Self { return self }
}

class GY<T> : GX<[T]> { }

// CHECK-LABEL: sil @_TF12dynamic_self23testDynamicSelfDispatch{{.*}} : $@thin (@owned Y) -> ()
func testDynamicSelfDispatch(y: Y) {
// CHECK: bb0([[Y:%[0-9]+]] : $Y):
// CHECK:   [[Y_AS_X:%[0-9]+]] = upcast [[Y]] : $Y to $X
// CHECK:   [[X_F:%[0-9]+]] = class_method [[Y_AS_X]] : $X, #X.f!1 : Self -> () -> Self , $@cc(method) @thin (@owned X) -> @owned X
// CHECK:   [[X_RESULT:%[0-9]+]] = apply [[X_F]]([[Y_AS_X]]) : $@cc(method) @thin (@owned X) -> @owned X
// CHECK:   [[Y_RESULT:%[0-9]+]] = unchecked_ref_cast [[X_RESULT]] : $X to $Y
// CHECK:   strong_release [[Y_RESULT]] : $Y
  y.f()
}

// CHECK-LABEL: sil @_TF12dynamic_self30testDynamicSelfDispatchGeneric{{.*}} : $@thin (@owned GY<Int>) -> ()
func testDynamicSelfDispatchGeneric(gy: GY<Int>) {
  // CHECK: bb0([[GY:%[0-9]+]] : $GY<Int>):
  // CHECK:   [[GY_AS_GX:%[0-9]+]] = upcast [[GY]] : $GY<Int> to $GX<Array<Int>>
  // CHECK:   [[GX_F:%[0-9]+]] = class_method [[GY_AS_GX]] : $GX<Array<Int>>, #GX.f!1 : <T> Self -> () -> Self , $@cc(method) @thin <τ_0_0> (@owned GX<τ_0_0>) -> @owned GX<τ_0_0>
  // CHECK:   [[GX_RESULT:%[0-9]+]] = apply [[GX_F]]<[Int]>([[GY_AS_GX]]) : $@cc(method) @thin <τ_0_0> (@owned GX<τ_0_0>) -> @owned GX<τ_0_0>
  // CHECK:   [[GY_RESULT:%[0-9]+]] = unchecked_ref_cast [[GX_RESULT]] : $GX<Array<Int>> to $GY<Int>
  // CHECK:   strong_release [[GY_RESULT]] : $GY<Int>
  gy.f()
}

// CHECK-LABEL: sil @_TF12dynamic_self21testArchetypeDispatch{{.*}} : $@thin <T where T : P> (@in T) -> ()
func testArchetypeDispatch<T: P>(t: T) {
  // CHECK: bb0([[T:%[0-9]+]] : $*T):
  // CHECK:   [[ARCHETYPE_F:%[0-9]+]] = witness_method $T, #P.f!1 : $@cc(witness_method) @thin <τ_0_0 where τ_0_0 : P> (@out τ_0_0, @inout τ_0_0) -> ()
  // CHECK:   [[T_RESULT:%[0-9]+]] = alloc_stack $T
  // CHECK:   [[SELF_RESULT:%[0-9]+]] = apply [[ARCHETYPE_F]]<T>([[T_RESULT]]#1, [[T]]) : $@cc(witness_method) @thin <τ_0_0 where τ_0_0 : P> (@out τ_0_0, @inout τ_0_0) -> ()
  t.f()
}

// CHECK-LABEL: sil @_TF12dynamic_self23testExistentialDispatch{{.*}}
func testExistentialDispatch(p: P) {
// CHECK: bb0([[P:%[0-9]+]] : $*P):
// CHECK:   [[PCOPY:%[0-9]+]] = alloc_stack $P
// CHECK:   copy_addr [[P]] to [initialization] [[PCOPY]]#1 : $*P
// CHECK:   [[PCOPY_ADDR:%[0-9]+]] = open_existential [[PCOPY]]#1 : $*P to $*@opened([[N:[0-9]+]]) P
// CHECK:   [[P_RESULT:%[0-9]+]] = alloc_stack $P
// CHECK:   [[P_RESULT_ADDR:%[0-9]+]] = init_existential [[P_RESULT]]#1 : $*P, $*@opened([[N]]) P
// CHECK:   [[P_F_METHOD:%[0-9]+]] = witness_method $@opened([[N]]) P, #P.f!1 : $@cc(witness_method) @thin <τ_0_0 where τ_0_0 : P> (@out τ_0_0, @inout τ_0_0) -> ()
// CHECK:   apply [[P_F_METHOD]]<@opened([[N]]) P>([[P_RESULT_ADDR]], [[PCOPY_ADDR]]) : $@cc(witness_method) @thin <τ_0_0 where τ_0_0 : P> (@out τ_0_0, @inout τ_0_0) -> ()
// CHECK:   destroy_addr [[P_RESULT]]#1 : $*P
// CHECK:   destroy_addr [[PCOPY_ADDR]] : $*@opened([[N]]) P
// CHECK:   dealloc_stack [[P_RESULT]]#0 : $*@local_storage P
// CHECK:   dealloc_stack [[PCOPY]]#0 : $*@local_storage P
// CHECK:   destroy_addr [[P]] : $*P
  p.f()
}

// CHECK-LABEL: sil @_TF12dynamic_self28testExistentialDispatchClass{{.*}} : $@thin (@owned CP) -> ()
func testExistentialDispatchClass(cp: CP) {
// CHECK: bb0([[CP:%[0-9]+]] : $CP):
// CHECK:   strong_retain [[CP]] : $CP
// CHECK:   [[CP_ADDR:%[0-9]+]] = open_existential_ref [[CP]] : $CP to $@opened([[N:[0-9]+]]) CP
// CHECK:   [[CP_F:%[0-9]+]] = witness_method $@opened([[N]]) CP, #CP.f!1 : $@cc(witness_method) @thin <τ_0_0 where τ_0_0 : CP> (@owned τ_0_0) -> @owned τ_0_0
// CHECK:   [[CP_F_RESULT:%[0-9]+]] = apply [[CP_F]]<@opened([[N]]) CP>([[CP_ADDR]]) : $@cc(witness_method) @thin <τ_0_0 where τ_0_0 : CP> (@owned τ_0_0) -> @owned τ_0_0
// CHECK:   [[RESULT_EXISTENTIAL:%[0-9]+]] = init_existential_ref [[CP_F_RESULT]] : $@opened([[N]]) CP, $CP
// CHECK:   strong_release [[CP_F_RESULT]] : $@opened([[N]]) CP
// CHECK:   strong_release [[CP]] : $CP
  cp.f()
}

@objc class ObjC {
  @objc func method() -> Self { return self }
}

// CHECK-LABEL: sil @_TF12dynamic_self21testAnyObjectDispatch{{.*}} : $@thin (@owned AnyObject) -> ()
func testAnyObjectDispatch(o: AnyObject) {
  // CHECK: dynamic_method_br [[O_OBJ:%[0-9]+]] : $Builtin.UnknownObject, #ObjC.method!1.foreign, bb1, bb2

  // CHECK: bb1([[METHOD:%[0-9]+]] : $@cc(objc_method) @thin (Builtin.UnknownObject) -> @autoreleased AnyObject):
  // CHECK:   [[VAR_9:%[0-9]+]] = partial_apply [[METHOD]]([[O_OBJ]]) : $@cc(objc_method) @thin (Builtin.UnknownObject) -> @autoreleased AnyObject
  var x = o.method
}

// <rdar://problem/16270889> Dispatch through ObjC metatypes.
class ObjCInit {
  @objc required init() { }
}

// CHECK: sil @_TF12dynamic_self12testObjCInit{{.*}} : $@thin (@thick ObjCInit.Type) -> ()
func testObjCInit(meta: ObjCInit.Type) {
// CHECK: bb0([[THICK_META:%[0-9]+]] : $@thick ObjCInit.Type):
// CHECK:   [[O:%[0-9]+]] = alloc_box $ObjCInit
// CHECK:   [[OBJC_META:%[0-9]+]] = thick_to_objc_metatype [[THICK_META]] : $@thick ObjCInit.Type to $@objc_metatype ObjCInit.Type
// CHECK:   [[OBJ:%[0-9]+]] = alloc_ref_dynamic [objc] [[OBJC_META]] : $@objc_metatype ObjCInit.Type, $ObjCInit
// CHECK:   [[INIT:%[0-9]+]] = class_method [volatile] [[OBJ]] : $ObjCInit, #ObjCInit.init!initializer.1.foreign : ObjCInit.Type -> () -> ObjCInit , $@cc(objc_method) @thin (@owned ObjCInit) -> @owned ObjCInit
// CHECK:   [[RESULT_OBJ:%[0-9]+]] = apply [[INIT]]([[OBJ]]) : $@cc(objc_method) @thin (@owned ObjCInit) -> @owned ObjCInit
// CHECK:   store [[RESULT_OBJ]] to [[O]]#1 : $*ObjCInit
// CHECK:   strong_release [[O]]#0 : $Builtin.NativeObject
// CHECK:   [[RESULT:%[0-9]+]] = tuple ()
// CHECK:   return [[RESULT]] : $()
  var o = meta()
}

class OptionalResult {
  func foo() -> Self? { return self }
}
// CHECK-LABEL: sil @_TFC12dynamic_self14OptionalResult3foofDS0_FT_GSqDS0__ : $@cc(method) @thin (@owned OptionalResult) -> @owned Optional<OptionalResult>
// CHECK:      [[T0:%.*]] = function_ref @_TFSs24_injectValueIntoOptionalU__FQ_GSqQ__ :
// CHECK-NEXT: apply [transparent] [[T0]]<OptionalResult>([[T1:%.*]]#1, [[T2:%.*]]#1)
// CHECK-NEXT: dealloc_stack [[T2]]#0
// CHECK-NEXT: [[T0:%.*]] = load [[T1]]#1
// CHECK-NEXT: dealloc_stack [[T1]]#0
// CHECK-NEXT: strong_release %0
// CHECK-NEXT: return [[T0]] : $Optional<OptionalResult>

class OptionalResultInheritor : OptionalResult {
  func bar() {}
}

func testOptionalResult(v : OptionalResultInheritor) {
  v.foo()?.bar()
}
// CHECK-LABEL: sil @_TF12dynamic_self18testOptionalResult{{.*}} : $@thin (@owned OptionalResultInheritor) -> ()
// CHECK:      [[T0:%.*]] = class_method [[V:%.*]] : $OptionalResult, #OptionalResult.foo!1 : Self -> () -> Self? , $@cc(method) @thin (@owned OptionalResult) -> @owned Optional<OptionalResult>
// CHECK-NEXT: apply [[T0]]([[V]])
// CHECK:      [[T0:%.*]] = function_ref @_TFSs22_doesOptionalHaveValueU__FRGSqQ__Bi1_
// CHECK-NEXT: apply [transparent] [[T0]]<OptionalResult>
// CHECK:      [[T0:%.*]] = function_ref @_TFSs17_getOptionalValueU__FGSqQ__Q_
// CHECK-NEXT: [[T1:%.*]] = alloc_stack $OptionalResult
// CHECK-NEXT: apply [transparent] [[T0]]<OptionalResult>([[T1]]#1,
// CHECK-NEXT: [[T2:%.*]] = load [[T1]]#1
// CHECK-NEXT: [[T4:%.*]] = unchecked_ref_cast [[T2]] : $OptionalResult to $OptionalResultInheritor
// CHECK:      [[T0:%.*]] = function_ref @_TFSs24_injectValueIntoOptionalU__FQ_GSqQ__ :
// CHECK-NEXT: apply [transparent] [[T0]]<OptionalResultInheritor>

// CHECK-LABEL: sil_witness_table X: P module dynamic_self {
// CHECK: method #P.f!1: @_TTWC12dynamic_self1XS_1PFS1_1fUS1___fRQPS1_FT_S2_

// CHECK-LABEL: sil_witness_table X: CP module dynamic_self {
// CHECK: method #CP.f!1: @_TTWC12dynamic_self1XS_2CPFS1_1fUS1___fQPS1_FT_S2_
