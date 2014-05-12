// RUN: rm -rf %t
// RUN: mkdir %t
// RUN: %swift -emit-module -o %t %S/Inputs/def_class.swift
// RUN: %swift -emit-module -o %t -I %t %S/Inputs/def_override.swift
// RUN: llvm-bcanalyzer %t/def_override.swiftmodule | FileCheck %s
// RUN: %swift -parse -I %t %s -verify

// CHECK-NOT: UnknownCode

import def_override

let methods = OverrideFunc()
methods.reset()

let baseMethods: StillEmpty = methods
baseMethods.reset()


let props = OverrideComputedProperty()
props.value = props.value + 1
println(props.readOnly)

let baseProps: ComputedProperty = props
baseProps.value = baseProps.value + 1
println(baseProps.readOnly)


let newSetter = OverrideAddsSetter()
newSetter.readOnly = newSetter.value


let simpleSubscript1 = OverrideSimpleSubscript()
println(simpleSubscript1[4])

let newSetterSubscript = OverrideAddsSubscriptSetter()
newSetterSubscript[4] = newSetterSubscript[5]


let simpleSubscript2 = OverrideComplexSubscript()
simpleSubscript2[4, true] = 5
println(simpleSubscript2[4, true])
