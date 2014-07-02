// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -import-cf-types -module-cache-path %t/clang-module-cache -sdk %S/Inputs -target x86_64-apple-macosx10.9 %s -emit-silgen -o - | FileCheck %s

import CoreCooling

// CHECK: sil @_TF2cf8useEmAllFCSo16CCMagnetismModelT_ :
func useEmAll(model: CCMagnetismModel) {
// CHECK: function_ref @CCPowerSupplyGetDefault : $@cc(cdecl) @thin () -> @autoreleased ImplicitlyUnwrappedOptional<CCPowerSupply>
  let power = CCPowerSupplyGetDefault()

// CHECK: function_ref @CCRefrigeratorCreate : $@cc(cdecl) @thin (ImplicitlyUnwrappedOptional<CCPowerSupply>) -> ImplicitlyUnwrappedOptional<Unmanaged<CCRefrigerator>>
  let unmanagedFridge = CCRefrigeratorCreate(power)

// CHECK: function_ref @CCRefrigeratorSpawn : $@cc(cdecl) @thin (ImplicitlyUnwrappedOptional<CCPowerSupply>) -> @owned ImplicitlyUnwrappedOptional<CCRefrigerator>
  let managedFridge = CCRefrigeratorSpawn(power)

// CHECK: function_ref @CCRefrigeratorOpen : $@cc(cdecl) @thin (ImplicitlyUnwrappedOptional<CCRefrigerator>) -> ()
  CCRefrigeratorOpen(managedFridge)

// CHECK: function_ref @CCRefrigeratorCopy : $@cc(cdecl) @thin (ImplicitlyUnwrappedOptional<CCRefrigerator>) -> @owned ImplicitlyUnwrappedOptional<CCRefrigerator>
  let copy = CCRefrigeratorCopy(managedFridge)

// CHECK: function_ref @CCRefrigeratorClone : $@cc(cdecl) @thin (ImplicitlyUnwrappedOptional<CCRefrigerator>) -> @autoreleased ImplicitlyUnwrappedOptional<CCRefrigerator>
  let clone = CCRefrigeratorClone(managedFridge)

// CHECK: function_ref @CCRefrigeratorDestroy : $@cc(cdecl) @thin (@owned ImplicitlyUnwrappedOptional<CCRefrigerator>) -> ()
  CCRefrigeratorDestroy(clone)

// CHECK: class_method [volatile] %0 : $CCMagnetismModel, #CCMagnetismModel.refrigerator!1.foreign : CCMagnetismModel -> () -> Unmanaged<CCRefrigerator>! , $@cc(objc_method) @thin (CCMagnetismModel) -> ImplicitlyUnwrappedOptional<Unmanaged<CCRefrigerator>>
  let f0 = model.refrigerator()

// CHECK: class_method [volatile] %0 : $CCMagnetismModel, #CCMagnetismModel.getRefrigerator!1.foreign : CCMagnetismModel -> () -> CCRefrigerator! , $@cc(objc_method) @thin (CCMagnetismModel) -> @autoreleased ImplicitlyUnwrappedOptional<CCRefrigerator>
  let f1 = model.getRefrigerator()

// CHECK: class_method [volatile] %0 : $CCMagnetismModel, #CCMagnetismModel.takeRefrigerator!1.foreign : CCMagnetismModel -> () -> CCRefrigerator! , $@cc(objc_method) @thin (CCMagnetismModel) -> @owned ImplicitlyUnwrappedOptional<CCRefrigerator>
  let f2 = model.takeRefrigerator()

// CHECK: class_method [volatile] %0 : $CCMagnetismModel, #CCMagnetismModel.borrowRefrigerator!1.foreign : CCMagnetismModel -> () -> CCRefrigerator! , $@cc(objc_method) @thin (CCMagnetismModel) -> @autoreleased ImplicitlyUnwrappedOptional<CCRefrigerator>
  let f3 = model.borrowRefrigerator()

// CHECK: class_method [volatile] %0 : $CCMagnetismModel, #CCMagnetismModel.setRefrigerator!1.foreign : CCMagnetismModel -> (CCRefrigerator!) -> Void , $@cc(objc_method) @thin (ImplicitlyUnwrappedOptional<CCRefrigerator>, CCMagnetismModel) -> ()
  model.setRefrigerator(copy)

// CHECK: class_method [volatile] %0 : $CCMagnetismModel, #CCMagnetismModel.giveRefrigerator!1.foreign : CCMagnetismModel -> (CCRefrigerator!) -> Void , $@cc(objc_method) @thin (@owned ImplicitlyUnwrappedOptional<CCRefrigerator>, CCMagnetismModel) -> ()
  model.giveRefrigerator(copy)

  // rdar://16846555
  let prop: CCRefrigerator = model.fridgeProp
}
