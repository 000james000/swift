// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir -g -o - | FileCheck %s
// Don't crash when emitting debug info for anonymous variables.
// CHECK: variable{{.*}}[_]
protocol F_ {
  func successor() -> Self
}

protocol F : F_ {
  func ~> (Self, (_Distance, (Self))) -> Int
}

struct _Distance {}

func ~> <I: F_>(self_:I, (_Distance, (I))) -> Int {
  self_.successor()
  println("F")
  return 0
}
