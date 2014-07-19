// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_OBJECT_DOT_1 | FileCheck %s -check-prefix=FOO_OBJECT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_OBJECT_DOT_2 | FileCheck %s -check-prefix=FOO_OBJECT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_OBJECT_DOT_3 | FileCheck %s -check-prefix=FOO_OBJECT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_OBJECT_DOT_4 | FileCheck %s -check-prefix=FOO_OBJECT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_OBJECT_DOT_5 | FileCheck %s -check-prefix=FOO_OBJECT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_OBJECT_NO_DOT_1 | FileCheck %s -check-prefix=FOO_OBJECT_NO_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_OBJECT_NO_DOT_2 | FileCheck %s -check-prefix=FOO_OBJECT_NO_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_STRUCT_DOT_1 | FileCheck %s -check-prefix=FOO_STRUCT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_STRUCT_NO_DOT_1 | FileCheck %s -check-prefix=FOO_STRUCT_NO_DOT

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CF1 | FileCheck %s -check-prefix=CF1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CF2 | FileCheck %s -check-prefix=CF2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CF3 | FileCheck %s -check-prefix=CF3
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CF4 | FileCheck %s -check-prefix=CF4

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_FUNC_0 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_FUNC_0
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_FUNC_1 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_FUNC_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_FUNC_2 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_FUNC_2

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_VARARG_FUNC_0 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_VARARG_FUNC_0
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_VARARG_FUNC_1 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_VARARG_FUNC_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_VARARG_FUNC_2 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_VARARG_FUNC_2

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_OVERLOADED_FUNC_1 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_OVERLOADED_FUNC_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_OVERLOADED_FUNC_2 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_OVERLOADED_FUNC_2

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_CURRIED_FUNC_1 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_CURRIED_FUNC_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_CURRIED_FUNC_2 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_CURRIED_FUNC_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_CURRIED_FUNC_3 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_CURRIED_FUNC_3
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IMPLICITLY_CURRIED_CURRIED_FUNC_4 | FileCheck %s -check-prefix=IMPLICITLY_CURRIED_CURRIED_FUNC_4

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IN_SWITCH_CASE_1 | FileCheck %s -check-prefix=IN_SWITCH_CASE
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IN_SWITCH_CASE_2 | FileCheck %s -check-prefix=IN_SWITCH_CASE
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IN_SWITCH_CASE_3 | FileCheck %s -check-prefix=IN_SWITCH_CASE
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=IN_SWITCH_CASE_4 | FileCheck %s -check-prefix=IN_SWITCH_CASE

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=VF1 | FileCheck %s -check-prefix=VF1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=VF2 | FileCheck %s -check-prefix=VF2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=BASE_MEMBERS | FileCheck %s -check-prefix=BASE_MEMBERS
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=BASE_MEMBERS_STATIC | FileCheck %s -check-prefix=BASE_MEMBERS_STATIC

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=PROTO_MEMBERS_NO_DOT_1 | FileCheck %s -check-prefix=PROTO_MEMBERS_NO_DOT_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=PROTO_MEMBERS_NO_DOT_2 | FileCheck %s -check-prefix=PROTO_MEMBERS_NO_DOT_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=PROTO_MEMBERS_NO_DOT_3 | FileCheck %s -check-prefix=PROTO_MEMBERS_NO_DOT_3

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=PROTO_MEMBERS_1 | FileCheck %s -check-prefix=PROTO_MEMBERS_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=PROTO_MEMBERS_2 | FileCheck %s -check-prefix=PROTO_MEMBERS_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=PROTO_MEMBERS_3 | FileCheck %s -check-prefix=PROTO_MEMBERS_3
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=PROTO_MEMBERS_4 | FileCheck %s -check-prefix=PROTO_MEMBERS_4

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_0 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_0
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_1 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_2 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_3 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_3
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_4 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_4
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_5 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_5
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_6 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_6
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_7 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_7

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_VARARG_FUNCTION_CALL_1 | FileCheck %s -check-prefix=INSIDE_VARARG_FUNCTION_CALL_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_VARARG_FUNCTION_CALL_2 | FileCheck %s -check-prefix=INSIDE_VARARG_FUNCTION_CALL_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_VARARG_FUNCTION_CALL_3 | FileCheck %s -check-prefix=INSIDE_VARARG_FUNCTION_CALL_3

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_OVERLOADED_FUNCTION_CALL_1 | FileCheck %s -check-prefix=INSIDE_OVERLOADED_FUNCTION_CALL_1

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_CURRIED_FUNCTION_CALL_1 | FileCheck %s -check-prefix=INSIDE_CURRIED_FUNCTION_CALL_1

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1 | FileCheck %s -check-prefix=INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_FUNC_PARAM_1 | FileCheck %s -check-prefix=FOO_OBJECT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_FUNC_PARAM_2 | FileCheck %s -check-prefix=FOO_OBJECT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_FUNC_PARAM_3 | FileCheck %s -check-prefix=RESOLVE_FUNC_PARAM_3
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_FUNC_PARAM_4 | FileCheck %s -check-prefix=RESOLVE_FUNC_PARAM_4
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_FUNC_PARAM_5 | FileCheck %s -check-prefix=RESOLVE_FUNC_PARAM_5
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_FUNC_PARAM_6 | FileCheck %s -check-prefix=RESOLVE_FUNC_PARAM_6

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_CONSTRUCTOR_PARAM_1 | FileCheck %s -check-prefix=FOO_OBJECT_DOT
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_CONSTRUCTOR_PARAM_2 | FileCheck %s -check-prefix=RESOLVE_CONSTRUCTOR_PARAM_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_CONSTRUCTOR_PARAM_3 | FileCheck %s -check-prefix=RESOLVE_CONSTRUCTOR_PARAM_3

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FUNC_PAREN_PATTERN_1 | FileCheck %s -check-prefix=FUNC_PAREN_PATTERN_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FUNC_PAREN_PATTERN_2 | FileCheck %s -check-prefix=FUNC_PAREN_PATTERN_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=FUNC_PAREN_PATTERN_3 | FileCheck %s -check-prefix=FUNC_PAREN_PATTERN_3

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CHAINED_CALLS_1 | FileCheck %s -check-prefix=CHAINED_CALLS_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CHAINED_CALLS_2 | FileCheck %s -check-prefix=CHAINED_CALLS_2

// Disabled because we aren't handling failures well.
// FIXME: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CHAINED_CALLS_3 | FileCheck %s -check-prefix=CHAINED_CALLS_3

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_1 | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_2 | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_3 | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_3
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_4 | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_4
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_5 | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_5
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_ERROR_1 | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_ERROR_1

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_1_STATIC | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_1_STATIC
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_2_STATIC | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_2_STATIC
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_3_STATIC | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_3_STATIC
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_4_STATIC | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_4_STATIC
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_GENERIC_PARAMS_5_STATIC | FileCheck %s -check-prefix=RESOLVE_GENERIC_PARAMS_5_STATIC

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=TC_UNSOLVED_VARIABLES_1 | FileCheck %s -check-prefix=TC_UNSOLVED_VARIABLES_1
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=TC_UNSOLVED_VARIABLES_2 | FileCheck %s -check-prefix=TC_UNSOLVED_VARIABLES_2
// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=TC_UNSOLVED_VARIABLES_3 | FileCheck %s -check-prefix=TC_UNSOLVED_VARIABLES_3

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=RESOLVE_MODULES_1 | FileCheck %s -check-prefix=RESOLVE_MODULES_1

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=INTERPOLATED_STRING_1 | FileCheck %s -check-prefix=FOO_OBJECT_DOT

// Test code completion of expressions that produce a value.

struct FooStruct {
  var instanceVar = 0

