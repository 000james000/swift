// RUN: rm -rf %t/clang-module-cache
// RUN: %swift %clang-importer-sdk -module-cache-path %t/clang-module-cache -I %S/Inputs/custom-modules -target x86_64-apple-macosx10.9 -emit-silgen -o - %s | FileCheck %s

import ObjectiveC
import Foundation
import objc_ext
import TestProtocols

class A {
  @objc func foo() -> String? {
    return ""
  }
// CHECK:    sil hidden @_TToFC8optional1A3foofS0_FT_GSqSS_ : $@cc(objc_method) @thin (A) -> @autoreleased Optional<NSString>
// CHECK:      [[T0:%.*]] = function_ref @_TFC8optional1A3foofS0_FT_GSqSS_
// CHECK-NEXT: [[T1:%.*]] = apply [[T0]](%0)
// CHECK-NEXT: [[TMP_OPTNSSTR:%.*]] = alloc_stack $Optional<NSString>
// CHECK-NEXT: [[TMP_OPTSTR:%.*]] = alloc_stack $Optional<String>
// CHECK-NEXT: store [[T1]] to [[TMP_OPTSTR]]#1
// CHECK:      [[T0:%.*]] = function_ref @_TFSs22_doesOptionalHaveValueU__FRGSqQ__Bi1_
// CHECK-NEXT: [[T1:%.*]] = apply [transparent] [[T0]]<String>([[TMP_OPTSTR]]#1
// CHECK-NEXT: cond_br [[T1]]
//   Something branch: project value, translate, inject into result.
// CHECK:      [[TMP_STR:%.*]] = unchecked_take_enum_data_addr [[TMP_OPTSTR]]
// CHECK-NEXT: [[STR:%.*]] = load [[TMP_STR]]
// CHECK:      [[T0:%.*]] = function_ref @swift_StringToNSString
// CHECK-NEXT: [[T1:%.*]] = apply [[T0]]([[STR]])
// CHECK-NEXT: [[TMP_NSSTR:%.*]] = init_enum_data_addr [[TMP_OPTNSSTR]]
// CHECK-NEXT: store [[T1]] to [[TMP_NSSTR]]
// CHECK-NEXT: inject_enum_addr [[TMP_OPTNSSTR]]{{.*}}Some
// CHECK-NEXT: br
//   Nothing branch: inject nothing into result.
// CHECK:      inject_enum_addr [[TMP_OPTNSSTR]]{{.*}}None
// CHECK-NEXT: br
//   Continuation.
// CHECK:      [[T0:%.*]] = load [[TMP_OPTNSSTR]]
// CHECK-NEXT: dealloc_stack [[TMP_OPTSTR]]
// CHECK-NEXT: dealloc_stack [[TMP_OPTNSSTR]]
// CHECK-NEXT: autorelease_return [[T0]]

  @objc func bar(#x : String?) {}
// CHECK:    sil hidden @_TToFC8optional1A3barfS0_FT1xGSqSS__T_ : $@cc(objc_method) @thin (Optional<NSString>, A) -> ()
// CHECK:      [[TMP_OPTSTR:%.*]] = alloc_stack $Optional<String>
// CHECK-NEXT: [[TMP_OPTNSSTR:%.*]] = alloc_stack $Optional<NSString>
// CHECK-NEXT: store {{.*}} to [[TMP_OPTNSSTR]]#1
// CHECK:      [[T0:%.*]] = function_ref @_TFSs22_doesOptionalHaveValueU__FRGSqQ__Bi1_
// CHECK-NEXT: [[T1:%.*]] = apply [transparent] [[T0]]<NSString>([[TMP_OPTNSSTR]]#1
// CHECK-NEXT: cond_br [[T1]]
//   Something branch: project value, translate, inject into result.
// CHECK:      [[TMP_NSSTR:%.*]] = unchecked_take_enum_data_addr [[TMP_OPTNSSTR]]
// CHECK-NEXT: [[NSSTR:%.*]] = load [[TMP_NSSTR]]
// CHECK:      [[T0:%.*]] = function_ref @swift_NSStringToString
//   Make a temporary initialized string that we're going to clobber as part of the conversion process (?).
// CHECK-NEXT: [[T1:%.*]] = apply [[T0]]([[NSSTR]])
// CHECK-NEXT: [[TMP_STR2:%.*]] = init_enum_data_addr [[TMP_OPTSTR]]
// CHECK-NEXT: store [[T1]] to [[TMP_STR2]]
// CHECK-NEXT: inject_enum_addr [[TMP_OPTSTR]]{{.*}}Some
// CHECK-NEXT: br
//   Nothing branch: inject nothing into result.
// CHECK:      inject_enum_addr [[TMP_OPTSTR]]{{.*}}None
// CHECK-NEXT: br
//   Continuation.
// CHECK:      [[T0:%.*]] = load [[TMP_OPTSTR]]
// CHECK-NEXT: dealloc_stack [[TMP_OPTNSSTR]]
// CHECK-NEXT: dealloc_stack [[TMP_OPTSTR]]
// CHECK:      [[T1:%.*]] = function_ref @_TFC8optional1A3barfS0_FT1xGSqSS__T_
// CHECK-NEXT: [[T2:%.*]] = apply [[T1]]([[T0]], %1)
// CHECK-NEXT: return [[T2]] : $()
}


// rdar://15144951
class TestWeak : NSObject {
  weak var b : WeakObject? = nil
}
class WeakObject : NSObject {}
