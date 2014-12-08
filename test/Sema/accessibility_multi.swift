// RUN: %swift -parse -primary-file %s %S/Inputs/accessibility_multi_other.swift -verify -enable-access-control

func reset(inout value: Int) { value = 0 }

func testGlobals() {
  println(privateSetGlobal)
  privateSetGlobal = 42 // expected-error {{cannot assign to 'privateSetGlobal'}}
  reset(&privateSetGlobal) // expected-error {{type of expression is ambiguous without more context}}
}

func testProperties(var instance: Members) {
  println(instance.privateSetProp)
  instance.privateSetProp = 42 // expected-error {{cannot assign to 'privateSetProp' in 'instance'}}
  reset(&instance.privateSetProp) // expected-error {{cannot assign to immutable value of type 'Int'}}
}

func testSubscript(var instance: Members) {
  println(instance[])
  instance[] = 42 // expected-error {{cannot assign to the result of this expression}}
  reset(&instance[]) // expected-error {{type of expression is ambiguous without more context}}
}
