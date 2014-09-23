// RUN: %swift -emit-silgen %s | FileCheck %s

protocol P {}
class C: P {}

struct LoadableStruct {
  var x: C

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers14LoadableStructCfMS0_FT10alwaysFailCS_1C_GSqS0__
  // CHECK:       bb0([[C:%.*]] : $C
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $LoadableStruct
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         br [[FAIL:bb[0-9]+]]
  // CHECK:       [[FAIL]]:
  // CHECK:         strong_release [[C]]
  // CHECK:         [[NIL:%.*]] = enum $Optional<LoadableStruct>, #Optional.None!enumelt
  // CHECK:         br [[EXIT:bb[0-9]+]]([[NIL]] : $Optional<LoadableStruct>)
  // CHECK:       [[EXIT]]([[RESULT:%.*]] : $Optional<LoadableStruct>):
  // CHECK:         return [[RESULT]]
  init?(alwaysFail: C) {
    return nil
  }

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers14LoadableStructCfMS0_FT3optSb_GSqS0__
  init?(opt: Bool) {
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $LoadableStruct
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [rootself] [[SELF_BOX]]#1
    x = C()
  // CHECK:       [[FAILURE:bb[0-9]+]]:
  // CHECK:         [[NIL:%.*]] = enum $Optional<LoadableStruct>, #Optional.None
  // CHECK:         br [[EXIT:bb.*]]([[NIL]] : $Optional<LoadableStruct>)
  // CHECK:       [[EXIT]]([[RESULT:%.*]] : $Optional<LoadableStruct>):
  // CHECK:         return [[RESULT]]

  // CHECK:       bb{{.*}}:
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         br [[FAILURE]]
    if opt {
      return nil
    }

  // CHECK:       bb{{.*}}:
  // CHECK:         [[SELF:%.*]] = load [[SELF_MARKED]]
  // CHECK:         [[SELF_OPT:%.*]] = enum $Optional<LoadableStruct>, #Optional.Some!enumelt.1, [[SELF]]
  // CHECK:         br [[EXIT]]([[SELF_OPT]] : $Optional<LoadableStruct>)

  }

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers14LoadableStructCfMS0_FT3iuoSb_GSQS0__
  init!(iuo: Bool) {
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $LoadableStruct
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [rootself] [[SELF_BOX]]#1
    x = C()
  // CHECK:       [[FAILURE:bb[0-9]+]]:
  // CHECK:         [[NIL:%.*]] = enum $ImplicitlyUnwrappedOptional<LoadableStruct>, #ImplicitlyUnwrappedOptional.None
  // CHECK:         br [[EXIT:bb.*]]([[NIL]] : $ImplicitlyUnwrappedOptional<LoadableStruct>)
  // CHECK:       [[EXIT]]([[RESULT:%.*]] : $ImplicitlyUnwrappedOptional<LoadableStruct>):
  // CHECK:         return [[RESULT]]

  // CHECK:       bb{{.*}}:
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         br [[FAILURE]]
    if iuo {
      return nil
    }

  // CHECK:       bb{{.*}}:
  // CHECK:         [[SELF:%.*]] = load [[SELF_MARKED]]
  // CHECK:         [[SELF_OPT:%.*]] = enum $ImplicitlyUnwrappedOptional<LoadableStruct>, #ImplicitlyUnwrappedOptional.Some!enumelt.1, [[SELF]]
  // CHECK:         br [[EXIT]]([[SELF_OPT]] : $ImplicitlyUnwrappedOptional<LoadableStruct>)

  }

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers14LoadableStructCfMS0_FT15delegatesOptOptSb_GSqS0__
  init?(delegatesOptOpt: Bool) {
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $LoadableStruct
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]]#1
  // CHECK:         [[DELEGATEE_INIT:%.*]] = function_ref @_TFV21failable_initializers14LoadableStructCfMS0_FT3optSb_GSqS0__
  // CHECK:         [[DELEGATEE_SELF:%.*]] = apply [[DELEGATEE_INIT]]
  // CHECK:         [[DELEGATEE_SELF_MAT:%.*]] = alloc_stack $Optional<LoadableStruct>
  // CHECK:         store [[DELEGATEE_SELF]] to [[DELEGATEE_SELF_MAT]]
  // CHECK:         [[HAS_VALUE_FN:%.*]] = function_ref @_TFSs22_doesOptionalHaveValueU__FRGSqQ__Bi1_
  // CHECK:         [[HAS_VALUE:%.*]] = apply [transparent] [[HAS_VALUE_FN]]<LoadableStruct>([[DELEGATEE_SELF_MAT]]#1)
  // CHECK:         cond_br [[HAS_VALUE]], [[DOES_HAVE_VALUE:bb[0-9]+]], [[DOESNT_HAVE_VALUE:bb[0-9]+]]
  // -- TODO: failure
  // CHECK:       [[FAILURE:bb[0-9]+]]:
  // CHECK:         [[NIL:%.*]] = enum $Optional<LoadableStruct>, #Optional.None
  // CHECK:         br [[EXIT:bb.*]]([[NIL]] : $Optional<LoadableStruct>)
  // CHECK:       [[DOESNT_HAVE_VALUE]]:
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         destroy_addr [[DELEGATEE_SELF_MAT]]
  // CHECK:         dealloc_stack [[DELEGATEE_SELF_MAT]]
  // CHECK:         br [[FAILURE]]
  // CHECK:       [[DOES_HAVE_VALUE]]:
  // CHECK:         [[GET_VALUE_FN:%.*]] = function_ref @_TFSs17_getOptionalValueU__FGSqQ__Q_
  // CHECK:         [[TMP:%.*]] = alloc_stack $LoadableStruct
  // CHECK:         apply [transparent] [[GET_VALUE_FN]]<LoadableStruct>([[TMP]]#1, [[DELEGATEE_SELF_MAT]]#1)
  // CHECK:         [[DELEGATEE_SELF_VAL:%.*]] = load [[TMP]]
  // CHECK:         assign [[DELEGATEE_SELF_VAL]] to [[SELF_MARKED]]
  // CHECK:         dealloc_stack [[TMP]]
  // CHECK:         dealloc_stack [[DELEGATEE_SELF_MAT]]
    self.init(opt: true)
  }

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers14LoadableStructCfMS0_FT16delegatesNormIUOSb_S0_
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $LoadableStruct
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]]#1
  // CHECK:         [[DELEGATEE_INIT:%.*]] = function_ref @_TFV21failable_initializers14LoadableStructCfMS0_FT3iuoSb_GSQS0__
  // CHECK:         [[DELEGATEE_SELF:%.*]] = apply [[DELEGATEE_INIT]]
  // CHECK:         [[DELEGATEE_SELF_MAT:%.*]] = alloc_stack $ImplicitlyUnwrappedOptional<LoadableStruct>
  // CHECK:         store [[DELEGATEE_SELF]] to [[DELEGATEE_SELF_MAT]]
  // CHECK:         [[GET_VALUE_FN:%.*]] = function_ref @_TFSs36_getImplicitlyUnwrappedOptionalValueU__FGSQQ__Q_
  // CHECK:         [[TMP:%.*]] = alloc_stack $LoadableStruct
  // CHECK:         apply [transparent] [[GET_VALUE_FN]]<LoadableStruct>([[TMP]]#1, [[DELEGATEE_SELF_MAT]]#1)
  // CHECK:         [[DELEGATEE_SELF_VAL:%.*]] = load [[TMP]]
  // CHECK:         assign [[DELEGATEE_SELF_VAL]] to [[SELF_MARKED]]
  // CHECK:         dealloc_stack [[TMP]]
  // CHECK:         dealloc_stack [[DELEGATEE_SELF_MAT]]
  init(delegatesNormIUO: Bool) {
    self.init(iuo: true)
  }
}

