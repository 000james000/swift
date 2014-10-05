// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -emit-silgen -parse-as-library -module-cache-path %t/clang-module-cache -target x86_64-apple-macosx10.9 -sdk %S/Inputs -I %S/Inputs -enable-source-import %s | FileCheck %s
// RUN: %swift -emit-ir -parse-as-library -module-cache-path %t/clang-module-cache -target x86_64-apple-macosx10.9 -sdk %S/Inputs -I %S/Inputs -enable-source-import %s | FileCheck %s -check-prefix=IR

import Foundation
import UIKit

@UIApplicationMain
class MyDelegate : UIApplicationDelegate {}

// CHECK-LABEL: sil @main
// CHECK:         function_ref @UIApplicationMain
// IR-LABEL: define i32 @main
// IR:            call i32 @UIApplicationMain

// Ensure that we coexist with normal references to the functions we
// implicitly reference in the synthesized main.
func foo(x: AnyObject.Type) -> String {
  return NSStringFromClass(x)
}
func bar() {
  UIApplicationMain(0, nil, nil, nil)
}
