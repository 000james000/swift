// RUN: rm -rf %t && mkdir -p %t
// RUN: cp -R %S/Inputs/mixed-target %t

// RUN: %swiftc_driver -Xfrontend %clang-importer-sdk -module-cache-path %t -I %S/../Inputs/custom-modules -import-objc-header %t/mixed-target/header.h -emit-module-path %t/MixedWithHeader.swiftmodule %S/Inputs/mixed-with-header.swift %S/../../Inputs/empty.swift -module-name MixedWithHeader
// RUN: %swiftc_driver -Xfrontend %clang-importer-sdk -module-cache-path %t -I %t -I %S/../Inputs/custom-modules -import-objc-header %t/mixed-target/header-again.h -emit-module-path %t/MixedWithHeaderAgain.swiftmodule %S/Inputs/mixed-with-header-again.swift %S/../../Inputs/empty.swift -module-name MixedWithHeaderAgain
// RUN: %swift %clang-importer-sdk -module-cache-path %t -I %S/../Inputs/custom-modules -I %t -parse %s -verify

// RUN: rm %t/mixed-target/header.h
// RUN: not %swift %clang-importer-sdk -module-cache-path %t -I %t -I %S/../Inputs/custom-modules -parse %s 2>&1 | FileCheck %s -check-prefix=USE-SERIALIZED-HEADER
// XFAIL: linux

// USE-SERIALIZED-HEADER: redefinition of 'Point'
// USE-SERIALIZED-HEADER: previous definition is here

import MixedWithHeaderAgain

func testLine(line: Line) {
  testLineImpl(line)
}

func useOriginal(a: ForwardClass, b: Derived, c: ForwardClassUser) {
  let conformer = c as ProtoConformer
  testOriginal(a, b, conformer)
  doSomething(a)
}
