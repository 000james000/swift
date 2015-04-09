// RUN: rm -rf %t
// RUN: mkdir %t
// RUN: %target-swift-frontend -emit-module -o %t/test.swiftmodule %s
// RUN: %target-build-swift -g -o %t/sdk-link %s
// RUN: %target-run %t/sdk-link | FileCheck %s

// REQUIRES: objc_interop

import Foundation

// CHECK: {{^}}ABCDEF{{$}}
println(("ABC" as NSString).stringByAppendingString("DEF"))