  mutating
  func instanceFunc0() {}
  mutating
  func instanceFunc1(a: Int) {}
  mutating
  func instanceFunc2(a: Int, inout b: Double) {}
  mutating
  func instanceFunc3(a: Int, _: (Float, Double)) {}
  mutating
  func instanceFunc4(a: Int?, b: Int!, inout c: Int?, inout d: Int!) {}
  mutating
  func instanceFunc5() -> Int? {}
  mutating
  func instanceFunc6() -> Int! {}
  mutating
  func instanceFunc7(#a: Int) {}
  mutating
  func instanceFunc8(a: (Int, Int)) {}

  mutating
  func varargInstanceFunc0(v: Int...) {}
  mutating
  func varargInstanceFunc1(a: Float, v: Int...) {}
  mutating
  func varargInstanceFunc2(a: Float, b: Double, v: Int...) {}

  mutating
  func overloadedInstanceFunc1() -> Int {}
  mutating
  func overloadedInstanceFunc1() -> Double {}

  mutating
  func overloadedInstanceFunc2(x: Int) -> Int {}
  mutating
  func overloadedInstanceFunc2(x: Double) -> Int {}

  mutating
  func builderFunc1(a: Int) -> FooStruct { return self }

  subscript(i: Int) -> Double {
    get {
      return Double(i)
    }
    set(v) {
      instanceVar = i
    }
  }

  subscript(i: Int, j: Int) -> Double {
    get {
      return Double(i + j)
    }
    set(v) {
      instanceVar = i + j
    }
  }

  mutating
  func curriedVoidFunc1()() {}
  mutating
  func curriedVoidFunc2()(a: Int) {}
  mutating
  func curriedVoidFunc3(a: Int)() {}
  mutating
  func curriedVoidFunc4(a: Int)(b: Int) {}
  mutating
  func curriedVoidFunc5(a: Int)(b: Int, _: (Float, Double)) {}

  mutating
  func curriedStringFunc1()() -> String {}
  mutating
  func curriedStringFunc2()(a: Int) -> String {}
  mutating
  func curriedStringFunc3(a: Int)() -> String {}
  mutating
  func curriedStringFunc4(a: Int)(b: Int) -> String {}
  mutating
  func curriedStringFunc5(a: Int)(b: Int, _: (Float, Double)) -> String {}

  mutating
  func selectorVoidFunc1(_ a: Int, b x: Float) {}
  mutating
  func selectorVoidFunc2(_ a: Int, b x: Float, c y: Double) {}
  mutating
  func selectorVoidFunc3(_ a: Int, b _: (Float, Double)) {}

  mutating
  func selectorStringFunc1(_ a: Int, b x: Float) -> String {}
  mutating
  func selectorStringFunc2(_ a: Int,  b x: Float, c y: Double) -> String {}
  mutating
  func selectorStringFunc3(_ a: Int, b _: (Float, Double)) -> String{}

  struct NestedStruct {}
  class NestedClass {}
  enum NestedEnum {}
  // Can not declare a nested protocol.
  // protocol NestedProtocol {}

  typealias NestedTypealias = Int

  static var staticVar: Int = 4

  static func staticFunc0() {}
  static func staticFunc1(a: Int) {}

  static func overloadedStaticFunc1() -> Int {}
  static func overloadedStaticFunc1() -> Double {}

  static func overloadedStaticFunc2(x: Int) -> Int {}
  static func overloadedStaticFunc2(x: Double) -> Int {}
}

extension FooStruct {
  var extProp: Int {
    get {
      return 42
    }
    set(v) {}
  }

  mutating
  func extFunc0() {}

  static var extStaticProp: Int {
    get {
      return 42
    }
    set(v) {}
  }

  static func extStaticFunc0() {}

  struct ExtNestedStruct {}
  class ExtNestedClass {}
  enum ExtNestedEnum {
    case ExtEnumX(Int)
  }

