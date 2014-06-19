// RUN: %target-run-simple-swift | FileCheck %s

// CHECK: testing...
println("testing...")

class C {}

func bridgedStatus<T>(_: T.Type) -> String {
  let bridged = isBridgedToObjectiveC(T.self)
  let verbatim = isBridgedVerbatimToObjectiveC(T.self)
  if !bridged && verbatim {
    return "IS NOT BRIDGED BUT IS VERBATIM?!"
  }
  return bridged ? 
    verbatim ? "is bridged verbatim" : "is custom-bridged"
    : "is unbridged"
}

func testBridging<T>(x: T, name: String) {
  println("\(name) \(bridgedStatus(T.self))")
  var b : String
  if let result: AnyObject = bridgeToObjectiveC(x) {
    b = "bridged as " + (
      (result as? C) ? "C" : (result as? T) ? "itself" : "an unknown type")
  }
  else {
    b = "did not bridge"
  }
  println("\(name) instance \(b)")
}

//===----------------------------------------------------------------------===//
struct BridgedValueType : _BridgedToObjectiveC {
  static func getObjectiveCType() -> Any.Type {
    return C.self
  }
  func bridgeToObjectiveC() -> C {
    return C()
  }
  static func bridgeFromObjectiveC(x: C) -> BridgedValueType? {
    _preconditionFailure("implement")
  }
}

// CHECK-NEXT: BridgedValueType is custom-bridged
// CHECK-NEXT: BridgedValueType instance bridged as C
testBridging(BridgedValueType(), "BridgedValueType")

//===----------------------------------------------------------------------===//
struct UnbridgedValueType {}

// CHECK-NEXT: UnbridgedValueType is unbridged
// CHECK-NEXT: UnbridgedValueType instance did not bridge
testBridging(UnbridgedValueType(), "UnbridgedValueType")
  
//===----------------------------------------------------------------------===//
class PlainClass {}

// CHECK-NEXT: PlainClass is bridged verbatim
// CHECK-NEXT: PlainClass instance bridged as itself
testBridging(PlainClass(), "PlainClass")

//===----------------------------------------------------------------------===//
struct ConditionallyBridged<T>
  : _BridgedToObjectiveC, _ConditionallyBridgedToObjectiveC {
  static func getObjectiveCType() -> Any.Type {
    return C.self
  }
  func bridgeToObjectiveC() -> C {
    return C()
  }
  static func bridgeFromObjectiveC(x: C) -> ConditionallyBridged<T>? {
    _preconditionFailure("implement")
  }
  static func bridgeFromObjectiveCConditional(x: C) -> ConditionallyBridged<T>?{
    _preconditionFailure("implement")
  }
  static func isBridgedToObjectiveC() -> Bool {
    return !((T.self as Any) as? String.Type)
  }
}

// CHECK-NEXT: ConditionallyBridged<Int> is custom-bridged
// CHECK-NEXT: ConditionallyBridged<Int> instance bridged as C
testBridging(ConditionallyBridged<Int>(), "ConditionallyBridged<Int>")

// CHECK-NEXT: ConditionallyBridged<String> is unbridged
// CHECK-NEXT: ConditionallyBridged<String> instance did not bridge
testBridging(
  ConditionallyBridged<String>(), "ConditionallyBridged<String>")

// CHECK-NEXT: done.
println("done.")
