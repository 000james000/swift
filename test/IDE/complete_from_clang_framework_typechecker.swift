// RUN: %swift -parse -verify %s -F %S/Inputs/mock-sdk

import Foo
// Don't import 'FooHelper'.

func test1() {
  let a = Foo.FooEnum1X
  let b = Foo.FooSubEnum1X

  // Even though 'FooHelper' is a separate module, because 'Foo' re-exports it,
  // we allow qualifying members of 'FooHelper' with 'Foo'.
  // If this changes, code completion needs to change.
  let c = Foo.FooHelperSubEnum1X
}

