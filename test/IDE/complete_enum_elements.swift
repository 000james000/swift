// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_1 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=WITH_GLOBAL_RESULTS < %t.enum.txt
// RUN: FileCheck %s -check-prefix=FOO_ENUM_TYPE_CONTEXT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_2 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=WITH_GLOBAL_RESULTS < %t.enum.txt
// RUN: FileCheck %s -check-prefix=FOO_ENUM_TYPE_CONTEXT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_3 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=WITH_GLOBAL_RESULTS < %t.enum.txt
// RUN: FileCheck %s -check-prefix=BAR_ENUM_TYPE_CONTEXT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_4 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=WITH_GLOBAL_RESULTS < %t.enum.txt
// RUN: FileCheck %s -check-prefix=BAZ_ENUM_TYPE_CONTEXT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_5 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=WITH_GLOBAL_RESULTS < %t.enum.txt
// RUN: FileCheck %s -check-prefix=QUX_ENUM_TYPE_CONTEXT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_WITH_DOT_1 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=FOO_ENUM_DOT_ELEMENTS < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_WITH_QUAL_1 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=FOO_ENUM_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_EXPR_ERROR_1 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=FOO_ENUM_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_IN_PATTERN_1 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=WITH_GLOBAL_RESULTS < %t.enum.txt
// RUN: FileCheck %s -check-prefix=ENUM_SW_IN_PATTERN_1 < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_SW_IN_PATTERN_2 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=ERROR_COMMON < %t.enum.txt
// RUN: FileCheck %s -check-prefix=ENUM_SW_IN_PATTERN_2 < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_NO_DOT_1 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=FOO_ENUM_NO_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_NO_DOT_2 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=BAR_ENUM_NO_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_NO_DOT_3 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=BAZ_INT_ENUM_NO_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_NO_DOT_4 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=BAZ_T_ENUM_NO_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_NO_DOT_5 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=QUX_ENUM_NO_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_DOT_1 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=FOO_ENUM_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_DOT_2 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=BAR_ENUM_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_DOT_3 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=BAZ_INT_ENUM_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_DOT_4 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=BAZ_T_ENUM_DOT < %t.enum.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_QUAL_DOT_5 > %t.enum.txt
// RUN: FileCheck %s -check-prefix=QUX_ENUM_DOT < %t.enum.txt

// ERROR_COMMON: found code completion token
// ERROR_COMMON-NOT: Begin completions

//===---
//===--- Test that we can complete enum elements.
//===---

//===--- Helper types.

enum FooEnum {
  case Foo1
  case Foo2
}

// FOO_ENUM_TYPE_CONTEXT: Begin completions
// FOO_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Foo1[#FooEnum.Type -> FooEnum#]{{$}}
// FOO_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Foo2[#FooEnum.Type -> FooEnum#]{{$}}
// FOO_ENUM_TYPE_CONTEXT: End completions

// FOO_ENUM_NO_DOT: Begin completions
// FOO_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal: .Foo1[#FooEnum.Type -> FooEnum#]{{$}}
// FOO_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal: .Foo2[#FooEnum.Type -> FooEnum#]{{$}}
// FOO_ENUM_NO_DOT-NEXT: End completions

// FOO_ENUM_DOT: Begin completions
// FOO_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal: Foo1[#FooEnum.Type -> FooEnum#]{{$}}
// FOO_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal: Foo2[#FooEnum.Type -> FooEnum#]{{$}}
// FOO_ENUM_DOT-NEXT: End completions

// FOO_ENUM_DOT_ELEMENTS: Begin completions, 2 items
// FOO_ENUM_DOT_ELEMENTS-NEXT: Decl[EnumElement]/ExprSpecific: Foo1[#FooEnum.Type -> FooEnum#]{{$}}
// FOO_ENUM_DOT_ELEMENTS-NEXT: Decl[EnumElement]/ExprSpecific: Foo2[#FooEnum.Type -> FooEnum#]{{$}}
// FOO_ENUM_DOT_ELEMENTS-NEXT: End completions

enum BarEnum {
  case Bar1
  case Bar2()
  case Bar3(a: Int)
  case Bar4(a: Int, b: Float)
  case Bar5(a: Int, (b: Float))
  case Bar6(a: Int, b: (c: Float))
  case Bar7(a: Int, (b: Float, c: Double))
  case Bar8(a: Int, b: (c: Float, d: Double))
  case Bar9(Int)
  case Bar10(Int, Float)
  case Bar11(Int, (Float))
  case Bar12(Int, (Float, Double))

