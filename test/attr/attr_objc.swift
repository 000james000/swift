// RUN: %swift %s -verify -parse
// RUN: %swift-ide-test -print-ast-typechecked -source-filename %s -target x86_64-apple-macosx10.9 -function-definitions=true -prefer-type-repr=false -print-implicit-attrs=true -explode-pattern-binding-decls=true | FileCheck %s

class PlainClass {}
struct PlainStruct {}
enum PlainEnum {}
protocol PlainProtocol {} // expected-note {{protocol 'PlainProtocol' declared here}}


@objc class Class_ObjC1 {}

@class_protocol
protocol Protocol_Class1 {} // expected-note {{protocol 'Protocol_Class1' declared here}}

@class_protocol
protocol Protocol_Class2 {}

@class_protocol @objc
protocol Protocol_ObjC1 {}

@objc
protocol Protocol_ObjC2 {}




//===--- Subjects of @objc attribute.

@objc
var subject_globalVar: Int // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

var subject_getterSetter: Int {
  @objc
  get { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
    return 0
  }
  @objc
  set { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  }
}

var subject_global_observingAccesorsVar1: Int = 0 {
  @objc
  willSet { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  }
  @objc
  didSet { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  }
}

class subject_getterSetter1 {
  var instanceVar1: Int {
    @objc
    get { // expected-error {{'@objc' getter for non-'@objc' property}}
      return 0
    }
  }

  var instanceVar2: Int {
    get {
      return 0
    }
    @objc
    set { // expected-error {{'@objc' setter for non-'@objc' property}}
    }
  }

  var instanceVar3: Int {
    @objc
    get { // expected-error {{'@objc' getter for non-'@objc' property}}
      return 0
    }
    @objc
    set { // expected-error {{'@objc' setter for non-'@objc' property}}
    }
  }

  var observingAccesorsVar1: Int = 0 {
    @objc
    willSet { // expected-error {{observing accessors are not allowed to be marked @objc}}
    }
    @objc
    didSet { // expected-error {{observing accessors are not allowed to be marked @objc}}
    }
  }
}

class subject_staticVar1 {
  @objc
  class var staticVar1: Int = 42 // expected-error {{class variables not yet supported}}

  @objc
  class var staticVar2: Int { return 42 }
}

@objc
func subject_freeFunc() { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  @objc
  var subject_localVar: Int // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  func subject_nestedFreeFunc() { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  }
}

@objc
func subject_genericFunc<T>(t: T) { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  @objc
  var subject_localVar: Int // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  func subject_instanceFunc() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
}

func subject_funcParam(a: @objc Int) { // expected-error {{attribute can only be applied to declarations, not types}}
}

@objc
struct subject_struct { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  @objc
  var subject_instanceVar: Int // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  init() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  func subject_instanceFunc() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
}

@objc
struct subject_genericStruct<T> { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  @objc
  var subject_instanceVar: Int // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  init() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  func subject_instanceFunc() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
}

@objc
class subject_class1 { // no-error
  @objc
  var subject_instanceVar: Int // no-error

  @objc
  init() {} // no-error

  @objc
  func subject_instanceFunc() {} // no-error
}

@objc
class subject_class2 : Protocol_Class1, PlainProtocol { // no-error
}

@objc
class subject_genericClass<T> { // no-error
  @objc
  var subject_instanceVar: Int // expected-error{{variable in a generic class cannot be represented in Objective-C}}

  @objc
  init() {} // expected-error{{initializer in a generic class cannot be represented in Objective-C}}

  @objc
  func subject_instanceFunc() {} // expected-error{{method in a generic class cannot be represented in Objective-C}}
}

@objc
enum subject_enum { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  @objc
  case subject_enumElement1 // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  case subject_enumElement2(Int) // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  init() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  func subject_instanceFunc() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
}

@objc
enum subject_genericEnum<T> { // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
  @objc
  case subject_enumElement1 // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  case subject_enumElement2(Int) // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  init() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}

  @objc
  func subject_instanceFunc() {} // expected-error {{only classes, protocols, methods, properties, and subscript declarations can be declared '@objc'}}
}


@objc
protocol subject_protocol1 {
  @objc
  var subject_instanceVar: Int { get }

  @objc
  func subject_instanceFunc()
}

@objc @class_protocol
protocol subject_protocol2 {} // no-error
// CHECK-LABEL: @class_protocol @objc protocol subject_protocol2 {

@class_protocol @objc
protocol subject_protocol3 {} // no-error
// CHECK-LABEL: @objc @class_protocol protocol subject_protocol3 {

@objc
protocol subject_protocol4 : PlainProtocol {} // expected-error {{@objc protocol 'subject_protocol4' cannot refine non-@objc protocol 'PlainProtocol'}}

@objc
protocol subject_protocol5 : Protocol_Class1 {} // expected-error {{@objc protocol 'subject_protocol5' cannot refine non-@objc protocol 'Protocol_Class1'}}

@objc
protocol subject_protocol6 : Protocol_ObjC1 {}

protocol subject_containerProtocol1 {
  @objc
  var subject_instanceVar: Int { get }

  @objc
  func subject_instanceFunc()

  @objc
  class func subject_staticFunc()
}

@objc @class_protocol
protocol subject_containerObjCProtocol1 {
  func func_Curried1()()
  // expected-error@-1 {{method cannot be marked @objc because curried functions cannot be represented in Objective-C}}
  // expected-note@-2 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  func func_FunctionReturn1() -> PlainStruct
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-note@-3 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  func func_FunctionParam1(a: PlainStruct)
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-note@-3 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  func func_Variadic(AnyObject...)
  // expected-error @-1{{method cannot be marked @objc because it has a variadic parameter}}
  // expected-note @-2{{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  subscript(a: PlainStruct) -> Int { get }
  // expected-error@-1 {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-note@-3 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}

  var varNonObjC1: PlainStruct { get }
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-note@-3 {{inferring '@objc' because the declaration is a member of an '@objc' protocol}}
}

@objc @class_protocol
protocol subject_containerObjCProtocol2 {
  init(a: Int)
  @objc init(a: Double)

  func func1() -> Int
  @objc func func1_() -> Int

  var instanceVar1: Int { get set }
  @objc var instanceVar1_: Int { get set }

  subscript(i: Int) -> Int { get set }
  @objc subscript(i: String) -> Int { get set}
}

func concreteContext1() {
  @objc
  class subject_inConcreteContext {}
}

class ConcreteContext2 {
  @objc
  class subject_inConcreteContext {}
}

func genericContext1<T>() {
  @objc
  class subject_inGenericContext {}

  class subject_constructor_inGenericContext {
    @objc
    init() {} // expected-error{{initializer in a generic class cannot be represented in Objective-C}}
  }

  class subject_var_inGenericContext {
    @objc
    var subject_instanceVar: Int = 0 // expected-error{{variable in a generic class cannot be represented in Objective-C}}
  }

  class subject_func_inGenericContext {
    @objc
    func f() {} // expected-error{{method in a generic class cannot be represented in Objective-C}}
  }
}

class GenericContext2<T> {
  @objc
  class subject_inGenericContext {}

  @objc
  func f() {} // expected-error{{method in a generic class cannot be represented in Objective-C}}
}

class GenericContext3<T> {
  class MoreNested {
    @objc
    class subject_inGenericContext {}

    @objc
    func f() {} // expected-error{{method in a generic class cannot be represented in Objective-C}}
  }
}


