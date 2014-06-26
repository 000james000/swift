// RUN: %target-run-simple-swift | FileCheck %s

import Foundation

struct Guts {
  var internalValue = 42
  var value: Int { 
    get {
      return internalValue
    }
  }
}

class Target : NSString {
  // This ObjC-typed property is observed by KVO
  var objcValue: String

  // This Swift-typed property causes vtable usage on this class.
  var swiftValue: Guts

  init() {
    self.swiftValue = Guts()
    self.objcValue = ""
    super.init()
  }

  func print() { 
    println("swiftValue \(self.swiftValue.value), objcValue \(objcValue)")
  }
}

class Observer : NSObject {
  var target: Target?

  init() { target = nil; super.init() }

  func observeTarget(t: Target) {
    target = t
    target!.addObserver(self, forKeyPath:"objcValue", 
      options:NSKeyValueObservingOptions.New | NSKeyValueObservingOptions.Old, 
      context: nil)
  }

  override func observeValueForKeyPath(path:String,
                               ofObject obj:AnyObject, 
                                     change:Dictionary<NSObject, AnyObject>,
                                    context:UnsafePointer<Void>) {
    target!.print()
  }
}


var t = Target()
var o = Observer()
println("unobserved")
// CHECK: unobserved
t.objcValue = "one"
t.objcValue = "two"
println("registering observer")
// CHECK-NEXT: registering observer
o.observeTarget(t)
println("Now witness the firepower of this fully armed and operational panopticon!")
// CHECK-NEXT: panopticon
t.objcValue = "three"
// CHECK-NEXT: swiftValue 42, objcValue three
t.objcValue = "four"
// CHECK-NEXT: swiftValue 42, objcValue four

//===========================================================================//
// Test using a proper global context reference.
//===========================================================================//

var kvoContext = Int()

class ObserverKVO : NSObject {
  var target: Target?

  init() { target = nil; super.init() }

  func observeTarget(target: Target) {
    self.target = target
    self.target!.addObserver(self,
       forKeyPath:"objcValue",
       options:NSKeyValueObservingOptions.New | NSKeyValueObservingOptions.Old, 
       context: &kvoContext)
  }
  
  func removeTarget() {
    self.target!.removeObserver(self, forKeyPath:"objcValue",
                                      context: &kvoContext)
  }

  override func observeValueForKeyPath(path:String,
                                       ofObject obj:AnyObject,
                                       change:Dictionary<NSObject, AnyObject>,
                                       context:UnsafePointer<Void>) {
    if context == &kvoContext {
      target!.print()
    }
  }
}


var t2 = Target()
var o2 = ObserverKVO()
println("unobserved 2")
t2.objcValue = "one"
t2.objcValue = "two"
println("registering observer 2")
o2.observeTarget(t2)
println("Now witness the firepower of this fully armed and operational panopticon!")
t2.objcValue = "three"
t2.objcValue = "four"
o2.removeTarget()
println("target removed")

// CHECK: registering observer 2
// CHECK-NEXT: Now witness the firepower of this fully armed and operational panopticon!
// CHECK-NEXT: swiftValue 42, objcValue three
// CHECK-NEXT: swiftValue 42, objcValue four
// CHECK-NEXT: target removed
