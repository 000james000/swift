// RUN: %swift %s -verify -enable-character-literals

//===----------------------------------------------------------------------===//
// Tests and samples.
//===----------------------------------------------------------------------===//

// Comment.  With unicode characters: ¡ç®åz¥!

// Various function types.
var func1 : () -> ()    // No input, no output.
var func2 : (Int) -> Int
var func3 : () -> () -> ()                   // Takes nothing, returns a fn.
var func3a : () -> (() -> ())                // same as func3
var func4 : (fn : @auto_closure () -> ()) -> () // Takes a fn, returns nothing.
var func6 : (fn : (Int,Int) -> Int) -> ()    // Takes a fn, returns nothing.
var func7 : () -> (Int,Int,Int)              // Takes nothing, returns tuple.

// Top-Level expressions.  These are 'main' content.
func1()
4+7

var bind_test1 : () -> () = func1
var bind_test2 : Int = 4; func1 // expected-error {{expression resolves to an unused l-value}}

func basictest() {
  // Simple integer variables.
  var x : Int
  var x2 = 4              // Simple Type inference.
  var x3 = 4+x*(4+x2)/97  // Basic Expressions.

  // Declaring a variable Void, aka (), is fine too.
  var v : Void

  var x4 : Bool = true
  var x5 : Bool =
        4 // expected-error {{cannot convert the expression's type 'Int' to type 'Bool'}}

  //var x6 : Float = 4+5

  var x7 = 4; 5   // TODO: 5 should get a "unused expr" warning.

  // Test implicit conversion of integer literal to non-Int64 type.
  var x8 : Int8 = 4
  x8 = x8 + 1
  x8 + 1
  0 + x8
  1.0 + x8 // expected-error{{could not find an overload for '+' that accepts the supplied arguments}}
  var x9 : Int16 = x8 + 1 // expected-error{{could not find an overload for '+' that accepts the supplied arguments}}

  // Various tuple types.
  var tuple1 : ()
  var tuple2 : (Int)
  var tuple3 : (Int, Int, ())
  var tuple2a : (a : Int)
  var tuple3a : (a : Int, b : Int, c : ())

  var tuple4 = (1, 2)        // Tuple literal.
  var tuple5 = (1, 2, 3, 4)  // Tuple literal.
  var tuple6 = (1 2)  // expected-error {{expected ',' separator}} {{18-18=,}}

  // Brace expressions.
  // FIXME: Defaulting to () -> () for function expressions?
  var brace3 = { // expected-error{{cannot convert the expression's type '() -> $T0' to type '$T1'}}
    var brace2 = 42  // variable shadowing.
    brace2+7
  }

  // Function calls.
  var call1 : () = func1()
  var call2 = func2(1)
  var call3 : () = func3()()

  // Cannot call an integer.
  bind_test2() // expected-error {{invalid use of '()' to call a value of non-function type 'Int'}}
}

// Infix operators and attribute lists.
operator infix %% {
  associativity left
  precedence 2
}

func %%(a: Int, b: Int) -> () {}
var infixtest : () = 4 % 2 + 27 %% 123



// The 'func' keyword gives a nice simplification for function definitions.
func funcdecl1(a: Int, y: Int) {}
func funcdecl2() {
  return funcdecl1(4, 2)
}
func funcdecl3() -> Int {
  return 12
}
func funcdecl4(a: ((Int) -> Int), b: Int) {}
func signal(sig: Int, f: (Int) -> Void) -> (Int) -> Void {}

// Doing fun things with named arguments.  Basic stuff first.
func funcdecl6(a: Int, b: Int) -> Int { a+b }

// Can dive into tuples, 'b' is a reference to a whole tuple, c and d are
// fields in one.  Cannot dive into functions or through aliases.
func funcdecl7(a: Int, b: (c: Int, d: Int), third: (c: Int, d: Int)) -> Int {
  a + b.0 + b.c + third.0 + third.1
  b.foo // expected-error {{'(c: Int, d: Int)' does not have a member named 'foo'}}
}

