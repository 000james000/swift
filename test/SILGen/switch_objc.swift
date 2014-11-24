// RUN: rm -rf %t.mcp
// RUN: %swift %clang-importer-sdk -emit-silgen %s -module-cache-path %t.mcp | FileCheck %s
// XFAIL: linux

import Foundation

// CHECK-LABEL: sil hidden @_TF11switch_objc13matchesEitherFT5inputCSo4Hive1aS0_1bS0__Sb :
func matchesEither(#input: Hive, #a: Hive, #b: Hive) -> Bool {
  switch input {
  // CHECK:   function_ref @_TF10ObjectiveCoi2teFTCSo8NSObjectS0__Sb
  // CHECK:   cond_br {{%.*}}, [[YES_CASE1:bb[0-9]+]], [[NOT_CASE1:bb[0-9]+]]
  // CHECK: [[YES_CASE1]]:
  // CHECK:   br [[RET_TRUE:bb[0-9]+]]
  // CHECK: [[NOT_CASE1]]:
  // CHECK:   function_ref @_TF10ObjectiveCoi2teFTCSo8NSObjectS0__Sb
  // CHECK:   cond_br {{%.*}}, [[YES_CASE2:bb[0-9]+]], [[NOT_CASE2:bb[0-9]+]]
  // CHECK: [[YES_CASE2]]:
  // CHECK:   br [[RET_TRUE]]
  case a, b:
  // CHECK: [[RET_TRUE]]:
  // CHECK:   function_ref @_TFSbCfMSbFT22_builtinBooleanLiteralBi1__Sb
    return true

  // CHECK: [[NOT_CASE2]]:
  // CHECK:   br [[RET_FALSE:bb[0-9]+]]
  default:
  // CHECK: [[RET_FALSE]]:
  // CHECK:   function_ref @_TFSbCfMSbFT22_builtinBooleanLiteralBi1__Sb
    return false
  }
}
