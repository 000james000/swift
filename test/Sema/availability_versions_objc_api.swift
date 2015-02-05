// RUN: %target-parse-verify-swift %clang-importer-sdk -I %S/Inputs/custom-modules -enable-experimental-availability-checking

// REQUIRES: OS=macosx
// REQUIRES: objc_interop

import Foundation

// Tests for uses of version-based potential unavailability imported from ObjC APIs.
func callUnavailableObjC() {
  let _ = NSAvailableOn10_10() // expected-error {{'NSAvailableOn10_10' is only available on OS X version 10.10 or greater}}
  
  
  if #os(OSX >= 10.10) {
    let o = NSAvailableOn10_10()
    
    // Properties
    let _ = o.propertyOn10_11 // expected-error {{'propertyOn10_11' is only available on OS X version 10.11 or greater}}
    o.propertyOn10_11 = 22 // expected-error {{'propertyOn10_11' is only available on OS X version 10.11 or greater}}
    
    // Methods
    o.methodAvailableOn10_11() // expected-error {{'methodAvailableOn10_11()' is only available on OS X version 10.11 or greater}}
    
    // Initializers
    
    let _ = NSAvailableOn10_10(stringOn10_11:"Hi") // expected-error {{'init(stringOn10_11:)' is only available on OS X version 10.11 or greater}}
  }
}

// Declarations with Objective-C-originated potentially unavailable APIs

func functionWithObjCParam(o: NSAvailableOn10_10) { // expected-error {{'NSAvailableOn10_10' is only available on OS X version 10.10 or greater}}
}

class ClassExtendingUnvailableClass : NSAvailableOn10_10 { // expected-error {{'NSAvailableOn10_10' is only available on OS X version 10.10 or greater}}
}

class ClassAdoptingUnavailableProtocol : NSProtocolAvailableOn10_10 { // expected-error {{'NSProtocolAvailableOn10_10' is only available on OS X version 10.10 or greater}}
}

// Enums from Objective-C

let _: NSUnavailableOptions = .First // expected-error {{'NSUnavailableOptions' is only available on OS X version 10.10 or greater}}

let _: NSOptionsWithUnavailableElement = .Third // expected-error {{'Third' is only available on OS X version 10.10 or greater}}

let _: NSUnavailableEnum = .First // expected-error {{'NSUnavailableEnum' is only available on OS X version 10.10 or greater}}

let _: NSEnumWithUnavailableElement = .Third // expected-error {{'Third' is only available on OS X version 10.10 or greater}}

// Differing availability on getters and setters imported from ObjC.

func gettersAndSettersFromObjC(o: NSAvailableOn10_9) {
  let _: Int = o.propertyOn10_10WithSetterOn10_11After  // expected-error {{'propertyOn10_10WithSetterOn10_11After' is only available on OS X version 10.10 or greater}}

  if #os(OSX >= 10.10) {
    // Properties with unavailable accessors declared before property in Objective-C header
    o.propertyOn10_10WithSetterOn10_11Before = 5 // expected-error {{setter for 'propertyOn10_10WithSetterOn10_11Before' is only available on OS X version 10.11 or greater}}
    let _: Int = o.propertyOn10_10WithGetterOn10_11Before // expected-error {{getter for 'propertyOn10_10WithGetterOn10_11Before' is only available on OS X version 10.11 or greater}}

    // Properties with unavailable accessors declared after property in Objective-C header
    o.propertyOn10_10WithSetterOn10_11After = 5 // expected-error {{setter for 'propertyOn10_10WithSetterOn10_11After' is only available on OS X version 10.11 or greater}}
    let _: Int = o.propertyOn10_10WithGetterOn10_11After // expected-error {{getter for 'propertyOn10_10WithGetterOn10_11After' is only available on OS X version 10.11 or greater}}

    // Property with unavailable setter redeclared in Objective-C category
    o.readOnlyRedeclaredWithSetterInCategory = 5 // expected-error {{setter for 'readOnlyRedeclaredWithSetterInCategory' is only available on OS X version 10.11 or greater}}
  }
}