// Error recovery.
func testfunc2 (_: ((), Int) -> Int) -> Int {}
func errorRecovery() {
  testfunc2({  // expected-error{{could not find an overload for '+' that accepts the supplied arguments}}
     $0 + 1})

  enum union1 {
    case bar
    case baz
  }
  var a: Int =
      .hello // expected-error {{'Int.Type' does not have a member named 'hello'}}
  var b: union1 = .bar // ok
  var c: union1 =
      .xyz  // expected-error {{'union1.Type' does not have a member named 'xyz'}}
  var d: (Int,Int,Int) =
      (1,2) // expected-error {{different number of elements}}
  var e: (Int,Int) =
      (1, 2, 3) // expected-error {{different number of elements}}

  var f: (Int,Int) =
      (1, 2, f : 3) // expected-error {{different number of elements}}
}

func acceptsInt(x: Int) {}
acceptsInt(unknown_var) // expected-error {{use of unresolved identifier 'unknown_var'}}

// TODO: Result can be named as well, but is writeonly.  Need to model lvalues
// and support the '=' operator.


// FIXME: Bogus error
var test1a: (Int) -> (Int) -> Int = { { $0 } } // expected-error{{'Int' is not a subtype of '()'}}
var test1b = { 42 }
var test1c = { { 42 } }
var test1d = { { { 42 } } }

func test2(a: Int)(b: Int) -> (c: Int) {
 a+b
 a+b+c // expected-error{{use of unresolved identifier 'c'}}
 return a+b
}


func test3(arg1: Int, arg2: Int) -> Int {
  return 4
}

func test4() -> ((arg1: Int, arg2: Int) -> Int) {
  return test3
}

func test5() {
  var a: (Int, Int)
  var
     b: ((Int) -> Int, Int) = a  // expected-error {{'Int' is not convertible to '(Int) -> Int'}}


  var c: (a: Int, b: Int)
  var d: (b: Int, a: Int) = c  // Ok, reshuffle tuple.
}


// Functions can obviously take and return values.
func w3(a: Int) -> Int { return a }
func w4(_: Int) -> Int { return 4 }



func b1() {}

func foo1(a: Int, b: Int) -> Int {}
func foo2(a: Int) -> (b: Int) -> Int {}
func foo3(a: Int = 2, b: Int = 3) {}

operator prefix ^^ {}

@prefix func ^^(a: Int) -> Int {
  return a + 1
}

func test_unary1() {
  var x: Int

  x = ^^(^^x)
  x = *x      // expected-error {{'*' is not a prefix unary operator}}
  x = x*      // expected-error {{'*' is not a postfix unary operator}}
  x = +(-x)
  x = + -x // expected-error {{unary operator cannot be separated from its operand}} {{8-9=}}
}
func test_unary2() {
  var x: Int
  // FIXME: second diagnostic is redundant.
  x = &; // expected-error {{expected expression after unary operator}} expected-error {{expected expression in assignment}}
}
func test_unary3() {
  var x: Int
  // FIXME: second diagnostic is redundant.
  x = &, // expected-error {{expected expression after unary operator}} expected-error {{expected expression in assignment}}
}

func test_as_1() {
  var x: Int
}
func test_as_2() {
  var x: Int
  x as [] // expected-error {{expected type after 'as'}}
}

func test_lambda() {
  // A simple closure.
  var a = { (value: Int) -> () in print(value+1) }

  // A recursive lambda.
  // FIXME: This should definitely be accepted.
  var fib = { (n: Int) -> Int in
    if (n < 2) {
      return n
    }
    
    return fib(n-1)+fib(n-2) // expected-error 2 {{variable used within its own initial value}}
  }
}

func test_lambda2() {
  { () -> protocol<Int> in // expected-error {{non-protocol type 'Int' cannot be used within 'protocol<...>'}}
    return 1
  }()
}

func test_floating_point() {
  var x = 0.0
  var y = 100.1
  var x1: Float = 0.0
  var x2: Double = 0.0
}

func test_nonassoc(x: Int, y: Int) -> Bool {
  // FIXME: the second error and note here should arguably disappear
  return x == y == x // expected-error {{non-associative operator is adjacent to operator of same precedence}}  expected-error {{could not find an overload for '==' that accepts the supplied arguments}}
}

