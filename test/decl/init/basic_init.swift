// RUN: %target-parse-verify-swift

class Foo {
  func bar(bar) {} // expected-error{{use of undeclared type 'bar'}}
}

class C {
	var triangle : triangle // expected-error{{use of undeclared type 'triangle'}}

	init() {}
}

typealias t = t // expected-error {{type alias 't' circularly references itself}}




// <rdar://problem/17564699> QoI: Structs should get convenience initializers
struct MyStruct {
  init(k: Int) {
  }
  convenience init() {  // expected-error {{delegating initializers in structs are not not marked with 'convenience'}} {{3-14=}}
    self.init(k: 1)
  }
}
