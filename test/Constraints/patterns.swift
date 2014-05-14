// RUN: %swift -parse -verify %s

// Leaf expression patterns are matched to corresponding pieces of a switch
// subject (TODO: or ~= expression) using ~= overload resolution.
switch (1, 2.5, "three") {
case (1, _, _):
  ()
// Double is IntegerLiteralConvertible
case (_, 2, _),
     (_, 2.5, _),
     (_, _, "three"):
  ()
// ~= overloaded for (Range<Int>, Int)
case (0..10, _, _),
     (0..10, 2.5, "three"),
     (0...9, _, _),
     (0...9, 2.5, "three"):
  ()
}

switch (1, 2) {
case (var a, a): // expected-error {{use of unresolved identifier 'a'}}
  ()
}

// 'is' patterns can perform the same checks that 'is' expressions do.

protocol P { func p() }

class B : P { 
  init() {} 
  func p() {}
  func b() {}
}
class D : B {
  init() { super.init() } 
  func d() {}
}
class E {
  init() {} 
  func e() {}
}

struct S : P {
  func p() {}
  func s() {}
}

// Existential-to-concrete.
var bp : P = B()
switch bp {
case is B,
     is D,
     is S:
  ()
case is E: // expected-error {{cannot cast from protocol type 'P' to non-conforming type 'E'}}
  ()
}

switch bp {
case let b as B:
  b.b()
case let d as D:
  d.b()
  d.d()
case let s as S:
  s.s()
case let e as E: // expected-error {{cannot cast from protocol type 'P' to non-conforming type 'E'}}
  e.e()
}

// Super-to-subclass.
var db : B = D()
switch db {
case is D:
  ()
case is E: // expected-error {{downcast from 'B' to unrelated type 'E'}}
  ()
}

// Raise an error if pattern productions are used in expressions.
var b = var a // expected-error{{pattern variable binding cannot appear in an expression}}
var c = is Int // expected-error{{prefix 'is' pattern cannot appear in an expression}}

// TODO: Bad recovery in these cases. Although patterns are never valid
// expr-unary productions, it would be good to parse them anyway for recovery.
//var e = 2 + var y
//var e = var y + 2
