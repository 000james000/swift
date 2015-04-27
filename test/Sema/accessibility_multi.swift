// RUN: %target-swift-frontend -parse -primary-file %s %S/Inputs/accessibility_multi_other.swift -verify -enable-access-control

func reset(inout value: Int) { value = 0 }

func testGlobals() {
  println(privateSetGlobal)
  privateSetGlobal = 42 // expected-error {{cannot assign to 'privateSetGlobal'}}
  reset(&privateSetGlobal) // expected-error {{cannot pass immutable value 'privateSetGlobal' as inout argument}}
}

func testProperties(var instance: Members) {
  println(instance.privateSetProp)
  instance.privateSetProp = 42 // expected-error {{cannot assign to 'privateSetProp' in 'instance'}}
  reset(&instance.privateSetProp) // expected-error {{cannot pass immutable value of type 'Int' as inout argument}}
}

func testSubscript(var instance: Members) {
  println(instance[])
  instance[] = 42 // expected-error {{cannot assign to the result of this expression}}
  reset(&instance[]) // expected-error {{cannot pass immutable value of type 'Int' as inout argument}}
}
