// RUN: rm -rf %t && mkdir %t
// RUN: %build-irgen-test-overlays
// RUN: %swift -sdk %S/Inputs -I %t %s -emit-ir | FileCheck %s

import Foundation

func foo() -> Int {
  // CHECK-LABEL: define internal i64 @_TToFCF10objc_local3fooFT_SiL_3Bar10returnFivefS0_FT_Si
  class Bar: NSObject {
    @objc func returnFive() -> Int { return 6 }
  }
  return 0
}


