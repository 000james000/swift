// RUN: %swift -sdk %S/../Inputs/objc-generics-sdk -I %S/../Inputs/objc-generics-sdk/swift-modules -enable-source-import -parse -parse-as-library -verify -target x86_64-apple-macosx10.9 %s

// REQUIRES: objc_generics

import Foundation

func testNSArrayBridging(hive: Hive) {
  let bees: [Bee] = hive.bees
}

func testNSDictionaryBridging(hive: Hive) {
  let beesByName: [String : Bee] = hive.beesByName // expected-error{{value of optional type '[String : Bee]?' not unwrapped;}}
}

func testNSSetBridging(hive: Hive) {
  let relatedHives: Set<Bee>= hive.allBees
}
