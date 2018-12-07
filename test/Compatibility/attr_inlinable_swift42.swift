// RUN: %target-typecheck-verify-swift -swift-version 4.2
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-testing
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-resilience
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-resilience -enable-testing

enum InternalEnum {
  // expected-note@-1 {{type declared here}}
  case apple
  case orange
}

@usableFromInline enum VersionedEnum {
  case apple
  case orange
  case pear(InternalEnum)
  // expected-warning@-1 {{type of enum case in '@usableFromInline' enum should be '@usableFromInline' or public}}
  case persimmon(String)
}

@usableFromInline protocol P {
  typealias T = Int
}

extension P {
  @inlinable func f() {
    _ = T.self // typealiases were not checked in Swift 4.2, but P.T inherits @usableFromInline in Swift 4.2 mode
  }
}
