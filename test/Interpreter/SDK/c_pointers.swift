// RUN: %target-run-simple-swift | FileCheck %s

import Foundation
#if os(OSX)
import AppKit
typealias XXColor = NSColor
#endif
#if os(iOS)
import UIKit
typealias XXColor = UIColor
#endif


// Exercise some common APIs that make use of C pointer arguments.

//
// Typed C pointers
//

let rgb = CGColorSpaceCreateDeviceRGB()
let cgRed = CGColorCreate(rgb, [1.0, 0.0, 0.0, 1.0])

let nsRed = XXColor(CGColor: cgRed)

CGColorRelease(cgRed)
CGColorSpaceRelease(rgb)

var r: CGFloat = 0.5, g: CGFloat = 0.5, b: CGFloat = 0.5, a: CGFloat = 0.5
nsRed.getRed(&r, green: &g, blue: &b, alpha: &a)

// CHECK-LABEL: Red is:
println("Red is:")
println("<\(r) \(g) \(b) \(a)>") // CHECK-NEXT: <1 0 0 1>

//
// Void C pointers
//

let data = NSData(bytes: [1.5, 2.25, 3.125], 
                  length: sizeof(Double.self) * 3)
var fromData = [0.25, 0.25, 0.25]
let notFromData = fromData
data.getBytes(&fromData, length: sizeof(Double.self) * 3)

// CHECK-LABEL: Data is:
println("Data is:")
println(fromData[0]) // CHECK-NEXT: 1.5
println(fromData[1]) // CHECK-NEXT: 2.25
println(fromData[2]) // CHECK-NEXT: 3.125

// CHECK-LABEL: Independent data is:
println("Independent data is:")
println(notFromData[0]) // CHECK-NEXT: 0.25
println(notFromData[1]) // CHECK-NEXT: 0.25
println(notFromData[2]) // CHECK-NEXT: 0.25

//
// ObjC pointers
//

class Canary: NSObject {
  deinit {
    println("died")
  }
}

var CanaryAssocObjectHandle: UInt8 = 0

// Attach an associated object with a loud deinit so we can see that the
// error died.
func hangCanary(o: AnyObject) {
  objc_setAssociatedObject(o, &CanaryAssocObjectHandle, Canary(),
                     objc_AssociationPolicy(OBJC_ASSOCIATION_RETAIN_NONATOMIC))
}

// CHECK-LABEL: NSError out:
println("NSError out:")
autoreleasepool {
  var err: NSError? = NSError()
  hangCanary(err!)
  if let s = NSString.stringWithContentsOfFile("/hopefully/does/not/exist\x1B",
                                               encoding: NSUTF8StringEncoding,
                                               error: &err) {
    fatal("file should not actually exist")
  } else if let err_ = err {
    // The original value should have died
    // CHECK-NEXT: died
    println(err_.code) // CHECK-NEXT: 260
    hangCanary(err_)
    err = nil
  } else {
    fatal("should have gotten an error")
  }
}
// The result error should have died with the autorelease pool
// CHECK-NEXT: died

class DumbString: NSString {
  override func characterAtIndex(x: Int) -> unichar { fatal("nope") }
  override var length: Int { return 0 }

  override class func stringWithContentsOfFile(s: String,
                                  encoding: NSStringEncoding,
                                  error: ObjCMutablePointer<NSError?>)
  -> DumbString? {
    error.pointee = NSError(domain: "Malicious Mischief", code: 594, userInfo: nil)
    return nil
  }
}

// CHECK-LABEL: NSError in:
println("NSError in:")
autoreleasepool {
  var err: NSError? = nil
  DumbString.stringWithContentsOfFile("foo", encoding: NSUTF8StringEncoding,
                                      error: &err)
  let err_ = err!
  println(err_.domain) // CHECK-NEXT: Malicious Mischief
  println(err_.code) // CHECK-NEXT: 594
  hangCanary(err_)
  err = nil
}
// The result error should have died with the autorelease pool
// CHECK-NEXT: died
