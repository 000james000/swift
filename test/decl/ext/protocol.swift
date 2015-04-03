// RUN: %target-parse-verify-swift

// ----------------------------------------------------------------------------
// Using protocol requirements from inside protocol extensions
// ----------------------------------------------------------------------------
protocol P1 {
  func reqP1a() -> Bool
}

extension P1 {
  func extP1a() -> Bool { return !reqP1a() }

  var extP1b: Bool {
    return self.reqP1a()
  }

  var extP1c: Bool {
    return extP1b && self.extP1a()
  }
}

protocol P2 {
  typealias AssocP2 : P1

  func reqP2a() -> AssocP2
}

extension P2 {
  func extP2a() -> AssocP2? { return reqP2a() }

  func extP2b() {
    self.reqP2a().reqP1a()
  }

  func extP2c() -> Self.AssocP2 { return extP2a()! }
}

protocol P3 {
  typealias AssocP3 : P2

  func reqP3a() -> AssocP3
}

extension P3 {
  func extP3a() -> AssocP3.AssocP2 {
    return reqP3a().reqP2a()
  }
}

protocol P4 {
  typealias AssocP4

  func reqP4a() -> AssocP4
}

// ----------------------------------------------------------------------------
// Using generics from inside protocol extensions
// ----------------------------------------------------------------------------
func acceptsP1<T : P1>(t: T) { }

extension P1 {
  func extP1d() { acceptsP1(self) }
}

func acceptsP2<T : P2>(t: T) { }

extension P2 {
  func extP2acceptsP1() { acceptsP1(reqP2a()) }
  func extP2acceptsP2() { acceptsP2(self) }
}

// Use of 'Self' as a return type within a protocol extension.
protocol SelfP1 {
  typealias AssocType
}

protocol SelfP2 {
}

func acceptSelfP1<T, U : SelfP1 where U.AssocType == T>(t: T, u: U) -> T {
  return t
}

extension SelfP1 {
  func tryAcceptSelfP1<Z : SelfP1 where Z.AssocType == Self>(z: Z) -> Self {
    return acceptSelfP1(self, z)
  }
}

// ----------------------------------------------------------------------------
// Initializers in protocol extensions
// ----------------------------------------------------------------------------
protocol InitP1 {
  init(string: String)
}

extension InitP1 {
  init(int: Int) { self.init(string: "integer") }
}

struct InitS1 : InitP1 {
  init(string: String) { }
}

class InitC1 : InitP1 {
  required init(string: String) { }
}

func testInitP1() {
  var is1 = InitS1(int: 5)
  is1 = InitS1(string: "blah") // check type

  var ic1 = InitC1(int: 5)
  ic1 = InitC1(string: "blah") // check type
}

// ----------------------------------------------------------------------------
// Subscript in protocol extensions
// ----------------------------------------------------------------------------
protocol SubscriptP1 {
  func readAt(i: Int) -> String
  func writeAt(i: Int, string: String)
}

extension SubscriptP1 {
  subscript(i: Int) -> String {
    get { return readAt(i) }
    set(newValue) { writeAt(i, string: newValue) }
  }
}

struct SubscriptS1 : SubscriptP1 {
  func readAt(i: Int) -> String { return "hello" }
  func writeAt(i: Int, string: String) { }
}

struct SubscriptC1 : SubscriptP1 {
  func readAt(i: Int) -> String { return "hello" }
  func writeAt(i: Int, string: String) { }
}

func testSubscriptP1(var ss1: SubscriptS1, var sc1: SubscriptC1,
                     i: Int, s: String) {
  let s1 = ss1[i]
  ss1[i] = s

  let s2 = sc1[i]
  sc1[i] = s
}

// ----------------------------------------------------------------------------
// Using protocol extensions on types that conform to the protocols.
// ----------------------------------------------------------------------------
struct S1 : P1 {
  func reqP1a() -> Bool { return true }
}

func useS1(s1: S1) -> Bool {
  s1.reqP1a()
  return s1.extP1a() && s1.extP1b
}

// ----------------------------------------------------------------------------
// Protocol extensions with additional requirements
// ----------------------------------------------------------------------------
extension P4 where Self.AssocP4 : P1 {
  func extP4a() {
    acceptsP1(reqP4a())
  }
}

struct S4aHelper { }
struct S4bHelper : P1 {
  func reqP1a() -> Bool { return true }
}

struct S4a : P4 {
  func reqP4a() -> S4aHelper { return S4aHelper() }
}

struct S4b : P4 {
  func reqP4a() -> S4bHelper { return S4bHelper() }
}

struct S4c : P4 {
  func reqP4a() -> Int { return 0 }
}

struct S4d : P4 {
  func reqP4a() -> Bool { return false }
}

extension P4 where Self.AssocP4 == Int {
  func extP4Int() { }
}

extension P4 where Self.AssocP4 == Bool {
  func extP4a() -> Bool { return reqP4a() }
}