func test_module_member() {
  var x = Swift.true
}

// More realistic examples.

func fib(n: Int) -> Int {
  if (n < 2) {
    return n
  }

  return fib(n-2) + fib(n-1)
}

//===----------------------------------------------------------------------===//
// Integer Literals
//===----------------------------------------------------------------------===//

// FIXME: Should warn about integer constants being too large <rdar://problem/14070127>
var
   il_a: Bool = 4  // expected-error {{cannot convert the expression's type 'Int' to type 'Bool'}}
var il_b: Int8
   = 123123
var il_c: Int8 = 4  // ok

struct int_test1 {
  // FIXME: Will be diagnosed when we switch to formal protocols
  static func convertFromIntegerLiteral() {}
}

var il_d: int_test1 = 4 // expected-error{{cannot convert the expression's type 'Int' to type 'int_test1'}}


struct int_test2 {
  static func convertFromIntegerLiteral(_: Int32) {}
  static func convertFromIntegerLiteral(_: Int8) {}
}

var il_e: int_test2 = 4 // expected-error {{cannot convert the expression's type 'Int' to type 'int_test2'}}

struct int_test3 {
  func convertFromIntegerLiteral() {}
}

var il_f: int_test3 = 4  // expected-error{{cannot convert the expression's type 'Int' to type 'int_test3'}}

struct int_test4 : IntegerLiteralConvertible {
  typealias IntegerLiteralType = Int
  static func convertFromIntegerLiteral(value: Int) -> int_test4 {} // user type.
}

var il_g: int_test4 = 4


// Defined conversion function's argument isn't compatible with literals.
struct int_test5 {
  static func convertFromIntegerLiteral(_: Bool) -> int_test5 {}
}

var il_h: int_test5 =
   4 //expected-error {{cannot convert the expression's type 'Int' to type 'int_test5'}}

// This just barely fits in Int64.
var il_i: Int64  = 18446744073709551615

// This constant is too large to fit in an Int64, but it is fine for Int128.
// FIXME: Should warn about the first. <rdar://problem/14070127>
var il_j: Int64  = 18446744073709551616
// var il_k: Int128 = 18446744073709551616

var bin_literal: Int64 = 0b100101
var hex_literal: Int64 = 0x100101
var oct_literal: Int64 = 0o100101

// verify that we're not using C rules
var oct_literal_test: Int64 = 0123
assert(oct_literal_test == 123)

// ensure that we swallow random invalid chars after the first invalid char
var invalid_num_literal: Int64 = 0QWERTY  // expected-error{{expected a digit after integer literal prefix}}
var invalid_bin_literal: Int64 = 0bQWERTY // expected-error{{expected a digit after integer literal prefix}}
var invalid_hex_literal: Int64 = 0xQWERTY // expected-error{{expected a digit after integer literal prefix}}
var invalid_oct_literal: Int64 = 0oQWERTY // expected-error{{expected a digit after integer literal prefix}}
var invalid_exp_literal: Double = 1.0e+QWERTY // expected-error{{expected a digit in floating point exponent}}

// rdar://11088443
var negative_int32: Int32 = -1

// <rdar://problem/11287167>
var tupleelemvar = 1
print((tupleelemvar, tupleelemvar).1)

func int_literals() {
  // Fits exactly in 64-bits - rdar://11297273
  var a = 1239123123123123
  // Overly large integer.
  // FIXME: Should warn about it. <rdar://problem/14070127>
  var b = 123912312312312312312
  
}

// <rdar://problem/12830375>
func tuple_of_rvalues(a:Int, b:Int) -> Int {
  return (a, b).1
}

extension Int {
  func testLexingMethodAfterIntLiteral() {}
  func _0() {}
}

123.testLexingMethodAfterIntLiteral()
0b101.testLexingMethodAfterIntLiteral()
0o123.testLexingMethodAfterIntLiteral()
0x1FFF.testLexingMethodAfterIntLiteral()

