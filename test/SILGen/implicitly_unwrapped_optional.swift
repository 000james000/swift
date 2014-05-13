// RUN: %swift -emit-silgen %s | FileCheck %s

func foo(var #f: (()->())!) {
  f?()
}
// CHECK:    sil @{{.*}}foo{{.*}} : $@thin (@owned ImplicitlyUnwrappedOptional<() -> ()>) -> () {
// CHECK:    bb0([[T0:%.*]] : $ImplicitlyUnwrappedOptional<() -> ()>):
// CHECK-NEXT: [[F:%.*]] = alloc_box $ImplicitlyUnwrappedOptional<() -> ()>
// CHECK-NEXT: store [[T0]] to [[F]]#1
// CHECK-NEXT: [[RESULT:%.*]] = alloc_stack $Optional<()>
// CHECK-NEXT: [[TEMP_RESULT:%.*]] = alloc_stack $()
//   Copy 'f' into a temporary.
// CHECK-NEXT: [[TEMP_OPTFN:%.*]] = alloc_stack $Optional<() -> ()>
// CHECK-NEXT: [[TEMP_FN:%.*]] = alloc_stack $@callee_owned (@out (), @in ()) -> ()
// CHECK-NEXT: [[TEMP_UOPTFN:%.*]] = alloc_stack $ImplicitlyUnwrappedOptional<() -> ()>
// CHECK-NEXT: copy_addr [[F]]#1 to [initialization] [[TEMP_UOPTFN]]#1
//   Switch out on the copied (() -> ())!:
// CHECK-NEXT: function_ref
// CHECK-NEXT: [[T0:%.*]] = function_ref @_TFSs41_doesImplicitlyUnwrappedOptionalHaveValueU__FRGSQQ__Bi1_ : $@thin <τ_0_0> (@inout ImplicitlyUnwrappedOptional<τ_0_0>) -> Builtin.Int1
// CHECK-NEXT: [[T1:%.*]] = apply [transparent] [[T0]]<() -> ()>([[TEMP_UOPTFN]]#1)
// CHECK-NEXT: cond_br [[T1]], bb2, bb1
//   If it doesn't have a value, kill all the temporaries and jump to
//   the first nothing block.
// CHECK:    bb1:
// CHECK-NEXT: destroy_addr [[TEMP_UOPTFN]]#1
// CHECK-NEXT: dealloc_stack [[TEMP_UOPTFN]]#0
// CHECK-NEXT: dealloc_stack [[TEMP_FN]]#0
// CHECK-NEXT: br bb3
//   If it does, pull the value out of the implicitly unwrapped optional...
// CHECK:    bb2:
// CHECK-NEXT: function_ref
// CHECK-NEXT: [[T0:%.*]] = function_ref @_TFSs36_getImplicitlyUnwrappedOptionalValueU__FGSQQ__Q_ : $@thin <τ_0_0> (@out τ_0_0, @in ImplicitlyUnwrappedOptional<τ_0_0>) -> ()
// CHECK-NEXT: [[TEMP_FN2:%.*]] = alloc_stack $@callee_owned (@out (), @in ()) -> ()
// CHECK-NEXT: apply [transparent] [[T0]]<() -> ()>([[TEMP_FN2]]#1, [[TEMP_UOPTFN]]#1)
// CHECK-NEXT: [[FN0:%.*]] = load [[TEMP_FN2]]#1
//   ...unnecessarily reabstract back to () -> ()...
// CHECK-NEXT: function_ref reabstraction thunk
// CHECK-NEXT: [[T0:%.*]] = function_ref @_TTRXFo_iT__iT__XFo__dT__ : $@thin (@owned @callee_owned (@out (), @in ()) -> ()) -> ()
// CHECK-NEXT: [[FN1:%.*]] = partial_apply [[T0]]([[FN0]])
//   ...and then back to (@out (), @in ()) -> ()...
// CHECK-NEXT: function_ref reabstraction thunk
// CHECK-NEXT: [[T0:%.*]] = function_ref @_TTRXFo__dT__XFo_iT__iT__ : $@thin (@out (), @in (), @owned @callee_owned () -> ()) -> ()
// CHECK-NEXT: [[FN2:%.*]] = partial_apply [[T0]]([[FN1]])
//   ...inject into an optional...
// CHECK-NEXT: store [[FN2]] to [[TEMP_FN]]
// CHECK-NEXT: // function_ref Swift._injectValueIntoOptional
// CHECK-NEXT: [[T0:%.*]] = function_ref @_TFSs24_injectValueIntoOptionalU__FQ_GSqQ__
// CHECK-NEXT: apply [transparent] [[T0]]<() -> ()>([[TEMP_OPTFN]]#1, [[TEMP_FN]]#1)
// CHECK-NEXT: dealloc_stack [[TEMP_FN2]]#0
// CHECK-NEXT: dealloc_stack [[TEMP_UOPTFN]]#0
// CHECK-NEXT: dealloc_stack [[TEMP_FN]]#0
// CHECK-NEXT: br bb4
//   (first nothing block)
// CHECK:    bb3:
// CHECK-NEXT: // function_ref Swift._injectNothingIntoOptional
// CHECK-NEXT: [[T0:%.*]] = function_ref @_TFSs26_injectNothingIntoOptionalU__FT_GSqQ__
// CHECK-NEXT: apply [transparent] [[T0]]<() -> ()>([[TEMP_OPTFN]]#1)
// CHECK-NEXT: br bb4
//   The rest of this is tested in optional.swift

func wrap<T>(#x: T) -> T! { return x }

// CHECK: sil @_TF29implicitly_unwrapped_optional16wrap_then_unwrapU__FT1xQ__Q_
func wrap_then_unwrap<T>(#x: T) -> T {
  // CHECK: [[FORCE:%.*]] = function_ref @_TFSs36_getImplicitlyUnwrappedOptionalValueU__FGSQQ__Q_
  // CHECK: apply [transparent] [[FORCE]]<{{.*}}>(%0, {{%.*}})
  return wrap(x: x)!
}

// CHECK: sil @_TF29implicitly_unwrapped_optional10tuple_bindFT1xGSQTSiSS___GSqSS_ : $@thin (@owned ImplicitlyUnwrappedOptional<(Int, String)>) -> @owned Optional<String> {
func tuple_bind(#x: (Int, String)!) -> String? {
  return x?.1
  // CHECK:   cond_br {{%.*}}, [[NONNULL:bb[0-9]+]], [[NULL:bb[0-9]+]]
  // CHECK: [[NONNULL]]:
  // CHECK:   [[STRING:%.*]] = tuple_extract {{%.*}} : $(Int, String), 1
  // CHECK-NOT: release_value [[STRING]]
}

func tuple_bind_implicitly_unwrapped(#x: (Int, String)!) -> String {
  return x.1
}

func return_any() -> AnyObject! { return nil }
func bind_any() {
  let object : AnyObject? = return_any()
}
