// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir | FileCheck %s

// FIXME: Not a SIL test because we can't parse dynamic Self in SIL.
// <rdar://problem/16931299>

class C {
  class func fromMetatype() -> Self? { return nil }
  // CHECK-LABEL: define i64 @_TFC21dynamic_self_metadata1C12fromMetatypefMDS0_FT_GSqDS0__(%swift.type*)
  // CHECK:         call void @_TFSq21convertFromNilLiteralU__fMGSqQ__FT_GSqQ__(%Sq.16* noalias sret {{%.*}}, %swift.type* %0)

  func fromInstance() -> Self? { return nil }
  // CHECK-LABEL: define i64 @_TFC21dynamic_self_metadata1C12fromInstancefDS0_FT_GSqDS0__(%C21dynamic_self_metadata1C*)
  // CHECK:         [[OBJECT:%.*]] = bitcast %C21dynamic_self_metadata1C* %0 to %objc_object*
  // CHECK:         [[TYPE:%.*]] = call %swift.type* @swift_getObjectType(%objc_object* [[OBJECT]])
  // CHECK:         call void @_TFSq21convertFromNilLiteralU__fMGSqQ__FT_GSqQ__(%Sq.16* noalias sret {{%.*}}, %swift.type* [[TYPE]]
}



