// RUN: rm -rf %t && mkdir %t
// RUN: %swift %s -parse -emit-objc-header-path %t/accessibility.h
// RUN: FileCheck -check-prefix=CHECK -check-prefix=CHECK-PUBLIC %s < %t/accessibility.h
// RUN: %check-in-clang %t/accessibility.h

// RUN: %swift %clang-importer-sdk -module-cache-path %t/clang-module-cache %s -parse -import-objc-header %S/../Inputs/empty.h -emit-objc-header-path %t/accessibility-internal.h
// RUN: FileCheck -check-prefix=CHECK -check-prefix=CHECK-INTERNAL %s < %t/accessibility-internal.h
// RUN: %check-in-clang %t/accessibility-internal.h

// CHECK-LABEL: @interface A_Public{{$}}
// CHECK-INTERNAL-NEXT: init
// CHECK-NEXT: @end
@objc public class A_Public {}

// CHECK-PUBLIC-NOT: B_Internal
// CHECK-INTERNAL-LABEL: @interface B_Internal{{$}}
// CHECK-INTERNAL-NEXT: init
// CHECK-INTERNAL-NEXT: @end
@objc internal class B_Internal {}

// CHECK-NOT: C_Private
@objc private class C_Private {}
