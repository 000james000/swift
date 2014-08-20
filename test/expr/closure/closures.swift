// RUN: %swift %s -verify

var func4 : (fn : @autoclosure () -> ()) -> ()
var func6 : (fn : (Int,Int) -> Int) -> ()


// Expressions can be auto-closurified, so that they can be evaluated separately
// from their definition.
var closure1 : @autoclosure () -> Int = 4  // Function producing 4 whenever it is called.
var closure2 : (Int,Int) -> Int = { 4 } // FIXME: expected-error{{tuple types '(Int, Int)' and '()' have a different number of elements (2 vs. 0)}}
var closure3a : ()->()->(Int,Int) = {{ (4, 2) }} // multi-level closing.
var closure3b : (Int,Int)->(Int)->(Int,Int) = {{ (4, 2) }} // FIXME: expected-error{{different number of elements}}
var closure4 : (Int,Int) -> Int = { $0 + $1 }
var closure5 : (Double) -> Int =
   { // expected-error{{'Double' is not a subtype of 'Int'}}
       $0 + 1.0 }

var closure6 = $0  // expected-error {{anonymous closure argument not contained in a closure}}

var closure7 : Int =
   { 4 }  // expected-error {{function produces expected type 'Int'; did you mean to call it with '()'?}}

func funcdecl1(a: Int, y: Int) {}
func funcdecl3() -> Int {}
func funcdecl4(a: ((Int)->Int), b: Int) {} // expected-note{{in initialization of parameter 'a'}}

func funcdecl5(a: Int, y: Int) {
  // Pass in a closure containing the call to funcdecl3.
  funcdecl4({ funcdecl3() }, 12) // FIXME: expected-error{{'Int' is not a subtype of '()'}}
  func6({$0 + $1})       // Closure with two named anonymous arguments
  func6({($0) + $1})    // Closure with sequence expr inferred type
  func6({($0) + $0})    // expected-error{{'UInt8' is not a subtype of 'Int'}}


  var testfunc : ((), Int) -> Int
  testfunc(          
           {$0+1})  // expected-error {{missing argument for parameter #2 in call}}

  funcdecl5(1, 2) // recursion.

  // Element access from a tuple.
  var a : (Int, f : Int, Int)
  var b = a.1+a.f

  // Tuple expressions with named elements.
  var i : (y : Int, x : Int) = (x : 42, y : 11)
  funcdecl1(123, 444)
  
  // Calls.
  4()  // expected-error {{invalid use of '()' to call a value of non-function type 'Int'}}
  
  
  // rdar://12017658 - Infer some argument types from func6.
  func6({ a, b -> Int in a+b})
  // Return type inference.
  func6({ a,b in a+b })
  
  // Infer incompatible type.
  // FIXME: Need to relate diagnostic to return type
  func6({a,b->Float in 4.0 })    // expected-error {{'Float' is not a subtype of 'Int'}}

  // Pattern doesn't need to name arguments.
  func6({ _,_ in 4 })
  
  var fn = {} // FIXME: maybe? expected-error{{cannot convert the expression's type '() -> () -> $T0' to type '() -> () -> $T0'}}
  var fn2 = { 4 }
  
  
  var c : Int = { a,b-> Int in a+b} // expected-error{{'(($T0, ($T0, $T1) -> Int) -> Int, (($T0, $T1) -> Int, $T1) -> Int) -> Int' is not convertible to 'Int'}}
  
  
}

// rdar://11935352 - closure with no body.
func closure_no_body(p: () -> ()) {
  return closure_no_body({})
}


// rdar://12019415
func t() {
  var u8 : UInt8
  var x : Bool

  if 0xA0..<0xBF ~= Int(u8) && x {
  }
}

// <rdar://problem/11927184>
func f0(a: Any) -> Int { return 1 }
assert(f0(1) == 1)


