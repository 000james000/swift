// RUN: %target-swift-frontend -parse -primary-file %s %S/Inputs/accessibility_multi_other.swift -verify
func read(value: Int) {}
func reset(inout value: Int) { value = 0 }

func testGlobals() {
  read(privateSetGlobal)
  privateSetGlobal = 42 // expected-error {{cannot assign to 'privateSetGlobal'}}
  reset(&privateSetGlobal) // expected-error {{cannot pass immutable value 'privateSetGlobal' as inout argument}}
}

func testProperties(var instance: Members) {
  read(instance.privateSetProp)
  instance.privateSetProp = 42 // expected-error {{cannot assign to property 'privateSetProp' with inaccessible setter}}
  reset(&instance.privateSetProp) // expected-error {{cannot pass immutable value of type 'Int' as inout argument}}
}

func testSubscript(var instance: Members) {
  read(instance[])
  instance[] = 42 // expected-error {{cannot assign to subscript with inaccessible setter}}
  reset(&instance[]) // expected-error {{cannot pass immutable value of type 'Int' as inout argument}}
}