class subject_subscriptIndexed1 {
  @objc
  subscript(a: Int) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptIndexed2 {
  @objc
  subscript(a: Int8) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptIndexed3 {
  @objc
  subscript(a: UInt8) -> Int { // no-error
    get { return 0 }
  }
}

class subject_subscriptKeyed1 {
  @objc
  subscript(a: String) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed2 {
  @objc
  subscript(a: Class_ObjC1) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed3 {
  @objc
  subscript(a: Class_ObjC1.Type) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed4 {
  @objc
  subscript(a: Protocol_ObjC1) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed5 {
  @objc
  subscript(a: Protocol_ObjC1.Type) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed6 {
  @objc
  subscript(a: protocol<Protocol_ObjC1, Protocol_ObjC2>) -> Int { // no-error
    get { return 0 }
  }
}
class subject_subscriptKeyed7 {
  @objc
  subscript(a: protocol<Protocol_ObjC1, Protocol_ObjC2>.Type) -> Int { // no-error
    get { return 0 }
  }
}



class subject_subscriptInvalid1 {
  @objc
  subscript(a: Float32) -> Int { // expected-error {{subscript cannot be marked @objc because its key type is neither an integer nor an object}}
    get { return 0 }
  }
}
class subject_subscriptInvalid2 {
  @objc
  subscript(a: PlainClass) -> Int {
  // expected-error@-1 {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid3 {
  @objc
  subscript(a: PlainClass.Type) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid4 {
  @objc
  subscript(a: PlainStruct) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{Swift structs cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid5 {
  @objc
  subscript(a: PlainEnum) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{enums cannot be represented in Objective-C}}
    get { return 0 }
  }
}
class subject_subscriptInvalid6 {
  @objc
  subscript(a: PlainProtocol) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol 'PlainProtocol' is not '@objc'}}
    get { return 0 }
  }
}
class subject_subscriptInvalid7 {
  @objc
  subscript(a: Protocol_Class1) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol 'Protocol_Class1' is not '@objc'}}
    get { return 0 }
  }
}
class subject_subscriptInvalid8 {
  @objc
  subscript(a: protocol<Protocol_Class1, Protocol_Class2>) -> Int { // expected-error {{subscript cannot be marked @objc because its type cannot be represented in Objective-C}}
    // expected-note@-1{{protocol 'Protocol_Class1' is not '@objc'}}
    get { return 0 }
  }
}
class subject_subscriptInvalid9<T> {
  @objc
  subscript(a: Int) -> Int { // expected-error{{subscript in a generic class cannot be represented in Objective-C}}
    get { return 0 }
  }
}

//===--- Tests for @objc inference.

@objc
class infer_instanceFunc1 {
// CHECK-LABEL: @objc class infer_instanceFunc1 {

  func func1() {}
// CHECK-LABEL: @objc func func1() {

  @objc func func1_() {} // no-error

  func func2(a: Int) {}
// CHECK-LABEL: @objc func func2(a: Int) {

  @objc func func2_(a: Int) {} // no-error

  func func3(a: Int) -> Int {}
// CHECK-LABEL: @objc func func3(a: Int) -> Int {

  @objc func func3_(a: Int) -> Int {} // no-error

  func func4(a: Int, b: Double) {}
// CHECK-LABEL: @objc func func4(a: Int, b: Double) {

  @objc func func4_(a: Int, b: Double) {} // no-error

  func func5(a: String) {}
// CHECK-LABEL: @objc func func5(a: String) {

  @objc func func5_(a: String) {} // no-error

  func func6() -> String {}
// CHECK-LABEL: @objc func func6() -> String {

  @objc func func6_() -> String {} // no-error

  func func7(a: PlainClass) {}
// CHECK-LABEL: {{^}} func func7(a: PlainClass) {

  @objc func func7_(a: PlainClass) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  func func7m(a: PlainClass.Type) {}
// CHECK-LABEL: {{^}} func func7m(a: PlainClass.Type) {

  @objc func func7m_(a: PlainClass.Type) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}

  func func8() -> PlainClass {}
// CHECK-LABEL: {{^}} func func8() -> PlainClass {

  @objc func func8_() -> PlainClass {}
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  func func8m() -> PlainClass.Type {}
// CHECK-LABEL: {{^}} func func8m() -> PlainClass.Type {

  @objc func func8m_() -> PlainClass.Type {}
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}

  func func9(a: PlainStruct) {}
// CHECK-LABEL: {{^}} func func9(a: PlainStruct) {

  @objc func func9_(a: PlainStruct) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  func func10() -> PlainStruct {}
// CHECK-LABEL: {{^}} func func10() -> PlainStruct {

  @objc func func10_() -> PlainStruct {}
  // expected-error@-1 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  func func11(a: PlainEnum) {}
// CHECK-LABEL: {{^}} func func11(a: PlainEnum) {

  @objc func func11_(a: PlainEnum) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift enums cannot be represented in Objective-C}}

  func func12(a: PlainProtocol) {}
// CHECK-LABEL: {{^}} func func12(a: PlainProtocol) {

  @objc func func12_(a: PlainProtocol) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'PlainProtocol' is not '@objc'}}

  func func13(a: Class_ObjC1) {}
// CHECK-LABEL: @objc func func13(a: Class_ObjC1) {

  @objc func func13_(a: Class_ObjC1) {} // no-error

  func func14(a: Protocol_Class1) {}
// CHECK-LABEL: {{^}} func func14(a: Protocol_Class1) {

  @objc func func14_(a: Protocol_Class1) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'Protocol_Class1' is not '@objc'}}

  func func15(a: Protocol_ObjC1) {}
// CHECK-LABEL: @objc func func15(a: Protocol_ObjC1) {
  @objc func func15_(a: Protocol_ObjC1) {} // no-error

  func func16(a: AnyObject) {}
// CHECK-LABEL: @objc func func16(a: AnyObject) {

  @objc func func16_(a: AnyObject) {} // no-error

  func func17(a: () -> ()) {}
// CHECK-LABEL: {{^}}  @objc func func17(a: () -> ()) {

  @objc func func17_(a: () -> ()) {}

  func func18(a: (Int) -> (), b: Int) {}
// CHECK-LABEL: {{^}}  @objc func func18(a: (Int) -> (), b: Int)

  @objc func func18_(a: (Int) -> (), b: Int) {}

  func func19(a: (String) -> (), b: Int) {}
