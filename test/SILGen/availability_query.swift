// RUN: %swift -emit-silgen -target x86_64-apple-macosx10.9 -enable-experimental-availability-checking %s | FileCheck %s

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 9
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 8
// CHECK: [[FUNC:%.*]] = function_ref @_TFSs26_stdlib_isOSVersionAtLeastFTBwBwBw_Bi1_ : $@thin (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@thin (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[BOOL_FUNC:%.*]] = function_ref @_TFSs8_getBoolFBi1_Sb : $@thin (Builtin.Int1) -> Bool
// CHECK: [[BOOL_RESULT:%.*]] = apply [transparent] [[BOOL_FUNC]]([[QUERY_RESULT]]) : $@thin (Builtin.Int1) -> Bool
if #os(OSX >= 10.9.8, iOS >= 7.1) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[QUERY_FUNC:%.*]] = function_ref @_TFSs26_stdlib_isOSVersionAtLeastFTBwBwBw_Bi1_ : $@thin (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[QUERY_FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@thin (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[BOOL_FUNC:%.*]] = function_ref @_TFSs8_getBoolFBi1_Sb : $@thin (Builtin.Int1) -> Bool
// CHECK: [[BOOL_RESULT:%.*]] = apply [transparent] [[BOOL_FUNC]]([[QUERY_RESULT]]) : $@thin (Builtin.Int1) -> Bool
if #os(OSX >= 10.10) {
}

// CHECK: [[MAJOR:%.*]] = integer_literal $Builtin.Word, 10
// CHECK: [[MINOR:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[PATCH:%.*]] = integer_literal $Builtin.Word, 0
// CHECK: [[QUERY_FUNC:%.*]] = function_ref @_TFSs26_stdlib_isOSVersionAtLeastFTBwBwBw_Bi1_ : $@thin (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[QUERY_RESULT:%.*]] = apply [[QUERY_FUNC]]([[MAJOR]], [[MINOR]], [[PATCH]]) : $@thin (Builtin.Word, Builtin.Word, Builtin.Word) -> Builtin.Int1
// CHECK: [[BOOL_FUNC:%.*]] = function_ref @_TFSs8_getBoolFBi1_Sb : $@thin (Builtin.Int1) -> Bool
// CHECK: [[BOOL_RESULT:%.*]] = apply [transparent] [[BOOL_FUNC]]([[QUERY_RESULT]]) : $@thin (Builtin.Int1) -> Bool
if #os(OSX >= 10) {
}
