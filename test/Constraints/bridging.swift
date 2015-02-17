// RUN: %target-parse-verify-swift

// REQUIRES: objc_interop

import Foundation

class BridgedClass : NSObject, NSCopying { 
  func copyWithZone(zone: NSZone) -> AnyObject {
    return self
  }
}

class BridgedClassSub : BridgedClass { }

struct BridgedStruct : Hashable, _ObjectiveCBridgeable {
  var hashValue: Int { return 0 }

 static func _isBridgedToObjectiveC() -> Bool {
    return true
  }
  
  static func _getObjectiveCType() -> Any.Type {
    return BridgedClass.self
  }

  func _bridgeToObjectiveC() -> BridgedClass {
    return BridgedClass()
  }

  static func _forceBridgeFromObjectiveC(
    x: BridgedClass, 
    inout result: BridgedStruct?) {
  }

  static func _conditionallyBridgeFromObjectiveC(
    x: BridgedClass,
    inout result: BridgedStruct?
  ) -> Bool {
    return true
  }
}

func ==(x: BridgedStruct, y: BridgedStruct) -> Bool { return true }

struct NotBridgedStruct : Hashable { 
  var hashValue: Int { return 0 }
}

func ==(x: NotBridgedStruct, y: NotBridgedStruct) -> Bool { return true }

class OtherClass : Hashable { 
  var hashValue: Int { return 0 }
}
func ==(x: OtherClass, y: OtherClass) -> Bool { return true }

// Basic bridging
func bridgeToObjC(s: BridgedStruct) -> BridgedClass {
  return s
  return s as BridgedClass
}

func bridgeToAnyObject(s: BridgedStruct) -> AnyObject {
  return s
  return s as AnyObject
}

func bridgeFromObjC(c: BridgedClass) -> BridgedStruct {
  return c // expected-error{{'BridgedClass' is not convertible to 'BridgedStruct'}}
  return c as BridgedStruct
}

func bridgeFromObjCDerived(s: BridgedClassSub) -> BridgedStruct {
  return s // expected-error{{'BridgedClassSub' is not convertible to 'BridgedStruct'}}
  return s as BridgedStruct
}

// Array -> NSArray
func arrayToNSArray() {
  var nsa: NSArray

  nsa = [AnyObject]()
  nsa = [BridgedClass]()
  nsa = [OtherClass]()
  nsa = [BridgedStruct]()
  nsa = [NotBridgedStruct]() // expected-error{{cannot assign a value of type '[(NotBridgedStruct)]' to a value of type 'NSArray'}}

  nsa = [AnyObject]() as NSArray
  nsa = [BridgedClass]() as NSArray
  nsa = [OtherClass]() as NSArray
  nsa = [BridgedStruct]() as NSArray
  nsa = [NotBridgedStruct]() as NSArray // expected-error{{'[(NotBridgedStruct)]' is not convertible to 'NSArray'}}
}

// NSArray -> Array
func nsArrayToArray(nsa: NSArray) {
  var arr1: [AnyObject] = nsa // expected-error{{'NSArray' is not implicitly convertible to '[AnyObject]'; did you mean to use 'as' to explicitly convert?}}
  var arr2: [BridgedClass] = nsa // expected-error{{'NSArray' is not convertible to '[BridgedClass]'}}
  var arr3: [OtherClass] = nsa // expected-error{{'NSArray' is not convertible to '[OtherClass]'}}
  var arr4: [BridgedStruct] = nsa // expected-error{{'NSArray' is not convertible to '[BridgedStruct]'}}
  var arr5: [NotBridgedStruct] = nsa // expected-error{{'NSArray' is not convertible to '[NotBridgedStruct]'}}

  var arr1b: [AnyObject] = nsa as [AnyObject]
  var arr2b: [BridgedClass] = nsa as [BridgedClass] // expected-error{{'NSArray' is not convertible to '[BridgedClass]'; did you mean to use 'as!' to force downcast?}}
  var arr3b: [OtherClass] = nsa as [OtherClass] // expected-error{{'NSArray' is not convertible to '[OtherClass]'; did you mean to use 'as!' to force downcast?}}
  var arr4b: [BridgedStruct] = nsa as [BridgedStruct] // expected-error{{'NSArray' is not convertible to '[BridgedStruct]'; did you mean to use 'as!' to force downcast?}}
  var arr5b: [NotBridgedStruct] = nsa as [NotBridgedStruct] // expected-error{{'NSArray' is not convertible to '[NotBridgedStruct]'}}

  var arr6: Array = nsa as Array
  arr6 = arr1
  arr1 = arr6
}

