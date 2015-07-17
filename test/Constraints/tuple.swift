// RUN: %target-parse-verify-swift

// Test various tuple constraints.

func f0(x x: Int, y: Float) {}

var i : Int
var j : Int
var f : Float

func f1(y y: Float, rest: Int...) {}

func f2(_: (x: Int, y: Int) -> Int) {}
func f2xy(x x: Int, y: Int) -> Int {}
func f2ab(a a: Int, b: Int) -> Int {}
func f2yx(y y: Int, x: Int) -> Int {}

func f3(x: (x: Int, y: Int) -> ()) {}
func f3a(x: Int, y: Int) {}
func f3b(_: Int) {}

func f4(rest: Int...) {}
func f5(x: (Int, Int)) {}

func f6(_: (i: Int, j: Int), k: Int = 15) {}

//===--------------------------------------------------------------------===//
// Conversions and shuffles
//===--------------------------------------------------------------------===//

// Variadic functions.
f4()
f4(1)
f4(1, 2, 3)

f2(f2xy)
f2(f2ab)
f2(f2yx) // expected-error{{cannot invoke 'f2' with an argument list of type '((y: Int, x: Int) -> Int)'}} expected-note {{expected an argument list of type '((x: Int, y: Int) -> Int)'}}

f3(f3a)
f3(f3b) // expected-error{{cannot invoke 'f3' with an argument list of type '((Int) -> ())'}} expected-note{{expected an argument list of type '((x: Int, y: Int) -> ())'}}

func getIntFloat() -> (int: Int, float: Float) {}
var values = getIntFloat()
func wantFloat(_: Float) {}
wantFloat(values.float)

var e : (x: Int..., y: Int) // expected-error{{unexpected '...' before the end of a tuple list}}

typealias Interval = (a:Int, b:Int)
func takeInterval(x: Interval) {}
takeInterval(Interval(1, 2))

f5((1,1))

// Tuples with existentials
var any : Any = ()
any = (1, 2)
any = (label: 4)

// Scalars don't have .0/.1/etc
i = j.0 // expected-error{{'Int' does not have a member named '0'}}
any.1 // expected-error{{'Any' does not have a member named '1'}}

// Fun with tuples
protocol PosixErrorReturn {
  static func errorReturnValue() -> Self
}

extension Int : PosixErrorReturn {
  static func errorReturnValue() -> Int { return -1 }
}

func posixCantFail<A, T : protocol<Comparable, PosixErrorReturn>>
  (f:(A) -> T)(args:A) -> T
{
  let result = f(args)
  assert(result != T.errorReturnValue())
  return result
}

func open(name: String, oflag: Int) -> Int { }

var foo: Int = 0

var fd = posixCantFail(open)(args: ("foo", 0))

// Tuples and lvalues
class C {
  init() {}
  func f(_: C) {}
}

func testLValue(var c: C) {
  c.f(c)
  
  let x = c
  c = x
}


// <rdar://problem/21444509> Crash in TypeChecker::coercePatternToType
func invalidPatternCrash(let k : Int) {
  switch k {
  case (k, cph_: k) as UInt8:  // expected-error {{tuple pattern cannot match values of the non-tuple type 'UInt8'}} expected-warning {{cast from 'Int' to unrelated type 'UInt8' always fails}}
    break
  }
}

// <rdar://problem/21875219> Tuple to tuple conversion with IdentityExpr / AnyTryExpr hang
class Paws {
  init() throws {}
}

func scruff() -> (AnyObject?, ErrorType?) {
  do {
    return try (Paws(), nil)
  } catch {
    return (nil, error)
  }
}