// CHECK-LABEL: {{^}}  @objc func func19(a: (String) -> (), b: Int) {

  @objc func func19_(a: (String) -> (), b: Int) {}

  func func_FunctionReturn1() -> () -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn1() -> () -> () {

  @objc func func_FunctionReturn1_() -> () -> () {}

  func func_FunctionReturn2() -> (Int) -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn2() -> (Int) -> () {

  @objc func func_FunctionReturn2_() -> (Int) -> () {}

  func func_FunctionReturn3() -> () -> Int {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn3() -> () -> Int {

  @objc func func_FunctionReturn3_() -> () -> Int {}

  func func_FunctionReturn4() -> (String) -> () {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn4() -> (String) -> () {

  @objc func func_FunctionReturn4_() -> (String) -> () {}

  func func_FunctionReturn5() -> () -> String {}
// CHECK-LABEL: {{^}}  @objc func func_FunctionReturn5() -> () -> String {

  @objc func func_FunctionReturn5_() -> () -> String {}


  func func_ZeroParams1() {}
// CHECK-LABEL: @objc func func_ZeroParams1() {

  @objc func func_ZeroParams1a() {} // no-error


  func func_OneParam1(a: Int) {}
// CHECK-LABEL: @objc func func_OneParam1(a: Int) {

  @objc func func_OneParam1a(a: Int) {} // no-error


  func func_TupleStyle1(a: Int, b: Int) {}
  // CHECK-LABEL: {{^}} @objc func func_TupleStyle1(a: Int, b: Int) {

  @objc func func_TupleStyle1a(a: Int, b: Int) {}

  func func_TupleStyle2(a: Int, b: Int, c: Int) {}
// CHECK-LABEL: {{^}} @objc func func_TupleStyle2(a: Int, b: Int, c: Int) {

  @objc func func_TupleStyle2a(a: Int, b: Int, c: Int) {}

  func func_Curried1()() {}
// CHECK-LABEL: {{^}} func func_Curried1()() {

  @objc func func_Curried1_()() {}
  // expected-error@-1 {{method cannot be marked @objc because curried functions cannot be represented in Objective-C}}

  func func_Curried2()(a: Int) {}
// CHECK-LABEL: {{^}} func func_Curried2()(a: Int) {

  @objc func func_Curried2_()(a: Int) {}
  // expected-error@-1 {{method cannot be marked @objc because curried functions cannot be represented in Objective-C}}

  func func_Curried3()() -> Int {}
// CHECK-LABEL: {{^}} func func_Curried3()() -> Int {

  @objc func func_Curried3_()() -> Int {}
  // expected-error@-1 {{method cannot be marked @objc because curried functions cannot be represented in Objective-C}}

  func func_Curried4()(a: String) {}
// CHECK-LABEL: {{^}} func func_Curried4()(a: String) {

  @objc func func_Curried4_()(a: String) {}
  // expected-error@-1 {{method cannot be marked @objc because curried functions cannot be represented in Objective-C}}

  func func_Curried5()() -> String {}
// CHECK-LABEL: {{^}} func func_Curried5()() -> String {

  @objc func func_Curried5_()() -> String {}
  // expected-error@-1 {{method cannot be marked @objc because curried functions cannot be represented in Objective-C}}


  // Check that we produce diagnostics for every parameter and return type.
  @objc func func_MultipleDiags(a: PlainStruct, b: PlainEnum) -> protocol<> {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter 1 cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // expected-error@-3 {{method cannot be marked @objc because the type of the parameter 2 cannot be represented in Objective-C}}
  // expected-note@-4 {{Swift enums cannot be represented in Objective-C}}
  // expected-error@-5 {{method cannot be marked @objc because its result type cannot be represented in Objective-C}}
  // expected-note@-6 {{'protocol<>' is not considered '@objc'; use 'AnyObject' instead}}

  @objc func func_UnnamedParam1(_: Int) {} // no-error

  @objc func func_UnnamedParam2(_: PlainStruct) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
}

@objc
class infer_constructor1 {
// CHECK-LABEL: @objc class infer_constructor1

  init() {}
  // CHECK: @objc init()

  init(a: Int) {}
  // CHECK: @objc init(a: Int)

  init(a: PlainStruct) {}
  // CHECK: {{^}} init(a: PlainStruct)
}

@objc
class infer_destructor1 {
// CHECK-LABEL: @objc class infer_destructor1

  deinit {}
  // CHECK: @objc deinit
}

// @!objc
class infer_destructor2 {
// CHECK-LABEL: {{^}}class infer_destructor2

  deinit {}
  // CHECK: @objc deinit
}

@objc
class infer_instanceVar1 {
// CHECK-LABEL: @objc class infer_instanceVar1 {
  init() {}

  var instanceVar1: Int
  // CHECK: @objc var instanceVar1: Int

  var (instanceVar2, instanceVar3): (Int, PlainProtocol)
  // CHECK: @objc var instanceVar2: Int
  // CHECK: {{^}}  var instanceVar3: PlainProtocol

  @objc var (instanceVar1_, instanceVar2_): (Int, PlainProtocol)
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'PlainProtocol' is not '@objc'}}

  var intstanceVar4: Int {
  // CHECK: @objc var intstanceVar4: Int {
    get {}
    // CHECK-NEXT: @objc get {}
  }

  var intstanceVar5: Int {
  // CHECK: @objc var intstanceVar5: Int {
    get {}
    // CHECK-NEXT: @objc get {}
    set {}
    // CHECK-NEXT: @objc set {}
  }

  @objc var instanceVar5_: Int {
  // CHECK: @objc var instanceVar5_: Int {
    get {}
    // CHECK-NEXT: @objc get {}
    set {}
    // CHECK-NEXT: @objc set {}
  }

  var observingAccesorsVar1: Int {
  // CHECK: @objc var observingAccesorsVar1: Int {
    willSet {}
    // CHECK-NEXT: {{^}} @final willSet {}
    didSet {}
    // CHECK-NEXT: {{^}} @final didSet {}
  }

  @objc var observingAccesorsVar1_: Int {
  // CHECK: {{^}} @objc var observingAccesorsVar1_: Int {
    willSet {}
    // CHECK-NEXT: {{^}} @final willSet {}
    didSet {}
    // CHECK-NEXT: {{^}} @final didSet {}
  }


  var var_Int: Int
// CHECK-LABEL: @objc var var_Int: Int

  var var_Bool: Bool
// CHECK-LABEL: @objc var var_Bool: Bool

  var var_CBool: CBool
// CHECK-LABEL: @objc var var_CBool: CBool

  var var_String: String
// CHECK-LABEL: @objc var var_String: String

  var var_Float: Float
  var var_Double: Double
// CHECK-LABEL: @objc var var_Float: Float
// CHECK-LABEL: @objc var var_Double: Double

  var var_Char: UnicodeScalar
// CHECK-LABEL: @objc var var_Char: UnicodeScalar

  //===--- Tuples.

  var var_tuple1: ()
// CHECK-LABEL: {{^}} var var_tuple1: ()

  @objc var var_tuple1_: ()
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{empty tuple type cannot be represented in Objective-C}}

  var var_tuple2: Void
// CHECK-LABEL: {{^}} var var_tuple2: Void

  @objc var var_tuple2_: Void
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{empty tuple type cannot be represented in Objective-C}}

  var var_tuple3: (Int)
// CHECK-LABEL: @objc var var_tuple3: (Int)

  @objc var var_tuple3_: (Int) // no-error

  var var_tuple4: (Int, Int)
// CHECK-LABEL: {{^}} var var_tuple4: (Int, Int)

  @objc var var_tuple4_: (Int, Int)
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{tuples cannot be represented in Objective-C}}

  //===--- Stdlib integer types.

  var var_Int8: Int8
  var var_Int16: Int16
  var var_Int32: Int32
  var var_Int64: Int64
// CHECK-LABEL: @objc var var_Int8: Int8
// CHECK-LABEL: @objc var var_Int16: Int16
// CHECK-LABEL: @objc var var_Int32: Int32
// CHECK-LABEL: @objc var var_Int64: Int64

  var var_UInt8: UInt8
  var var_UInt16: UInt16
  var var_UInt32: UInt32
  var var_UInt64: UInt64
// CHECK-LABEL: @objc var var_UInt8: UInt8
// CHECK-LABEL: @objc var var_UInt16: UInt16
// CHECK-LABEL: @objc var var_UInt32: UInt32
// CHECK-LABEL: @objc var var_UInt64: UInt64

  var var_COpaquePointer: COpaquePointer
// CHECK-LABEL: @objc var var_COpaquePointer: COpaquePointer

  var var_PlainClass: PlainClass
// CHECK-LABEL: {{^}}  var var_PlainClass: PlainClass

  @objc var var_PlainClass_: PlainClass
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{classes not annotated with @objc cannot be represented in Objective-C}}

  var var_PlainStruct: PlainStruct
// CHECK-LABEL: {{^}}  var var_PlainStruct: PlainStruct

  @objc var var_PlainStruct_: PlainStruct
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}

  var var_PlainEnum: PlainEnum
