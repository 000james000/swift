// RUN: %swift %clang-importer-sdk -parse -parse-as-library -verify -target x86_64-apple-macosx10.9 %s

import Foundation

extension String {
  func onlyOnString() -> String { return self }
}

extension Bool {
  func onlyOnBool() -> Bool { return self }
}

extension Array {
  func onlyOnArray() { }
}

extension Dictionary {
  func onlyOnDictionary() { }
}

func foo() {
  var sf : (String!) -> String? = NSStringToNSString
  var s : String = NSArray().nsstringProperty.onlyOnString()

  var bf : (Bool) -> Bool = BOOLtoBOOL
  var b : Bool = NSArray().boolProperty.onlyOnBool()

  var af: (Array<AnyObject>!) -> (Array<AnyObject>!) = arrayToArray
  NSArray().arrayProperty.onlyOnArray()

  var df: (Dictionary<NSObject, AnyObject>!) -> Dictionary<NSObject, AnyObject>!
    = dictToDict
  NSArray().dictProperty.onlyOnDictionary()
}

func allocateMagic(zone: NSZone) -> UnsafeMutablePointer<Void> {
  return allocate(zone)
}

func constPointerToObjC(objects: [AnyObject?]) -> NSArray {
  return NSArray(objects: objects, count: objects.count)
}

func mutablePointerToObjC(path: String) -> NSString {
  var err: NSError? = nil
  return NSString(contentsOfFile: path, error:&err)
}
