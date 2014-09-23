// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -module-cache-path %t/clang-module-cache -target x86_64-apple-macosx10.9 -sdk %S/Inputs -I %S/Inputs -enable-source-import %s -emit-silgen -emit-verbose-sil | FileCheck %s

import Foundation

final class Foo {
  @objc func foo() {}
  // CHECK-LABEL: sil hidden @_TToFC10objc_final3Foo3foofS0_FT_T_ : $@cc(objc_method) @thin (Foo) -> ()

  @objc var prop: Int = 0
  // CHECK-LABEL: sil hidden [transparent] @_TToFC10objc_final3Foog4propSi
  // CHECK-LABEL: sil hidden [transparent] @_TToFC10objc_final3Foos4propSi
}

// CHECK-LABEL: sil hidden @_TF10objc_final7callFooFCS_3FooT_
func callFoo(x: Foo) {
  // Calls to the final @objc method statically reference the native entry
  // point.
  // CHECK: function_ref @_TFC10objc_final3Foo3foofS0_FT_T_
  x.foo()

  // Final @objc properties are still accessed directly.
  // CHECK: [[PROP:%.*]] = ref_element_addr {{%.*}} : $Foo, #Foo.prop
  // CHECK: load [[PROP]] : $*Int
  let prop = x.prop
  // CHECK: [[PROP:%.*]] = ref_element_addr {{%.*}} : $Foo, #Foo.prop
  // CHECK: assign {{%.*}} to [[PROP]] : $*Int
  x.prop = prop
}
