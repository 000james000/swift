// RUN: %swift -parse %s -verify

var a : Int

func test() {
  var y : a   // expected-error {{use of undeclared type 'a'}} expected-note {{here}}
  var z : y   // expected-error {{'y' is not a type}}
}

var b : Int -> Int = {$0}

@autoclosure var c1 : () -> Int  // expected-error {{attribute can only be applied to types, not declarations}}
var c2 : (field : @autoclosure Int)  // expected-error {{attribute only applies to syntactic function types}}
// expected-error @-1{{cannot create a single-element tuple with an element label}}{{11-19=}}
var c3 : (field : @autoclosure Int -> Int)  // expected-error {{autoclosure argument type must be '()'}}
// expected-error @-1{{cannot create a single-element tuple with an element label}}{{11-19=}}

var d1 : (field : @autoclosure () -> Int)
// expected-error @-1{{cannot create a single-element tuple with an element label}}{{11-19=}}
var d2 : @autoclosure () -> Int = 4

var d3 : @autoclosure () -> Float =
   4

var d4 : @autoclosure () -> Int =
   d2 // expected-error{{function produces expected type 'Int'; did you mean to call it with '()'?}}

var e0 : [Int]
e0[] // expected-error {{cannot subscript a value of type '[Int]' with an index of type '()'}}

var f0 : [Float]
var f1 : [(Int,Int)]

var g : Swift // expected-error {{use of module 'Swift' as a type}}

var h0 : Int?
h0 == nil // no-warning
var h1 : Int??
h1! == nil // no-warning
var h2 : [Int?]
var h3 : [Int]?
var h3a : [[Int?]]
var h3b : [Int?]?
var h4 : ([Int])?
var h5 : ([([[Int??]])?])?
var h7 : (Int,Int)?
var h8 : (Int -> Int)?
var h9 : Int? -> Int?
var h10 : Int?.Type?.Type

var i = Int?(42)

var bad_io : (Int) -> (inout Int, Int)  // expected-error {{'inout' is only valid in parameter lists}}

func bad_io2(a: (inout Int, Int)) {}    // expected-error {{'inout' is only valid in parameter lists}}

// <rdar://problem/15588967> Array type sugar default construction syntax doesn't work
func test_array_construct<T>() {
  var a = [T]()   // 'T' is a local name
  var b = [Int]()  // 'Int is a global name'
  var c = [UnsafeMutablePointer<Int>]()  // UnsafeMutablePointer<Int> is a specialized name.
  var d = [UnsafeMutablePointer<Int?>]()  // Nesting.
  var e = [([UnsafeMutablePointer<Int>])]()
  var f = [(String, Float)]()

  
}

// <rdar://problem/15295763> default constructing an optional fails to typecheck
func test_optional_construct<T>() {
  var a = T?()    // Local name.
  var b = Int?()  // Global name
  var c = (Int?)() // Parenthesized name.
}

// Test disambiguation of generic parameter lists in expression context.

struct Gen<T> {}

var y0 : Gen<Int?>
var y1 : Gen<Int??>
var y2 : Gen<[Int?]>
var y3 : Gen<[Int]?>
var y3a : Gen<[[Int?]]>
var y3b : Gen<[Int?]?>
var y4 : Gen<([Int])?>
var y5 : Gen<([([[Int??]])?])?>
var y7 : Gen<(Int,Int)?>
var y8 : Gen<(Int -> Int)?>
var y8a : Gen<[[Int]? -> Int]>
var y9 : Gen<Int? -> Int?>
var y10 : Gen<Int?.Type?.Type>
var y11 : Gen<Gen<Int>?>
var y12 : Gen<Gen<Int>?>?
var y13 : Gen<Gen<Int?>?>?
var y14 : Gen<Gen<Int?>>?
var y15 : Gen<Gen<Gen<Int?>>?>
var y16 : Gen<Gen<Gen<Int?>?>>
var y17 : Gen<Gen<Gen<Int?>?>>?

var z0 = Gen<Int?>()
var z1 = Gen<Int??>()
var z2 = Gen<[Int?]>()
var z3 = Gen<[Int]?>()
var z3a = Gen<[[Int?]]>()
var z3b = Gen<[Int?]?>()
var z4 = Gen<([Int])?>()
var z5 = Gen<([([[Int??]])?])?>()
var z7 = Gen<(Int,Int)?>()
var z8 = Gen<(Int -> Int)?>()
var z8a = Gen<[[Int]? -> Int]>()
var z9 = Gen<Int? -> Int?>()
var z10 = Gen<Int?.Type?.Type>()
var z11 = Gen<Gen<Int>?>()
var z12 = Gen<Gen<Int>?>?()
var z13 = Gen<Gen<Int?>?>?()
var z14 = Gen<Gen<Int?>>?()
var z15 = Gen<Gen<Gen<Int?>>?>()
var z16 = Gen<Gen<Gen<Int?>?>>()
var z17 = Gen<Gen<Gen<Int?>?>>?()

y0  = z0 
y1  = z1 
y2  = z2 
y3  = z3 
y3a = z3a
y3b = z3b
y4  = z4 
y5  = z5 
y7  = z7 
y8  = z8 
y8a = z8a
y9  = z9 
y10 = z10
y11 = z11
y12 = z12
y13 = z13
y14 = z14
y15 = z15
y16 = z16
y17 = z17
