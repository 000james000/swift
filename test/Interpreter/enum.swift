// RUN: rm -rf %t  &&  mkdir %t
// RUN: %target-build-swift -Xfrontend -enable-experimental-patterns %s -o %t/a.out
// RUN: %target-run %t/a.out | FileCheck %s

enum Singleton {
  case x(Int, UnicodeScalar)
}

enum NoPayload {
  case x
  case y
  case z
}

enum SinglePayloadTrivial {
  case x(UnicodeScalar, Int)
  case y
  case z
}

enum MultiPayloadTrivial {
  case x(UnicodeScalar, Int)
  case y(Int, Double)
  case z
}

var s = Singleton.x(1, "a")
switch s {
case .x(var int, var char):
  // CHECK: 1
  println(int)
  // CHECK: a
  println(char)
}

func printNoPayload(v: NoPayload) {
  switch v {
  case .x:
    println("NoPayload.x")
  case .y:
    println("NoPayload.y")
  case .z:
    println("NoPayload.z")
  }
}

// CHECK: NoPayload.x
printNoPayload(.x)
// CHECK: NoPayload.y
printNoPayload(.y)
// CHECK: NoPayload.z
printNoPayload(.z)

func printSinglePayloadTrivial(v: SinglePayloadTrivial) {
  switch v {
  case .x(var char, var int):
    println("SinglePayloadTrivial.x(\(char), \(int))")
  case .y:
    println("SinglePayloadTrivial.y")
  case .z:
    println("SinglePayloadTrivial.z")
  }
}

// CHECK: SinglePayloadTrivial.x(b, 2)
printSinglePayloadTrivial(.x("b", 2))
// CHECK: SinglePayloadTrivial.y
printSinglePayloadTrivial(.y)
// CHECK: SinglePayloadTrivial.z
printSinglePayloadTrivial(.z)

func printMultiPayloadTrivial(v: MultiPayloadTrivial) {
  switch v {
  case .x(var char, var int):
    println("MultiPayloadTrivial.x(\(char), \(int))")
  case .y(var int, var double):
    println("MultiPayloadTrivial.y(\(int), \(double))")
  case .z:
    println("MultiPayloadTrivial.z")
  }
}

// CHECK: MultiPayloadTrivial.x(c, 3)
printMultiPayloadTrivial(.x("c", 3))
// CHECK: MultiPayloadTrivial.y(4, 5.5)
printMultiPayloadTrivial(.y(4, 5.5))
// CHECK: MultiPayloadTrivial.z
printMultiPayloadTrivial(.z)

protocol Runcible {
  func runce()
}

struct Spoon : Runcible {
  var xxx = 0
  func runce() { println("Spoon!") }
}

struct Hat : Runcible {
  var xxx : Float = 0
  func runce() { println("Hat!") }
}

enum SinglePayloadAddressOnly {
  case x(Runcible)
  case y
}

func printSinglePayloadAddressOnly(v: SinglePayloadAddressOnly) {
  switch v {
  case .x(var runcible):
    runcible.runce()
  case .y:
    println("Why?")
  }
}

// CHECK: Spoon!
printSinglePayloadAddressOnly(.x(Spoon()))
// CHECK: Hat!
printSinglePayloadAddressOnly(.x(Hat()))
// CHECK: Why?
printSinglePayloadAddressOnly(.y)

enum MultiPayloadAddressOnly {
  case x(Runcible)
  case y(String, Runcible)
  case z
}

func printMultiPayloadAddressOnly(v: MultiPayloadAddressOnly) {
  switch v {
  case .x(var runcible):
    runcible.runce()
  case .y(var s, var runcible):
    print("\(s) ")
    runcible.runce()
  case .z:
    println("Zed.")
  }
}

// CHECK: Spoon!
printMultiPayloadAddressOnly(.x(Spoon()))
// CHECK: Porkpie Hat!
printMultiPayloadAddressOnly(.y("Porkpie", Hat()))
// CHECK: Zed.
printMultiPayloadAddressOnly(.z)