// CHECK-LABEL: {{^}}  var var_PlainEnum: PlainEnum

  @objc var var_PlainEnum_: PlainEnum
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift enums cannot be represented in Objective-C}}

  var var_PlainProtocol: PlainProtocol
// CHECK-LABEL: {{^}}  var var_PlainProtocol: PlainProtocol

  @objc var var_PlainProtocol_: PlainProtocol
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'PlainProtocol' is not '@objc'}}

  var var_ClassObjC: Class_ObjC1
// CHECK-LABEL: @objc var var_ClassObjC: Class_ObjC1

  @objc var var_ClassObjC_: Class_ObjC1 // no-error

  var var_ProtocolClass: Protocol_Class1
// CHECK-LABEL: {{^}}  var var_ProtocolClass: Protocol_Class1

  @objc var var_ProtocolClass_: Protocol_Class1
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'Protocol_Class1' is not '@objc'}}

  var var_ProtocolObjC: Protocol_ObjC1
// CHECK-LABEL: @objc var var_ProtocolObjC: Protocol_ObjC1

  @objc var var_ProtocolObjC_: Protocol_ObjC1 // no-error


  var var_PlainClassMetatype: PlainClass.Type
// CHECK-LABEL: {{^}}  var var_PlainClassMetatype: PlainClass.Type

  @objc var var_PlainClassMetatype_: PlainClass.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainStructMetatype: PlainStruct.Type
// CHECK-LABEL: {{^}}  var var_PlainStructMetatype: PlainStruct.Type

  @objc var var_PlainStructMetatype_: PlainStruct.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainEnumMetatype: PlainEnum.Type
// CHECK-LABEL: {{^}}  var var_PlainEnumMetatype: PlainEnum.Type

  @objc var var_PlainEnumMetatype_: PlainEnum.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_PlainExistentialMetatype: PlainProtocol.Type
// CHECK-LABEL: {{^}}  var var_PlainExistentialMetatype: PlainProtocol.Type

  @objc var var_PlainExistentialMetatype_: PlainProtocol.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ClassObjCMetatype: Class_ObjC1.Type
// CHECK-LABEL: @objc var var_ClassObjCMetatype: Class_ObjC1.Type

  @objc var var_ClassObjCMetatype_: Class_ObjC1.Type // no-error

  var var_ProtocolClassMetatype: Protocol_Class1.Type
// CHECK-LABEL: {{^}}  var var_ProtocolClassMetatype: Protocol_Class1.Type

  @objc var var_ProtocolClassMetatype_: Protocol_Class1.Type
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}

  var var_ProtocolObjCMetatype1: Protocol_ObjC1.Type
// CHECK-LABEL: @objc var var_ProtocolObjCMetatype1: Protocol_ObjC1.Type

  @objc var var_ProtocolObjCMetatype1_: Protocol_ObjC1.Type // no-error

  var var_ProtocolObjCMetatype2: Protocol_ObjC2.Type
// CHECK-LABEL: @objc var var_ProtocolObjCMetatype2: Protocol_ObjC2.Type

  @objc var var_ProtocolObjCMetatype2_: Protocol_ObjC2.Type // no-error

  var var_AnyObject1: AnyObject
  var var_AnyObject2: AnyObject.Type
// CHECK-LABEL: @objc var var_AnyObject1: AnyObject
// CHECK-LABEL: @objc var var_AnyObject2: AnyObject.Type

  var var_Existential0: protocol<>
// CHECK-LABEL: {{^}}  var var_Existential0: protocol<>

  @objc var var_Existential0_: protocol<>
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{'protocol<>' is not considered '@objc'; use 'AnyObject' instead}}

  var var_Existential1: protocol<PlainProtocol>
// CHECK-LABEL: {{^}}  var var_Existential1: PlainProtocol

  @objc var var_Existential1_: protocol<PlainProtocol>
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'PlainProtocol' is not '@objc'}}

  var var_Existential2: protocol<PlainProtocol, PlainProtocol>
// CHECK-LABEL: {{^}}  var var_Existential2: PlainProtocol

  @objc var var_Existential2_: protocol<PlainProtocol, PlainProtocol>
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'PlainProtocol' is not '@objc'}}

  var var_Existential3: protocol<PlainProtocol, Protocol_Class1>
// CHECK-LABEL: {{^}}  var var_Existential3: protocol<PlainProtocol, Protocol_Class1>

  @objc var var_Existential3_: protocol<PlainProtocol, Protocol_Class1>
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'PlainProtocol' is not '@objc'}}

  var var_Existential4: protocol<PlainProtocol, Protocol_ObjC1>
// CHECK-LABEL: {{^}}  var var_Existential4: protocol<PlainProtocol, Protocol_ObjC1>

  @objc var var_Existential4_: protocol<PlainProtocol, Protocol_ObjC1>
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'PlainProtocol' is not '@objc'}}

  var var_Existential5: protocol<Protocol_Class1>
// CHECK-LABEL: {{^}}  var var_Existential5: Protocol_Class1

  @objc var var_Existential5_: protocol<Protocol_Class1>
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'Protocol_Class1' is not '@objc'}}

  var var_Existential6: protocol<Protocol_Class1, Protocol_Class2>
// CHECK-LABEL: {{^}}  var var_Existential6: protocol<Protocol_Class1, Protocol_Class2>

  @objc var var_Existential6_: protocol<Protocol_Class1, Protocol_Class2>
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'Protocol_Class1' is not '@objc'}}

  var var_Existential7: protocol<Protocol_Class1, Protocol_ObjC1>
// CHECK-LABEL: {{^}}  var var_Existential7: protocol<Protocol_Class1, Protocol_ObjC1>

  @objc var var_Existential7_: protocol<Protocol_Class1, Protocol_ObjC1>
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{protocol 'Protocol_Class1' is not '@objc'}}

  var var_Existential8: protocol<Protocol_ObjC1>
// CHECK-LABEL: @objc var var_Existential8: Protocol_ObjC1

  @objc var var_Existential8_: protocol<Protocol_ObjC1> // no-error

  var var_Existential9: protocol<Protocol_ObjC1, Protocol_ObjC2>
// CHECK-LABEL: @objc var var_Existential9: protocol<Protocol_ObjC1, Protocol_ObjC2>

  @objc var var_Existential9_: protocol<Protocol_ObjC1, Protocol_ObjC2> // no-error


  var var_ExistentialMetatype0: protocol<>.Type
  var var_ExistentialMetatype1: protocol<PlainProtocol>.Type
  var var_ExistentialMetatype2: protocol<PlainProtocol, PlainProtocol>.Type
  var var_ExistentialMetatype3: protocol<PlainProtocol, Protocol_Class1>.Type
  var var_ExistentialMetatype4: protocol<PlainProtocol, Protocol_ObjC1>.Type
  var var_ExistentialMetatype5: protocol<Protocol_Class1>.Type
  var var_ExistentialMetatype6: protocol<Protocol_Class1, Protocol_Class2>.Type
  var var_ExistentialMetatype7: protocol<Protocol_Class1, Protocol_ObjC1>.Type
  var var_ExistentialMetatype8: protocol<Protocol_ObjC1>.Type
  var var_ExistentialMetatype9: protocol<Protocol_ObjC1, Protocol_ObjC2>.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype0: protocol<>.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype1: PlainProtocol.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype2: PlainProtocol.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype3: protocol<PlainProtocol, Protocol_Class1>.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype4: protocol<PlainProtocol, Protocol_ObjC1>.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype5: Protocol_Class1.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype6: protocol<Protocol_Class1, Protocol_Class2>.Type