struct AddressOnlyStruct {
  var x: P

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers17AddressOnlyStructCfMS0_FT3optSb_GSqS0__
  init?(opt: Bool) {
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $AddressOnlyStruct
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [rootself] [[SELF_BOX]]#1
    x = C()
  // CHECK:       [[FAILURE:bb[0-9]+]]:
  // CHECK:         inject_enum_addr %0 : $*Optional<AddressOnlyStruct>, #Optional.None!enumelt
  // CHECK:         br [[EXIT:bb[0-9]+]]
  // CHECK:       [[EXIT]]:
  // CHECK:         return
  // CHECK:       bb{{.*}}:
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         br [[FAILURE]]
    if opt {
      return nil
    }

  // CHECK:       bb{{.*}}:
  // CHECK:         [[DEST_PAYLOAD:%.*]] = init_enum_data_addr %0 : $*Optional<AddressOnlyStruct>, #Optional.Some
  // CHECK:         copy_addr [[SELF_MARKED]] to [initialization] [[DEST_PAYLOAD]]
  // CHECK:         inject_enum_addr %0 : $*Optional<AddressOnlyStruct>, #Optional.Some
  // CHECK:         br [[EXIT]]
  }

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers17AddressOnlyStructCfMS0_FT3iuoSb_GSQS0__
  init!(iuo: Bool) {
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $AddressOnlyStruct
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [rootself] [[SELF_BOX]]#1
    x = C()
  // CHECK:       [[FAILURE:bb[0-9]+]]:
  // CHECK:         inject_enum_addr %0 : $*ImplicitlyUnwrappedOptional<AddressOnlyStruct>, #ImplicitlyUnwrappedOptional.None!enumelt
  // CHECK:         br [[EXIT:bb[0-9]+]]
  // CHECK:       bb{{.*}}:
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         br [[FAILURE]]
    if iuo {
      return nil
    }

  // CHECK:       bb{{.*}}:
  // CHECK:         [[DEST_PAYLOAD:%.*]] = init_enum_data_addr %0 : $*ImplicitlyUnwrappedOptional<AddressOnlyStruct>, #ImplicitlyUnwrappedOptional.Some
  // CHECK:         copy_addr [[SELF_MARKED]] to [initialization] [[DEST_PAYLOAD]]
  // CHECK:         inject_enum_addr %0 : $*ImplicitlyUnwrappedOptional<AddressOnlyStruct>, #ImplicitlyUnwrappedOptional.Some
  }

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers17AddressOnlyStructCfMS0_FT15delegatesOptOptSb_GSqS0__
  init?(delegatesOptOpt: Bool) {
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $AddressOnlyStruct
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]]#1
  // CHECK:         [[DELEGATEE_INIT:%.*]] = function_ref @_TFV21failable_initializers17AddressOnlyStructCfMS0_FT3optSb_GSqS0__
  // CHECK:         [[DELEGATEE_SELF:%.*]] = alloc_stack $Optional<AddressOnlyStruct>
  // CHECK:         apply [[DELEGATEE_INIT]]([[DELEGATEE_SELF]]
  // CHECK:         [[HAS_VALUE_FN:%.*]] = function_ref @_TFSs22_doesOptionalHaveValueU__FRGSqQ__Bi1_
  // CHECK:         [[HAS_VALUE:%.*]] = apply [transparent] [[HAS_VALUE_FN]]<AddressOnlyStruct>([[DELEGATEE_SELF]]#1)
  // CHECK:         cond_br [[HAS_VALUE]], [[DOES_HAVE_VALUE:bb[0-9]+]], [[DOESNT_HAVE_VALUE:bb[0-9]+]]
  // -- TODO: failure
  // CHECK:       [[FAILURE:bb[0-9]+]]:
  // CHECK:         inject_enum_addr %0 : $*Optional<AddressOnlyStruct>, #Optional.None!enumelt
  // CHECK:         br [[EXIT:bb[0-9]+]]
  // CHECK:       [[DOESNT_HAVE_VALUE]]:
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         destroy_addr [[DELEGATEE_SELF]]
  // CHECK:         dealloc_stack [[DELEGATEE_SELF]]
  // CHECK:         br [[FAILURE]]
  // CHECK:       [[DOES_HAVE_VALUE]]:
  // CHECK:         [[GET_VALUE_FN:%.*]] = function_ref @_TFSs17_getOptionalValueU__FGSqQ__Q_
  // CHECK:         [[TMP:%.*]] = alloc_stack $AddressOnlyStruct
  // CHECK:         apply [transparent] [[GET_VALUE_FN]]<AddressOnlyStruct>([[TMP]]#1, [[DELEGATEE_SELF]]#1)
  // CHECK:         copy_addr [take] [[TMP]]#1 to [[SELF_MARKED]]
  // CHECK:         dealloc_stack [[TMP]]
  // CHECK:         dealloc_stack [[DELEGATEE_SELF]]
    self.init(opt: true)
  }

  // CHECK-LABEL: sil hidden @_TFV21failable_initializers17AddressOnlyStructCfMS0_FT16delegatesNormIUOSb_S0_
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $AddressOnlyStruct
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]]#1
  // CHECK:         [[DELEGATEE_INIT:%.*]] = function_ref @_TFV21failable_initializers17AddressOnlyStructCfMS0_FT3iuoSb_GSQS0__
  // CHECK:         [[DELEGATEE_SELF:%.*]] = alloc_stack $ImplicitlyUnwrappedOptional<AddressOnlyStruct>
  // CHECK:         apply [[DELEGATEE_INIT]]([[DELEGATEE_SELF]]
  // CHECK:         [[GET_VALUE_FN:%.*]] = function_ref @_TFSs36_getImplicitlyUnwrappedOptionalValueU__FGSQQ__Q_
  // CHECK:         [[TMP:%.*]] = alloc_stack $AddressOnlyStruct
  // CHECK:         apply [transparent] [[GET_VALUE_FN]]<AddressOnlyStruct>([[TMP]]#1, [[DELEGATEE_SELF]]#1)
  // CHECK:         copy_addr [take] [[TMP]]#1 to [[SELF_MARKED]]
  // CHECK:         dealloc_stack [[TMP]]
  // CHECK:         dealloc_stack [[DELEGATEE_SELF]]
  init(delegatesNormIUO: Bool) {
    self.init(iuo: true)
  }
}

