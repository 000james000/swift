// RUN: %swift -emit-silgen %s | FileCheck %s

struct S {}
struct R {}

protocol ReturnsMetatype {
  typealias Assoc
  mutating
  func getAssocMetatype() -> Assoc.Type
}

struct ConformsToReturnsMetatype : ReturnsMetatype {
  // CHECK-LABEL: sil hidden @_TTWV18result_abstraction25ConformsToReturnsMetatypeS_15ReturnsMetatypeS_FS1_16getAssocMetatypeUS1__U__fRQPS1_FT_MQS2_5Assoc : $@cc(witness_method) @thin (@inout ConformsToReturnsMetatype) -> @thick S.Type
  // CHECK:         function_ref @_TFV18result_abstraction25ConformsToReturnsMetatype16getAssocMetatypefRS0_FT_MVS_1S : $@cc(method) @thin (@inout ConformsToReturnsMetatype) -> @thin S.Type
  mutating
  func getAssocMetatype() -> S.Type {
    return S.self
  }
}

protocol ReturnsFunction {
  typealias Arg
  typealias Result
  func getFunc() -> Arg -> Result
}

struct ConformsToReturnsFunction : ReturnsFunction {
  // CHECK-LABEL: sil hidden @_TTWV18result_abstraction25ConformsToReturnsFunctionS_15ReturnsFunctionS_FS1_7getFuncUS1__U___fRQPS1_FT_FQS2_3ArgQS2_6Result : $@cc(witness_method) @thin (@inout ConformsToReturnsFunction) -> @owned @callee_owned (@out R, @in S) -> ()
  // CHECK:         function_ref @_TTRXFo_dV18result_abstraction1S_dVS_1R_XFo_iS0__iS1__ : $@thin (@out R, @in S, @owned @callee_owned (S) -> R) -> ()
  func getFunc() -> S -> R {
    return {s in R()}
  }
}

protocol ReturnsAssoc {
  typealias Assoc
  mutating
  func getAssoc() -> Assoc
}

struct ConformsToReturnsAssocWithMetatype : ReturnsAssoc {
  typealias Assoc = S.Type
  // CHECK-LABEL: sil hidden @_TTWV18result_abstraction34ConformsToReturnsAssocWithMetatypeS_12ReturnsAssocS_FS1_8getAssocUS1__U__fRQPS1_FT_QS2_5Assoc : $@cc(witness_method) @thin (@out @thick S.Type, @inout ConformsToReturnsAssocWithMetatype) -> ()
  // CHECK:         function_ref @_TFV18result_abstraction34ConformsToReturnsAssocWithMetatype8getAssocfRS0_FT_MVS_1S : $@cc(method) @thin (@inout ConformsToReturnsAssocWithMetatype) -> @thin S.Type
  mutating
  func getAssoc() -> S.Type {
    return S.self
  }
}

struct ConformsToReturnsAssocWithFunction : ReturnsAssoc {
  typealias Assoc = S -> R
  // CHECK-LABEL: sil hidden @_TTWV18result_abstraction34ConformsToReturnsAssocWithFunctionS_12ReturnsAssocS_FS1_8getAssocUS1__U__fRQPS1_FT_QS2_5Assoc : $@cc(witness_method) @thin (@out @callee_owned (@out R, @in S) -> (), @inout ConformsToReturnsAssocWithFunction) -> ()
  // CHECK:         function_ref @_TFV18result_abstraction34ConformsToReturnsAssocWithFunction8getAssocfRS0_FT_FVS_1SVS_1R : $@cc(method) @thin (@inout ConformsToReturnsAssocWithFunction) -> @owned @callee_owned (S) -> R
  mutating
  func getAssoc() -> S -> R {
    return {s in R()}
  }
}
