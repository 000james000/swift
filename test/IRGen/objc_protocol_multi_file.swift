// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -target x86_64-apple-macosx10.9 -primary-file %s %S/Inputs/objc_protocol_multi_file_helper.swift -g -emit-ir | FileCheck %s

// This used to crash <rdar://problem/17929944>.
// To tickle the crash, SubProto must not be used elsewhere in this file.
protocol SubProto : BaseProto {}
// CHECK: @_TMp24objc_protocol_multi_file8SubProto = constant %swift.protocol