// CHECK-LABEL: {{^}}  var var_ExistentialMetatype7: protocol<Protocol_Class1, Protocol_ObjC1>.Type
// CHECK-LABEL: @objc var var_ExistentialMetatype8: Protocol_ObjC1.Type
// CHECK-LABEL: @objc var var_ExistentialMetatype9: protocol<Protocol_ObjC1, Protocol_ObjC2>.Type


  var var_UnsafePointer1: UnsafePointer<Int>
  var var_UnsafePointer2: UnsafePointer<Bool>
  var var_UnsafePointer3: UnsafePointer<CBool>
  var var_UnsafePointer4: UnsafePointer<String>
  var var_UnsafePointer5: UnsafePointer<Float>
  var var_UnsafePointer6: UnsafePointer<Double>
  var var_UnsafePointer7: UnsafePointer<COpaquePointer>
  var var_UnsafePointer8: UnsafePointer<PlainClass>
  var var_UnsafePointer9: UnsafePointer<PlainStruct>
  var var_UnsafePointer10: UnsafePointer<PlainEnum>
  var var_UnsafePointer11: UnsafePointer<PlainProtocol>
  var var_UnsafePointer12: UnsafePointer<AnyObject>
  var var_UnsafePointer13: UnsafePointer<AnyObject.Type>
  var var_UnsafePointer100: UnsafePointer<()>
  var var_UnsafePointer101: UnsafePointer<Void>
  var var_UnsafePointer102: UnsafePointer<(Int, Int)>
// CHECK-LABEL: @objc var var_UnsafePointer1: UnsafePointer<Int>
// CHECK-LABEL: @objc var var_UnsafePointer2: UnsafePointer<Bool>
// CHECK-LABEL: @objc var var_UnsafePointer3: UnsafePointer<CBool>
// CHECK-LABEL: {{^}}  var var_UnsafePointer4: UnsafePointer<String>
// CHECK-LABEL: @objc var var_UnsafePointer5: UnsafePointer<Float>
// CHECK-LABEL: @objc var var_UnsafePointer6: UnsafePointer<Double>
// CHECK-LABEL: @objc var var_UnsafePointer7: UnsafePointer<COpaquePointer>
// CHECK-LABEL: {{^}}  var var_UnsafePointer8: UnsafePointer<PlainClass>
// CHECK-LABEL: {{^}}  var var_UnsafePointer9: UnsafePointer<PlainStruct>
// CHECK-LABEL: {{^}}  var var_UnsafePointer10: UnsafePointer<PlainEnum>
// CHECK-LABEL: {{^}}  var var_UnsafePointer11: UnsafePointer<PlainProtocol>
// CHECK-LABEL: @objc var var_UnsafePointer12: UnsafePointer<AnyObject>
// CHECK-LABEL: @objc var var_UnsafePointer13: UnsafePointer<AnyObject.Type>
// CHECK-LABEL: {{^}} @objc var var_UnsafePointer100: UnsafePointer<()>
// CHECK-LABEL: {{^}} @objc var var_UnsafePointer101: UnsafePointer<Void>
// CHECK-LABEL: {{^}}  var var_UnsafePointer102: UnsafePointer<(Int, Int)>

  var var_Optional1: Class_ObjC1?
  var var_Optional2: Protocol_ObjC1?
  var var_Optional3: Class_ObjC1.Type?
  var var_Optional4: Protocol_ObjC1.Type?
  var var_Optional5: AnyObject?
  var var_Optional6: AnyObject.Type?
  var var_Optional7: String?
  var var_Optional8: protocol<Protocol_ObjC1>?
  var var_Optional9: protocol<Protocol_ObjC1>.Type?
  var var_Optional10: protocol<Protocol_ObjC1, Protocol_ObjC2>?
  var var_Optional11: protocol<Protocol_ObjC1, Protocol_ObjC2>.Type?

// CHECK-LABEL: @objc var var_Optional1: Class_ObjC1?
// CHECK-LABEL: @objc var var_Optional2: Protocol_ObjC1?
// CHECK-LABEL: @objc var var_Optional3: Class_ObjC1.Type?
// CHECK-LABEL: @objc var var_Optional4: Protocol_ObjC1.Type?
// CHECK-LABEL: @objc var var_Optional5: AnyObject?
// CHECK-LABEL: @objc var var_Optional6: AnyObject.Type?
// CHECK-LABEL: @objc var var_Optional7: String?
// CHECK-LABEL: @objc var var_Optional8: Protocol_ObjC1?
// CHECK-LABEL: @objc var var_Optional9: Protocol_ObjC1.Type?
// CHECK-LABEL: @objc var var_Optional10: protocol<Protocol_ObjC1, Protocol_ObjC2>?
// CHECK-LABEL: @objc var var_Optional11: protocol<Protocol_ObjC1, Protocol_ObjC2>.Type?


  var var_ImplicitlyUnwrappedOptional1: Class_ObjC1!
  var var_ImplicitlyUnwrappedOptional2: Protocol_ObjC1!
  var var_ImplicitlyUnwrappedOptional3: Class_ObjC1.Type!
  var var_ImplicitlyUnwrappedOptional4: Protocol_ObjC1.Type!
  var var_ImplicitlyUnwrappedOptional5: AnyObject!
  var var_ImplicitlyUnwrappedOptional6: AnyObject.Type!
  var var_ImplicitlyUnwrappedOptional7: String!
  var var_ImplicitlyUnwrappedOptional8: protocol<Protocol_ObjC1>!
  var var_ImplicitlyUnwrappedOptional9: protocol<Protocol_ObjC1, Protocol_ObjC2>!

// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional1: Class_ObjC1!
// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional2: Protocol_ObjC1!
// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional3: Class_ObjC1.Type!
// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional4: Protocol_ObjC1.Type!
// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional5: AnyObject!
// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional6: AnyObject.Type!
// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional7: String!
// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional8: Protocol_ObjC1!
// CHECK-LABEL: @objc var var_ImplicitlyUnwrappedOptional9: protocol<Protocol_ObjC1, Protocol_ObjC2>!

  var var_Optional_fail1: PlainClass?
  var var_Optional_fail2: PlainClass.Type?
  var var_Optional_fail3: PlainClass!
  var var_Optional_fail4: PlainStruct?
  var var_Optional_fail5: PlainStruct.Type?
  var var_Optional_fail6: PlainEnum?
  var var_Optional_fail7: PlainEnum.Type?
  var var_Optional_fail8: PlainProtocol?
  var var_Optional_fail9: protocol<>?
  var var_Optional_fail10: protocol<PlainProtocol>?
  var var_Optional_fail11: protocol<PlainProtocol, Protocol_ObjC1>?
  var var_Optional_fail12: Int?
  var var_Optional_fail13: Bool?
  var var_Optional_fail14: CBool?
  var var_Optional_fail16: COpaquePointer?
  var var_Optional_fail17: UnsafePointer<Int>?
  var var_Optional_fail18: UnsafePointer<Class_ObjC1>?
  var var_Optional_fail20: AnyObject??
  var var_Optional_fail21: AnyObject.Type??
