// RUN: %swift -target x86_64-apple-macosx10.9 -primary-file %s -emit-ir -g -o - | FileCheck %s
// XFAIL: linux

protocol IntegerArithmeticType {
  class func uncheckedSubtract(lhs: Self, rhs: Self) -> (Self, Bool)
}

protocol RandomAccessIndexType : IntegerArithmeticType {
  typealias Distance : IntegerArithmeticType
  class func uncheckedSubtract(lhs: Self, rhs: Self) -> (Distance, Bool)
}

// CHECK: metadata ![[TT:[^,]+]]} ; [ DW_TAG_structure_type ] [_TtTQQq_F9archetype16ExistentialTuple{{.*}}]
// archetype.ExistentialTuple <A : RandomAccessIndexType, B>(x : A, y : A) -> B
// CHECK: _TF9archetype16ExistentialTuple{{.*}} [ DW_TAG_subprogram ] [line [[@LINE+1]]] [def] [ExistentialTuple]
func ExistentialTuple<T: RandomAccessIndexType>(x: T, y: T) -> T.Distance {
  // (B, Swift.Bool)
  // CHECK: metadata !"0x100\00tmp\00[[@LINE+1]]\000"{{.*}}, metadata ![[TT]]} ; [ DW_TAG_auto_variable ] [tmp]
  var tmp : (T.Distance, Bool) = T.uncheckedSubtract(x, rhs: y)
  return _overflowChecked((tmp.0, tmp.1))
}