enum TrivialGeneric<T, U> {
  case x(T, U)
}

func unwrapTrivialGeneric<T, U>(tg: TrivialGeneric<T, U>) -> (T, U) {
  switch tg {
  case .x(var t, var u):
    return (t, u)
  }
}

func wrapTrivialGeneric<T, U>(t: T, u: U) -> TrivialGeneric<T, U> {
  return .x(t, u)
}

var tg : TrivialGeneric<Int, String> = .x(23, "skidoo")
// CHECK: 23 skidoo
switch tg {
case .x(var t, var u):
  println("\(t) \(u)")
}

// CHECK: 413 dream
switch unwrapTrivialGeneric(.x(413, "dream")) {
case (var t, var u):
  println("\(t) \(u)")
}

// CHECK: 1 is the loneliest number that you'll ever do
switch wrapTrivialGeneric(1, "is the loneliest number that you'll ever do") {
case .x(var t, var u):
  println("\(t) \(u)")
}

enum Ensemble<S : Runcible, H : Runcible> {
  case x(S, H)
}

func concreteEnsemble(e: Ensemble<Spoon, Hat>) {
  switch e {
  case .x(var spoon, var hat):
    spoon.runce()
    hat.runce()
  }
}

func genericEnsemble<T : Runcible, U : Runcible>(e: Ensemble<T, U>) {
  switch e {
  case .x(var t, var u):
    t.runce()
    u.runce()
  }
}

// CHECK: Spoon!
// CHECK: Hat!
concreteEnsemble(.x(Spoon(), Hat()))
// CHECK: Spoon!
// CHECK: Hat!
genericEnsemble(.x(Spoon(), Hat()))
// CHECK: Spoon!
// CHECK: Spoon!
genericEnsemble(.x(Spoon(), Spoon()))
// CHECK: Hat!
// CHECK: Spoon!
genericEnsemble(.x(Hat(), Spoon()))

enum Optionable<T> {
  case Mere(T)
  case Nought

  init() { self = .Nought }
  init(_ x:T) { self = .Mere(x) }
}

func tryRunce<T : Runcible>(x: Optionable<T>) {
  switch x {
  case .Mere(var r):
    r.runce()
  case .Nought:
    println("nought")
  }
}

// CHECK: Spoon!
tryRunce(.Mere(Spoon()))
// CHECK: Hat!
tryRunce(.Mere(Hat()))
// CHECK: nought
tryRunce(Optionable<Spoon>.Nought)
// CHECK: nought
tryRunce(Optionable<Hat>.Nought)
// CHECK: Spoon!
tryRunce(Optionable(Spoon()))
// CHECK: Hat!
tryRunce(Optionable(Hat()))

func optionableInts() {
  var optionables: Optionable<Int>[] = [
    .Mere(219),
    .Nought,
    .Nought,
    .Mere(20721)
  ]

  for o in optionables {
    switch o {
    case .Mere(var x):
      println(x)
    case .Nought:
      println("---")
    }
  }
}
// CHECK: 219
// CHECK: ---
// CHECK: ---
// CHECK: 20721
optionableInts()

enum Suit { case Spades, Hearts, Diamonds, Clubs }

func println(suit: Suit) {
  switch suit {
  case .Spades:
    println("♠")
  case .Hearts:
    println("♡")
  case .Diamonds:
    println("♢")
  case .Clubs:
    println("♣")
  }
}

func optionableSuits() {
  var optionables: Optionable<Suit>[] = [
    .Mere(.Spades),
    .Mere(.Diamonds),
    .Nought,
    .Mere(.Hearts)
  ]

  for o in optionables {
    switch o {
    case .Mere(var x):
      println(x)
    case .Nought:
      println("---")
    }
  }
}