func testP4(s4a: S4a, s4b: S4b, s4c: S4c, s4d: S4d) {
  s4a.extP4a() // expected-error{{cannot invoke 'extP4a' with }}
  s4b.extP4a() // ok
  s4c.extP4a() // expected-error{{cannot invoke 'extP4a' with no arguments}}
  s4c.extP4Int() // okay
  var b1 = s4d.extP4a() // okay, "Bool" version
  b1 = true // checks type above
  s4d.extP4Int() // expected-error{{cannot invoke 'extP4Int' with no arguments}}
}

// ----------------------------------------------------------------------------
// Using protocol extensions to satisfy requirements
// ----------------------------------------------------------------------------
protocol P5 {
  func reqP5a()
}

// extension of P5 provides a witness for P6
extension P5 {
  func reqP6a() { reqP5a() }
}

protocol P6 {
  func reqP6a()
}

// S6a uses P5.reqP6a
struct S6a : P5 { 
  func reqP5a() { }
}

extension S6a : P6 { }

// S6b uses P5.reqP6a
struct S6b : P5, P6 { 
  func reqP5a() { }
}

// S6c uses P5.reqP6a
struct S6c : P6 { 
}

extension S6c : P5 {
  func reqP5a() { }
}

// S6d does not use P5.reqP6a
struct S6d : P6 { 
  func reqP6a() { }
}

extension S6d : P5 {
  func reqP5a() { }
}

protocol P7 {
  typealias P7Assoc

  func getP7Assoc() -> P7Assoc
}

struct P7FromP8<T> { }

protocol P8 {
  typealias P8Assoc
  func getP8Assoc() -> P8Assoc
}

// extension of P8 provides conformance to P7Assoc
extension P8 {
  func getP7Assoc() -> P7FromP8<P8Assoc> { return P7FromP8() }
}

// Okay, P7 requirements satisfied by P8
struct P8a : P8, P7 {
  func getP8Assoc() -> Bool { return true }
}

func testP8a(p8a: P8a) {
  var p7 = p8a.getP7Assoc()
  p7 = P7FromP8<Bool>() // okay, check type of above
}

// Okay, P7 requirements explicitly specified
struct P8b : P8, P7 {
  func getP7Assoc() -> Int { return 5 }
  func getP8Assoc() -> Bool { return true }
}

func testP8b(p8b: P8b) {
  var p7 = p8b.getP7Assoc()
  p7 = 17 // check type of above
}

protocol PConforms1 {
}

extension PConforms1 {
  func pc2() { } // expected-note{{candidate exactly matches}}
}

protocol PConforms2 : PConforms1, MakePC2Ambiguous {
  func pc2() // expected-note{{multiple matching functions named 'pc2()' with type '() -> ()'}}
}

protocol MakePC2Ambiguous {
}

extension MakePC2Ambiguous {
  func pc2() { } // expected-note{{candidate exactly matches}}
}

struct SConforms2a : PConforms2 { } // expected-error{{type 'SConforms2a' does not conform to protocol 'PConforms2'}}

struct SConforms2b : PConforms2 {
  func pc2() { }
}

// Satisfying requirements via protocol extensions for fun and profit
protocol _MySeq { }

protocol MySeq : _MySeq {
  typealias Generator : GeneratorType
  func myGenerate() -> Generator
}

protocol _MyCollection : _MySeq {
  typealias Index : ForwardIndexType

  var myStartIndex : Index { get }
  var myEndIndex : Index { get }

  typealias _Element
  subscript (i: Index) -> _Element { get }
}

protocol MyCollection : _MyCollection {
}

struct MyIndexedGenerator<C : _MyCollection> : GeneratorType {
  var container: C
  var index: C.Index

  mutating func next() -> C._Element? {
    if index == container.myEndIndex { return nil }
    let result = container[index]
    ++index
    return result
  }
}

struct OtherIndexedGenerator<C : _MyCollection> : GeneratorType {
  var container: C
  var index: C.Index

  mutating func next() -> C._Element? {
    if index == container.myEndIndex { return nil }
    let result = container[index]
    ++index
    return result
  }
}

extension _MyCollection {
  func myGenerate() -> MyIndexedGenerator<Self> {
    return MyIndexedGenerator(container: self, index: self.myEndIndex)
  }
}

struct SomeCollection1 : MyCollection {
  var myStartIndex: Int { return 0 }
  var myEndIndex: Int { return 10 }

  subscript (i: Int) -> String {
    return "blah"
  }
}

struct SomeCollection2 : MyCollection {
  var myStartIndex: Int { return 0 }
  var myEndIndex: Int { return 10 }

  subscript (i: Int) -> String {
    return "blah"
  }

  func myGenerate() -> OtherIndexedGenerator<SomeCollection2> {
    return OtherIndexedGenerator(container: self, index: self.myEndIndex)
  }
}

