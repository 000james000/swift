// RUN: %target-run-simple-swift | FileCheck %s

import Foundation

let opaqueNil: COpaquePointer = nil
if opaqueNil == nil {
  println("ok opaqueNil == nil")
  // CHECK: ok opaqueNil == nil
}

if opaqueNil != nil {
} else {
  println("ok opaqueNil != nil is false")
  // CHECK: ok opaqueNil != nil is false
}

let unsafeNil: UnsafePointer<Int> = nil
if unsafeNil == (nil as UnsafePointer<Int>) {
  println("ok unsafeNil == (nil as UnsafePointer<Int>)")
  // CHECK: ok unsafeNil == (nil as UnsafePointer<Int>)
}

let removed = NSFileManager.defaultManager().removeItemAtURL(NSURL(string:"/this/file/does/not/exist"), error:nil)
if !removed {
  println("ok !removed")
  // CHECK: ok !removed
}

var selNil: Selector = nil
if selNil == nil { 
  println("ok selNil == nil")
  // CHECK: ok selNil == nil
}
selNil = nil
if selNil == nil { 
  println("ok selNil == nil")
  // CHECK: ok selNil == nil
}