  mutating
  func barInstanceFunc() {}
  static var staticVar: Int = 12
  static func barStaticFunc() {}
}

// BAR_ENUM_TYPE_CONTEXT: Begin completions
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar1[#BarEnum.Type -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar2()[#BarEnum.Type -> () -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar3({#a: Int#})[#BarEnum.Type -> (a: Int) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar4({#a: Int#}, {#b: Float#})[#BarEnum.Type -> (a: Int, b: Float) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar5({#a: Int#}, ({#b: Float#}))[#BarEnum.Type -> (a: Int, (b: Float)) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar6({#a: Int#}, b: ({#c: Float#}))[#BarEnum.Type -> (a: Int, b: (c: Float)) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar7({#a: Int#}, ({#b: Float#}, {#c: Double#}))[#BarEnum.Type -> (a: Int, (b: Float, c: Double)) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar8({#a: Int#}, b: ({#c: Float#}, {#d: Double#}))[#BarEnum.Type -> (a: Int, b: (c: Float, d: Double)) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar9({#Int#})[#BarEnum.Type -> (Int) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar10({#Int#}, {#Float#})[#BarEnum.Type -> (Int, Float) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar11({#Int#}, {#Float#})[#BarEnum.Type -> (Int, (Float)) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Bar12({#Int#}, ({#Float#}, {#Double#}))[#BarEnum.Type -> (Int, (Float, Double)) -> BarEnum#]{{$}}
// BAR_ENUM_TYPE_CONTEXT: End completions

// BAR_ENUM_NO_DOT: Begin completions
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar1[#BarEnum.Type -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar2()[#BarEnum.Type -> () -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar3({#a: Int#})[#BarEnum.Type -> (a: Int) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar4({#a: Int#}, {#b: Float#})[#BarEnum.Type -> (a: Int, b: Float) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar5({#a: Int#}, ({#b: Float#}))[#BarEnum.Type -> (a: Int, (b: Float)) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar6({#a: Int#}, b: ({#c: Float#}))[#BarEnum.Type -> (a: Int, b: (c: Float)) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar7({#a: Int#}, ({#b: Float#}, {#c: Double#}))[#BarEnum.Type -> (a: Int, (b: Float, c: Double)) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar8({#a: Int#}, b: ({#c: Float#}, {#d: Double#}))[#BarEnum.Type -> (a: Int, b: (c: Float, d: Double)) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar9({#Int#})[#BarEnum.Type -> (Int) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar10({#Int#}, {#Float#})[#BarEnum.Type -> (Int, Float) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar11({#Int#}, {#Float#})[#BarEnum.Type -> (Int, (Float)) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Bar12({#Int#}, ({#Float#}, {#Double#}))[#BarEnum.Type -> (Int, (Float, Double)) -> BarEnum#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .barInstanceFunc({#self: &BarEnum#})[#() -> Void#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVar[#Int#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .barStaticFunc()[#Void#]{{$}}
// BAR_ENUM_NO_DOT-NEXT: End completions

// BAR_ENUM_DOT: Begin completions
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar1[#BarEnum.Type -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar2()[#BarEnum.Type -> () -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar3({#a: Int#})[#BarEnum.Type -> (a: Int) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar4({#a: Int#}, {#b: Float#})[#BarEnum.Type -> (a: Int, b: Float) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar5({#a: Int#}, ({#b: Float#}))[#BarEnum.Type -> (a: Int, (b: Float)) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar6({#a: Int#}, b: ({#c: Float#}))[#BarEnum.Type -> (a: Int, b: (c: Float)) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar7({#a: Int#}, ({#b: Float#}, {#c: Double#}))[#BarEnum.Type -> (a: Int, (b: Float, c: Double)) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar8({#a: Int#}, b: ({#c: Float#}, {#d: Double#}))[#BarEnum.Type -> (a: Int, b: (c: Float, d: Double)) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar9({#Int#})[#BarEnum.Type -> (Int) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar10({#Int#}, {#Float#})[#BarEnum.Type -> (Int, Float) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar11({#Int#}, {#Float#})[#BarEnum.Type -> (Int, (Float)) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Bar12({#Int#}, ({#Float#}, {#Double#}))[#BarEnum.Type -> (Int, (Float, Double)) -> BarEnum#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: barInstanceFunc({#self: &BarEnum#})[#() -> Void#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVar[#Int#]{{$}}
// BAR_ENUM_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   barStaticFunc()[#Void#]{{$}}
// BAR_ENUM_DOT-NEXT: End completions

enum BazEnum<T> {
  case Baz1
  case Baz2(T)

  mutating
  func bazInstanceFunc() {}
  static var staticVar: Int = 12
  static var staticVarT: T = 17
  static func bazStaticFunc() {}
}

// BAZ_ENUM_TYPE_CONTEXT: Begin completions
// BAZ_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Baz1[#<T> BazEnum<T>.Type -> BazEnum<T>#]{{$}}
// BAZ_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Baz2({#T#})[#<T> BazEnum<T>.Type -> (T) -> BazEnum<T>#]{{$}}
// BAZ_ENUM_TYPE_CONTEXT: End completions

// BAZ_INT_ENUM_NO_DOT: Begin completions, 6 items
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Baz1[#<T> BazEnum<T>.Type -> BazEnum<T>#]{{$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Baz2({#T#})[#<T> BazEnum<T>.Type -> (T) -> BazEnum<T>#]{{$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .bazInstanceFunc({#self: &BazEnum<Int>#})[#() -> Void#]{{$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVar[#Int#]{{$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVarT[#Int#]{{$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .bazStaticFunc()[#Void#]{{$}}
// BAZ_INT_ENUM_NO_DOT-NEXT: End completions

// BAZ_T_ENUM_NO_DOT: Begin completions, 6 items
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Baz1[#<T> BazEnum<T>.Type -> BazEnum<T>#]{{$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Baz2({#T#})[#<T> BazEnum<T>.Type -> (T) -> BazEnum<T>#]{{$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .bazInstanceFunc({#self: &BazEnum<T>#})[#() -> Void#]{{$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVar[#Int#]{{$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[StaticVar]/CurrNominal:      .staticVarT[#T#]{{$}}
// BAZ_T_ENUM_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .bazStaticFunc()[#Void#]{{$}}
// BAZ_T_ENUM_NO_DOT-NEXT: End completions

// BAZ_INT_ENUM_DOT: Begin completions, 6 items
// BAZ_INT_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Baz1[#<T> BazEnum<T>.Type -> BazEnum<T>#]{{$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Baz2({#T#})[#<T> BazEnum<T>.Type -> (T) -> BazEnum<T>#]{{$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: bazInstanceFunc({#self: &BazEnum<Int>#})[#() -> Void#]{{$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVar[#Int#]{{$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVarT[#Int#]{{$}}
// BAZ_INT_ENUM_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   bazStaticFunc()[#Void#]{{$}}
// BAZ_INT_ENUM_DOT-NEXT: End completions

// BAZ_T_ENUM_DOT: Begin completions, 6 items
// BAZ_T_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Baz1[#<T> BazEnum<T>.Type -> BazEnum<T>#]{{$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Baz2({#T#})[#<T> BazEnum<T>.Type -> (T) -> BazEnum<T>#]{{$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: bazInstanceFunc({#self: &BazEnum<T>#})[#() -> Void#]{{$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVar[#Int#]{{$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[StaticVar]/CurrNominal:      staticVarT[#T#]{{$}}
// BAZ_T_ENUM_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   bazStaticFunc()[#Void#]{{$}}
// BAZ_T_ENUM_DOT-NEXT: End completions

enum QuxEnum : Int {
  case Qux1 = 10
  case Qux2 = 20
}

// QUX_ENUM_TYPE_CONTEXT: Begin completions
// QUX_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Qux1[#QuxEnum.Type -> QuxEnum#]{{$}}
// QUX_ENUM_TYPE_CONTEXT-DAG: Decl[EnumElement]/ExprSpecific: .Qux2[#QuxEnum.Type -> QuxEnum#]{{$}}
// QUX_ENUM_TYPE_CONTEXT: End completions

// QUX_ENUM_NO_DOT: Begin completions, 5 items
// QUX_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Qux1[#QuxEnum.Type -> QuxEnum#]{{$}}
// QUX_ENUM_NO_DOT-NEXT: Decl[EnumElement]/CurrNominal:    .Qux2[#QuxEnum.Type -> QuxEnum#]{{$}}
// QUX_ENUM_NO_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   .fromRaw({#Int#})[#QuxEnum?#]{{$}}
// QUX_ENUM_NO_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: .toRaw({#self: QuxEnum#})[#() -> Int#]{{$}}
// QUX_ENUM_NO_DOT-NEXT: Decl[TypeAlias]/Super:            .Raw[#Int#]{{$}}
// QUX_ENUM_NO_DOT-NEXT: End completions

// QUX_ENUM_DOT: Begin completions, 5 items
// QUX_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Qux1[#QuxEnum.Type -> QuxEnum#]{{$}}
// QUX_ENUM_DOT-NEXT: Decl[EnumElement]/CurrNominal:    Qux2[#QuxEnum.Type -> QuxEnum#]{{$}}
// QUX_ENUM_DOT-NEXT: Decl[StaticMethod]/CurrNominal:   fromRaw({#Int#})[#QuxEnum?#]{{$}}
// QUX_ENUM_DOT-NEXT: Decl[InstanceMethod]/CurrNominal: toRaw({#self: QuxEnum#})[#() -> Int#]{{$}}
// QUX_ENUM_DOT-NEXT: Decl[TypeAlias]/Super:            Raw[#Int#]{{$}}
// QUX_ENUM_DOT-NEXT: End completions

func freeFunc() {}

// WITH_GLOBAL_RESULTS: Begin completions
// WITH_GLOBAL_RESULTS: Decl[FreeFunction]/CurrModule: freeFunc()[#Void#]{{$}}
// WITH_GLOBAL_RESULTS: End completions

//===--- Complete enum elements in 'switch'.

func testSwitch1(e: FooEnum) {
  switch e {
  case #^ENUM_SW_1^#
  }
}
func testSwitch2(e: FooEnum) {
  switch e {
  case .Foo1:
  case #^ENUM_SW_2^#
  }
}
func testSwitch3(e: BarEnum) {
  switch e {
  case #^ENUM_SW_3^#
  }
}
func testSwitch4(e: BazEnum<Int>) {
  switch e {
  case #^ENUM_SW_4^#
  }
}
func testSwitch5(e: QuxEnum) {
  switch e {
  case #^ENUM_SW_5^#
  }
}

func testSwitchWithDot1(e: FooEnum) {
  switch e {
  case .#^ENUM_SW_WITH_DOT_1^#
  }
}

func testSwitchWithQualification1(e: FooEnum) {
  switch e {
  case FooEnum.#^ENUM_SW_WITH_QUAL_1^#
  }
}

func testSwitchExprError1() {
  switch unknown_var {
  case FooEnum.#^ENUM_SW_EXPR_ERROR_1^#
  }
}

// FIXME
func testSwitchInPattern1(e: BazEnum<Int>) {
  switch e {
  case .Baz2(#^ENUM_SW_IN_PATTERN_1^#
  }
}
// ENUM_SW_IN_PATTERN_1-NOT: .Baz1

func testSwitchInPattern2(e: BazEnum<Int>) {
  switch e {
  case .Baz2(.#^ENUM_SW_IN_PATTERN_2^#
  }
}
// ENUM_SW_IN_PATTERN_2-NOT: .Baz1

//===--- Complete qualified references to enum elements.

func testQualifiedNoDot1() {
  var e = FooEnum#^ENUM_QUAL_NO_DOT_1^#
}
func testQualifiedNoDot2() {
  var e = BarEnum#^ENUM_QUAL_NO_DOT_2^#
}
func testQualifiedNoDot3() {
  var e = BazEnum<Int>#^ENUM_QUAL_NO_DOT_3^#
}
func testQualifiedNoDot4() {
  var e = BazEnum#^ENUM_QUAL_NO_DOT_4^#
}
func testQualifiedNoDot5() {
  var e = QuxEnum#^ENUM_QUAL_NO_DOT_5^#
}

func testQualifiedDot1() {
  var e = FooEnum.#^ENUM_QUAL_DOT_1^#
}
func testQualifiedDot2() {
  var e = BarEnum.#^ENUM_QUAL_DOT_2^#
}
func testQualifiedDot3() {
  var e = BazEnum<Int>.#^ENUM_QUAL_DOT_3^#
}
func testQualifiedDot4() {
  var e = BazEnum.#^ENUM_QUAL_DOT_4^#
}
func testQualifiedDot5() {
  var e = QuxEnum.#^ENUM_QUAL_DOT_5^#
}

