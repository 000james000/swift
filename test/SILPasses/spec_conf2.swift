// RUN: %swift -O -disable-arc-opts -sil-inline-threshold 0 -emit-sil %s | FileCheck %s
// We can't deserialize apply_inst with subst lists. When radar://14443304
// is fixed then we should convert this test to a SIL test.
protocol P { func p() }
protocol Q { func q() }

class Foo: P, Q {
  func p() {}
  func q() {}
}

func inner_function<T : protocol<P, Q> >(#In : T) { }
func outer_function<T : protocol<P, Q> >(#In : T) { inner_function(In: In) }

//CHECK: sil shared @_TTSC10spec_conf23FooS0_S_1PS0_S_1Q___TF10spec_conf214outer_functionUS_1PS_1Q__FT2InQ__T_
//CHECK: function_ref @_TTSC10spec_conf23FooS0_S_1PS0_S_1Q___TF10spec_conf214inner_functionUS_1PS_1Q__FT2InQ__T_
//CHECK-NEXT: retain
//CHECK-NEXT: apply
//CHECK: return

outer_function(In: Foo())
