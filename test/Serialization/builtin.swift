// RUN: rm -rf %t
// RUN: mkdir %t
// RUN: %target-swift-frontend -emit-module -parse-stdlib -o %t %S/Inputs/alias_builtin.swift
// RUN: llvm-bcanalyzer %t/alias_builtin.swiftmodule | FileCheck %s
// RUN: %target-swift-frontend -I %t -parse %s -verify

// CHECK-NOT: UnknownCode

import alias_builtin

var a : TheBuiltinInt64

// Check that it really is Builtin.Int64.
var wrapped = Int64(a) // okay
var badWrapped = Int32(a) // expected-error{{cannot find an initializer for type 'Int32' that accepts an argument list of type '(TheBuiltinInt64)'}}
