// RUN: %swift -parse -verify %s

func foo(a: Int) {
  foo(<#a#>) // expected-error 2 {{expected ',' separator}} \
             // expected-error 2 {{expected expression in list of expressions}} 
  foo(<\a\>) // expected-error 2 {{invalid character in source file}} \
             // expected-error 3 {{expected ',' separator}} \
             // expected-error {{operator with postfix spacing cannot start a subexpression}} \
             // expected-error {{expected expression in list of expressions}}
}

// rdar://15946844
func test1(inout var x : Int) {}  // expected-error {{'inout' arguments may not also be marked 'var' or 'let'}}
func test2(inout let x : Int) {}  // expected-error {{'inout' arguments may not also be marked 'var' or 'let'}}

// rdar://16601779
func foo() {
  runAction(SKAction.sequence() // expected-error {{use of unresolved identifier 'runAction'}}  expected-error {{use of unresolved identifier 'SKAction'}}  expected-note {{to match this opening '('}} expected-error {{expected ',' separator}}
    skview! // expected-error 2{{expected ',' separator}} expected-error {{use of unresolved identifier 'skview'}}
} // expected-error {{expected expression in list of expressions}} expected-error {{expected ')' in expression list}}

func test3(a: inout Int) {} // expected-error {{'inout' must appear before the parameter name}}{{12-12=inout }}{{15-20=}}

super.init() // expected-error {{'super' members cannot be referenced in a non-class type}} expected-error {{initializers may only be declared within a type}}

switch state { // expected-error {{use of unresolved identifier 'state'}}
  let duration : Int = 0 // expected-error {{all statements inside a switch must be covered by a 'case' or 'default'}} \
                         // expected-error {{expected expression}}
}
