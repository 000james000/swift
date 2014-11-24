// RUN: rm -rf %t && mkdir %t
// RUN: %build-irgen-test-overlays
// RUN: %swift -target x86_64-apple-macosx10.9 -module-cache-path %t/clang-module-cache -sdk %S/Inputs -I %t %s -emit-ir | FileCheck %s
// XFAIL: linux

import Foundation

func foo() -> Int {
  // CHECK-LABEL: define internal i64 @_TToFCF10objc_local3fooFT_SiL_3Bar10returnFivefS0_FT_Si
  class Bar: NSObject {
    @objc func returnFive() -> Int { return 6 }
  }
  return 0
}


