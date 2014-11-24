// RUN: rm -rf %t && mkdir -p %t
// RUN: %swift -emit-module-path %t/basic.swiftmodule %S/basic.swift

// RUN: %swift -emit-ir -module-name Foo %s -I %t -g -o - | FileCheck %s
// RUN: %swift -c -module-name Foo %s -I %t -g -o - | llvm-dwarfdump - | FileCheck --check-prefix=DWARF %s
// XFAIL: linux

// CHECK-DAG: ![[FOOMODULE:[0-9]+]] = {{.*}}[ DW_TAG_module ] [Foo]
// CHECK-DAG: metadata !{metadata !"0x3a\001\00", metadata ![[THISFILE:[0-9]+]], metadata ![[FOOMODULE]]} ; [ DW_TAG_imported_module ]
// CHECK-DAG: ![[THISFILE]] = metadata {{.*}}[ DW_TAG_file_type ] [{{.*}}test/DebugInfo/Imports.swift]
// CHECK-DAG: ![[SWIFTFILE:[0-9]+]] = {{.*}}[ DW_TAG_file_type ]{{.*}}Swift.swiftmodule
// CHECK-DAG: ![[SWIFTMODULE:[0-9]+]] = {{.*}}[ DW_TAG_module ] [Swift]
// CHECK-DAG: metadata !{metadata !"0x3a\000\00", metadata ![[SWIFTFILE]], metadata ![[SWIFTMODULE]]} ; [ DW_TAG_imported_module ]
// CHECK-DAG: ![[BASICFILE:[0-9]+]] = {{.*}}basic.swiftmodule
// CHECK-DAG: ![[BASICMODULE:[0-9]+]] = {{.*}}[ DW_TAG_module ] [basic]
// CHECK-DAG: metadata !{metadata !"0x3a\00[[@LINE+1]]\00", metadata ![[BASICFILE]], metadata ![[BASICMODULE]]} ; [ DW_TAG_imported_module ]
import basic
import typealias Swift.Optional

println(basic.foo(1, 2))

// DWARF: .debug_info
// DWARF: DW_TAG_module
// DWARF-DAG: "Foo"
// DWARF-DAG: "Swift"
// DWARF-DAG: "basic"

// DWARF-NOT: "Swift.Optional"

// DWARF-DAG: file_names{{.*}} Imports.swift
// DWARF-DAG: file_names{{.*}} Swift.swiftmodule
// DWARF-DAG: file_names{{.*}} basic.swift
