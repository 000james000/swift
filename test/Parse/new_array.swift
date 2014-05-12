// RUN: %swift -parse -verify %s

struct DefaultConstructible {
  init() {}
  init(x: Int) {}
  init(x: Int, y: String) {}
}

struct NotDefaultConstructible {
  init(x: Int) {}
  init(x: Int, y: String) {}
}

var a = new DefaultConstructible[10]
var b = new DefaultConstructible[10] { DefaultConstructible(x: $0) }
var c = new DefaultConstructible[10] { NotDefaultConstructible($0) } // expected-error{{}}
var d = new DefaultConstructible[10] { DefaultConstructible(22, $0) } // expected-error{{}}

var e = new NotDefaultConstructible[10] // expected-error{{}}
var f = new NotDefaultConstructible[10] { NotDefaultConstructible(x: $0) }
var g = new NotDefaultConstructible[10] { DefaultConstructible($0) } // expected-error{{}}
var h = new NotDefaultConstructible[10] { NotDefaultConstructible(22, $0) } // expected-error{{}}
