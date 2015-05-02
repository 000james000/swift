// RUN: rm -rf %t
// RUN: mkdir %t

// FIXME: BEGIN -enable-source-import hackaround
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/ObjectiveC.swift
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/CoreGraphics.swift
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/Foundation.swift
// RUN:  %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/simd.swift
// FIXME: END -enable-source-import hackaround

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource) -I %t -emit-module -emit-module-doc -o %t -module-name simd_test %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource) -I %t -parse-as-library %t/simd_test.swiftmodule -parse -emit-objc-header-path %t/simd.h -import-objc-header %S/../Inputs/empty.h -disable-objc-attr-requires-foundation-module
// RUN: FileCheck %s < %t/simd.h
// RUN: %check-in-clang %t/simd.h
// RUN: %check-in-clang -fno-modules %t/simd.h -include Foundation.h

// REQUIRES: objc_interop

import Foundation
import simd

// CHECK-LABEL: typedef float swift_Float_Vector4 __attribute__((__ext_vector_type__(4)));
// CHECK-LABEL: typedef double swift_Double_Vector2 __attribute__((__ext_vector_type__(2)));
// CHECK-LABEL: typedef int swift_Int32_Vector3 __attribute__((__ext_vector_type__(3)));

// -- The C simd module is useless to Swift.
// CHECK-NOT: @import simd;

// CHECK-LABEL: @interface Foo : NSObject
@objc class Foo: NSObject {
  // CHECK-LABEL: - (swift_Float_Vector4)doStuffWithFloat4:(swift_Float_Vector4)x;
  @objc func doStuffWithFloat4(x: Float.Vector4) -> Float.Vector4 { return x }
  // CHECK-LABEL: - (swift_Double_Vector2)doStuffWithDouble2:(swift_Double_Vector2)x;
  @objc func doStuffWithDouble2(x: Double.Vector2) -> Double.Vector2 { return x }
  // CHECK-LABEL: - (swift_Int32_Vector3)doStuffWithInt3:(swift_Int32_Vector3)x;
  @objc func doStuffWithInt3(x: Int32.Vector3) -> Int32.Vector3 { return x }
}