var selfRef = { selfRef() } // expected-error {{variable used within its own initial value}}
var nestedSelfRef = { // expected-error {{cannot convert the expression's type '() -> () -> $T0' to type '() -> () -> $T0'}}
  var recursive = { nestedSelfRef() } // expected-error {{variable used within its own initial value}}
  recursive()
}

var shadowed = { (shadowed: Int) -> Int in
  let x = shadowed
  return x
} // no-warning
var shadowedShort = { (shadowedShort: Int) -> Int in shadowedShort+1 } // no-warning


func anonymousClosureArgsInClosureWithArgs() {
  var a1 = { () in $0 } // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments}}
  var a2 = { () -> Int in $0 } // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments}}
  var a3 = { (z: Int) in $0 } // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments}}
}

func doStuff(fn : () -> Int) {}

// <rdar://problem/16193162> Require specifying self for locations in code where strong reference cycles are likely
class ExplicitSelfRequiredTest {
  var x = 42
  func method() -> Int {
    // explicit closure requires an explicit "self." base.
    doStuff({ ++self.x })
    doStuff({ ++x })    // expected-error {{reference to property 'x' in closure requires explicit 'self.' to make capture semantics explicit}}

    // autoclosure use is ok.
    func4(fn: print(x))

    // Methods follow the same rules as properties, uses of 'self' must be marked with "self."
    doStuff { method() }  // expected-error {{call to method 'method' in closure requires explicit 'self.' to make capture semantics explicit}}
    doStuff { self.method() }
    return 42
  }
}


// rdar://16393849 - 'var' and 'let' should be usable in closure argument lists.
var testClosureArgumentPatterns: (Int, Int) -> Int = { (var x, let y) in x+y+1 }


class SomeClass {
  var field : SomeClass?
  func foo() -> Int {}
}

func testCaptureBehavior(ptr : SomeClass) {
  // Test normal captures.
  weak var wv : SomeClass? = ptr
  unowned var uv : SomeClass = ptr
  unowned(unsafe) var uv1 : SomeClass = ptr
  unowned(safe) var uv2 : SomeClass = ptr
  doStuff { wv!.foo() }
  doStuff { uv.foo() }
  doStuff { uv1.foo() }
  doStuff { uv2.foo() }

  
  // Capture list tests
  var v1 : SomeClass? = ptr
  var v2 : SomeClass = ptr

  doStuff { [weak v1] in v1!.foo() }
  doStuff { [weak v1,                 // expected-note {{previous}}
             weak v1] in v1!.foo() }  // expected-error {{definition conflicts with previous value}}
  doStuff { [unowned v2] in v2.foo() }
  doStuff { [unowned(unsafe) v2] in v2.foo() }
  doStuff { [unowned(safe) v2] in v2.foo() }
  doStuff { [weak v1, weak v2] in v1!.foo() + v2!.foo() }

  var i = 42
  doStuff { [weak i] in i! }   // expected-error {{'weak' cannot be applied to non-class type 'Int'}}
}

extension SomeClass {
  func bar() {
    doStuff { [unowned self] in self.foo() }
    doStuff { [unowned xyz = self.field!] in xyz.foo() }
    doStuff { [weak xyz = self.field] in xyz!.foo() }

    // rdar://16889886 - Assert when trying to weak capture a property of self in a lazy closure
    doStuff { [weak self.field] in field!.foo() }   // expected-error {{fields may only be captured by assigning to a specific name}} expected-error {{reference to property 'field' in closure requires explicit 'self.' to make capture semantics explicit}}
    doStuff { [weak self&field] in 42 }  // expected-error {{expected ']' at end of capture list}}

  }
}


// <rdar://problem/16955318> Observed variable in a closure triggers an assertion
var closureWithObservedProperty: () -> () = {
  var a: Int = 42 {
  willSet {
    println("Will set a to \(newValue)")
  }
  didSet {
    println("Did set a with old value of \(oldValue)")
  }
  }
}

