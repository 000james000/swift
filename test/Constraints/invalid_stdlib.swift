// RUN: %swift -parse -parse-stdlib -verify %s

// This file is for tests that used to cause the type checker to crash.

class DictStringInt {
  class func convertFromDictionaryLiteral(xs: ()...) -> DictStringInt {} // expected-error{{broken standard library: cannot find Array type}}
}
