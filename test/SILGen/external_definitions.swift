// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -target x86_64-apple-macosx10.9 -module-cache-path %t/clang-module-cache -sdk %S/Inputs %s -emit-silgen | FileCheck %s

import ansible

var a = NSAnse(Ansible(bellsOn: NSObject()))

var anse = NSAnse

hasNoPrototype()

// CHECK-LABEL: sil @main
// -- Foreign function is referenced with C calling conv and ownership semantics
// CHECK:   [[NSANSE:%.*]] = function_ref @NSAnse : $@cc(cdecl) @thin (ImplicitlyUnwrappedOptional<Ansible>) -> @autoreleased ImplicitlyUnwrappedOptional<Ansible>
// CHECK:   [[ANSIBLE_CTOR:%.*]] = function_ref @_TFCSo7AnsibleCfMS_FT7bellsOnGSQPSs9AnyObject___GSQS__
// CHECK:   [[NSOBJECT_CTOR:%.*]] = function_ref @_TFCSo8NSObjectCfMS_FT_S_ : $@thin (@thick NSObject.Type) -> @owned NSObject
// CHECK:   [[ANSIBLE:%.*]] = apply [[ANSIBLE_CTOR]]
// CHECK:   [[NSANSE_RESULT:%.*]] = apply [[NSANSE]]([[ANSIBLE]])
// CHECK:   retain_autoreleased [[NSANSE_RESULT]]
// CHECK:   release_value [[ANSIBLE]] : $ImplicitlyUnwrappedOptional<Ansible>
// -- Referencing unapplied C function goes through a thunk
// CHECK:   [[NSANSE:%.*]] = function_ref @_TTOFSC6NSAnseFGSQCSo7Ansible_GSQS__ : $@thin (@owned ImplicitlyUnwrappedOptional<Ansible>) -> @owned ImplicitlyUnwrappedOptional<Ansible>
// -- Referencing unprototyped C function passes no parameters
// CHECK:   [[NOPROTO:%.*]] = function_ref @hasNoPrototype : $@cc(cdecl) @thin () -> ()
// CHECK:   apply [[NOPROTO]]()

// -- Constructors for imported Ansible
// CHECK-LABEL: sil  shared @_TFCSo7AnsibleCfMS_FT7bellsOnGSQPSs9AnyObject___GSQS__ : $@thin (@owned ImplicitlyUnwrappedOptional<AnyObject>, @thick Ansible.Type) -> @owned ImplicitlyUnwrappedOptional<Ansible>


// -- Constructors for imported NSObject
// CHECK-LABEL: sil  shared @_TFCSo8NSObjectCfMS_FT_S_ : $@thin (@thick NSObject.Type) -> @owned NSObject

// -- Native Swift thunk for NSAnse
// CHECK: sil shared @_TTOFSC6NSAnseFGSQCSo7Ansible_GSQS__ : $@thin (@owned ImplicitlyUnwrappedOptional<Ansible>) -> @owned ImplicitlyUnwrappedOptional<Ansible> {
// CHECK: bb0(%0 : $ImplicitlyUnwrappedOptional<Ansible>):
// CHECK:   %1 = function_ref @NSAnse : $@cc(cdecl) @thin (ImplicitlyUnwrappedOptional<Ansible>) -> @autoreleased ImplicitlyUnwrappedOptional<Ansible>
// CHECK:   %2 = apply %1(%0) : $@cc(cdecl) @thin (ImplicitlyUnwrappedOptional<Ansible>) -> @autoreleased ImplicitlyUnwrappedOptional<Ansible>
// CHECK:   strong_retain_autoreleased %2 : $ImplicitlyUnwrappedOptional<Ansible>
// CHECK:   release_value %0 : $ImplicitlyUnwrappedOptional<Ansible>
// CHECK:   return %2 : $ImplicitlyUnwrappedOptional<Ansible>
// CHECK: }

// -- Constructors for imported Ansible
// CHECK-LABEL: sil  shared @_TFCSo7AnsibleCfMS_FT_S_ : $@thin (@thick Ansible.Type) -> @owned Ansible

