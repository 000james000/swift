// RUN: %target-parse-verify-swift

class HasFunc {
  func HasFunc(_: HasFunc) { // expected-error {{use of undeclared type 'HasFunc'}}
  }
  func HasFunc() -> HasFunc { // expected-error {{use of undeclared type 'HasFunc'}}
    return HasFunc()
  }
  func SomethingElse(_: SomethingElse) { // expected-error {{use of undeclared type 'SomethingElse'}}
    return nil
  }
  func SomethingElse() -> SomethingElse? { // expected-error {{use of undeclared type 'SomethingElse'}}
    return nil
  }
}

class HasGenericFunc {
  func HasGenericFunc<HasGenericFunc : HasGenericFunc>(x: HasGenericFunc) -> HasGenericFunc { // expected-error {{use of undeclared type 'HasGenericFunc'}}
    return x
  }
  func SomethingElse<SomethingElse : SomethingElse>(_: SomethingElse) -> SomethingElse? { // expected-error {{use of undeclared type 'SomethingElse'}}
    return nil
  }
}

class HasProp {
  var HasProp: HasProp { // expected-error 2 {{use of undeclared type 'HasProp'}}
    return HasProp()
  }
  var SomethingElse: SomethingElse? { // expected-error 2 {{use of undeclared type 'SomethingElse'}}
    return nil
  }
}

protocol SomeProtocol {}
protocol ReferenceSomeProtocol {
  var SomeProtocol: SomeProtocol { get } // expected-error 2 {{use of undeclared type 'SomeProtocol'}}
}

func TopLevelFunc(x: TopLevelFunc) -> TopLevelFunc { return x } // expected-error {{use of undeclared type 'TopLevelFunc'}}'
func TopLevelGenericFunc<TopLevelGenericFunc : TopLevelGenericFunc>(x: TopLevelGenericFunc) -> TopLevelGenericFunc { return x } // expected-error {{use of undeclared type 'TopLevelGenericFunc'}}'
func TopLevelGenericFunc2<T : TopLevelGenericFunc2>(x: T) -> T { return x} // expected-error {{use of undeclared type 'TopLevelGenericFunc2'}}
var TopLevelVar: TopLevelVar? { return nil } // expected-error 2 {{use of undeclared type 'TopLevelVar'}}