123._0()
0b101._0()
0o123._0()
0x1FFF._0()

var separator1: Int = 1_
var separator2: Int = 1_000
var separator4: Int = 0b1111_0000_
var separator5: Int = 0b1111_0000
var separator6: Int = 0o127_777_
var separator7: Int = 0o127_777
var separator8: Int = 0x12FF_FFFF
var separator9: Int = 0x12FF_FFFF_

//===----------------------------------------------------------------------===//
// Float Literals
//===----------------------------------------------------------------------===//

var fl_a = 0.0
var fl_b: Double = 1.0
var fl_c: Float = 2.0
// FIXME: crummy diagnostic
var fl_d: Float = 2.0.0 // expected-error {{expected named member of numeric literal}}
var fl_e: Float = 1.0e42
var fl_f: Float = 1.0e+  // expected-error {{expected a digit in floating point exponent}} 
var fl_g: Float = 1.0E+42
var fl_h: Float = 2e-42
var vl_i: Float = -.45   // expected-error {{expected initial value after '='}}
var fl_j: Float = 0x1p0
var fl_k: Float = 0x1.0p0
var fl_l: Float = 0x1.0 // expected-error {{hexadecimal floating point literal must end with an exponent}}
var fl_m: Float = 0x1.FFFFFEP-2
var fl_n: Float = 0x1.fffffep+2
var fl_o: Float = 0x1.fffffep+ // expected-error {{expected a digit in floating point exponent}}

var if1: Double = 1.0 + 4  // integer literal ok as double.
var if2: Float = 1.0 + 4  // integer literal ok as float.

var fl_separator1: Double = 1_.2_
var fl_separator2: Double = 1_000.2_
var fl_separator3: Double = 1_000.200_001
var fl_separator4: Double = 1_000.200_001e1_
var fl_separator5: Double = 1_000.200_001e1_000
var fl_separator6: Double = 1_000.200_001e1_000
var fl_separator7: Double = 0x1_.0FFF_p1_
var fl_separator8: Double = 0x1_0000.0FFF_ABCDp10_001

var fl_bad_separator1: Double = 1e_ // expected-error {{expected a digit in floating point exponent}}
var fl_bad_separator2: Double = 0x1p_ // expected-error {{expected a digit in floating point exponent}}

//===----------------------------------------------------------------------===//
// String Literals
//===----------------------------------------------------------------------===//

var st_a = ""
var st_b: String = ""
var st_c = "asdfasd    // expected-error {{unterminated string literal}}

var st_d = " \t\n\r\"\'\\  "  // Valid simple escapes
var st_e = " \x12\u0012\U00000078 "  // Valid unicode escapes
var st_u1 = " \x1 "  // expected-error {{\x escape sequence expects 2 hex digits to follow it}}
var st_u2 = " \u123 "  // expected-error {{\u escape sequence expects 4 hex digits to follow it}}
var st_u3 = " \U1234567 "  // expected-error {{\U escape sequence expects 8 hex digits to follow it}}
var st_u4 = " \q "  // expected-error {{invalid escape sequence in literal}}

var st_u5 = " \UFFFFFFFF "  // expected-error {{invalid unicode code point}}
var st_u6 = " \uD7FF \uE000 "  // Fencepost UTF16 surrogate pairs.
var st_u7 = " \uD800 "  // expected-error {{invalid unicode code point}}
var st_u8 = " \uDFFF "  // expected-error {{invalid unicode code point}}
var st_u9 = " \xFF "    // expected-error {{invalid hex escape}}
var st_u10 = " \U0010FFFD "  // Last valid codepoint, 0xFFFE and 0xFFFF are reserved in each plane
var st_u11 = " \U00110000 "  // expected-error {{invalid unicode code point}}