// CHECK-NOT: @objc{{.*}}Optional_fail


  weak var var_Weak1: Class_ObjC1?
  weak var var_Weak2: Protocol_ObjC1?
  // <rdar://problem/16473062> weak and unowned variables of metatypes are rejected
  //weak var var_Weak3: Class_ObjC1.Type?
  //weak var var_Weak4: Protocol_ObjC1.Type?
  weak var var_Weak5: AnyObject?
  //weak var var_Weak6: AnyObject.Type?
  weak var var_Weak7: protocol<Protocol_ObjC1>?
  weak var var_Weak8: protocol<Protocol_ObjC1, Protocol_ObjC2>?

// CHECK-LABEL: @objc var var_Weak1: @sil_weak Class_ObjC1
// CHECK-LABEL: @objc var var_Weak2: @sil_weak Protocol_ObjC1
// CHECK-LABEL: @objc var var_Weak5: @sil_weak AnyObject
// CHECK-LABEL: @objc var var_Weak7: @sil_weak Protocol_ObjC1
// CHECK-LABEL: @objc var var_Weak8: @sil_weak protocol<Protocol_ObjC1, Protocol_ObjC2>


  weak var var_Weak_fail1: PlainClass?
  weak var var_Weak_bad2: PlainStruct?
  // expected-error@-1 {{'weak' cannot be applied to non-class type 'PlainStruct'}}
  weak var var_Weak_bad3: PlainEnum?
  // expected-error@-1 {{'weak' cannot be applied to non-class type 'PlainEnum'}}
  weak var var_Weak_bad4: String?
  // expected-error@-1 {{'weak' cannot be applied to non-class type 'String'}}
// CHECK-NOT: @objc{{.*}}Weak_fail


  unowned var var_Unowned1: Class_ObjC1
  unowned var var_Unowned2: Protocol_ObjC1
  // <rdar://problem/16473062> weak and unowned variables of metatypes are rejected
  //unowned var var_Unowned3: Class_ObjC1.Type
  //unowned var var_Unowned4: Protocol_ObjC1.Type
  unowned var var_Unowned5: AnyObject
  //unowned var var_Unowned6: AnyObject.Type
  unowned var var_Unowned7: protocol<Protocol_ObjC1>
  unowned var var_Unowned8: protocol<Protocol_ObjC1, Protocol_ObjC2>

// CHECK-LABEL: @objc var var_Unowned1: @sil_unowned Class_ObjC1
// CHECK-LABEL: @objc var var_Unowned2: @sil_unowned Protocol_ObjC1
// CHECK-LABEL: @objc var var_Unowned5: @sil_unowned AnyObject
// CHECK-LABEL: @objc var var_Unowned7: @sil_unowned Protocol_ObjC1
// CHECK-LABEL: @objc var var_Unowned8: @sil_unowned protocol<Protocol_ObjC1, Protocol_ObjC2>


  unowned var var_Unowned_fail1: PlainClass
  unowned var var_Unowned_bad2: PlainStruct
  // expected-error@-1 {{'unowned' cannot be applied to non-class type 'PlainStruct'}}
  unowned var var_Unowned_bad3: PlainEnum
  // expected-error@-1 {{'unowned' cannot be applied to non-class type 'PlainEnum'}}
  unowned var var_Unowned_bad4: String
  // expected-error@-1 {{'unowned' cannot be applied to non-class type 'String'}}