class RootClass {
  var x: C

  init(norm: Bool) {
    x = C()
  }

  init?(alwaysFail: Void) {
    return nil
  }

  // CHECK-LABEL: sil hidden @_TFC21failable_initializers9RootClasscfMS0_FT3optSb_GSqS0__
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [rootself]
  init?(opt: Bool) {
    x = C()
  // CHECK:         cond_br {{%.*}}, [[YES:bb[0-9]+]], [[NO:bb[0-9]+]]

  // CHECK:       [[FAILURE:bb[0-9]+]]:
  // CHECK:         [[NIL:%.*]] = enum $Optional<RootClass>, #Optional.None!enumelt
  // CHECK:         br [[EXIT:bb[0-9]+]]([[NIL]] : $Optional<RootClass>)
  // CHECK:       [[EXIT]]([[RESULT:%.*]] : $Optional<RootClass>):
  // CHECK:         return [[RESULT]]

    if opt {
  // CHECK:       [[YES:bb[0-9]+]]:
  // CHECK:         strong_release [[SELF_MARKED]]
  // CHECK:         br [[FAILURE]]
      return nil
    }

  // CHECK:       [[NO:bb[0-9]+]]:
  // CHECK:         [[SOME:%.*]] = enum $Optional<RootClass>, #Optional.Some!enumelt.1, [[SELF_MARKED]]
  // CHECK:         br [[EXIT]]([[SOME]] : $Optional<RootClass>)
  }

