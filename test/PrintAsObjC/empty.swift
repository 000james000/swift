// RUN: rm -rf %t
// RUN: mkdir %t
// RUN: %swift %clang-importer-sdk -module-cache-path %t/clang-module-cache %s -parse -emit-objc-header-path %t/empty.h
// RUN: FileCheck %s < %t/empty.h
// RUN: %check-in-clang %t/empty.h
// RUN: %check-in-clang -fno-modules %t/empty.h
// RUN: not %check-in-clang -I %S/Inputs/clang-headers %t/empty.h 2>&1 | FileCheck %s --check-prefix=CUSTOM-OBJC-PROLOGUE

// CHECK-NOT: @import Swift;

// CHECK-LABEL: #include <objc/NSObject.h>
// CHECK: #include <stdint.h>
// CHECK: #include <stddef.h>
// CHECK: #include <stdbool.h>

// CHECK: # define SWIFT_METATYPE(X)
// CHECK: # define SWIFT_CLASS
// CHECK: # define SWIFT_PROTOCOL
// CHECK: # define OBJC_DESIGNATED_INITIALIZER

// CHECK-LABEL: #if defined(__has_feature) && __has_feature(modules)
// CHECK-NEXT: #endif


// CHECK-NOT: {{[@;{}]}}

// CUSTOM-OBJC-PROLOGUE: swift/objc-prologue.h:1:2: error: "Prologue included"
