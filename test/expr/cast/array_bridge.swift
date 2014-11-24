// RUN: %swift -parse %s -verify
// XFAIL: linux

class A {
  var x = 0
}

struct B : _ObjectiveCBridgeable {
  static func _isBridgedToObjectiveC() -> Bool {
    return true
  }
  
  static func _getObjectiveCType() -> Any.Type {
    return A.self
  }
  func _bridgeToObjectiveC() -> A {
    return A()
  }
  static func _forceBridgeFromObjectiveC(
    x: A,
    inout result: B?
  ){
  }
  static func _conditionallyBridgeFromObjectiveC(
    x: A,
    inout result: B?
  ) -> Bool {
    return true
  }
}

var a: [A] = []
var b: [B] = []

a = b

b = a // expected-error {{'A' is not identical to 'B'}}

var aa: [[A]] = []
var bb: [[B]] = []

aa = bb // expected-error {{'B' is not identical to 'A'}}

class C {
}

// In this case, bridged conversion should win
class E {
  var x = 0
}

struct F : _ObjectiveCBridgeable {
  static func _isBridgedToObjectiveC() -> Bool {
    return true
  }
  
  static func _getObjectiveCType() -> Any.Type {
    return E.self
  }
  func _bridgeToObjectiveC() -> E {
    return E()
  }
  static func _forceBridgeFromObjectiveC(
    x: E,
    inout result: F?
  ) {
  }
  static func _conditionallyBridgeFromObjectiveC(
    x: E,
    inout result: F?
  ) -> Bool {
    return true
  }
}

var e: [E] = []
var f: [F] = []

e = f
f = e // expected-error {{'E' is not identical to 'F'}}

class G {
  var x = 0
}

struct H : _ObjectiveCBridgeable {
  static func _getObjectiveCType() -> Any.Type {
    return G.self
  }
  func _bridgeToObjectiveC() -> G {
    return G()
  }
  static func _forceBridgeFromObjectiveC(
    x: G,
    inout result: H?
  ) {
  }
  static func _conditionallyBridgeFromObjectiveC(
    x: G,
    inout result: H?
  ) -> Bool {
    return true
  }
  static func _isBridgedToObjectiveC() -> Bool {
    return false
  }
}

var g: [G] = []
var h: [H] = []

g = h // should type check, but cause a failure at runtime


struct I : _ObjectiveCBridgeable {
  static func _isBridgedToObjectiveC() -> Bool {
    return true
  }
  
  static func _getObjectiveCType() -> Any.Type {
    return A.self
  }
  func _bridgeToObjectiveC() -> AnyObject {
    return A()
  }
  static func _forceBridgeFromObjectiveC(
    x: AnyObject,
    inout result: I?
  ) {
  }
  static func _conditionallyBridgeFromObjectiveC(
    x: AnyObject,
    inout result: I?
  ) -> Bool {
    return true
  }
}

var aoa: [AnyObject] = []
var i: [I] = []

aoa = i
i = aoa // expected-error {{'AnyObject' is not identical to 'I'}}
