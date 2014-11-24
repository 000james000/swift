// RUN: %swift -target x86_64-apple-macosx10.9 -primary-file %s -emit-ir | FileCheck %s
// XFAIL: linux

// <rdar://problem/13793646>
struct OptionalStreamAdaptor<T: GeneratorType> {
  // CHECK: define hidden void @_TFV15generic_ternary21OptionalStreamAdaptor4nextUSs13GeneratorType_U__fRGS0_Q__FT_GSqQQ_7Element_(%Sq{{.*}}* noalias sret, %V15generic_ternary21OptionalStreamAdaptor* noalias, %swift.type* %Self)
  mutating
  func next() -> Optional<T.Element> {
    return x[0].next()
  }
  var x: [T]
}