  init!(iuo: Bool) {
    x = C()
    if iuo {
      return nil
    }
  }

  // CHECK-LABEL: sil hidden @_TFC21failable_initializers9RootClasscfMS0_FT16delegatesOptNormSb_GSqS0__
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $RootClass
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [delegatingself]
  // CHECK:         store [[SELF_MARKED]] to [[SELF_BOX]]
  // CHECK:         [[SELF_TAKEN:%.*]] = load [[SELF_BOX]]
  // CHECK:         [[INIT:%.*]] = class_method [[SELF_TAKEN]] : $RootClass, #RootClass.init
  // CHECK:         [[NEW_SELF:%.*]] = apply [[INIT]]({{.*}}, [[SELF_TAKEN]])
  // CHECK:         store [[NEW_SELF]] to [[SELF_BOX]]

  // CHECK:         [[RESULT_SELF:%.*]] = load [[SELF_BOX]]
  // CHECK:         strong_retain [[RESULT_SELF]]
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         [[SOME:%.*]] = enum $Optional<RootClass>, #Optional.Some!enumelt.1, [[RESULT_SELF]]
  // CHECK:         br [[EXIT:bb[0-9]+]]([[SOME]] : $Optional<RootClass>)
  
  // CHECK:       [[EXIT]]([[RESULT:%.*]] : $Optional<RootClass>):
  // CHECK:         return [[RESULT]]
  convenience init?(delegatesOptNorm: Bool) {
    self.init(norm: true)
  }