func dictionaryToNSDictionary() {
  // FIXME: These diagnostics are awful.

  var nsd: NSDictionary

  nsd = [NSObject : AnyObject]()
  nsd = [NSObject : AnyObject]() as NSDictionary
  nsd = [NSObject : BridgedClass]()
  nsd = [NSObject : BridgedClass]() as NSDictionary
  nsd = [NSObject : OtherClass]()
  nsd = [NSObject : OtherClass]() as NSDictionary
  nsd = [NSObject : BridgedStruct]()
  nsd = [NSObject : BridgedStruct]() as NSDictionary
  nsd = [NSObject : NotBridgedStruct]() // expected-error{{cannot assign a value of type '[NSObject : NotBridgedStruct]' to a value of type 'NSDictionary'}}
  nsd = [NSObject : NotBridgedStruct]() as NSDictionary // expected-error{{'[NSObject : NotBridgedStruct]' is not convertible to 'NSDictionary'}}

  nsd = [NSObject : BridgedClass?]() // expected-error{{cannot assign a value of type '[NSObject : BridgedClass?]' to a value of type 'NSDictionary'}}
  nsd = [NSObject : BridgedClass?]() as NSDictionary // expected-error{{'[NSObject : BridgedClass?]' is not convertible to 'NSDictionary'}}
  nsd = [NSObject : BridgedStruct?]()  // expected-error{{cannot assign a value of type '[NSObject : BridgedStruct?]' to a value of type 'NSDictionary'}}
  nsd = [NSObject : BridgedStruct?]() as NSDictionary //expected-error{{'[NSObject : BridgedStruct?]' is not convertible to 'NSDictionary'}}

  nsd = [BridgedClass : AnyObject]()
  nsd = [BridgedClass : AnyObject]() as NSDictionary
  nsd = [OtherClass : AnyObject]()
  nsd = [OtherClass : AnyObject]() as NSDictionary
  nsd = [BridgedStruct : AnyObject]()
  nsd = [BridgedStruct : AnyObject]() as NSDictionary
  nsd = [NotBridgedStruct : AnyObject]()  // expected-error{{cannot assign a value of type '[NotBridgedStruct : AnyObject]' to a value of type 'NSDictionary'}}
  nsd = [NotBridgedStruct : AnyObject]() as NSDictionary  // expected-error{{'[NotBridgedStruct : AnyObject]' is not convertible to 'NSDictionary'}}

  // <rdar://problem/17134986>
  var bcOpt: BridgedClass?
  nsd = [BridgedStruct() : bcOpt] // expected-error{{cannot assign a value of type '[BridgedStruct : BridgedClass?]' to a value of type 'NSDictionary'}}
}

// In this case, we should not implicitly convert Dictionary to NSDictionary.
struct NotEquatable {}
func notEquatableError(d: Dictionary<Int, NotEquatable>) -> Bool {
  // FIXME: Another awful diagnostic.
  return d == d // expected-error{{binary operator '==' cannot be applied to two Dictionary<Int, NotEquatable> operands}}
}

// NSString -> String
var nss1 = "Some great text" as NSString
var nss2: NSString = ((nss1 as String) + ", Some more text") as NSString

// <rdar://problem/17943223>
var inferDouble = 1.0/10
let d: Double = 3.14159
inferDouble = d

