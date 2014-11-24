// RUN: rm -rf %t && mkdir -p %t
// RUN: cp -R %S/Inputs/VerySmallObjCFramework.framework %t
// RUN: %clang -dynamiclib %S/Inputs/VerySmallObjCFramework.m -fmodules -F %t -o %t/VerySmallObjCFramework.framework/VerySmallObjCFramework
// RUN: %swift_driver -F %t %s | FileCheck %s
// XFAIL: linux

import VerySmallObjCFramework

// CHECK: 42
println(globalValue)
