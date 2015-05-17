// RUN: %target-run-simple-swift | FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-runtime

// rdar://20920874 is tracking the fix for compiling this test optimized.
// XFAIL: swift_test_mode_optimize
// XFAIL: swift_test_mode_optimize_unchecked

// Note: JIT mode is checked in Interpreter/protocol_lookup_jit.swift.

protocol Fooable {
  func foo()
}

struct S: Fooable {
  func foo() { print("S") }
}

class C: Fooable {
  func foo() { print("C") }
}

class D: C {
  override func foo() { print("D") }
}

class E: D {
  override func foo() { print("E") }
}

struct X {}

extension Int: Fooable {
  func foo() { print("Int") }
}

func fooify<T>(x: T) {
  if let foo = x as? Fooable {
    foo.foo()
  } else {
    print("not fooable")
  }
}

struct G<T>: Fooable {
  func foo() { print("G") }
}

struct H<T> {}

fooify(1)   // CHECK:      Int
fooify(2)   // CHECK-NEXT: Int
fooify(S()) // CHECK-NEXT: S
fooify(S()) // CHECK-NEXT: S
fooify(C()) // CHECK-NEXT: C
fooify(C()) // CHECK-NEXT: C
fooify(D()) // CHECK-NEXT: D
fooify(D()) // CHECK-NEXT: D
fooify(E()) // CHECK-NEXT: E
fooify(E()) // CHECK-NEXT: E
fooify(X()) // CHECK-NEXT: not fooable
fooify(X()) // CHECK-NEXT: not fooable
fooify(G<Int>()) // CHECK-NEXT: G
fooify(G<Float>()) // CHECK-NEXT: G
fooify(G<Int>()) // CHECK-NEXT: G
fooify(H<Int>()) // CHECK-NEXT: not fooable
fooify(H<Float>()) // CHECK-NEXT: not fooable
fooify(H<Int>()) // CHECK-NEXT: not fooable

// TODO: generics w/ dependent witness tables

// Check casts from existentials.

fooify(1 as Any)   // CHECK:      Int
fooify(2 as Any)   // CHECK-NEXT: Int
fooify(S() as Any) // CHECK-NEXT: S
fooify(S() as Any) // CHECK-NEXT: S
fooify(C() as Any) // CHECK-NEXT: C
fooify(C() as Any) // CHECK-NEXT: C
fooify(D() as Any) // CHECK-NEXT: D
fooify(D() as Any) // CHECK-NEXT: D
fooify(E() as Any) // CHECK-NEXT: E
fooify(E() as Any) // CHECK-NEXT: E
fooify(X() as Any) // CHECK-NEXT: not fooable
fooify(X() as Any) // CHECK-NEXT: not fooable
fooify(G<Int>() as Any) // CHECK-NEXT: G
fooify(G<Float>() as Any) // CHECK-NEXT: G
fooify(G<Int>() as Any) // CHECK-NEXT: G
fooify(H<Int>() as Any) // CHECK-NEXT: not fooable
fooify(H<Float>() as Any) // CHECK-NEXT: not fooable
fooify(H<Int>() as Any) // CHECK-NEXT: not fooable

protocol Barrable {
  func bar()
}
extension Int: Barrable {
  func bar() { print("Int.bar") }
}

let foo: Fooable = 2
if let bar = foo as? Barrable {
  bar.bar() // CHECK-NEXT: Int.bar
} else {
  print("not barrable")
}

let foo2: Fooable = S()
if let bar2 = foo2 as? Barrable {
  bar2.bar()
} else {
  print("not barrable") // CHECK-NEXT: not barrable
}

protocol Runcible: class {
  func runce()
}

#if _runtime(_ObjC)
@objc protocol Fungible: class {
  func funge()
}
#endif

extension C: Runcible {
  func runce() { print("C") }
}

#if _runtime(_ObjC)
extension D: Fungible {
  @objc func funge() { print("D") }
}
#endif

let c1: AnyObject = C()
let c2: Any = C()
if let fruncible = c1 as? protocol<Fooable, Runcible> {
  fruncible.foo() // CHECK-NEXT: C
  fruncible.runce() // CHECK-NEXT: C
} else {
  print("not fooable and runcible")
}
if let fruncible = c2 as? protocol<Fooable, Runcible> {
  fruncible.foo() // CHECK-NEXT: C
  fruncible.runce() // CHECK-NEXT: C
} else {
  print("not fooable and runcible")
}

// Protocol lookup for metatypes.
protocol StaticFoo {
  static func foo() -> String
}

class StaticBar {
  class func mightHaveFoo() -> String {
    if let selfAsFoo = self as? StaticFoo.Type {
      return selfAsFoo.foo()
    } else {
      return "no Foo for you"
    }
  }
}

class StaticWibble : StaticBar, StaticFoo {
  static func foo() -> String { return "StaticWibble.foo" }
}

// CHECK: no Foo for you
print(StaticBar.mightHaveFoo())

// CHECK: StaticWibble.foo
print(StaticWibble.mightHaveFoo())

#if _runtime(_ObjC)
let d: D = D()
let d1: AnyObject = D()
let d2: Any = D()
if let frungible = d1 as? protocol<Fooable, Runcible, Fungible> {
  frungible.foo() // CHECK-objc-NEXT: D
  frungible.runce() // CHECK-objc-NEXT: C
  frungible.funge() // CHECK-objc-NEXT: D
} else {
  print("not fooable, runcible, and fungible")
}

let inttype: Any.Type = Int.self
if let frungibleType = inttype as? protocol<Fooable, Runcible, Fungible>.Type {
  print("is fooable, runcible, and fungible")
} else {
  print("not fooable, runcible, and fungible") // CHECK-objc-NEXT: not
}

let dtype: Any.Type = D.self
if let frungibleType = dtype as? protocol<Fooable, Runcible, Fungible>.Type {
  print("is fooable, runcible, and fungible") // CHECK-objc-NEXT: is
} else {
  print("not fooable, runcible, and fungible")
}

func genericCast<U: AnyObject>(x: AnyObject, _: U.Type) -> U? {
  return x as? U
}

if let fungible = genericCast(d, Fungible.self) {
  fungible.funge() // CHECK-objc-NEXT: D
} else {
  print("not fungible")
}
#endif