// rdar://problem/17962491
var inferDouble2 = 1 % 3 / 3.0
let d2: Double = 3.14159
inferDouble2 = d2

// rdar://problem/18269449
var i1: Int = 1.5 * 3.5 // expected-error{{'Double' is not convertible to 'Int'}}

// rdar://problem/18330319
func rdar18330319(s: String, d: [String : AnyObject]) {
  let t = d[s] as! String?
}

// rdar://problem/19551164
func rdar19551164a(s: String, a: [String]) {}
func rdar19551164b(s: NSString, a: NSArray) {
  rdar19551164a(s, a) // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}}{{18-18= as String}}
  // expected-error@-1{{'NSArray' is not convertible to '[String]'; did you mean to use 'as!' to force downcast?}}{{21-21= as! [String]}}
}

// rdar://problem/19695671
func takesSet<T: Hashable>(p: Set<T>) {}
func takesDictionary<K: Hashable, V>(p: Dictionary<K, V>) {}
func takesArray<T>(t: Array<T>) {}
func rdar19695671() {
  takesSet(NSSet() as! Set) // expected-error{{'NSSet' is not convertible to 'Set<T>'}}
  takesDictionary(NSDictionary() as! Dictionary) // expected-error{{'NSDictionary' is not convertible to 'Dictionary<Key, Value>'}}
  takesArray(NSArray() as! Array) // expected-error{{'NSArray' is not convertible to 'Array<T>'}}
}


// This failed at one point while fixing rdar://problem/19600325.
func getArrayOfAnyObject(_: AnyObject) -> [AnyObject] { return [] }
func testCallback(f: (AnyObject) -> AnyObject?) {}
testCallback { return getArrayOfAnyObject($0) }

// <rdar://problem/19724719> Type checker thinks "(optionalNSString ?? nonoptionalNSString) as String" is a forced cast
func rdar19724719(f: (String) -> (), s1: NSString?, s2: NSString) {
  f((s1 ?? s2) as String)
}

// <rdar://problem/19770981>
func rdar19770981(s: String, ns: NSString) {
  func f(s: String) {}
  f(ns) // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}}{{7-7= as String}}
  f(ns as String)
  // 'as' has higher precedence than '>' so no parens are necessary with the fixit:
  s > ns // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}}{{9-9= as String}}
  s > ns as String
  ns > s // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}}{{5-5= as String}}
  ns as String > s

  // 'as' has lower precedence than '+' so add parens with the fixit:
  s + ns // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}}{{7-7=(}}{{9-9= as String)}}
  s + (ns as String)
  ns + s // expected-error{{'NSString' is not implicitly convertible to 'String'; did you mean to use 'as' to explicitly convert?}}{{3-3=(}}{{5-5= as String)}}
  (ns as String) + s
}

// <rdar://problem/19831919> Fixit offers as! conversions that are known to always fail
func rdar19831919() {
  var s1 = 1 + "str"; // expected-error{{binary operator '+' cannot be applied to operands of type 'Int' and 'String'}} expected-note{{overloads for '+' exist with these partially matching parameter lists: (Int, Int), (String, String), (Int, UnsafeMutablePointer<T>), (Int, UnsafePointer<T>)}}
}

// <rdar://problem/19831698> Incorrect 'as' fixits offered for invalid literal expressions
func rdar19831698() {
  var v70 = true + 1 // expected-error{{binary operator '+' cannot be applied to operands of type 'Bool' and 'Int'}} expected-note{{}}
  var v71 = true + 1.0 // expected-error{{binary operator '+' cannot be applied to operands of type 'Bool' and 'Double'}} expected-note{{}}
  var v72 = true + true // expected-error{{binary operator '+' cannot be applied to two Bool operands}}
  var v73 = true + [] // expected-error{{binary operator '+' cannot be applied to operands of type 'Bool' and 'NSArray'}}
  var v75 = true + "str" // expected-error{{binary operator '+' cannot be applied to operands of type 'Bool' and 'String'}} expected-note{{}}
}
