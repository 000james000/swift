// REQUIRES: OS=macosx
// RUN: %swift -target x86_64-apple-darwin %s -emit-ir -g -o - | FileCheck %s
// RUN: %swift -target x86_64-apple-darwin10 %s -c -g -o %t.o
// RUN: dwarfdump --verify --apple-types %t.o | FileCheck --check-prefix=CHECK-ACCEL %s
// RUN: dwarfdump --debug-info %t.o | FileCheck --check-prefix=CHECK-DWARF %s

// Verify that the unmangles basenames end up in the accelerator table.
// CHECK-ACCEL:	 str[0]{{.*}}"Int"
// CHECK-ACCEL:	 str[0]{{.*}}"foo"

// Verify that the mangled names end up in the debug info.
// CHECK-DWARF: TAG_structure_type
// CHECK-DWARF-NEXT: AT_name( "foo" )
// CHECK-DWARF-NEXT: AT_linkage_name( "_TtC4main3foo" )

// Verify the IR interface:
// CHECK: metadata !"_TtC4main3foo"} ; [ DW_TAG_structure_type ] [foo] [line [[@LINE+1]],
class foo {
	var x : Int = 1
}

func main() -> Int {
	var thefoo = foo();
	return thefoo.x
}

main()
