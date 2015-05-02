// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-silgen -parse-as-library -verify %s

// REQUIRES: objc_interop

import Foundation
import errors

func test0() {
  try ErrorProne.fail() // expected-error {{errors thrown from here are not handled}}
}

// Test "AndReturnError" stripping.
// rdar://20722195
func testAndReturnError() throws {
  try ErrorProne.fail()
  try ErrorProne.go()
  try ErrorProne.tryAndReturnError() // collides with 'try' keyword
}

func testInheritedInit() throws {
  try ReallyErrorProne(one: nil)
}

func testInheritedFactory() throws {
  try ReallyErrorProne(two: nil)
}

// Resolve a conflict between -foo and -foo: by just not
// importing the latter as throwing.
func testConflict1(obj: ErrorProne) throws {
  try obj.conflict1() // expected-warning {{no calls to throwing functions occur within 'try'}}
}
func testConflict1_error(obj: ErrorProne) throws {
  var error: NSError?
  obj.conflict1(&error)
}

// Resolve a conflict between -foo and -fooAndReturnError:
// by not changing the name of the latter.
func testConflict2(obj: ErrorProne) throws {
  try obj.conflict2() // expected-warning {{no calls to throwing functions occur within 'try'}}
}
func testConflict2_error(obj: ErrorProne) throws {
  try obj.conflict2AndReturnError()
}

// Resolve a conflict between -foo: and -foo:error: by not
// changing the name of the latter.
func testConflict3(obj: ErrorProne) throws {
  try obj.conflict3(nil) // expected-warning {{no calls to throwing functions occur within 'try'}}
}
func testConflict3_error(obj: ErrorProne) throws {
  try obj.conflict3(nil, error: ())
}