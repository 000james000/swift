// RUN: rm -rf %t && mkdir -p %t
// RUN: cp -R %S/Inputs/mixed-target %t

// RUN: %swift_driver -Xfrontend %clang-importer-sdk -module-cache-path %t -I %S/../Inputs/custom-modules -import-objc-header %t/mixed-target/header.h -emit-module-path %t/MixedWithHeader.swiftmodule %S/Inputs/mixed-with-header.swift %S/../../Inputs/empty.swift -module-name MixedWithHeader
// RUN: %swift %clang-importer-sdk -module-cache-path %t -I %t -I %S/../Inputs/custom-modules -parse %s -verify

// RUN: rm -rf %t/mixed-target/
// RUN: %swift %clang-importer-sdk -module-cache-path %t -I %t -I %S/../Inputs/custom-modules -parse %s -verify

import MixedWithHeader

func testReexportedClangModules(foo : FooProto) {
  let _: CInt = foo.bar
  let _: CInt = ExternIntX.x
}

func testCrossReferences(derived: Derived) {
  let obj: Base = derived
  let _: NSObject = obj.safeOverride(ForwardClass())
  let _: NSObject = obj.safeOverrideProto(ForwardProtoAdopter())

  testProtocolWrapper(ProtoConformer())
  testStruct(Point(x: 2,y: 3))
}