  typealias ExtNestedTypealias = Int
}

var fooObject: FooStruct

// FOO_OBJECT_DOT: Begin completions
// FOO_OBJECT_DOT-NEXT: Decl[InstanceVar]/CurrNominal:    instanceVar[#Int#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc0()[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc1({#(a): Int#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc2({#(a): Int#}, {#b: &Double#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc3({#(a): Int#}, {#(Float, Double)#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc4({#(a): Int?#}, {#b: Int!#}, {#c: &Int?#}, {#d: &Int!#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc5()[#Int?#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc6()[#Int!#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc7({#a: Int#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc8({#(a): (Int, Int)#})[#Void#]{{$}}
//
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc0({#(v): Int...#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc1({#(a): Float#}, {#v: Int...#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc2({#(a): Float#}, {#b: Double#}, {#v: Int...#})[#Void#]{{$}}
//
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc1()[#Int#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc1()[#Double#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc2({#(x): Int#})[#Int#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc2({#(x): Double#})[#Int#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: builderFunc1({#(a): Int#})[#FooStruct#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc1()[#() -> Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc2()[#(a: Int) -> Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc3({#(a): Int#})[#() -> Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc4({#(a): Int#})[#(b: Int) -> Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc5({#(a): Int#})[#(b: Int, (Float, Double)) -> Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc1()[#() -> String#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc2()[#(a: Int) -> String#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc3({#(a): Int#})[#() -> String#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc4({#(a): Int#})[#(b: Int) -> String#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc5({#(a): Int#})[#(b: Int, (Float, Double)) -> String#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc1({#(a): Int#}, {#b: Float#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc2({#(a): Int#}, {#b: Float#}, {#c: Double#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc3({#(a): Int#}, {#b: (Float, Double)#})[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorStringFunc1({#(a): Int#}, {#b: Float#})[#String#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorStringFunc2({#(a): Int#}, {#b: Float#}, {#c: Double#})[#String#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorStringFunc3({#(a): Int#}, {#b: (Float, Double)#})[#String#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceVar]/CurrNominal:    extProp[#Int#]{{$}}
// FOO_OBJECT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: extFunc0()[#Void#]{{$}}
// FOO_OBJECT_DOT-NEXT: End completions

// FOO_OBJECT_NO_DOT: Begin completions
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceVar]/CurrNominal:    .instanceVar[#Int#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc0()[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc1({#(a): Int#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc2({#(a): Int#}, {#b: &Double#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc3({#(a): Int#}, {#(Float, Double)#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc4({#(a): Int?#}, {#b: Int!#}, {#c: &Int?#}, {#d: &Int!#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc5()[#Int?#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc6()[#Int!#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc7({#a: Int#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc8({#(a): (Int, Int)#})[#Void#]{{$}}
//
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc0({#(v): Int...#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc1({#(a): Float#}, {#v: Int...#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc2({#(a): Float#}, {#b: Double#}, {#v: Int...#})[#Void#]{{$}}
//
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc1()[#Int#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc1()[#Double#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc2({#(x): Int#})[#Int#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc2({#(x): Double#})[#Int#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .builderFunc1({#(a): Int#})[#FooStruct#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[Subscript]/CurrNominal:      [{#i: Int#}][#Double#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[Subscript]/CurrNominal:      [{#i: Int#}, {#j: Int#}][#Double#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc1()[#() -> Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc2()[#(a: Int) -> Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc3({#(a): Int#})[#() -> Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc4({#(a): Int#})[#(b: Int) -> Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc5({#(a): Int#})[#(b: Int, (Float, Double)) -> Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc1()[#() -> String#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc2()[#(a: Int) -> String#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc3({#(a): Int#})[#() -> String#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc4({#(a): Int#})[#(b: Int) -> String#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc5({#(a): Int#})[#(b: Int, (Float, Double)) -> String#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc1({#(a): Int#}, {#b: Float#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc2({#(a): Int#}, {#b: Float#}, {#c: Double#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc3({#(a): Int#}, {#b: (Float, Double)#})[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc1({#(a): Int#}, {#b: Float#})[#String#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc2({#(a): Int#}, {#b: Float#}, {#c: Double#})[#String#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc3({#(a): Int#}, {#b: (Float, Double)#})[#String#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceVar]/CurrNominal:    .extProp[#Int#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .extFunc0()[#Void#]{{$}}
// FOO_OBJECT_NO_DOT-NEXT: End completions

// FOO_STRUCT_DOT: Begin completions
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc0({#self: &FooStruct#})[#() -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc1({#self: &FooStruct#})[#(Int) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc2({#self: &FooStruct#})[#(Int, inout b: Double) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc3({#self: &FooStruct#})[#(Int, (Float, Double)) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc4({#self: &FooStruct#})[#(Int?, b: Int!, inout c: Int?, inout d: Int!) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc5({#self: &FooStruct#})[#() -> Int?#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc6({#self: &FooStruct#})[#() -> Int!#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc7({#self: &FooStruct#})[#(a: Int) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: instanceFunc8({#self: &FooStruct#})[#((Int, Int)) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc0({#self: &FooStruct#})[#(Int...) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc1({#self: &FooStruct#})[#(Float, v: Int...) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: varargInstanceFunc2({#self: &FooStruct#})[#(Float, b: Double, v: Int...) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc1({#self: &FooStruct#})[#() -> Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc1({#self: &FooStruct#})[#() -> Double#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc2({#self: &FooStruct#})[#(Int) -> Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: overloadedInstanceFunc2({#self: &FooStruct#})[#(Double) -> Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: builderFunc1({#self: &FooStruct#})[#(Int) -> FooStruct#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc1({#self: &FooStruct#})[#() -> () -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc2({#self: &FooStruct#})[#() -> (a: Int) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc3({#self: &FooStruct#})[#(Int) -> () -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc4({#self: &FooStruct#})[#(Int) -> (b: Int) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedVoidFunc5({#self: &FooStruct#})[#(Int) -> (b: Int, (Float, Double)) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc1({#self: &FooStruct#})[#() -> () -> String#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc2({#self: &FooStruct#})[#() -> (a: Int) -> String#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc3({#self: &FooStruct#})[#(Int) -> () -> String#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc4({#self: &FooStruct#})[#(Int) -> (b: Int) -> String#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: curriedStringFunc5({#self: &FooStruct#})[#(Int) -> (b: Int, (Float, Double)) -> String#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc1({#self: &FooStruct#})[#(Int, b: Float) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc2({#self: &FooStruct#})[#(Int, b: Float, c: Double) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorVoidFunc3({#self: &FooStruct#})[#(Int, b: (Float, Double)) -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorStringFunc1({#self: &FooStruct#})[#(Int, b: Float) -> String#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorStringFunc2({#self: &FooStruct#})[#(Int, b: Float, c: Double) -> String#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: selectorStringFunc3({#self: &FooStruct#})[#(Int, b: (Float, Double)) -> String#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[Struct]/CurrNominal:         NestedStruct[#FooStruct.NestedStruct#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[Class]/CurrNominal:          NestedClass[#FooStruct.NestedClass#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[Enum]/CurrNominal:           NestedEnum[#FooStruct.NestedEnum#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[TypeAlias]/CurrNominal:      NestedTypealias[#Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVar[#Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   staticFunc0()[#Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   staticFunc1({#(a): Int#})[#Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   overloadedStaticFunc1()[#Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   overloadedStaticFunc1()[#Double#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   overloadedStaticFunc2({#(x): Int#})[#Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   overloadedStaticFunc2({#(x): Double#})[#Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: extFunc0({#self: &FooStruct#})[#() -> Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticVar]/CurrNominal:      extStaticProp[#Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   extStaticFunc0()[#Void#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[Struct]/CurrNominal:         ExtNestedStruct[#FooStruct.ExtNestedStruct#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[Class]/CurrNominal:          ExtNestedClass[#FooStruct.ExtNestedClass#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[Enum]/CurrNominal:           ExtNestedEnum[#FooStruct.ExtNestedEnum#]{{$}}
// FOO_STRUCT_DOT-NEXT: Decl[TypeAlias]/CurrNominal:      ExtNestedTypealias[#Int#]{{$}}
// FOO_STRUCT_DOT-NEXT: End completions

// FOO_STRUCT_NO_DOT: Begin completions
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc0({#self: &FooStruct#})[#() -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc1({#self: &FooStruct#})[#(Int) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc2({#self: &FooStruct#})[#(Int, inout b: Double) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc3({#self: &FooStruct#})[#(Int, (Float, Double)) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc4({#self: &FooStruct#})[#(Int?, b: Int!, inout c: Int?, inout d: Int!) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc5({#self: &FooStruct#})[#() -> Int?#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc6({#self: &FooStruct#})[#() -> Int!#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc7({#self: &FooStruct#})[#(a: Int) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc8({#self: &FooStruct#})[#((Int, Int)) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc0({#self: &FooStruct#})[#(Int...) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc1({#self: &FooStruct#})[#(Float, v: Int...) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .varargInstanceFunc2({#self: &FooStruct#})[#(Float, b: Double, v: Int...) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc1({#self: &FooStruct#})[#() -> Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc1({#self: &FooStruct#})[#() -> Double#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc2({#self: &FooStruct#})[#(Int) -> Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .overloadedInstanceFunc2({#self: &FooStruct#})[#(Double) -> Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .builderFunc1({#self: &FooStruct#})[#(Int) -> FooStruct#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc1({#self: &FooStruct#})[#() -> () -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc2({#self: &FooStruct#})[#() -> (a: Int) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc3({#self: &FooStruct#})[#(Int) -> () -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc4({#self: &FooStruct#})[#(Int) -> (b: Int) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedVoidFunc5({#self: &FooStruct#})[#(Int) -> (b: Int, (Float, Double)) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc1({#self: &FooStruct#})[#() -> () -> String#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc2({#self: &FooStruct#})[#() -> (a: Int) -> String#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc3({#self: &FooStruct#})[#(Int) -> () -> String#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc4({#self: &FooStruct#})[#(Int) -> (b: Int) -> String#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .curriedStringFunc5({#self: &FooStruct#})[#(Int) -> (b: Int, (Float, Double)) -> String#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc1({#self: &FooStruct#})[#(Int, b: Float) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc2({#self: &FooStruct#})[#(Int, b: Float, c: Double) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorVoidFunc3({#self: &FooStruct#})[#(Int, b: (Float, Double)) -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc1({#self: &FooStruct#})[#(Int, b: Float) -> String#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc2({#self: &FooStruct#})[#(Int, b: Float, c: Double) -> String#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .selectorStringFunc3({#self: &FooStruct#})[#(Int, b: (Float, Double)) -> String#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[Struct]/CurrNominal:         .NestedStruct[#FooStruct.NestedStruct#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[Class]/CurrNominal:          .NestedClass[#FooStruct.NestedClass#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[Enum]/CurrNominal:           .NestedEnum[#FooStruct.NestedEnum#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[TypeAlias]/CurrNominal:      .NestedTypealias[#Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVar[#Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .staticFunc0()[#Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .staticFunc1({#(a): Int#})[#Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .overloadedStaticFunc1()[#Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .overloadedStaticFunc1()[#Double#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .overloadedStaticFunc2({#(x): Int#})[#Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .overloadedStaticFunc2({#(x): Double#})[#Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[Constructor]/CurrNominal:    ({#instanceVar: Int#})[#FooStruct#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[Constructor]/CurrNominal:    ()[#FooStruct#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .extFunc0({#self: &FooStruct#})[#() -> Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .extStaticProp[#Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .extStaticFunc0()[#Void#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[Struct]/CurrNominal:         .ExtNestedStruct[#FooStruct.ExtNestedStruct#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[Class]/CurrNominal:          .ExtNestedClass[#FooStruct.ExtNestedClass#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[Enum]/CurrNominal:           .ExtNestedEnum[#FooStruct.ExtNestedEnum#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: Decl[TypeAlias]/CurrNominal:      .ExtNestedTypealias[#Int#]{{$}}
// FOO_STRUCT_NO_DOT-NEXT: End completions

func testObjectExpr() {
  fooObject.#^FOO_OBJECT_DOT_1^#
}

func testDotDotTokenSplitWithCodeCompletion() {
  fooObject.#^FOO_OBJECT_DOT_2^#.bar
}

func testObjectExprBuilderStyle1() {
  fooObject
    .#^FOO_OBJECT_DOT_3^#
}

func testObjectExprBuilderStyle2() {
  fooObject
    .builderFunc1(42).#^FOO_OBJECT_DOT_4^#
}

func testObjectExprBuilderStyle3() {
  fooObject
    .builderFunc1(42)
    .#^FOO_OBJECT_DOT_5^#
}

func testObjectExprWithoutDot() {
  fooObject#^FOO_OBJECT_NO_DOT_1^#
}

func testObjectExprWithoutSpaceAfterCodeCompletion() {
  fooObject#^FOO_OBJECT_NO_DOT_2^#.bar
}

func testMetatypeExpr() {
  FooStruct.#^FOO_STRUCT_DOT_1^#
}

func testMetatypeExprWithoutDot() {
  FooStruct#^FOO_STRUCT_NO_DOT_1^#
}

func testCurriedFunc() {
  fooObject.curriedVoidFunc1()#^CF1^#
// CF1: Begin completions
// CF1-NEXT: Pattern/ExprSpecific: ()[#Void#]{{$}}
// CF1-NEXT: End completions

  fooObject.curriedVoidFunc2()#^CF2^#
// CF2: Begin completions
// CF2-NEXT: Pattern/ExprSpecific: ({#a: Int#})[#Void#]{{$}}
// CF2-NEXT: End completions

  fooObject.curriedVoidFunc3(42)#^CF3^#
// CF3: Begin completions
// CF3-NEXT: Pattern/ExprSpecific: ()[#Void#]{{$}}
// CF3-NEXT: End completions

  fooObject.curriedVoidFunc4(42)#^CF4^#
// CF4: Begin completions
// CF4-NEXT: Pattern/ExprSpecific: ({#b: Int#})[#Void#]{{$}}
// CF4-NEXT: End completions
}

func testImplicitlyCurriedFunc(fs: FooStruct) {
  FooStruct.instanceFunc0(&fs)#^IMPLICITLY_CURRIED_FUNC_0^#
// IMPLICITLY_CURRIED_FUNC_0: Begin completions
// IMPLICITLY_CURRIED_FUNC_0-NEXT: Pattern/ExprSpecific: ()[#Void#]{{$}}
// IMPLICITLY_CURRIED_FUNC_0-NEXT: End completions

  FooStruct.instanceFunc1(&fs)#^IMPLICITLY_CURRIED_FUNC_1^#
// IMPLICITLY_CURRIED_FUNC_1: Begin completions
// IMPLICITLY_CURRIED_FUNC_1-NEXT: Pattern/ExprSpecific: ({#Int#})[#Void#]{{$}}
// IMPLICITLY_CURRIED_FUNC_1-NEXT: End completions

  FooStruct.instanceFunc2(&fs)#^IMPLICITLY_CURRIED_FUNC_2^#
// IMPLICITLY_CURRIED_FUNC_2: Begin completions
// IMPLICITLY_CURRIED_FUNC_2-NEXT: Pattern/ExprSpecific: ({#Int#}, {#b: &Double#})[#Void#]{{$}}
// IMPLICITLY_CURRIED_FUNC_2-NEXT: End completions

  FooStruct.varargInstanceFunc0(&fs)#^IMPLICITLY_CURRIED_VARARG_FUNC_0^#
// IMPLICITLY_CURRIED_VARARG_FUNC_0: Begin completions
// IMPLICITLY_CURRIED_VARARG_FUNC_0-NEXT: Pattern/ExprSpecific: ({#Int...#})[#Void#]{{$}}
// IMPLICITLY_CURRIED_VARARG_FUNC_0-NEXT: End completions

  FooStruct.varargInstanceFunc1(&fs)#^IMPLICITLY_CURRIED_VARARG_FUNC_1^#
// IMPLICITLY_CURRIED_VARARG_FUNC_1: Begin completions
// IMPLICITLY_CURRIED_VARARG_FUNC_1-NEXT: Pattern/ExprSpecific: ({#Float#}, {#v: Int...#})[#Void#]{{$}}
// IMPLICITLY_CURRIED_VARARG_FUNC_1-NEXT: End completions

  FooStruct.varargInstanceFunc2(&fs)#^IMPLICITLY_CURRIED_VARARG_FUNC_2^#
// IMPLICITLY_CURRIED_VARARG_FUNC_2: Begin completions
// IMPLICITLY_CURRIED_VARARG_FUNC_2-NEXT: Pattern/ExprSpecific: ({#Float#}, {#b: Double#}, {#v: Int...#})[#Void#]{{$}}
// IMPLICITLY_CURRIED_VARARG_FUNC_2-NEXT: End completions

  // This call is ambiguous, and the expression is invalid.
  // Ensure that we don't suggest to call the result.
  FooStruct.overloadedInstanceFunc1(&fs)#^IMPLICITLY_CURRIED_OVERLOADED_FUNC_1^#
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_1: found code completion token
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_1-NOT: Begin completions

  // This call is ambiguous, and the expression is invalid.
  // Ensure that we don't suggest to call the result.
  FooStruct.overloadedInstanceFunc2(&fs)#^IMPLICITLY_CURRIED_OVERLOADED_FUNC_2^#
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_2: found code completion token
// IMPLICITLY_CURRIED_OVERLOADED_FUNC_2-NOT: Begin completions

  FooStruct.curriedVoidFunc1(&fs)#^IMPLICITLY_CURRIED_CURRIED_FUNC_1^#
// IMPLICITLY_CURRIED_CURRIED_FUNC_1: Begin completions
// IMPLICITLY_CURRIED_CURRIED_FUNC_1-NEXT: Pattern/ExprSpecific: ()[#() -> ()#]{{$}}
// IMPLICITLY_CURRIED_CURRIED_FUNC_1-NEXT: End completions

  FooStruct.curriedVoidFunc2(&fs)#^IMPLICITLY_CURRIED_CURRIED_FUNC_2^#
// IMPLICITLY_CURRIED_CURRIED_FUNC_2: Begin completions
// IMPLICITLY_CURRIED_CURRIED_FUNC_2-NEXT: Pattern/ExprSpecific: ()[#(a: Int) -> ()#]{{$}}
// IMPLICITLY_CURRIED_CURRIED_FUNC_2-NEXT: End completions

  FooStruct.curriedVoidFunc3(&fs)#^IMPLICITLY_CURRIED_CURRIED_FUNC_3^#
// IMPLICITLY_CURRIED_CURRIED_FUNC_3: Begin completions
// IMPLICITLY_CURRIED_CURRIED_FUNC_3-NEXT: Pattern/ExprSpecific: ({#Int#})[#() -> ()#]{{$}}
// IMPLICITLY_CURRIED_CURRIED_FUNC_3-NEXT: End completions

  FooStruct.curriedVoidFunc4(&fs)#^IMPLICITLY_CURRIED_CURRIED_FUNC_4^#
// IMPLICITLY_CURRIED_CURRIED_FUNC_4: Begin completions
// IMPLICITLY_CURRIED_CURRIED_FUNC_4-NEXT: Pattern/ExprSpecific: ({#Int#})[#(b: Int) -> ()#]{{$}}
// IMPLICITLY_CURRIED_CURRIED_FUNC_4-NEXT: End completions
}

//===---
//===--- Test that we can complete inside 'case'.
//===---

func testSwitch1() {
  switch fooObject {
    case #^IN_SWITCH_CASE_1^#
  }

  switch fooObject {
    case 1, #^IN_SWITCH_CASE_2^#
  }

  switch unknown_var {
    case #^IN_SWITCH_CASE_3^#
  }

  switch {
    case #^IN_SWITCH_CASE_4^#
  }
}

// IN_SWITCH_CASE: Begin completions
// IN_SWITCH_CASE-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// IN_SWITCH_CASE-DAG: Decl[Struct]/CurrModule:    FooStruct[#FooStruct#]{{$}}
// IN_SWITCH_CASE: End completions

//===--- Helper types that are used in this test

struct FooGenericStruct<T> {
  init(t: T) { fooInstanceVarT = t }

  var fooInstanceVarT: T
  var fooInstanceVarTBrackets: [T]
  mutating
  func fooVoidInstanceFunc1(a: T) {}
  mutating
  func fooTInstanceFunc1(a: T) -> T { return a }
  mutating
  func fooUInstanceFunc1<U>(a: U) -> U { return a }

  static var fooStaticVarT: Int = 0
  static var fooStaticVarTBrackets: [Int] = [0]
  static func fooVoidStaticFunc1(a: T) {}
  static func fooTStaticFunc1(a: T) -> T { return a }
  static func fooUInstanceFunc1<U>(a: U) -> U { return a }
}

class FooClass {
  var fooClassInstanceVar = 0
  func fooClassInstanceFunc0() {}
  func fooClassInstanceFunc1(a: Int) {}
}

enum FooEnum {
}

protocol FooProtocol {
  var fooInstanceVar1: Int { get set }
  var fooInstanceVar2: Int { get }
  typealias FooTypeAlias1
  func fooInstanceFunc0() -> Double
  func fooInstanceFunc1(a: Int) -> Double
  subscript(i: Int) -> Double { get set }
}

class FooProtocolImpl : FooProtocol {
  var fooInstanceVar1 = 0
  val fooInstanceVar2 = 0
  typealias FooTypeAlias1 = Float
  init() {}
  func fooInstanceFunc0() -> Double {
    return 0.0
  }
  func fooInstanceFunc1(a: Int) -> Double {
    return Double(a)
  }
  subscript(i: Int) -> Double {
    return 0.0
  }
}

protocol FooExProtocol : FooProtocol {
  func fooExInstanceFunc0() -> Double
}

protocol BarProtocol {
  var barInstanceVar: Int { get set }
  typealias BarTypeAlias1
  func barInstanceFunc0() -> Double
  func barInstanceFunc1(a: Int) -> Double
}

protocol BarExProtocol : BarProtocol {
  func barExInstanceFunc0() -> Double
}

protocol BazProtocol {
  func bazInstanceFunc0() -> Double
}

typealias BarBazProtocolComposition = protocol<BarProtocol, BazProtocol>

var fooProtocolInstance: FooProtocol = FooProtocolImpl()
var fooBarProtocolInstance: protocol<FooProtocol, BarProtocol>
var fooExBarExProtocolInstance: protocol<FooExProtocol, BarExProtocol>

typealias FooTypealias = Int

//===--- Test that we can code complete inside function calls.

func testInsideFunctionCall0() {
  ERROR(#^INSIDE_FUNCTION_CALL_0^#
// INSIDE_FUNCTION_CALL_0: Begin completions
// INSIDE_FUNCTION_CALL_0-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_FUNCTION_CALL_0: End completions
}

func testInsideFunctionCall1() {
  var a = FooStruct()
  a.instanceFunc0(#^INSIDE_FUNCTION_CALL_1^#
// There should be no other results here because the function call
// unambigously resolves to overload that takes 0 arguments.
// INSIDE_FUNCTION_CALL_1: Begin completions
// INSIDE_FUNCTION_CALL_1-NEXT: Pattern/ExprSpecific: ['('])[#Void#]{{$}}
// INSIDE_FUNCTION_CALL_1-NEXT: End completions
}

func testInsideFunctionCall2() {
  var a = FooStruct()
  a.instanceFunc1(#^INSIDE_FUNCTION_CALL_2^#
// INSIDE_FUNCTION_CALL_2: Begin completions
// INSIDE_FUNCTION_CALL_2-DAG: Pattern/ExprSpecific:       ['(']{#(a): Int#})[#Void#]{{$}}
// INSIDE_FUNCTION_CALL_2-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_FUNCTION_CALL_2: End completions
}

func testInsideFunctionCall3() {
  FooStruct().instanceFunc1(42, #^INSIDE_FUNCTION_CALL_3^#
// INSIDE_FUNCTION_CALL_3: Begin completions
// FIXME: There should be no results here because the function call
// unambigously resolves to overload that takes 1 argument.
// INSIDE_FUNCTION_CALL_3-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_FUNCTION_CALL_3: End completions
}

func testInsideFunctionCall4() {
  var a = FooStruct()
  a.instanceFunc2(#^INSIDE_FUNCTION_CALL_4^#
// INSIDE_FUNCTION_CALL_4: Begin completions
// INSIDE_FUNCTION_CALL_4-DAG: Pattern/ExprSpecific:       ['(']{#(a): Int#}, {#b: &Double#})[#Void#]{{$}}
// INSIDE_FUNCTION_CALL_4-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_FUNCTION_CALL_4: End completions
}

func testInsideFunctionCall5() {
  FooStruct().instanceFunc2(42, #^INSIDE_FUNCTION_CALL_5^#
// INSIDE_FUNCTION_CALL_5: Begin completions
// INSIDE_FUNCTION_CALL_5-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_FUNCTION_CALL_5: End completions
}

func testInsideFunctionCall6() {
  var a = FooStruct()
  a.instanceFunc7(#^INSIDE_FUNCTION_CALL_6^#
// INSIDE_FUNCTION_CALL_6: Begin completions
// INSIDE_FUNCTION_CALL_6-NEXT: Pattern/ExprSpecific: ['(']{#a: Int#})[#Void#]{{$}}
// INSIDE_FUNCTION_CALL_6-NEXT: End completions
}

func testInsideFunctionCall7() {
  var a = FooStruct()
  a.instanceFunc8(#^INSIDE_FUNCTION_CALL_7^#
// INSIDE_FUNCTION_CALL_7: Begin completions
// INSIDE_FUNCTION_CALL_7: Pattern/ExprSpecific: ['(']{#(a): (Int, Int)#})[#Void#]{{$}}
// INSIDE_FUNCTION_CALL_7: End completions
}

func testInsideVarargFunctionCall1() {
  var a = FooStruct()
  a.varargInstanceFunc0(#^INSIDE_VARARG_FUNCTION_CALL_1^#
// INSIDE_VARARG_FUNCTION_CALL_1: Begin completions
// INSIDE_VARARG_FUNCTION_CALL_1-DAG: Pattern/ExprSpecific:       ['(']{#(v): Int...#})[#Void#]{{$}}
// INSIDE_VARARG_FUNCTION_CALL_1-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_VARARG_FUNCTION_CALL_1: End completions
}

func testInsideVarargFunctionCall2() {
  FooStruct().varargInstanceFunc0(42, #^INSIDE_VARARG_FUNCTION_CALL_2^#
// INSIDE_VARARG_FUNCTION_CALL_2: Begin completions
// INSIDE_VARARG_FUNCTION_CALL_2-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_VARARG_FUNCTION_CALL_2: End completions
}

func testInsideVarargFunctionCall3() {
  FooStruct().varargInstanceFunc0(42, 4242, #^INSIDE_VARARG_FUNCTION_CALL_3^#
// INSIDE_VARARG_FUNCTION_CALL_3: Begin completions
// INSIDE_VARARG_FUNCTION_CALL_3-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_VARARG_FUNCTION_CALL_3: End completions
}

func testInsideOverloadedFunctionCall1() {
  var a = FooStruct()
  a.overloadedInstanceFunc2(#^INSIDE_OVERLOADED_FUNCTION_CALL_1^#
// INSIDE_OVERLOADED_FUNCTION_CALL_1: Begin completions
// FIXME: produce call patterns here.
// INSIDE_OVERLOADED_FUNCTION_CALL_1-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_OVERLOADED_FUNCTION_CALL_1: End completions
}

func testInsideCurriedFunctionCall1() {
  var a = FooStruct()
  a.curriedVoidFunc4(42)(#^INSIDE_CURRIED_FUNCTION_CALL_1^#
// INSIDE_CURRIED_FUNCTION_CALL_1: Begin completions
// INSIDE_CURRIED_FUNCTION_CALL_1-DAG: Pattern/ExprSpecific: ['(']{#b: Int#})[#Void#]{{$}}
// INSIDE_CURRIED_FUNCTION_CALL_1: End completions
}

func testInsideFunctionCallOnClassInstance1(a: FooClass) {
  a.fooClassInstanceFunc1(#^INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1^#
// INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1: Begin completions
// INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1-DAG: Pattern/ExprSpecific:       ['(']{#(a): Int#})[#Void#]{{$}}
// INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1-DAG: Decl[GlobalVar]/CurrModule: fooObject[#FooStruct#]{{$}}
// INSIDE_FUNCTION_CALL_ON_CLASS_INSTANCE_1: End completions
}

//===--- Variables that have function types.

class FuncTypeVars {
  var funcVar1: () -> Double
  var funcVar2: (a: Int) -> Double
}

var funcTypeVarsObject: FuncTypeVars
func testFuncTypeVars() {
  funcTypeVarsObject.funcVar1#^VF1^#
// VF1: Begin completions
// VF1-NEXT: Pattern/ExprSpecific: ()[#Double#]{{$}}
// VF1-NEXT: End completions

  funcTypeVarsObject.funcVar2#^VF2^#
// VF2: Begin completions
// VF2-NEXT: Pattern/ExprSpecific: ({#a: Int#})[#Double#]{{$}}
// VF2-NEXT: End completions
}

//===--- Check that we look into base classes.

class MembersBase {
  var baseVar = 0
  func baseInstanceFunc() {}
  class func baseStaticFunc() {}
}

class MembersDerived : MembersBase {
  var derivedVar = 0
  func derivedInstanceFunc() {}
  class func derivedStaticFunc() {}
}

var membersDerived: MembersDerived
func testLookInBase() {
  membersDerived.#^BASE_MEMBERS^#
// BASE_MEMBERS: Begin completions
// BASE_MEMBERS-NEXT: Decl[InstanceVar]/CurrNominal:    derivedVar[#Int#]{{$}}
// BASE_MEMBERS-NEXT: Decl[InstanceMethod]/CurrNominal: derivedInstanceFunc()[#Void#]{{$}}
// BASE_MEMBERS-NEXT: Decl[InstanceVar]/Super:          baseVar[#Int#]{{$}}
// BASE_MEMBERS-NEXT: Decl[InstanceMethod]/Super:       baseInstanceFunc()[#Void#]{{$}}
// BASE_MEMBERS-NEXT: End completions
}

func testLookInBaseStatic() {
  MembersDerived.#^BASE_MEMBERS_STATIC^#
// BASE_MEMBERS_STATIC: Begin completions
// BASE_MEMBERS_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: derivedInstanceFunc({#self: MembersDerived#})[#() -> Void#]{{$}}
// BASE_MEMBERS_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   derivedStaticFunc()[#Void#]{{$}}
// BASE_MEMBERS_STATIC-NEXT: Decl[InstanceMethod]/Super:       baseInstanceFunc({#self: MembersBase#})[#() -> Void#]{{$}}
// BASE_MEMBERS_STATIC-NEXT: Decl[StaticMethod]/Super:         baseStaticFunc()[#Void#]{{$}}
// BASE_MEMBERS_STATIC-NEXT: End completions
}

//===--- Check that we can look into protocols.

func testLookInProtoNoDot1() {
  fooProtocolInstance#^PROTO_MEMBERS_NO_DOT_1^#
// PROTO_MEMBERS_NO_DOT_1: Begin completions
// PROTO_MEMBERS_NO_DOT_1-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVar1[#Int#]{{$}}
// PROTO_MEMBERS_NO_DOT_1-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVar2[#Int#]{{$}}
// PROTO_MEMBERS_NO_DOT_1-NEXT: Decl[InstanceMethod]/CurrNominal: .fooInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_1-NEXT: Decl[InstanceMethod]/CurrNominal: .fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_1-NEXT: Decl[Subscript]/CurrNominal:      [{#i: Int#}][#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_1-NEXT: End completions
}

func testLookInProtoNoDot2() {
  fooBarProtocolInstance#^PROTO_MEMBERS_NO_DOT_2^#
// PROTO_MEMBERS_NO_DOT_2: Begin completions
// PROTO_MEMBERS_NO_DOT_2-NEXT: Decl[InstanceVar]/CurrNominal:    .barInstanceVar[#Int#]{{$}}
// PROTO_MEMBERS_NO_DOT_2-NEXT: Decl[InstanceMethod]/CurrNominal: .barInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_2-NEXT: Decl[InstanceMethod]/CurrNominal: .barInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_2-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVar1[#Int#]{{$}}
// PROTO_MEMBERS_NO_DOT_2-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVar2[#Int#]{{$}}
// PROTO_MEMBERS_NO_DOT_2-NEXT: Decl[InstanceMethod]/CurrNominal: .fooInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_2-NEXT: Decl[InstanceMethod]/CurrNominal: .fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_2-NEXT: Decl[Subscript]/CurrNominal:      [{#i: Int#}][#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_2-NEXT: End completions
}

func testLookInProtoNoDot3() {
  fooExBarExProtocolInstance#^PROTO_MEMBERS_NO_DOT_3^#
// PROTO_MEMBERS_NO_DOT_3: Begin completions
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceVar]/Super:          .barInstanceVar[#Int#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceMethod]/Super:       .barInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceMethod]/Super:       .barInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceMethod]/CurrNominal: .barExInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceVar]/Super:          .fooInstanceVar1[#Int#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceVar]/Super:          .fooInstanceVar2[#Int#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceMethod]/Super:       .fooInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceMethod]/Super:       .fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[Subscript]/Super:            [{#i: Int#}][#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: Decl[InstanceMethod]/CurrNominal: .fooExInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_NO_DOT_3-NEXT: End completions
}

func testLookInProto1() {
  fooProtocolInstance.#^PROTO_MEMBERS_1^#
// PROTO_MEMBERS_1: Begin completions
// PROTO_MEMBERS_1-NEXT: Decl[InstanceVar]/CurrNominal:    fooInstanceVar1[#Int#]{{$}}
// PROTO_MEMBERS_1-NEXT: Decl[InstanceVar]/CurrNominal:    fooInstanceVar2[#Int#]{{$}}
// PROTO_MEMBERS_1-NEXT: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_1-NEXT: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_1-NEXT: End completions
}

func testLookInProto2() {
  fooBarProtocolInstance.#^PROTO_MEMBERS_2^#
// PROTO_MEMBERS_2: Begin completions
// PROTO_MEMBERS_2-NEXT: Decl[InstanceVar]/CurrNominal:    barInstanceVar[#Int#]{{$}}
// PROTO_MEMBERS_2-NEXT: Decl[InstanceMethod]/CurrNominal: barInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_2-NEXT: Decl[InstanceMethod]/CurrNominal: barInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_2-NEXT: Decl[InstanceVar]/CurrNominal:    fooInstanceVar1[#Int#]{{$}}
// PROTO_MEMBERS_2-NEXT: Decl[InstanceVar]/CurrNominal:    fooInstanceVar2[#Int#]{{$}}
// PROTO_MEMBERS_2-NEXT: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_2-NEXT: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_2-NEXT: End completions
}

func testLookInProto3() {
  fooExBarExProtocolInstance.#^PROTO_MEMBERS_3^#
// PROTO_MEMBERS_3: Begin completions
// PROTO_MEMBERS_3-NEXT: Decl[InstanceVar]/Super:          barInstanceVar[#Int#]{{$}}
// PROTO_MEMBERS_3-NEXT: Decl[InstanceMethod]/Super:       barInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_3-NEXT: Decl[InstanceMethod]/Super:       barInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_3-NEXT: Decl[InstanceMethod]/CurrNominal: barExInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_3-NEXT: Decl[InstanceVar]/Super:          fooInstanceVar1[#Int#]{{$}}
// PROTO_MEMBERS_3-NEXT: Decl[InstanceVar]/Super:          fooInstanceVar2[#Int#]{{$}}
// PROTO_MEMBERS_3-NEXT: Decl[InstanceMethod]/Super:       fooInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_3-NEXT: Decl[InstanceMethod]/Super:       fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// PROTO_MEMBERS_3-NEXT: Decl[InstanceMethod]/CurrNominal: fooExInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_3-NEXT: End completions
}

func testLookInProto4(a: protocol<FooProtocol, BarBazProtocolComposition>) {
  a.#^PROTO_MEMBERS_4^#
// PROTO_MEMBERS_4: Begin completions
// PROTO_MEMBERS_4-DAG: Decl[InstanceMethod]/CurrNominal: fooInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_4-DAG: Decl[InstanceMethod]/CurrNominal: barInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_4-DAG: Decl[InstanceMethod]/CurrNominal: bazInstanceFunc0()[#Double#]{{$}}
// PROTO_MEMBERS_4: End completions
}

//===--- Check that we can resolve function parameters.

func testResolveFuncParam1(fs: FooStruct) {
  fs.#^RESOLVE_FUNC_PARAM_1^#
}

class TestResolveFuncParam2 {
  func testResolveFuncParam2a(fs: FooStruct) {
    fs.#^RESOLVE_FUNC_PARAM_2^#
  }
}

func testResolveFuncParam3<Foo : FooProtocol>(foo: Foo) {
  foo.#^RESOLVE_FUNC_PARAM_3^#
// RESOLVE_FUNC_PARAM_3: Begin completions
// RESOLVE_FUNC_PARAM_3-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar1[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_3-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar2[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_3-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc0()[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_3-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_3-NEXT: End completions
}

func testResolveFuncParam4<FooBar : protocol<FooProtocol, BarProtocol>>(fooBar: FooBar) {
  fooBar.#^RESOLVE_FUNC_PARAM_4^#
// RESOLVE_FUNC_PARAM_4: Begin completions
// RESOLVE_FUNC_PARAM_4-NEXT: Decl[InstanceVar]/Super:    barInstanceVar[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_4-NEXT: Decl[InstanceMethod]/Super: barInstanceFunc0()[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_4-NEXT: Decl[InstanceMethod]/Super: barInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_4-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar1[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_4-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar2[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_4-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc0()[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_4-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_4-NEXT: End completions
}

func testResolveFuncParam5<FooExBarEx : protocol<FooExProtocol, BarExProtocol>>(a: FooExBarEx) {
  a.#^RESOLVE_FUNC_PARAM_5^#
// RESOLVE_FUNC_PARAM_5: Begin completions
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceVar]/Super:    barInstanceVar[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceMethod]/Super: barInstanceFunc0()[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceMethod]/Super: barInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceMethod]/Super: barExInstanceFunc0()[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar1[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar2[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc0()[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: Decl[InstanceMethod]/Super: fooExInstanceFunc0()[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_5-NEXT: End completions
}

func testResolveFuncParam6<Foo : FooProtocol where Foo : FooClass>(foo: Foo) {
  foo.#^RESOLVE_FUNC_PARAM_6^#
// RESOLVE_FUNC_PARAM_6: Begin completions
// RESOLVE_FUNC_PARAM_6-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar1[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_6-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar2[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_6-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc0()[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_6-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// RESOLVE_FUNC_PARAM_6-NEXT: Decl[InstanceVar]/Super:    fooClassInstanceVar[#Int#]{{$}}
// RESOLVE_FUNC_PARAM_6-NEXT: Decl[InstanceMethod]/Super: fooClassInstanceFunc0()[#Void#]{{$}}
// RESOLVE_FUNC_PARAM_6-NEXT: Decl[InstanceMethod]/Super: fooClassInstanceFunc1({#(a): Int#})[#Void#]{{$}}
// RESOLVE_FUNC_PARAM_6-NEXT: End completions
}

class TestResolveConstructorParam1 {
  init(fs: FooStruct) {
    fs.#^RESOLVE_CONSTRUCTOR_PARAM_1^#
  }
}

class TestResolveConstructorParam2 {
  init<Foo : FooProtocol>(foo: Foo) {
    foo.#^RESOLVE_CONSTRUCTOR_PARAM_2^#
// RESOLVE_CONSTRUCTOR_PARAM_2: Begin completions
// RESOLVE_CONSTRUCTOR_PARAM_2-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar1[#Int#]{{$}}
// RESOLVE_CONSTRUCTOR_PARAM_2-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar2[#Int#]{{$}}
// RESOLVE_CONSTRUCTOR_PARAM_2-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc0()[#Double#]{{$}}
// RESOLVE_CONSTRUCTOR_PARAM_2-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// RESOLVE_CONSTRUCTOR_PARAM_2-NEXT: End completions
  }
}

class TestResolveConstructorParam3<Foo : FooProtocol> {
  init(foo: Foo) {
    foo.#^RESOLVE_CONSTRUCTOR_PARAM_3^#
// RESOLVE_CONSTRUCTOR_PARAM_3: Begin completions
// RESOLVE_CONSTRUCTOR_PARAM_3-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar1[#Int#]{{$}}
// RESOLVE_CONSTRUCTOR_PARAM_3-NEXT: Decl[InstanceVar]/Super:    fooInstanceVar2[#Int#]{{$}}
// RESOLVE_CONSTRUCTOR_PARAM_3-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc0()[#Double#]{{$}}
// RESOLVE_CONSTRUCTOR_PARAM_3-NEXT: Decl[InstanceMethod]/Super: fooInstanceFunc1({#(a): Int#})[#Double#]{{$}}
// RESOLVE_CONSTRUCTOR_PARAM_3-NEXT: End completions
  }
}

//===--- Check that we can handle ParenPattern in function arguments.

struct FuncParenPattern {
  init(_: Int) {}
  init(_: (Int, Int)) {}

  mutating
  func instanceFunc(_: Int) {}

  subscript(_: Int) -> Int {
    get {
      return 0
    }
  }
}

func testFuncParenPattern1(fpp: FuncParenPattern) {
  fpp#^FUNC_PAREN_PATTERN_1^#
// FUNC_PAREN_PATTERN_1: Begin completions
// FUNC_PAREN_PATTERN_1-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc({#Int#})[#Void#]{{$}}
// FUNC_PAREN_PATTERN_1-NEXT: Decl[Subscript]/CurrNominal: [{#(Int)#}][#Int#]{{$}}
// FUNC_PAREN_PATTERN_1-NEXT: End completions
}

func testFuncParenPattern2(fpp: FuncParenPattern) {
  FuncParenPattern#^FUNC_PAREN_PATTERN_2^#
// FUNC_PAREN_PATTERN_2: Begin completions
// FUNC_PAREN_PATTERN_2-NEXT: Decl[Constructor]/CurrNominal: ({#Int#})[#FuncParenPattern#]{{$}}
// FUNC_PAREN_PATTERN_2-NEXT: Decl[Constructor]/CurrNominal: ({#(Int, Int)#})[#FuncParenPattern#]{{$}}
// FUNC_PAREN_PATTERN_2-NEXT: Decl[InstanceMethod]/CurrNominal: .instanceFunc({#self: &FuncParenPattern#})[#(Int) -> Void#]{{$}}
// FUNC_PAREN_PATTERN_2-NEXT: End completions
}

func testFuncParenPattern3(var fpp: FuncParenPattern) {
  fpp.instanceFunc#^FUNC_PAREN_PATTERN_3^#
// FUNC_PAREN_PATTERN_3: Begin completions
// FUNC_PAREN_PATTERN_3-NEXT: Pattern/ExprSpecific: ({#Int#})[#Void#]{{$}}
// FUNC_PAREN_PATTERN_3-NEXT: End completions
}

//===--- Check that we can code complete after function calls

struct SomeBuilder {
  init(a: Int) {}
  func doFoo() -> SomeBuilder { return self }
  func doBar() -> SomeBuilder { return self }
  func doBaz(z: Double) -> SomeBuilder { return self }
}

func testChainedCalls1() {
  SomeBuilder(42)#^CHAINED_CALLS_1^#
// CHAINED_CALLS_1: Begin completions
// CHAINED_CALLS_1-DAG: Decl[InstanceMethod]/CurrNominal: .doFoo()[#SomeBuilder#]{{$}}
// CHAINED_CALLS_1-DAG: Decl[InstanceMethod]/CurrNominal: .doBar()[#SomeBuilder#]{{$}}
// CHAINED_CALLS_1-DAG: Decl[InstanceMethod]/CurrNominal: .doBaz({#(z): Double#})[#SomeBuilder#]{{$}}
// CHAINED_CALLS_1: End completions
}

func testChainedCalls2() {
  SomeBuilder(42).doFoo()#^CHAINED_CALLS_2^#
// CHAINED_CALLS_2: Begin completions
// CHAINED_CALLS_2-DAG: Decl[InstanceMethod]/CurrNominal: .doFoo()[#SomeBuilder#]{{$}}
// CHAINED_CALLS_2-DAG: Decl[InstanceMethod]/CurrNominal: .doBar()[#SomeBuilder#]{{$}}
// CHAINED_CALLS_2-DAG: Decl[InstanceMethod]/CurrNominal: .doBaz({#(z): Double#})[#SomeBuilder#]{{$}}
// CHAINED_CALLS_2: End completions
}

func testChainedCalls3() {
  // doBaz() takes a Double.  Check that we can recover.
  SomeBuilder(42).doFoo().doBaz(SomeBuilder(24))#^CHAINED_CALLS_3^#
// CHAINED_CALLS_3: Begin completions
// CHAINED_CALLS_3-DAG: Decl[InstanceMethod]/CurrNominal: .doFoo()[#SomeBuilder#]{{$}}
// CHAINED_CALLS_3-DAG: Decl[InstanceMethod]/CurrNominal: .doBar()[#SomeBuilder#]{{$}}
// CHAINED_CALLS_3-DAG: Decl[InstanceMethod]/CurrNominal: .doBaz({#z: Double#})[#SomeBuilder#]{{$}}
// CHAINED_CALLS_3: End completions
}

//===--- Check that we can code complete expressions that have generic parameters

func testResolveGenericParams1() {
  FooGenericStruct<FooStruct>()#^RESOLVE_GENERIC_PARAMS_1^#
// RESOLVE_GENERIC_PARAMS_1: Begin completions
// RESOLVE_GENERIC_PARAMS_1-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#FooStruct#]{{$}}
// RESOLVE_GENERIC_PARAMS_1-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[FooStruct]#]{{$}}
// RESOLVE_GENERIC_PARAMS_1-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): FooStruct#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_1-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): FooStruct#})[#FooStruct#]{{$}}
// RESOLVE_GENERIC_PARAMS_1-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_1-NEXT: End completions

  FooGenericStruct<FooStruct>#^RESOLVE_GENERIC_PARAMS_1_STATIC^#
// RESOLVE_GENERIC_PARAMS_1_STATIC: Begin completions
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[Constructor]/CurrNominal:    ({#t: FooStruct#})[#FooGenericStruct<FooStruct>#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#self: &FooGenericStruct<FooStruct>#})[#FooStruct -> Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#self: &FooGenericStruct<FooStruct>#})[#FooStruct -> FooStruct#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#self: &FooGenericStruct<FooStruct>#})[#(U) -> U#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): FooStruct#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): FooStruct#})[#FooStruct#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_1_STATIC-NEXT: End completions
}

func testResolveGenericParams2<Foo : FooProtocol>(foo: Foo) {
  FooGenericStruct<Foo>()#^RESOLVE_GENERIC_PARAMS_2^#
// RESOLVE_GENERIC_PARAMS_2: Begin completions
// RESOLVE_GENERIC_PARAMS_2-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#Foo#]{{$}}
// RESOLVE_GENERIC_PARAMS_2-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[Foo]#]{{$}}
// RESOLVE_GENERIC_PARAMS_2-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): Foo#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_2-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): Foo#})[#Foo#]{{$}}
// RESOLVE_GENERIC_PARAMS_2-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_2-NEXT: End completions

  FooGenericStruct<Foo>#^RESOLVE_GENERIC_PARAMS_2_STATIC^#
// RESOLVE_GENERIC_PARAMS_2_STATIC: Begin completions
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[Constructor]/CurrNominal:    ({#t: Foo#})[#FooGenericStruct<Foo>#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#self: &FooGenericStruct<Foo>#})[#Foo -> Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#self: &FooGenericStruct<Foo>#})[#Foo -> Foo#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#self: &FooGenericStruct<Foo>#})[#(U) -> U#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): Foo#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): Foo#})[#Foo#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_2_STATIC-NEXT: End completions
}

struct TestResolveGenericParams3_4<T> {
  func testResolveGenericParams3() {
    FooGenericStruct<FooStruct>()#^RESOLVE_GENERIC_PARAMS_3^#
// RESOLVE_GENERIC_PARAMS_3: Begin completions
// RESOLVE_GENERIC_PARAMS_3-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#FooStruct#]{{$}}
// RESOLVE_GENERIC_PARAMS_3-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[FooStruct]#]{{$}}
// RESOLVE_GENERIC_PARAMS_3-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): FooStruct#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_3-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): FooStruct#})[#FooStruct#]{{$}}
// RESOLVE_GENERIC_PARAMS_3-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_3-NEXT: End completions

    FooGenericStruct<FooStruct>#^RESOLVE_GENERIC_PARAMS_3_STATIC^#
// RESOLVE_GENERIC_PARAMS_3_STATIC: Begin completions, 9 items
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[Constructor]/CurrNominal:    ({#t: FooStruct#})[#FooGenericStruct<FooStruct>#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#self: &FooGenericStruct<FooStruct>#})[#FooStruct -> Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#self: &FooGenericStruct<FooStruct>#})[#FooStruct -> FooStruct#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#self: &FooGenericStruct<FooStruct>#})[#(U) -> U#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): FooStruct#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): FooStruct#})[#FooStruct#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_3_STATIC-NEXT: End completions
  }

  func testResolveGenericParams4(t: T) {
    FooGenericStruct<T>(t)#^RESOLVE_GENERIC_PARAMS_4^#
// RESOLVE_GENERIC_PARAMS_4: Begin completions
// RESOLVE_GENERIC_PARAMS_4-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#T#]{{$}}
// RESOLVE_GENERIC_PARAMS_4-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[T]#]{{$}}
// RESOLVE_GENERIC_PARAMS_4-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): T#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_4-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): T#})[#T#]{{$}}
// RESOLVE_GENERIC_PARAMS_4-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_4-NEXT: End completions

    FooGenericStruct<T>#^RESOLVE_GENERIC_PARAMS_4_STATIC^#
// RESOLVE_GENERIC_PARAMS_4_STATIC: Begin completions
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[Constructor]/CurrNominal:    ({#t: T#})[#FooGenericStruct<T>#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#self: &FooGenericStruct<T>#})[#T -> Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#self: &FooGenericStruct<T>#})[#T -> T#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#self: &FooGenericStruct<T>#})[#(U) -> U#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): T#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): T#})[#T#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_4_STATIC-NEXT: End completions
  }

  func testResolveGenericParams5<U>(u: U) {
    FooGenericStruct<U>(u)#^RESOLVE_GENERIC_PARAMS_5^#
// RESOLVE_GENERIC_PARAMS_5: Begin completions
// RESOLVE_GENERIC_PARAMS_5-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarT[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_5-NEXT: Decl[InstanceVar]/CurrNominal:    .fooInstanceVarTBrackets[#[U]#]{{$}}
// RESOLVE_GENERIC_PARAMS_5-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#(a): U#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_5-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_5-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_5-NEXT: End completions

    FooGenericStruct<U>#^RESOLVE_GENERIC_PARAMS_5_STATIC^#
// RESOLVE_GENERIC_PARAMS_5_STATIC: Begin completions
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[Constructor]/CurrNominal:    ({#t: U#})[#FooGenericStruct<U>#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooVoidInstanceFunc1({#self: &FooGenericStruct<U>#})[#U -> Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooTInstanceFunc1({#self: &FooGenericStruct<U>#})[#U -> U#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[InstanceMethod]/CurrNominal: .fooUInstanceFunc1({#self: &FooGenericStruct<U>#})[#(U) -> U#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarT[#Int#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[StaticVar]/CurrNominal:      .fooStaticVarTBrackets[#[Int]#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooVoidStaticFunc1({#(a): U#})[#Void#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooTStaticFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: Decl[StaticMethod]/CurrNominal:   .fooUInstanceFunc1({#(a): U#})[#U#]{{$}}
// RESOLVE_GENERIC_PARAMS_5_STATIC-NEXT: End completions
  }
}

func testResolveGenericParamsError1() {
  // There is no type 'Foo'.  Check that we don't crash.
  // FIXME: we could also display correct completion results here, because
  // swift does not have specialization, and the set of completion results does
  // not depend on the generic type argument.
  FooGenericStruct<NotDefinedType>()#^RESOLVE_GENERIC_PARAMS_ERROR_1^#
// RESOLVE_GENERIC_PARAMS_ERROR_1: found code completion token
// RESOLVE_GENERIC_PARAMS_ERROR_1-NOT: Begin completions
}

//===--- Check that we can code complete expressions that have unsolved type variables.

class BuilderStyle<T> {
  var count = 0
  func addString(s: String) -> BuilderStyle<T> {
    count++
    return self
  }
  func add(t: T) -> BuilderStyle<T> {
    count++
    return self
  }
  func get() -> Int {
    return count
  }
}

func testTypeCheckWithUnsolvedVariables1() {
  BuilderStyle().#^TC_UNSOLVED_VARIABLES_1^#
}
// TC_UNSOLVED_VARIABLES_1: Begin completions
// TC_UNSOLVED_VARIABLES_1-NEXT: Decl[InstanceVar]/CurrNominal: count[#Int#]{{$}}
// TC_UNSOLVED_VARIABLES_1-NEXT: Decl[InstanceMethod]/CurrNominal: addString({#(s): String#})[#BuilderStyle<T>#]{{$}}
// TC_UNSOLVED_VARIABLES_1-NEXT: Decl[InstanceMethod]/CurrNominal: add({#(t): T#})[#BuilderStyle<T>#]{{$}}
// TC_UNSOLVED_VARIABLES_1-NEXT: Decl[InstanceMethod]/CurrNominal: get()[#Int#]{{$}}
// TC_UNSOLVED_VARIABLES_1-NEXT: End completions

func testTypeCheckWithUnsolvedVariables2() {
  BuilderStyle().addString("abc").#^TC_UNSOLVED_VARIABLES_2^#
}
// TC_UNSOLVED_VARIABLES_2: Begin completions
// TC_UNSOLVED_VARIABLES_2-NEXT: Decl[InstanceVar]/CurrNominal:    count[#Int#]{{$}}
// TC_UNSOLVED_VARIABLES_2-NEXT: Decl[InstanceMethod]/CurrNominal: addString({#(s): String#})[#BuilderStyle<T>#]{{$}}
// TC_UNSOLVED_VARIABLES_2-NEXT: Decl[InstanceMethod]/CurrNominal: add({#(t): T#})[#BuilderStyle<T>#]{{$}}
// TC_UNSOLVED_VARIABLES_2-NEXT: Decl[InstanceMethod]/CurrNominal: get()[#Int#]{{$}}
// TC_UNSOLVED_VARIABLES_2-NEXT: End completions

func testTypeCheckWithUnsolvedVariables3() {
  BuilderStyle().addString("abc").add(42).#^TC_UNSOLVED_VARIABLES_3^#
}
// TC_UNSOLVED_VARIABLES_3: Begin completions
// TC_UNSOLVED_VARIABLES_3-NEXT: Decl[InstanceVar]/CurrNominal:    count[#Int#]{{$}}
// TC_UNSOLVED_VARIABLES_3-NEXT: Decl[InstanceMethod]/CurrNominal: addString({#(s): String#})[#BuilderStyle<Int>#]{{$}}
// TC_UNSOLVED_VARIABLES_3-NEXT: Decl[InstanceMethod]/CurrNominal: add({#(t): Int#})[#BuilderStyle<Int>#]{{$}}
// TC_UNSOLVED_VARIABLES_3-NEXT: Decl[InstanceMethod]/CurrNominal: get()[#Int#]{{$}}
// TC_UNSOLVED_VARIABLES_3-NEXT: End completions

//===--- Check that we can look up into modules

func testResolveModules1() {
  Swift#^RESOLVE_MODULES_1^#
// RESOLVE_MODULES_1: Begin completions
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule:    .Int8[#Int8#]{{$}}
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule:    .Int16[#Int16#]{{$}}
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule:    .Int32[#Int32#]{{$}}
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule:    .Int64[#Int64#]{{$}}
// RESOLVE_MODULES_1-DAG: Decl[Struct]/OtherModule:    .Bool[#Bool#]{{$}}
// RESOLVE_MODULES_1-DAG: Decl[TypeAlias]/OtherModule: .Float32[#Float#]{{$}}
// RESOLVE_MODULES_1: End completions
}

//===--- Check that we can complete inside interpolated string literals

func testInterpolatedString1() {
  "\(fooObject.#^INTERPOLATED_STRING_1^#)"
}
