// RUN: %swift -parse %s -verify

typealias IntegerLiteralType = Int32

// Simple coercion of literals.
func simple() {
  var x1 : Int8 = 1
  var x2 : Int16 = 1
}


// Coercion of literals through operators.
func operators(x1: Int8) {
  var x2 : Int8 = 1 + 2
  var x3 : Int8 = 1 + x1
  var x4 : Int8 = x2 + 1
  var x5 : Int8 = x1 + x2 + 1 + 4 + x3 + 5
}

// Check coercion failure due to overflow.
struct X { }
struct Y { }
func accept_integer(x: Int8) -> X { } // expected-note 2{{found this candidate}}
func accept_integer(x: Int16) -> Y { } // expected-note 2{{found this candidate}}

func overflow_check() {
  var y : Y = accept_integer(500)

  accept_integer(17) // expected-error{{ambiguous use of 'accept_integer'}}
  accept_integer(1000000) // expected-error{{ambiguous use of 'accept_integer'}}
}

// Coercion chaining.
struct meters : IntegerLiteralConvertible { 
  var value : Int8
  
  init(_ value: Int8) {
    self.value = value
  }

  typealias IntegerLiteralType = Int8
  static func convertFromIntegerLiteral(value: Int8) -> meters {
    return meters(value)
  }
}

struct supermeters : IntegerLiteralConvertible { // expected-error{{type 'supermeters' does not conform to protocol 'IntegerLiteralConvertible'}}
  var value : meters
  
  typealias IntegerLiteralType = meters // expected-note{{possibly intended match 'IntegerLiteralType' does not conform to '_BuiltinIntegerLiteralConvertible'}}
  static func convertFromIntegerLiteral(value: meters) -> supermeters {
    return supermeters(value: value)
  }
}

func chaining() {
  var length : meters = 17;
  // FIXME: missing truncation warning <rdar://problem/14070127>.
  var long_length : meters = 500;
  var really_long_length : supermeters = 10
}

func memberaccess() {
  Int32(5.value)
  // FIXME: This should work
  var x : Int32 = 7.value // expected-error{{could not find member 'value'}}
}
