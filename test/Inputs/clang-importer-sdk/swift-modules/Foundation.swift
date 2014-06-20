@exported import ObjectiveC
@exported import CoreGraphics
@exported import Foundation

@asmname("swift_StringToNSString")
func _convertStringToNSString(string: String) -> NSString

@asmname("swift_NSStringToString")
func _convertNSStringToString(nsstring: NSString) -> String

struct NSZone {
  var pointer : COpaquePointer
}

func == (lhs: NSObject, rhs: NSObject) -> Bool {
  return lhs.isEqual(rhs)
}

let NSUTF8StringEncoding: UInt = 8

// NSArray bridging entry points
func _convertNSArrayToArray<T>(nsarr: NSArray) -> T[] {
  return T[]()
}

func _convertArrayToNSArray<T>(arr: T[]) -> NSArray {
  return NSArray()
}

// NSDictionary bridging entry points
func _convertDictionaryToNSDictionary<KeyType, ValueType>(
    d: Dictionary<KeyType, ValueType>
) -> NSDictionary {
  return NSDictionary()
}

func _convertNSDictionaryToDictionary<K: NSObject, V: AnyObject>(
       d: NSDictionary
     ) -> Dictionary<K, V> {
  return Dictionary<K, V>()
}

extension String : _BridgedToObjectiveC {
  static func getObjectiveCType() -> Any.Type {
    return NSString.self
  }
  func bridgeToObjectiveC() -> NSString {
    return NSString()
  }
  static func bridgeFromObjectiveC(x: NSString) -> String {
    _fatalError("implement")
  }
}

extension Int : _BridgedToObjectiveC {
  static func getObjectiveCType() -> Any.Type {
    return NSNumber.self
  }
  func bridgeToObjectiveC() -> NSNumber {
    return NSNumber()
  }
  static func bridgeFromObjectiveC(x: NSNumber) -> Int {
    _fatalError("implement")
  }
}

extension Array : _BridgedToObjectiveC {
  static func getObjectiveCType() -> Any.Type {
    return NSArray.self
  }
  func bridgeToObjectiveC() -> NSArray {
    return NSArray()
  }
  static func bridgeFromObjectiveC(x: NSArray) -> Array {
    return []
  }
}

extension Dictionary : _BridgedToObjectiveC {
  static func getObjectiveCType() -> Any.Type {
    return NSDictionary.self
  }
  func bridgeToObjectiveC() -> NSDictionary {
    return NSDictionary()
  }
  static func bridgeFromObjectiveC(x: NSDictionary) -> Dictionary {
    return [:]
  }
}