func stringliterals() {
 var ch_a = 'ab // expected-error {{unterminated character literal}}

  // rdar://11385385
  var x = 4
  "Hello \(x+1) world"
  
  "Error: \(x+1"; // expected-error {{unexpected '"' character in string interpolation}}
  
  "Error: \(x+1   // expected-error {{unterminated string literal}}
  ;

  // FIXME: bad diagnostics.
  /* expected-error {{unterminated string literal}} expected-error 2{{expected ',' separator}} expected-note {{to match this opening '('}}  */ var x2 : () = ("hello" + "
  ; // expected-error {{expected expression in list of expressions}}
} // expected-error {{expected ')' in expression list}}

//===----------------------------------------------------------------------===//
// InOut arguments
//===----------------------------------------------------------------------===//

func takesInt(x: Int) {} // expected-note{{in initialization of parameter 'x'}}
func takesExplicitInt(inout x: Int) { }

func testInOut(inout arg: Int) {
  var x: Int
  takesExplicitInt(x) // expected-error{{passing value of type 'Int' to an inout parameter requires explicit '&'}}
  takesExplicitInt(&x)
  takesInt(&x) // expected-error{{'inout Int' is not convertible to 'Int'}}
  var y = &x // expected-error{{reference to 'Int' not used to initialize a inout parameter}} \
             // expected-error {{type 'inout Int' of variable is not materializable}}
  var z = &arg // expected-error{{reference to 'Int' not used to initialize a inout parameter}} \
             // expected-error {{type 'inout Int' of variable is not materializable}}

  takesExplicitInt(5) // expected-error {{cannot convert the expression's type '()' to type 'inout Int'}}
}

//===----------------------------------------------------------------------===//
// Bool operators
//===----------------------------------------------------------------------===//

func boolTest(a: Bool, b: Bool) {
  var t1 = a != b
  var t2 = a == b
  var t3 = !a
  var t4 = ~a
}

//===----------------------------------------------------------------------===//
// Conversions
//===----------------------------------------------------------------------===//

var pi_f: Float
var pi_d: Double

struct SpecialPi {} // Type with no implicit construction.

var pi_s: SpecialPi

func getPi() -> Float {} // expected-note 3 {{found this candidate}}
func getPi() -> Double {} // expected-note 3 {{found this candidate}}
func getPi() -> SpecialPi {}

enum Empty { }

extension Empty {
  init(_ f: Float) { }
}

func conversionTest(inout a: Double, inout b: Int) {
  var f: Float
  var d: Double
  a = Double(b)
  a = Double(f)
  a = Double(d) // no-warning
  b = Int(a)
  f = Float(b)

  var pi_f1 = Float(pi_f)
  var pi_d1 = Double(pi_d)
  var pi_s1 = SpecialPi(pi_s) // expected-error {{cannot convert the expression's type 'SpecialPi' to type '$T3'}}

  var pi_f2 = Float(getPi()) // expected-error {{ambiguous use of 'getPi'}}
  var pi_d2 = Double(getPi()) // expected-error {{ambiguous use of 'getPi'}}
  var pi_s2: SpecialPi = getPi() // no-warning
  
  var float = Float.self
  var pi_f3 = float(getPi()) // expected-error {{ambiguous use of 'getPi'}}
  var pi_f4 = float(pi_f)

  var e = Empty(f)
  // FIXME: Need note pointing back to the initializer.
  var e2 = Empty(d) // expected-error{{'Double' is not convertible to 'Float'}}
  var e3 = Empty(Float(d))
}

struct Rule {
  var target: String
  var dependencies: String
}

var ruleVar: Rule
ruleVar = Rule("a") // expected-error {{cannot convert the expression's type '()' to type '(target: String, dependencies: String)'}}

//===----------------------------------------------------------------------===//
// Unary Operators
//===----------------------------------------------------------------------===//

func unaryOps(inout i8: Int8, inout i64: Int64) {
  i8 = ~i8
  ++i64
  --i8

  // FIXME: Weird diagnostic.
  ++Int64(5) // expected-error{{could not find an overload for '++' that accepts the supplied arguments}}
}

//===----------------------------------------------------------------------===//
// Iteration
//===----------------------------------------------------------------------===//

func..(x: Double, y: Double) -> Double {
   return x + y
}

func iterators() {
  var a = 0..42
  var b = 0.0..42.0
}

//===----------------------------------------------------------------------===//
// New expressions
//===----------------------------------------------------------------------===//

struct TypeWithoutSlice {}
struct TypeWithBadSlice {}
struct SliceTypeWithBadSlice {}
struct Dummy {}

func newTest() {
  var t1 = new Int[5]

  var i: Int
  var t2 = new Int[5][]
  var t3 = new Int[] // expected-error {{must specify length of array to allocate}}
  var t4 = new Int[5][i] // expected-error {{sized multidimensional arrays are not supported}}
  var t5 = new Int[i]

  var i2: Dummy
  var t6 = new Int[i2] // expected-error {{type 'Dummy' does not conform to protocol 'ArrayBound'}}

  var t9 = new Int[100+1]

  var t10 = new Int[5][5] // expected-error{{sized multidimensional arrays are not supported}}
}

func arraySubscript(inout a: Int[], i: Int, inout value: Int, inout aa: Int[][]) {
  a[i] = value
  value = a[i+1]

  aa[i][i+1] = value
  value = aa[i+1][i]
  aa[i] = a
  a = aa[i+1]
}

//===----------------------------------------------------------------------===//
// Magic literal expressions
//===----------------------------------------------------------------------===//

func magic_literals() {
  var x = __FILE__
  var y = __LINE__ + __COLUMN__
  var z: UInt8 = __LINE__ + __COLUMN__
}

//===----------------------------------------------------------------------===//
// lvalue processing
//===----------------------------------------------------------------------===//


operator infix +-+= {}
@assignment @infix func +-+= (inout x: Int, y: Int) -> Int { return 0}

func lvalue_processing() {
  var i = 0
  ++i   // obviously ok

  var fn = (+-+=)

  var n = 42
  fn(n, 12)  // expected-error {{passing value of type 'Int' to an inout parameter requires explicit '&'}}
  fn(&n, 12)

  n +-+= 12

  (+-+=)(&n, 12)  // ok.
  (+-+=)(n, 12) // expected-error {{passing value of type 'Int' to an inout parameter requires explicit '&'}}
}

struct Foo {
  func method() {}
}

func test() {
  var x = int_test3()

  // rdar://15708430
  (&x).convertFromIntegerLiteral()  // expected-error {{could not find member 'convertFromIntegerLiteral'}}
}



//===----------------------------------------------------------------------===//
// Collection Literals
//===----------------------------------------------------------------------===//

func arrayLiterals() { 
  var a = [1,2,3]
  var b : Int[] = []
  var c = []  // expected-error {{cannot convert the expression's type 'Array' to type '$T2'}}
}

func dictionaryLiterals() {
  var a = [1 : "foo",2 : "bar",3 : "baz"]
  var b : Dictionary<Int, String> = [:]
  var c = [:]  // expected-error {{cannot convert the expression's type 'Dictionary' to type '$T3'}}
}

func invalidDictionaryLiteral() {
  // FIXME: lots of unnecessary diagnostics.

  var a = [1: ??? // expected-error {{expected value in dictionary literal}} expected-error 2{{expected ',' separator}} expected-error {{expected key expression in dictionary literal}} expected-error {{expected ']' in container literal expression}} expected-note {{to match this opening '['}}
  var b = [1: ???] // expected-error {{expected value in dictionary literal}} expected-error 2{{expected ',' separator}} expected-error {{expected key expression in dictionary literal}}
  var c = [1: "one" ???] // expected-error {{expected key expression in dictionary literal}} expected-error 2{{expected ',' separator}} expected-error {{expected expression after '?' in ternary expression}}
  var d = [1: "one", ???] // expected-error {{expected key expression in dictionary literal}} expected-error {{expected ',' separator}}
  var e = [1: "one", 2] // expected-error {{expected ':' in dictionary literal}}
  var f = [1: "one", 2 ???] // expected-error {{expected expression after '?' in ternary expression}} expected-error 2{{expected ',' separator}} expected-error {{expected key expression in dictionary literal}}
  var g = [1: "one", 2: ???] // expected-error {{expected value in dictionary literal}} expected-error 2{{expected ',' separator}} expected-error {{expected key expression in dictionary literal}}
}

