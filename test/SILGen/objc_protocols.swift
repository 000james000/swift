// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -module-cache-path %t/clang-module-cache -target x86_64-apple-macosx10.9 -sdk %S/Inputs -I %S/Inputs -enable-source-import %s -emit-silgen | FileCheck %s

import gizmo
import objc_protocols_Bas

@objc protocol NSRuncing {
  func runce() -> NSObject
  func copyRuncing() -> NSObject

  func foo()
}

@objc protocol NSFunging {
  func funge()

  func foo()
}

protocol Ansible {
  func anse()
}

// CHECK-LABEL: sil  @_TF14objc_protocols12objc_generic
func objc_generic<T : NSRuncing>(x: T) -> (NSObject, NSObject) {
  return (x.runce(), x.copyRuncing())
  // -- Result of runce is retain_autoreleased according to default objc conv
  // CHECK: [[METHOD:%.*]] = witness_method [volatile] {{\$.*}},  #NSRuncing.runce!1.foreign
  // CHECK: [[RESULT1:%.*]] = apply [[METHOD]]<T>([[THIS1:%.*]]) : $@cc(objc_method) @thin <τ_0_0 where τ_0_0 : NSRuncing> (τ_0_0) -> @autoreleased NSObject
  // CHECK: retain_autoreleased [[RESULT1]] : $NSObject

  // -- Result of copyRuncing is received retained according to -copy family
  // CHECK: [[METHOD:%.*]] = witness_method [volatile] {{\$.*}},  #NSRuncing.copyRuncing!1.foreign
  // CHECK: [[RESULT2:%.*]] = apply [[METHOD]]<T>([[THIS2:%.*]]) : $@cc(objc_method) @thin <τ_0_0 where τ_0_0 : NSRuncing> (τ_0_0) -> @owned NSObject
  // CHECK-NOT: retain_autoreleased

  // -- Arguments are not consumed by objc calls
  // CHECK: release [[THIS2]]
}

// CHECK-LABEL: sil  @_TF14objc_protocols13objc_protocol
func objc_protocol(x: NSRuncing) -> (NSObject, NSObject) {
  return (x.runce(), x.copyRuncing())
  // -- Result of runce is retain_autoreleased according to default objc conv
  // CHECK: [[THIS1:%.*]] = project_existential_ref [[THIS1_ORIG:%.*]] :
  // CHECK: [[METHOD:%.*]] = protocol_method [volatile] [[THIS1_ORIG]] : {{.*}}, #NSRuncing.runce!1.foreign
  // CHECK: [[RESULT1:%.*]] = apply [[METHOD]]([[THIS1]]) : $@cc(objc_method) @thin (@sil_self NSRuncing) -> @autoreleased NSObject
  // CHECK: retain_autoreleased [[RESULT1]] : $NSObject

  // -- Result of copyRuncing is received retained according to -copy family
  // CHECK: [[THIS2:%.*]] = project_existential_ref [[THIS2_ORIG:%.*]] :
  // CHECK: [[METHOD:%.*]] = protocol_method [volatile] [[THIS2_ORIG]] : {{.*}}, #NSRuncing.copyRuncing!1.foreign
  // CHECK: [[RESULT2:%.*]] = apply [[METHOD]]([[THIS2:%.*]]) : $@cc(objc_method) @thin (@sil_self NSRuncing) -> @owned NSObject
  // CHECK-NOT: retain_autoreleased

  // -- Arguments are not consumed by objc calls
  // CHECK: release [[THIS2_ORIG]]
}

// CHECK-LABEL: sil  @_TF14objc_protocols25objc_protocol_composition
func objc_protocol_composition(x: protocol<NSRuncing, NSFunging>) {
  // CHECK: [[THIS:%.*]] = project_existential_ref [[THIS_ORIG:%.*]] : $protocol<NSFunging, NSRuncing>
  // CHECK: [[METHOD:%.*]] = protocol_method [volatile] [[THIS_ORIG]] : {{.*}}, #NSRuncing.runce!1.foreign
  // CHECK: apply [[METHOD]]([[THIS]]) : $@cc(objc_method) @thin (@sil_self NSRuncing) -> @autoreleased NSObject
  x.runce()

  // CHECK: [[THIS:%.*]] = project_existential_ref [[THIS_ORIG:%.*]] : $protocol<NSFunging, NSRuncing>
  // CHECK: [[METHOD:%.*]] = protocol_method [volatile] [[THIS_ORIG]] : {{.*}}, #NSFunging.funge!1.foreign
  // CHECK: apply [[METHOD]]([[THIS]]) : $@cc(objc_method) @thin (@sil_self NSFunging) -> ()
  x.funge()
}
// -- ObjC thunks get emitted for ObjC protocol conformances

class Foo : NSRuncing, NSFunging, Ansible {
  // -- NSRuncing
  func runce() -> NSObject { return NSObject() }
  func copyRuncing() -> NSObject { return NSObject() }

  // -- NSFunging
  func funge() {}

  // -- Both NSRuncing and NSFunging
  func foo() {}

  // -- Ansible
  func anse() {}
}

// CHECK-LABEL: sil  @_TToFC14objc_protocols3Foo5runcefS0_FT_CSo8NSObject
// CHECK-LABEL: sil  @_TToFC14objc_protocols3Foo11copyRuncingfS0_FT_CSo8NSObject
// CHECK-LABEL: sil  @_TToFC14objc_protocols3Foo5fungefS0_FT_T_
// CHECK-LABEL: sil  @_TToFC14objc_protocols3Foo3foofS0_FT_T_
// CHECK-NOT: sil @_TToF{{.*}}anse{{.*}}

class Bar { }