// CHECK-NOT: @objc{{.*}}Unowned_fail


  var var_FunctionType1: () -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType1: () -> ()

  var var_FunctionType2: (Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType2: (Int) -> ()

  var var_FunctionType3: (Int) -> Int
// CHECK-LABEL: {{^}}  @objc var var_FunctionType3: (Int) -> Int

  var var_FunctionType4: (Int, Double) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType4: (Int, Double) -> ()

  var var_FunctionType5: (String) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType5: (String) -> ()

  var var_FunctionType6: () -> String
// CHECK-LABEL: {{^}}  @objc var var_FunctionType6: () -> String

  var var_FunctionType7: (PlainClass) -> ()
// CHECK-NOT: @objc var var_FunctionType7: (PlainClass) -> ()

  var var_FunctionType8: () -> PlainClass
// CHECK-NOT: @objc var var_FunctionType8: () -> PlainClass

  var var_FunctionType9: (PlainStruct) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType9: (PlainStruct) -> ()

  var var_FunctionType10: () -> PlainStruct
// CHECK-LABEL: {{^}}  var var_FunctionType10: () -> PlainStruct

  var var_FunctionType11: (PlainEnum) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType11: (PlainEnum) -> ()

  var var_FunctionType12: (PlainProtocol) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType12: (PlainProtocol) -> ()

  var var_FunctionType13: (Class_ObjC1) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType13: (Class_ObjC1) -> ()

  var var_FunctionType14: (Protocol_Class1) -> ()
// CHECK-LABEL: {{^}}  var var_FunctionType14: (Protocol_Class1) -> ()

  var var_FunctionType15: (Protocol_ObjC1) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType15: (Protocol_ObjC1) -> ()

  var var_FunctionType16: (AnyObject) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType16: (AnyObject) -> ()

  var var_FunctionType17: (() -> ()) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType17: (() -> ()) -> ()

  var var_FunctionType18: ((Int) -> (), Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType18: ((Int) -> (), Int) -> ()

  var var_FunctionType19: ((String) -> (), Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionType19: ((String) -> (), Int) -> ()


  var var_FunctionTypeReturn1: () -> () -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn1: () -> () -> ()

  @objc var var_FunctionTypeReturn1_: () -> () -> () // no-error

  var var_FunctionTypeReturn2: () -> (Int) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn2: () -> (Int) -> ()

  @objc var var_FunctionTypeReturn2_: () -> (Int) -> () // no-error

  var var_FunctionTypeReturn3: () -> () -> Int
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn3: () -> () -> Int

  @objc var var_FunctionTypeReturn3_: () -> () -> Int // no-error

  var var_FunctionTypeReturn4: () -> (String) -> ()
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn4: () -> (String) -> ()

  @objc var var_FunctionTypeReturn4_: () -> (String) -> () // no-error

  var var_FunctionTypeReturn5: () -> () -> String
// CHECK-LABEL: {{^}}  @objc var var_FunctionTypeReturn5: () -> () -> String

  @objc var var_FunctionTypeReturn5_: () -> () -> String // no-error


  var var_BlockFunctionType1: @objc_block () -> ()
// CHECK-LABEL: @objc var var_BlockFunctionType1: @objc_block () -> ()

  @objc var var_BlockFunctionType1_: @objc_block () -> () // no-error
}

@objc
class infer_instanceVar2<
    GP_Unconstrained,
    GP_PlainClass : PlainClass,
    GP_PlainProtocol : PlainProtocol,
    GP_Class_ObjC : Class_ObjC1,
    GP_Protocol_Class : Protocol_Class1,
    GP_Protocol_ObjC : Protocol_ObjC1> {
// CHECK-LABEL: @objc class infer_instanceVar2<{{.*}}> {
  init() {}

  var var_GP_Unconstrained: GP_Unconstrained
// CHECK-LABEL: {{^}}  var var_GP_Unconstrained: GP_Unconstrained

  @objc var var_GP_Unconstrained_: GP_Unconstrained
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_PlainClass: GP_PlainClass
// CHECK-LABEL: {{^}}  var var_GP_PlainClass: GP_PlainClass

  @objc var var_GP_PlainClass_: GP_PlainClass
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_PlainProtocol: GP_PlainProtocol
// CHECK-LABEL: {{^}}  var var_GP_PlainProtocol: GP_PlainProtocol

  @objc var var_GP_PlainProtocol_: GP_PlainProtocol
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Class_ObjC: GP_Class_ObjC
// CHECK-LABEL: {{^}}  var var_GP_Class_ObjC: GP_Class_ObjC

  @objc var var_GP_Class_ObjC_: GP_Class_ObjC
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Protocol_Class: GP_Protocol_Class
// CHECK-LABEL: {{^}}  var var_GP_Protocol_Class: GP_Protocol_Class

  @objc var var_GP_Protocol_Class_: GP_Protocol_Class
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  var var_GP_Protocol_ObjC: GP_Protocol_ObjC
// CHECK-LABEL: {{^}}  var var_GP_Protocol_ObjC: GP_Protocol_ObjC

  @objc var var_GP_Protocol_ObjCa: GP_Protocol_ObjC
  // expected-error@-1 {{property cannot be marked @objc because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}

  func func_GP_Unconstrained(a: GP_Unconstrained) {}
// CHECK-LABEL: {{^}} func func_GP_Unconstrained(a: GP_Unconstrained) {

  @objc func func_GP_Unconstrained_(a: GP_Unconstrained) {}
  // expected-error@-1 {{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-2 {{generic type parameters cannot be represented in Objective-C}}
  // expected-error@-3 {{method in a generic class cannot be represented in Objective-C}}
}

class infer_instanceVar3 : Class_ObjC1 {
// CHECK-LABEL: @objc class infer_instanceVar3 : Class_ObjC1 {

  var v1: Int = 0
// CHECK-LABEL: @objc var v1: Int
}


@objc @class_protocol
protocol infer_instanceVar4 {
// CHECK-LABEL: @class_protocol @objc protocol infer_instanceVar4 {

  var v1: Int { get }
// CHECK-LABEL: @objc var v1: Int { get }
}

// @!objc
class infer_instanceVar5 {
// CHECK-LABEL: {{^}}class infer_instanceVar5 {

  @objc
  var intstanceVar1: Int {
  // CHECK: @objc var intstanceVar1: Int
    get {}
    // CHECK: @objc get {}
    set {}
    // CHECK: @objc set {}
  }
}

@objc
class infer_staticVar1 {
// CHECK-LABEL: @objc class infer_staticVar1 {

  class var staticVar1: Int = 42 // expected-error {{class variables not yet supported}}
  // CHECK: @objc class var staticVar1: Int
}

// @!objc
class infer_subscript1 {
// CHECK-LABEL: class infer_subscript1

  @objc
  subscript(i: Int) -> Int {
  // CHECK: @objc subscript (i: Int) -> Int
    get {}
    // CHECK: @objc get {}
    set {}
    // CHECK: @objc set {}
  }
}


@class_protocol @objc
protocol infer_throughConformanceProto1 {
// CHECK-LABEL: @objc @class_protocol protocol infer_throughConformanceProto1 {

  func funcObjC1()
  var varObjC1: Int { get }
  var varObjC2: Int { get set }
  // CHECK: @objc func funcObjC1()
  // CHECK: @objc var varObjC1: Int { get }
  // CHECK: @objc var varObjC2: Int { get set }
}

class infer_throughConformance1 : infer_throughConformanceProto1 {
// CHECK-LABEL: {{^}}class infer_throughConformance1 : infer_throughConformanceProto1 {
  func funcObjC1() {}
  var varObjC1: Int = 0
  var varObjC2: Int = 0
  // CHECK: @objc func funcObjC1() {
  // CHECK: @objc var varObjC1: Int
  // CHECK: @objc var varObjC2: Int

  func nonObjC1() {}
  // CHECK: {{^}} func nonObjC1() {

  // CHECK: @objc deinit  {
}


class infer_class1 : PlainClass {}
// CHECK-LABEL: {{^}}class infer_class1 : PlainClass {

class infer_class2 : Class_ObjC1 {}
// CHECK-LABEL: @objc class infer_class2 : Class_ObjC1 {

class infer_class3 : infer_class2 {}
// CHECK-LABEL: @objc class infer_class3 : infer_class2 {

class infer_class4 : Protocol_Class1 {}
// CHECK-LABEL: {{^}}class infer_class4 : Protocol_Class1 {

class infer_class5 : Protocol_ObjC1 {}
// CHECK-LABEL: {{^}}class infer_class5 : Protocol_ObjC1 {

//
// If a protocol conforms to an @objc protocol, this does not infer @objc on
// the protocol itself, or on the newly introduced requirements.  Only the
// inherited @objc requirements get @objc.
//
// Same rule applies to classes.
//

protocol infer_protocol1 {
// CHECK-LABEL: {{^}}protocol infer_protocol1 {

  func func_Curried1()() // no-error
  // CHECK: {{^}} func func_Curried1()()

  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

protocol infer_protocol2 : Protocol_Class1 {
// CHECK-LABEL: {{^}}protocol infer_protocol2 : Protocol_Class1 {

  func func_Curried1()() // no-error
  // CHECK: {{^}} func func_Curried1()()

  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

protocol infer_protocol3 : Protocol_ObjC1 {
// CHECK-LABEL: {{^}}protocol infer_protocol3 : Protocol_ObjC1 {

  func func_Curried1()() // no-error
  // CHECK: {{^}} func func_Curried1()()

  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

protocol infer_protocol4 : Protocol_Class1, Protocol_ObjC1 {
// CHECK-LABEL: {{^}}protocol infer_protocol4 : Protocol_Class1, Protocol_ObjC1 {

  func func_Curried1()() // no-error
  // CHECK: {{^}} func func_Curried1()()

  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

protocol infer_protocol5 : Protocol_ObjC1, Protocol_Class1 {
// CHECK-LABEL: {{^}}protocol infer_protocol5 : Protocol_ObjC1, Protocol_Class1 {

  func func_Curried1()() // no-error
  // CHECK: {{^}} func func_Curried1()()

  func nonObjC1()
  // CHECK: {{^}} func nonObjC1()
}

class C {
  // Don't crash.
  @objc func foo(x: Undeclared) {} // expected-error {{use of undeclared type 'Undeclared'}}
  @IBAction func myAction(sender: Undeclared) {} // expected-error {{use of undeclared type 'Undeclared'}}
}

//===---
//===--- @IBOutlet implies @objc
//===---

class HasIBOutlet {
// CHECK-LABEL: {{^}}class HasIBOutlet {

  init() {}

  @IBOutlet weak var goodOutlet: Class_ObjC1!
  // CHECK-LABEL: {{^}}  @objc @IBOutlet var goodOutlet: @sil_weak Class_ObjC1!

  @IBOutlet var badOutlet: PlainStruct
  // expected-error@-1 {{'IBOutlet' property cannot have non-object type 'PlainStruct'}}
  // CHECK-LABEL: {{^}}  @IBOutlet var badOutlet: PlainStruct
}

//===---
//===--- @NSManaged implies @objc
//===---

class HasNSManaged {
// CHECK-LABEL: {{^}}class HasNSManaged {

  init() {}

  @NSManaged
  var goodManaged: Class_ObjC1
  // CHECK-LABEL: {{^}}  @objc @NSManaged var goodManaged: Class_ObjC1

  @NSManaged
  var badManaged: PlainStruct
  // expected-error@-1 {{property cannot be marked @NSManaged because its type cannot be represented in Objective-C}}
  // expected-note@-2 {{Swift structs cannot be represented in Objective-C}}
  // CHECK-LABEL: {{^}}  @NSManaged var badManaged: PlainStruct
}

//===---
//===--- Pointer argument types
//===---

@objc class TakesCPointers {
// CHECK-LABEL: {{^}}@objc class TakesCPointers {
  func constUnsafePointer(p: ConstUnsafePointer<Int>) {}
  // CHECK-LABEL: @objc func constUnsafePointer(p: ConstUnsafePointer<Int>) {

  func constUnsafePointerToAnyObject(p: ConstUnsafePointer<AnyObject>) {}
  // CHECK-LABEL: @objc func constUnsafePointerToAnyObject(p: ConstUnsafePointer<AnyObject>) {

  func constUnsafePointerToClass(p: ConstUnsafePointer<TakesCPointers>) {}
  // CHECK-LABEL: @objc func constUnsafePointerToClass(p: ConstUnsafePointer<TakesCPointers>) {

  func mutableUnsafePointer(p: UnsafePointer<Int>) {}
  // CHECK-LABEL: @objc func mutableUnsafePointer(p: UnsafePointer<Int>) {

  func mutableStrongUnsafePointerToAnyObject(p: UnsafePointer<AnyObject>) {}
  // CHECK-LABEL: {{^}} @objc func mutableStrongUnsafePointerToAnyObject(p: UnsafePointer<AnyObject>) {

  func mutableAutoreleasingUnsafePointerToAnyObject(p: AutoreleasingUnsafePointer<AnyObject>) {}
  // CHECK-LABEL: {{^}} @objc func mutableAutoreleasingUnsafePointerToAnyObject(p: AutoreleasingUnsafePointer<AnyObject>) {

}

// @objc with nullary names
@objc(NSObjC2)
class Class_ObjC2 {
// CHECK-LABEL: @objc(NSObjC2) class Class_ObjC2

  @objc(isFoo)
  func foo() -> Bool {}
  // CHECK-LABEL: @objc(isFoo) func foo() -> Bool {
}

@objc() // expected-error{{expected name within parentheses of '@objc' attribute}}
class Class_ObjC3 { 
}

// @objc with selector names
extension PlainClass {
  // CHECK-LABEL: @objc(setFoo:) func
  @objc(setFoo:)
  func foo(b: Bool) { }

  // CHECK-LABEL: @objc(setWithRed:green:blue:alpha:) func set
  @objc(setWithRed:green:blue:alpha:)
  func set(Float, green: Float, blue: Float, alpha: Float)  { }

  // CHECK-LABEL: @objc(createWithRed:green:blue:alpha:) class func createWith
  @objc(createWithRed:green blue:alpha)
  class func createWithRed(Float, green: Float, blue: Float, alpha: Float) { }
  // expected-error@-2{{missing ':' after selector piece in '@objc' attribute}}{{28-28=:}}
  // expected-error@-3{{missing ':' after selector piece in '@objc' attribute}}{{39-39=:}}

  // CHECK-LABEL: @objc(::) func badlyNamed
  @objc(::)
  func badlyNamed(Int, y: Int) {}
}

@objc(Class:) // expected-error{{'@objc' class must have a simple name}}{{12-13=}}
class BadClass1 { }

@objc(Protocol:) // expected-error{{'@objc' protocol must have a simple name}}{{15-16=}}
protocol BadProto1 { }

class BadClass2 {
  @objc(badprop:foo:wibble:) // expected-error{{'@objc' property must have a simple name}}{{16-28=}}
  var badprop: Int = 5

  @objc(foo) // expected-error{{'@objc' subscript cannot have a name; did you mean to put the name on the getter or setter?}}
  subscript (i: Int) -> Int {
    get {
      return i
    }
  }

  @objc(foo) // expected-error{{'@objc' method name provides names for 0 arguments, but method has one parameter}}
  func noArgNamesOneParam(x: Int) { }
  
  @objc(foo) // expected-error{{'@objc' method name provides names for 0 arguments, but method has one parameter}}
  func noArgNamesOneParam2(Int) { }

  @objc(foo) // expected-error{{'@objc' method name provides names for 0 arguments, but method has 2 parameters}}
  func noArgNamesTwoParams(Int, y: Int) { }

  @objc(foo:) // expected-error{{'@objc' method name provides one argument name, but method has 2 parameters}}
  func oneArgNameTwoParams(Int, y: Int) { }

  @objc(foo:) // expected-error{{'@objc' method name provides one argument name, but method has 0 parameters}}
  func oneArgNameNoParams() { }

  @objc(foo:) // expected-error{{'@objc' initializer name provides one argument name, but initializer has 0 parameters}}
  init() { }

  var _prop = 5
  @objc var prop: Int {
    @objc(property) get { return _prop }
    @objc(setProperty:) set { _prop = newValue }
  }

  var prop2: Int {
    @objc(property) get { return _prop } // expected-error{{'@objc' getter for non-'@objc' property}}
    @objc(setProperty:) set { _prop = newValue } // expected-error{{'@objc' setter for non-'@objc' property}}
  }

  var prop3: Int {
    @objc(setProperty:) didSet { } // expected-error{{observing accessors are not allowed to be marked @objc}}
  }

  @objc
  subscript (c: Class_ObjC1) -> Class_ObjC1 {
    @objc(getAtClass:) get {
      return c
    }

    @objc(setAtClass:class:) set {
    }
  }
}

// Swift overrides that aren't also @objc overrides.
class Super {
  @objc(renamedFoo)
  var foo: Int { get { return 3 } } // expected-note{{overridden declaration is here}}
}

class Sub : Super {
  @objc(foo) // expected-error{{declaration has a different @objc name from the declaration it overrides ('foo' vs. 'renamedFoo')}}
  override var foo: Int { get { return 5 } }
}

enum NotObjCEnum { case X }
struct NotObjCStruct {}

// Closure arguments can only be @objc if their parameters and returns are.
// CHECK-LABEL: @objc class ClosureArguments
@objc class ClosureArguments {
  // CHECK: @objc func foo
  @objc func foo(f: Int -> ()) {}
  // CHECK: @objc func bar
  @objc func bar(f: NotObjCEnum -> NotObjCStruct) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func bas
  @objc func bas(f: NotObjCEnum -> ()) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func zim
  @objc func zim(f: () -> NotObjCStruct) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func zang
  @objc func zang(f: (NotObjCEnum, NotObjCStruct) -> ()) {} // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}} expected-note{{function types cannot be represented in Objective-C unless their parameters and returns can be}}
  // CHECK: @objc func fooImplicit
  func fooImplicit(f: Int -> ()) {}
  // CHECK: {{^}}  func barImplicit
  func barImplicit(f: NotObjCEnum -> NotObjCStruct) {}
  // CHECK: {{^}}  func basImplicit
  func basImplicit(f: NotObjCEnum -> ()) {}
  // CHECK: {{^}}  func zimImplicit
  func zimImplicit(f: () -> NotObjCStruct) {}
  // CHECK: {{^}}  func zangImplicit
  func zangImplicit(f: (NotObjCEnum, NotObjCStruct) -> ()) {}
}

typealias GoodBlock = @objc_block Int -> ()
typealias BadBlock = @objc_block NotObjCEnum -> () // expected-error{{@objc_block type is not representable in Objective-C}}

