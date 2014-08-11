// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir | FileCheck %s

// CHECK: [[A:%C13generic_types1A]] = type <{ [[REF:%swift.refcounted]], [[INT:%Si]] }>
// CHECK: [[INT]] = type <{ i64 }>
// CHECK: [[B:%C13generic_types1B]] = type <{ [[REF:%swift.refcounted]], [[UNSAFE:%VSs20UnsafeMutablePointer]] }>
// CHECK: [[C:%C13generic_types1C]] = type
// CHECK: [[D:%C13generic_types1D]] = type

// CHECK: @_TMPdC13generic_types1A = global [[A_METADATA_T:{.*\* } }]] {
// CHECK:   %swift.type* (%swift.type_pattern*, i8**)* [[A_METADATA_CREATE:@[a-z0-9_]+]],
// CHECK:   i32 328,
// CHECK:   i16 1,
// CHECK:   i16 16,
// CHECK:   [8 x i8*] zeroinitializer,
// CHECK:   void ([[A]]*)* @_TFC13generic_types1AD,
// CHECK:   i8** @_TWVBo,
// CHECK:   i64 0,
// CHECK:   %objc_class* @"OBJC_CLASS_$_SwiftObject",
// CHECK:   %swift.opaque* @_objc_empty_cache,
// CHECK:   %swift.opaque* @_objc_empty_vtable,
// CHECK:   i64 1,
// CHECK:   i32 3,
// CHECK:   i32 0,
// CHECK:   i32 24,
// CHECK:   i16 7,
// CHECK:   i16 0,
// CHECK:   i32 136,
// CHECK:   i32 16,
// CHECK:   %swift.type* null,
// CHECK:   void (%swift.opaque*, [[A]]*)* @_TFC13generic_types1A3run
// CHECK:   %C13generic_types1A* (i64, %C13generic_types1A*)* @_TFC13generic_types1AcU__fMGS0_Q__FT1ySi_GS0_Q__
// CHECK: }
// CHECK: @_TMPdC13generic_types1B = global [[B_METADATA_T:{.* } }]] {
// CHECK:   %swift.type* (%swift.type_pattern*, i8**)* [[B_METADATA_CREATE:@[a-z0-9_]+]],
// CHECK:   i32 320,
// CHECK:   i16 1,
// CHECK:   i16 16,
// CHECK:   [8 x i8*] zeroinitializer,
// CHECK:   void ([[B]]*)* @_TFC13generic_types1BD,
// CHECK:   i8** @_TWVBo,
// CHECK:   i64 0,
// CHECK:   %objc_class* @"OBJC_CLASS_$_SwiftObject",
// CHECK:   %swift.opaque* @_objc_empty_cache,
// CHECK:   %swift.opaque* @_objc_empty_vtable,
// CHECK:   i64 1,
// CHECK:   i32 3,
// CHECK:   i32 0,
// CHECK:   i32 24,
// CHECK:   i16 7,
// CHECK:   i16 0,
// CHECK:   i32 128,
// CHECK:   i32 16,
// CHECK:   %swift.type* null
// CHECK: }
// CHECK: @_TMPdC13generic_types1C = global [[C_METADATA_T:{.*\* } }]] {
// CHECK:   void ([[C]]*)* @_TFC13generic_types1CD,
// CHECK:   i8** @_TWVBo,
// CHECK:   i64 0,
// CHECK:   %swift.type* null,
// CHECK:   %swift.opaque* @_objc_empty_cache,
// CHECK:   %swift.opaque* @_objc_empty_vtable,
// CHECK:   i64 1,
// CHECK:   void (%swift.opaque*, [[A]]*)* @_TFC13generic_types1A3run
// CHECK: }
// CHECK: @_TMPdC13generic_types1D = global [[D_METADATA_T:{.*\* } }]] {
// CHECK:   void ([[D]]*)* @_TFC13generic_types1DD,
// CHECK:   i8** @_TWVBo,
// CHECK:   i64 0,
// CHECK:   %swift.type* null,
// CHECK:   %swift.opaque* @_objc_empty_cache,
// CHECK:   %swift.opaque* @_objc_empty_vtable,
// CHECK:   i64 1,
// CHECK:   void (i64, [[D]]*)* @_TFC13generic_types1D3run
// CHECK: }

// CHECK: define internal %swift.type* [[A_METADATA_CREATE]](%swift.type_pattern*, i8**) {
// CHECK: entry:
// CHECK:   [[T0:%.*]] = load i8** %1
// CHECK:   %T = bitcast i8* [[T0]] to %swift.type*
// CHECK:   [[SUPER:%.*]] = call %objc_class* @swift_getInitializedObjCClass(%objc_class* @"OBJC_CLASS_$_SwiftObject")
// CHECK:   [[METADATA:%.*]] = call %swift.type* @swift_allocateGenericClassMetadata(%swift.type_pattern* %0, i8** %1, %objc_class* [[SUPER]])
// CHECK:   [[SELF_ARRAY:%.*]] = bitcast %swift.type* [[METADATA]] to i8**
// CHECK:   [[T0:%.*]] = bitcast %swift.type* %T to i8*
// CHECK:   [[T1:%.*]] = getelementptr inbounds i8** [[SELF_ARRAY]], i32 9
// CHECK:   store i8* [[T0]], i8** [[T1]], align 8
// CHECK:   ret %swift.type* [[METADATA]]
// CHECK: }

// CHECK: define internal %swift.type* [[B_METADATA_CREATE]](%swift.type_pattern*, i8**) {
// CHECK: entry:
// CHECK:   [[T0:%.*]] = load i8** %1
// CHECK:   %T = bitcast i8* [[T0]] to %swift.type*
// CHECK:   [[SUPER:%.*]] = call %objc_class* @swift_getInitializedObjCClass(%objc_class* @"OBJC_CLASS_$_SwiftObject")
// CHECK:   [[METADATA:%.*]] = call %swift.type* @swift_allocateGenericClassMetadata(%swift.type_pattern* %0, i8** %1, %objc_class* [[SUPER]])
// CHECK:   [[SELF_ARRAY:%.*]] = bitcast %swift.type* [[METADATA]] to i8**
// CHECK:   [[T0:%.*]] = bitcast %swift.type* %T to i8*
// CHECK:   [[T1:%.*]] = getelementptr inbounds i8** [[SELF_ARRAY]], i32 9
// CHECK:   store i8* [[T0]], i8** [[T1]], align 8
// CHECK:   ret %swift.type* [[METADATA]]
// CHECK: }

class A<T> {
  var x = 0

  func run(t: T) {}
  init(y : Int) {}
}

class B<T> {
  var ptr : UnsafeMutablePointer<T> = nil
  deinit {
    ptr.destroy()
  }
}

class C<T> : A<Int> {}

class D<T> : A<Int> {
  override func run(t: Int) {}
}

struct E<T> {
  var x : Int
  func foo() { bar() }
  func bar() {}
}
