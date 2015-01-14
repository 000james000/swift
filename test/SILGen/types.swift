// RUN: %swift -parse-as-library -emit-silgen %s | FileCheck %s

class C {
  var member: Int = 0

  // Methods have method calling convention.
  // CHECK-LABEL: sil hidden  @{{.*}}C3foo{{.*}} : $@cc(method) @thin (Int, @owned C) -> ()
  func foo(#x: Int) {
    // CHECK: bb0([[X:%[0-9]+]] : $Int, [[THIS:%[0-9]+]] : $C):
    member = x

    // CHECK: strong_retain %1 : $C
    // CHECK: [[FN:%[0-9]+]] = class_method %1 : $C, #C.member!setter.1
    // CHECK: apply [[FN]](%0, %1) : $@cc(method) @thin (Int, @owned C) -> ()
    // CHECK: strong_release %1 : $C


  }
}

struct S {
  var member: Int

  // CHECK-LABEL: sil hidden  @{{.*}}foo{{.*}} : $@cc(method) @thin (Int, @inout S) -> ()
  mutating
  func foo(var #x: Int) {
    // CHECK: bb0([[X:%[0-9]+]] : $Int, [[THIS:%[0-9]+]] : $*S):
    member = x
    // CHECK: [[XADDR:%[0-9]+]] = alloc_box $Int
    // CHECK: [[THIS_LOCAL:%[0-9]+]] = alloc_box $S
    // CHECK: [[MEMBER:%[0-9]+]] = struct_element_addr [[THIS_LOCAL]]#1 : $*S, #S.member
    // CHECK: copy_addr [[XADDR]]#1 to [[MEMBER]]
  }

  class SC {
    // CHECK-LABEL: sil hidden  @_TFCV5types1S2SC3barfS1_FT_T_
    func bar() {}
  }
}

func f() {
  class FC {
    // CHECK-LABEL: sil shared @_TFC5typesL33_92C453E47D5C73E45C8465DFD20D0D6B_2FC3zimfS0_FT_T_
    func zim() {}
  }
}

func g(#b : Bool) {
  if (b) {
    class FC {
      // CHECK-LABEL: sil shared @_TFC5typesL33_92C453E47D5C73E45C8465DFD20D0D6B0_2FC3zimfS0_FT_T_
      func zim() {}
    }
  } else {
    class FC {
      // CHECK-LABEL: sil shared @_TFC5typesL33_92C453E47D5C73E45C8465DFD20D0D6B1_2FC3zimfS0_FT_T_
      func zim() {}
    }
  }
}

struct ReferencedFromFunctionStruct {
  let f: ReferencedFromFunctionStruct -> () = {x in ()}
  let g: ReferencedFromFunctionEnum -> () = {x in ()}
}

enum ReferencedFromFunctionEnum {
  case f(ReferencedFromFunctionEnum -> ())
  case g(ReferencedFromFunctionStruct -> ())
}

// CHECK-LABEL: sil hidden @_TF5types34referencedFromFunctionStructFieldsFVS_28ReferencedFromFunctionStructTFS0_T_FOS_26ReferencedFromFunctionEnumT__
// CHECK:         [[F:%.*]] = struct_extract [[X:%.*]] : $ReferencedFromFunctionStruct, #ReferencedFromFunctionStruct.f
// CHECK:         [[F]] : $@callee_owned (@owned ReferencedFromFunctionStruct) -> ()
// CHECK:         [[G:%.*]] = struct_extract [[X]] : $ReferencedFromFunctionStruct, #ReferencedFromFunctionStruct.g
// CHECK:         [[G]] : $@callee_owned (@owned ReferencedFromFunctionEnum) -> ()
func referencedFromFunctionStructFields(x: ReferencedFromFunctionStruct)
    -> (ReferencedFromFunctionStruct -> (), ReferencedFromFunctionEnum -> ()) {
  return (x.f, x.g)
}

// CHECK-LABEL: sil hidden @_TF5types32referencedFromFunctionEnumFieldsFOS_26ReferencedFromFunctionEnumTGSqFS0_T__GSqFVS_28ReferencedFromFunctionStructT___
// CHECK:       bb{{[0-9]+}}([[F:%.*]] : $@callee_owned (@owned ReferencedFromFunctionEnum) -> ()):
// CHECK:       bb{{[0-9]+}}([[G:%.*]] : $@callee_owned (@owned ReferencedFromFunctionStruct) -> ()):
func referencedFromFunctionEnumFields(x: ReferencedFromFunctionEnum)
    -> (
      (ReferencedFromFunctionEnum -> ())?,
      (ReferencedFromFunctionStruct -> ())?
    ) {
  switch x {
  case .f(let f):
    return (f, nil)
  case .g(let g):
    return (nil, g)
  }
}
