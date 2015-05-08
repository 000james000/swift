// RUN: %target-run-simple-swift | FileCheck %s

// REQUIRES: objc_interop

import Foundation
import SpriteKit

// SKColor is NSColor on OS X and UIColor on iOS.

var r = CGFloat(0)
var g = CGFloat(0)
var b = CGFloat(0)
var a = CGFloat(0)
var color = SKColor.redColor()
color.getRed(&r, green:&g, blue:&b, alpha:&a)
print("color \(r) \(g) \(b) \(a)")
// CHECK: color 1.0 0.0 0.0 1.0

#if os(OSX)
func f(c: NSColor) {
  print("colortastic")
}
#endif
#if os(iOS) || os(tvOS)
func f(c: UIColor) {
  print("colortastic")
}
#endif
f(color)
// CHECK: colortastic
