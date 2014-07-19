// RUN: rm -rf %t  &&  mkdir %t
// RUN: %target-build-swift -module-name MangleTest %s -o %t/a.out
// RUN: %target-run %t/a.out | FileCheck %s
import Foundation

/* FIXME: SwiftObject doesn't support -description
class Foo { }
var anyFoo: AnyObject = Foo()
println(anyFoo.description())

@objc class Bar { }
var anyBar: AnyObject = Bar()
println(anyBar.description())
*/

// Check whether the class name comes out properly in the description
// CHECK: MangleTest.Wibble
@objc class Wibble : NSObject { }
var anyWibble: AnyObject = Wibble()
println(anyWibble.description)

// Check whether we can lookup the class with this name.
var anyWibbleClass: AnyClass = NSClassFromString("_TtC10MangleTest6Wibble")
var anyWibbleClass2 = anyWibble.dynamicType
assert(NSStringFromClass(anyWibbleClass) == "MangleTest.Wibble")
assert(NSStringFromClass(anyWibbleClass2) == "MangleTest.Wibble")

anyWibbleClass = NSClassFromString("MangleTest.Wibble")
anyWibbleClass2 = anyWibble.dynamicType
assert(NSStringFromClass(anyWibbleClass) == "MangleTest.Wibble")
assert(NSStringFromClass(anyWibbleClass2) == "MangleTest.Wibble")

