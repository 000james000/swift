// RUN: rm -rf %t/clang-module-cache
// RUN: %swift %clang-importer-sdk -emit-sil -module-cache-path %t/clang-module-cache -I %S/Inputs/custom-modules -target x86_64-apple-macosx10.9 %s -verify
// RUN: ls -lR %t/clang-module-cache | FileCheck %s
// CHECK: ObjectiveC{{.*}}.pcm

import AppKit
import objc_ext
import TestProtocols
import ObjCParseExtras

// Subclassing and designated initializers
func testNSInterestingDesignated() {
  NSInterestingDesignated()
  NSInterestingDesignated(string:"hello")
  NSInterestingDesignatedSub()
  NSInterestingDesignatedSub(string:"hello")
}

extension NSDocument {
  convenience init(string: String) {
    self.init(URL: string)
  }
}

class MyDocument1 : NSDocument {
  init() { 
    super.init()
  }
}

func createMyDocument1() {
  var md = MyDocument1()
  md = MyDocument1(URL: "http://llvm.org")

  // Inherited convenience init.
  md = MyDocument1(string: "http://llvm.org")
}

class MyDocument2 : NSDocument {
  init(URL url: String) {
    return super.init(URL: url) // expected-error{{must call a designated initializer of the superclass 'NSDocument'}}
  }
}

class MyDocument3 : NSAwesomeDocument {
  init() { 
    super.init()
  }
}

func createMyDocument3() {
  var md = MyDocument3()
  md = MyDocument3(URL: "http://llvm.org")
}

class MyInterestingDesignated : NSInterestingDesignatedSub { 
  init(string str: String) {
    super.init(string: str)
  }

  init(int i: Int) {
    super.init() // expected-error{{must call a designated initializer of the superclass 'NSInterestingDesignatedSub'}}
  }
}

func createMyInterestingDesignated() {
  var md = MyInterestingDesignated(URL: "http://llvm.org")
}

func testNoReturn(a : NSAwesomeDocument) -> Int {
  a.noReturnMethod(42)
  return 17    // TODO: In principle, we should produce an unreachable code diagnostic here.
}

// Initializer inheritance from protocol-specified initializers.
class MyViewController : NSViewController {
}

class MyTableViewController : NSTableViewController {
}

class MyOtherTableViewController : NSTableViewController { // expected-error{{class 'MyOtherTableViewController' does not implement its superclass's required members}}
  init(int i: Int)  {
    super.init(int: i)
  }
}

class MyThirdTableViewController : NSTableViewController {
  init(int i: Int)  {
    super.init(int: i)
  }

  required init(coder: NSCoder) {
    super.init(coder: coder)
  }
}

func checkInitWithCoder(coder: NSCoder) {
  NSViewController(coder: coder)
  NSTableViewController(coder: coder)
  MyViewController(coder: coder)
  MyTableViewController(coder: coder)
  MyOtherTableViewController(coder: coder) // expected-error{{cannot convert the expression's type 'MyOtherTableViewController' to type '(int: Int)'}}
  MyThirdTableViewController(coder: coder)
}

// <rdar://problem/16838409>
class MyDictionary1 : NSDictionary {}

func getMyDictionary1() {
  var nsd = MyDictionary1()
}

// <rdar://problem/16838515>
class MyDictionary2 : NSDictionary {
  init() {
    super.init()
  }
}