// CHECK: ♠
// CHECK: ♢
// CHECK: ---
// CHECK: ♡
optionableSuits()

func optionableRuncibles<T : Runcible>(x: T) {
  var optionables: Optionable<T>[] = [
    .Mere(x),
    .Nought,
    .Mere(x),
    .Nought
  ]

  for o in optionables {
    switch o {
    case .Mere(var x):
      x.runce()
    case .Nought:
      println("---")
    }
  }
}
// CHECK: Spoon!
// CHECK: ---
// CHECK: Spoon!
// CHECK: ---
optionableRuncibles(Spoon())
// CHECK: Hat!
// CHECK: ---
// CHECK: Hat!
// CHECK: ---
optionableRuncibles(Hat())

// <rdar://problem/15383966>

@class_protocol protocol ClassProtocol {}

class Rdar15383966 : ClassProtocol
{
    var id : Int

    init(_ anID : Int) {
      println("X(\(anID))")
      id = anID
    }

    deinit {
       println("~X(\(id))")
    }
}


func generic<T>(x: T?) { }
func generic_over_classes<T : ClassProtocol>(x: T?) { }
func nongeneric(x: Rdar15383966?) { }

func test_generic()
{
    var x: Rdar15383966? = Rdar15383966(1)
    generic(x)
    x = .None
    generic(x)
}
// CHECK: X(1)
// CHECK: ~X(1)
test_generic()

func test_nongeneric()
{
    var x: Rdar15383966? = Rdar15383966(2)
    nongeneric(x)
    x = .None
    nongeneric(x)
}
// CHECK: X(2)
// CHECK: ~X(2)
test_nongeneric()

func test_generic_over_classes()
{
    var x: Rdar15383966? = Rdar15383966(3)
    generic_over_classes(x)
    x = .None
    generic_over_classes(x)
}
// CHECK: X(3)
// CHECK: ~X(3)
test_generic_over_classes()

struct S { 
  var a: Int32; var b: Int64 

  init(_ a: Int32, _ b: Int64) {
    self.a = a
    self.b = b
  }
}

enum MultiPayloadSpareBitAggregates {
  case x(Int32, Int64)
  case y(Rdar15383966, Rdar15383966)
  case z(S)
}

func test_spare_bit_aggregate(x: MultiPayloadSpareBitAggregates) {
  switch x {
  case .x(var i32, var i64):
    println(".x(\(i32), \(i64))")
  case .y(var a, var b):
    println(".y(\(a.id), \(b.id))")
  case .z(S(a: var a, b: var b)):
    println(".z(\(a), \(b))")
  }
}

println("---")
// CHECK: .x(22, 44)
test_spare_bit_aggregate(.x(22, 44))
// CHECK: X(222)
// CHECK: X(444)
// CHECK: .y(222, 444)
// CHECK: ~X(222)
// CHECK: ~X(444)
test_spare_bit_aggregate(.y(Rdar15383966(222), Rdar15383966(444)))
// CHECK: .z(333, 666)
test_spare_bit_aggregate(.z(S(333, 666)))

println("---")
struct OptionalTuple<T> {
  var value : (T, T)?

  init(_ value: (T, T)?) {
    self.value = value
  }
}
func test_optional_generic_tuple<T>(a: OptionalTuple<T>) -> T {
  println("optional pair is same size as pair: \(sizeofValue(a) == sizeof(T)*2)")
  return a.value!.0
}
println("Int result: \(test_optional_generic_tuple(OptionalTuple<Int>((5, 6))))")
// CHECK: optional pair is same size as pair: false
// CHECK: Int result: 5

class AnyOldClass {
  var x: Int
  init(_ value: Int) { x = value }
}
println("class result: \(test_optional_generic_tuple(OptionalTuple((AnyOldClass(10), AnyOldClass(11)))).x)")
// CHECK: optional pair is same size as pair: true
// CHECK: class result: 10