extension Bar : NSRuncing {
  func runce() -> NSObject { return NSObject() }
  func copyRuncing() -> NSObject { return NSObject() }
  func foo() {}
}

// CHECK-LABEL: sil  @_TToFC14objc_protocols3Bar5runcefS0_FT_CSo8NSObject
// CHECK-LABEL: sil  @_TToFC14objc_protocols3Bar11copyRuncingfS0_FT_CSo8NSObject
// CHECK-LABEL: sil  @_TToFC14objc_protocols3Bar3foofS0_FT_T_

// class Bas from objc_protocols_Bas module
extension Bas : NSRuncing {
  // runce() implementation from the original definition of Bas
  func copyRuncing() -> NSObject { return NSObject() }
  func foo() {}
}

// CHECK-LABEL: sil  @_TToFE14objc_protocolsC18objc_protocols_Bas3Bas11copyRuncingfS1_FT_CSo8NSObject
// CHECK-LABEL: sil  @_TToFE14objc_protocolsC18objc_protocols_Bas3Bas3foofS1_FT_T_

// -- Inherited objc protocols

protocol Fungible : NSFunging { }

class Zim : Fungible {
  func funge() {}
  func foo() {}
}

// CHECK-LABEL: sil  @_TToFC14objc_protocols3Zim5fungefS0_FT_T_
// CHECK-LABEL: sil  @_TToFC14objc_protocols3Zim3foofS0_FT_T_

// class Zang from objc_protocols_Bas module
extension Zang : Fungible {
  // funge() implementation from the original definition of Zim
  func foo() {}
}

// CHECK-LABEL: sil  @_TToFE14objc_protocolsC18objc_protocols_Bas4Zang3foofS1_FT_T_

// -- objc protocols with property requirements in extensions
//    <rdar://problem/16284574>

@objc protocol NSCounting {
  var count: Int {get}
}

class StoredPropertyCount {
  let count = 0
}

extension StoredPropertyCount: NSCounting {}
// CHECK-LABEL: sil [transparent] @_TToFC14objc_protocols19StoredPropertyCountg5countSi

class ComputedPropertyCount {
  var count: Int { return 0 }
}

extension ComputedPropertyCount: NSCounting {}
// CHECK-LABEL: sil @_TToFC14objc_protocols21ComputedPropertyCountg5countSi

// -- adding @objc protocol conformances to native ObjC classes should not
//    emit thunks since the methods are already available to ObjC.

// Gizmo declared in Inputs/usr/include/Gizmo.h
extension Gizmo : NSFunging { }

// CHECK-NOT: _TTo{{.*}}5Gizmo{{.*}}

@objc class InformallyFunging {
  @objc func funge() {}
  @objc func foo() {}
}

extension InformallyFunging: NSFunging { }

@objc protocol Initializable {
  init(int: Int)
}

// CHECK-LABEL: sil @_TF14objc_protocols28testInitializableExistential
func testInitializableExistential(im: Initializable.Type, i: Int) -> Initializable {
  // CHECK: bb0([[META:%[0-9]+]] : $@thick Initializable.Type, [[I:%[0-9]+]] : $Int):
// CHECK:   [[I2_BOX:%[0-9]+]] = alloc_box $Initializable
// CHECK:   [[ARCHETYPE_META:%[0-9]+]] = open_existential_ref [[META]] : $@thick Initializable.Type to $@thick @opened(0) Initializable.Type
// CHECK:   [[ARCHETYPE_META_OBJC:%[0-9]+]] = thick_to_objc_metatype [[ARCHETYPE_META]] : $@thick @opened(0) Initializable.Type to $@objc_metatype @opened(0) Initializable.Type
// CHECK:   [[I2_ALLOC:%[0-9]+]] = alloc_ref_dynamic [objc] [[ARCHETYPE_META_OBJC]] : $@objc_metatype @opened(0) Initializable.Type, $@opened(0) Initializable
// CHECK:   [[INIT_WITNESS:%[0-9]+]] = witness_method [volatile] $@opened(0) Initializable, #Initializable.init!initializer.1.foreign : $@cc(objc_method) @thin <τ_0_0 where τ_0_0 : Initializable> (Int, @owned τ_0_0) -> @owned τ_0_0
// CHECK:   [[I2:%[0-9]+]] = apply [[INIT_WITNESS]]<@opened(0) Initializable>([[I]], [[I2_ALLOC]]) : $@cc(objc_method) @thin <τ_0_0 where τ_0_0 : Initializable> (Int, @owned τ_0_0) -> @owned τ_0_0
// CHECK:   [[I2_EXIST_CONTAINER:%[0-9]+]] = init_existential_ref [[I2]] : $@opened(0) Initializable : $@opened(0) Initializable, $Initializable
// CHECK:   store [[I2_EXIST_CONTAINER]] to [[I2_BOX]]#1 : $*Initializable
// CHECK:   [[I2:%[0-9]+]] = load [[I2_BOX]]#1 : $*Initializable
// CHECK:   strong_retain [[I2]] : $Initializable
// CHECK:   strong_release [[I2_BOX]]#0 : $Builtin.NativeObject
// CHECK:   return [[I2]] : $Initializable
  var i2 = im(int: i)
  return i2
}

class InitializableConformer: Initializable {
  required init(int: Int) {}
}
// CHECK-LABEL: sil @_TToFC14objc_protocols22InitializableConformercfMS0_FT3intSi_S0_

final class InitializableConformerByExtension {
  init() {}
}

extension InitializableConformerByExtension: Initializable {
  convenience init(int: Int) {
    self.init()
  }
}
// CHECK-LABEL: sil @_TToFC14objc_protocols33InitializableConformerByExtensioncfMS0_FT3intSi_S0_