  // CHECK-LABEL: sil hidden @_TFC21failable_initializers9RootClasscfMS0_FT15delegatesOptOptSb_GSqS0__
  // CHECK:         [[SELF_BOX:%.*]] = alloc_box $RootClass
  // CHECK:         [[SELF_MARKED:%.*]] = mark_uninitialized [delegatingself]
  // CHECK:         store [[SELF_MARKED]] to [[SELF_BOX]]
  // CHECK:         [[SELF_TAKEN:%.*]] = load [[SELF_BOX]]
  // CHECK:         [[INIT:%.*]] = class_method [[SELF_TAKEN]] : $RootClass, #RootClass.init
  // CHECK:         [[NEW_SELF_OPT:%.*]] = apply [[INIT]]({{.*}}, [[SELF_TAKEN]])
  // CHECK:         [[NEW_SELF_OPT_MAT:%.*]] = alloc_stack $Optional<RootClass>
  // CHECK:         store [[NEW_SELF_OPT]] to [[NEW_SELF_OPT_MAT]]
  // CHECK:         [[DOES_OPT_HAVE_VALUE:%.*]] = function_ref @_TFSs22_doesOptionalHaveValueU__FRGSqQ__Bi1_
  // CHECK:         [[HAS_VALUE:%.*]] = apply [transparent] [[DOES_OPT_HAVE_VALUE]]<RootClass>([[NEW_SELF_OPT_MAT]]#1)
  // CHECK:         cond_br [[HAS_VALUE]], [[HAS_VALUE:bb[0-9]+]], [[NO_VALUE:bb[0-9]+]]

  // CHECK:       [[FAILURE:bb[0-9]+]]:
  // CHECK:         [[NIL:%.*]] = enum $Optional<RootClass>, #Optional.None!enumelt
  // CHECK:         br [[EXIT:bb[0-9]+]]([[NIL]] : $Optional<RootClass>)
  // CHECK:       [[EXIT]]([[RESULT:%.*]] : $Optional<RootClass>):
  // CHECK:         return [[RESULT]]

  // CHECK:       [[NO_VALUE]]:
  // CHECK:          dealloc_box $RootClass, [[SELF_BOX]]
  // CHECK:          destroy_addr [[NEW_SELF_OPT_MAT]]
  // CHECK:          dealloc_stack [[NEW_SELF_OPT_MAT]]
  // CHECK:          br [[FAILURE]]

  // CHECK:       [[HAS_VALUE]]:
  // CHECK:         [[GET_OPTIONAL_VALUE:%.*]] = function_ref @_TFSs17_getOptionalValueU__FGSqQ__Q_
  // CHECK:         [[TMP:%.*]] = alloc_stack $RootClass
  // CHECK:         apply [transparent] [[GET_OPTIONAL_VALUE]]<RootClass>([[TMP]]#1, [[NEW_SELF_OPT_MAT]]#1)
  // CHECK:         [[NEW_SELF:%.*]] = load [[TMP]]
  // CHECK:         store [[NEW_SELF]] to [[SELF_BOX]]
  // CHECK:         dealloc_stack [[TMP]]
  // CHECK:         dealloc_stack [[NEW_SELF_OPT_MAT]]

  // CHECK:         [[SELF_RESULT:%.*]] = load [[SELF_BOX]]
  // CHECK:         strong_retain [[SELF_RESULT]]
  // CHECK:         strong_release [[SELF_BOX]]
  // CHECK:         [[SOME:%.*]] = enum $Optional<RootClass>, #Optional.Some!enumelt.1, [[SELF_RESULT]]
  // CHECK:         br [[EXIT]]([[SOME]] : $Optional<RootClass>)
  convenience init?(delegatesOptOpt: Bool) {
    self.init(opt: true)
  }

  convenience init(delegatesNormIUO: Bool) {
    self.init(iuo: true)
  }
}

class SubClass: RootClass {
  var y: C

  init(normInheritNorm: Bool) {
    y = C()
    super.init(norm: true)
  }

  init(normInheritIUO: Bool) {
    y = C()
    super.init(iuo: true)
  }

  init?(optInheritNorm: Bool) {
    y = C()
    super.init(norm: true)
    if optInheritNorm {
      return nil
    }
  }

  init?(optInheritOpt: Bool) {
    y = C()
    super.init(opt: true)
    if optInheritOpt {
      return nil
    }
  }
}
