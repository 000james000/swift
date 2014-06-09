// RUN: sed -n -e '1,/NO_ERRORS_UP_TO_HERE$/ p' %s > %t_no_errors.swift
// RUN: %swift -verify -parse %t_no_errors.swift

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_PA > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_PA < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PA < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_PB > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_PB < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PB < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_PA_PB > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_PA_PB < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PA < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PB < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_BA > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_BA < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_BA < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_BA_PA > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_BA_PA < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_BA < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PA < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_BA_PB > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_BA_PB < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_BA < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PB < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_BB > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_BB < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_BB < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_BE > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_BE < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_BE < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_BE_PA > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_BE_PA < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_BE < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PA < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_BE_PA_PE > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_BE_PA_PE < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_BE < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PA < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=CLASS_PEI_PE > %t.txt
// RUN: FileCheck %s -check-prefix=CLASS_PEI_PE < %t.txt
// RUN: FileCheck %s -check-prefix=WITH_PEI < %t.txt

// RUN: %swift-ide-test -code-completion -source-filename %s -code-completion-token=NESTED_NOMINAL > %t.txt
// RUN: FileCheck %s -check-prefix=NESTED_NOMINAL < %t.txt

@objc
class TagPA {}
@objc
protocol ProtocolA {
  init(fromProtocolA: Int)

  func protoAFunc()
  @optional func protoAFuncOptional()

  @noreturn
  func protoAFuncWithAttr()

  subscript(a: TagPA) -> Int { get }

  var protoAVarRW: Int { get set }
  var protoAVarRO: Int { get }
}
// WITH_PA: Begin completions
// WITH_PA-DAG: Decl[Constructor]/Super:    init(fromProtocolA: Int) {|}{{$}}
// WITH_PA-DAG: Decl[InstanceMethod]/Super: func protoAFunc() {|}{{$}}
// WITH_PA-DAG: Decl[InstanceMethod]/Super: func protoAFuncOptional() {|}{{$}}
// WITH_PA-DAG: Decl[InstanceMethod]/Super: @noreturn func protoAFuncWithAttr() {|}{{$}}
// WITH_PA: End completions

struct TagPB {}
protocol ProtocolB : ProtocolA {
  init(fromProtocolB: Int)

  func protoBFunc()

  subscript(a: TagPB) -> Int { get }

  var protoBVarRW: Int { get set }
  var protoBVarRO: Int { get }
}
// WITH_PB: Begin completions
// WITH_PB-DAG: Decl[Constructor]/Super:    init(fromProtocolA: Int) {|}{{$}}
// WITH_PB-DAG: Decl[InstanceMethod]/Super: func protoAFunc() {|}{{$}}
// WITH_PB-DAG: Decl[InstanceMethod]/Super: @noreturn func protoAFuncWithAttr() {|}{{$}}
// WITH_PB-DAG: Decl[Constructor]/Super:    init(fromProtocolB: Int) {|}{{$}}
// WITH_PB-DAG: Decl[InstanceMethod]/Super: func protoBFunc() {|}{{$}}
// WITH_PB: End completions

struct TagPE {}
protocol ProtocolE {
  init(fromProtocolE: Int)

  func protoEFunc()

  subscript(a: TagPE) -> Int { get }

  var protoEVarRW: Int { get set }
  var protoEVarRO: Int { get }
}
// WITH_PE: Begin completions
// WITH_PE-DAG: Decl[Constructor]/Super:    init(fromProtocolE: Int) {|}{{$}}
// WITH_PE-DAG: Decl[InstanceMethod]/Super: func protoEFunc() {|}{{$}}
// WITH_PE: End completions

@noreturn @asmname("exit")
func exit()

class BaseA {
  init(fromBaseA: Int) {}
  init(fromBaseAWithParamName foo: Int, withOther bar: Double) {}
  convenience init(convenienceFromBaseA: Double) {
    self.init(fromBaseA: 0)
  }

  func baseAFunc(foo x: Int) {}
  func baseAFunc2(foo x: Int) {}

  @noreturn
  func baseAFuncWithAttr() {
    exit()
  }

  var baseAVarRW: Int { get { return 0 } set {} }
  var baseAVarRO: Int { return 0 }
}
// WITH_BA: Begin completions
// WITH_BA-DAG: Decl[Constructor]/Super:    init(fromBaseA: Int) {|}{{$}}
// WITH_BA-DAG: Decl[Constructor]/Super:    init(fromBaseAWithParamName foo: Int, withOther bar: Double) {|}{{$}}
// WITH_BA-DAG: Decl[InstanceMethod]/Super: override func baseAFunc(foo x: Int) {|}{{$}}
// WITH_BA-DAG: Decl[InstanceMethod]/Super: override func baseAFunc2(foo x: Int) {|}{{$}}
// WITH_BA-DAG: Decl[InstanceMethod]/Super: override @noreturn func baseAFuncWithAttr() {|}{{$}}
// WITH_BA: End completions