func testSomeCollections(sc1: SomeCollection1, sc2: SomeCollection2) {
  var mig = sc1.myGenerate()
  mig = MyIndexedGenerator(container: sc1, index: sc1.myStartIndex)

  var ig = sc2.myGenerate()
  ig = MyIndexedGenerator(container: sc2, index: sc2.myStartIndex) // expected-error{{cannot assign a value of type 'MyIndexedGenerator<SomeCollection2>' to a value of type 'OtherIndexedGenerator<SomeCollection2>'}}
}

// ----------------------------------------------------------------------------
// Partial ordering of protocol extension members
// ----------------------------------------------------------------------------

// Partial ordering between members of protocol extensions and members
// of concrete types.
struct S1b : P1 {
  func reqP1a() -> Bool { return true }

  func extP1a() -> Int { return 0 }
}

func useS1b(s1b: S1b) {
  var x = s1b.extP1a() // uses S1b.extP1a due to partial ordering
  x = 5 // checks that "x" deduced to "Int" above
  var b: Bool = s1b.extP1a() // still uses P1.ext1Pa due to type annotation
}

// Partial ordering between members of protocol extensions for
// different protocols.
protocol PInherit1 { }

protocol PInherit2 : PInherit1 { }

protocol PInherit3 : PInherit2 { }

protocol PInherit4 : PInherit2 { }

extension PInherit1 {
  func order1() -> Int { return 0 }
}

extension PInherit2 {
  func order1() -> Bool { return true }
}

extension PInherit3 {
  func order1() -> Double { return 1.0 }
}

extension PInherit4 {
  func order1() -> String { return "hello" }
}

struct SInherit1 : PInherit1 { }
struct SInherit2 : PInherit2 { }
struct SInherit3 : PInherit3 { }
struct SInherit4 : PInherit4 { }

func testPInherit(si2 : SInherit2, si3: SInherit3, si4: SInherit4) {
  var b1 = si2.order1() // PInherit2.order1
  b1 = true // check that the above returned Bool

  var d1 = si3.order1() // PInherit3.order1
  d1 = 3.14159 // check that the above returned Double

  var s1 = si4.order1() // PInherit4.order1
  s1 = "hello" // check that the above returned String

  // Other versions are still visible, since they may have different
  // types.
  b1 = si3.order1() // PInherit2.order1
  var i1: Int = si3.order1() // PInherit1.order1
}

protocol PConstrained1 {
  typealias AssocTypePC1
}

extension PConstrained1 {
  func pc1() -> Int { return 0 }
}

extension PConstrained1 where Self.AssocTypePC1 : PInherit2 {
  func pc1() -> Bool { return true }
}

extension PConstrained1 where Self.AssocTypePC1 : PInherit3 {
  func pc1() -> String { return "hello" }
}

struct SConstrained1 : PConstrained1 {
  typealias AssocTypePC1 = SInherit1
}

struct SConstrained2 : PConstrained1 {
  typealias AssocTypePC1 = SInherit2
}

struct SConstrained3 : PConstrained1 {
  typealias AssocTypePC1 = SInherit3
}

func testPConstrained1(sc1: SConstrained1, sc2: SConstrained2,
                       sc3: SConstrained3) {
  var i = sc1.pc1() // PConstrained1.pc1
  i = 17 // checks type of above

  var b = sc2.pc1() // PConstrained1 (with PInherit2).pc1
  b = true // checks type of above

  var s = sc3.pc1() // PConstrained1 (with PInherit3).pc1
  s = "hello" // checks type of above
}

protocol PConstrained2 {
  typealias AssocTypePC2
}

protocol PConstrained3 : PConstrained2 {
}

extension PConstrained2 where Self.AssocTypePC2 : PInherit1 {
  func pc2() -> Bool { return true } // expected-note{{found this candidate}}
}

extension PConstrained3 {
  func pc2() -> String { return "hello" } // expected-note{{found this candidate}}
}

struct SConstrained3a : PConstrained3 {
  typealias AssocTypePC2 = Int
}

struct SConstrained3b : PConstrained3 {
  typealias AssocTypePC2 = SInherit3
}

func testSConstrained3(sc3a: SConstrained3a, sc3b: SConstrained3b) {
  var s = sc3a.pc2() // PConstrained3.pc2
  s = "hello"

  sc3b.pc2() // expected-error{{ambiguous use of 'pc2'}}
  s = sc3b.pc2()
  var b: Bool = sc3b.pc2()
}

// ----------------------------------------------------------------------------
// Semantic restrictions
// ----------------------------------------------------------------------------

// Extension cannot have an inheritance clause.
protocol BadProto1 { }
protocol BadProto2 { }

extension BadProto1 : BadProto2 { } // expected-error{{extension of protocol 'BadProto1' cannot have an inheritance clause}}

extension BadProto2 {
  struct S { } // expected-error{{type 'S' cannot be defined within a protocol extension}}
  class C { } // expected-error{{type 'C' cannot be defined within a protocol extension}}
  enum E { } // expected-error{{type 'E' cannot be defined within a protocol extension}}
}