class BaseB : BaseA {
  override func baseAFunc2(foo x: Int) {}

  init(fromBaseB: Int) {}
  convenience init(convenienceFromBaseB: Double) {
    self.init(fromBaseB: 0)
  }

  func baseBFunc() {}

  var baseBVarRW: Int { get { return 0 } set {} }
  var baseBVarRO: Int { return 0 }
}
// WITH_BB: Begin completions
// WITH_BB-DAG: Decl[InstanceMethod]/Super: override func baseAFunc(foo x: Int) {|}{{$}}
// WITH_BB-DAG: Decl[InstanceMethod]/Super: override func baseAFunc2(foo x: Int) {|}{{$}}
// WITH_BB-DAG: Decl[InstanceMethod]/Super: override @noreturn func baseAFuncWithAttr() {|}{{$}}
// WITH_BB-DAG: Decl[Constructor]/Super:    init(fromBaseB: Int) {|}{{$}}
// WITH_BB-DAG: Decl[InstanceMethod]/Super: override func baseBFunc() {|}{{$}}
// WITH_BB: End completions

class BaseE : ProtocolE {
  init(fromProtocolE: Int) {}

  func protoEFunc() {}

  subscript(a: TagPE) -> Int { return 0 }

  var protoEVarRW: Int { get { return 0 } set {} }
  var protoEVarRO: Int { return 0 }

  init(fromBaseE: Int) {}

  func baseEFunc() {}

  var baseEVarRW: Int { get { return 0 } set {} }
  var baseEVarRO: Int { return 0 }
}
// WITH_BE: Begin completions
// WITH_BE-DAG: Decl[Constructor]/Super:    init(fromProtocolE: Int) {|}{{$}}
// WITH_BE-DAG: Decl[InstanceMethod]/Super: override func protoEFunc() {|}{{$}}
// WITH_BE-DAG: Decl[Constructor]/Super:    init(fromBaseE: Int) {|}{{$}}
// WITH_BE-DAG: Decl[InstanceMethod]/Super: override func baseEFunc() {|}{{$}}
// WITH_BE: End completions

class ProtocolEImpl /* : ProtocolE but does not implement the protocol */ {
  init(fromProtocolE: Int) {}

  func protoEFunc() {}

  subscript(a: TagPE) -> Int { return 0 }

  var protoEVarRW: Int { get { return 0 } set {} }
  var protoEVarRO: Int { return 0 }
}
// WITH_PEI: Begin completions
// WITH_PEI-DAG: Decl[Constructor]/Super:    init(fromProtocolE: Int) {|}{{$}}
// WITH_PEI-DAG: Decl[InstanceMethod]/Super: override func protoEFunc() {|}{{$}}
// WITH_PEI: End completions

// NO_ERRORS_UP_TO_HERE

class TestClass_PA : ProtocolA {
  func ERROR() {}

  #^CLASS_PA^#
}
// CLASS_PA: Begin completions, 4 items

class TestClass_PB : ProtocolB {
  #^CLASS_PB^#
}
// CLASS_PB: Begin completions, 6 items

class TestClass_PA_PB : ProtocolA, ProtocolB {
  #^CLASS_PA_PB^#
}
// CLASS_PA_PB: Begin completions, 6 items

class TestClass_BA : BaseA {
  #^CLASS_BA^#
}
// CLASS_BA: Begin completions, 5 items

class TestClass_BA_PA : BaseA, ProtocolA {
  #^CLASS_BA_PA^#
}
// CLASS_BA_PA: Begin completions, 9 items

class TestClass_BA_PB : BaseA, ProtocolB {
  #^CLASS_BA_PB^#
}
// CLASS_BA_PB: Begin completions, 11 items

class TestClass_BB : BaseB {
  #^CLASS_BB^#
}
// CLASS_BB: Begin completions, 5 items

class TestClass_BE : BaseE {
  #^CLASS_BE^#
}
// CLASS_BE: Begin completions, 4 items

class TestClass_BE_PA : BaseE, ProtocolA {
  #^CLASS_BE_PA^#
}
// CLASS_BE_PA: Begin completions, 8 items

class TestClass_BE_PA_PE : BaseE, ProtocolA, ProtocolE {
  #^CLASS_BE_PA_PE^#
}
// CLASS_BE_PA_PE: Begin completions, 8 items

class TestClass_PEI_PE : ProtocolEImpl, ProtocolE {
  #^CLASS_PEI_PE^#
}
// CLASS_PEI_PE: Begin completions, 2 items

class OuterNominal : ProtocolA {
  class Inner {
    #^NESTED_NOMINAL^#
  }
}
// NESTED_NOMINAL: found code completion token
// NESTED_NOMINAL-NOT: Begin completions

