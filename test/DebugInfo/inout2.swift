// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir -g -o - | FileCheck %s
// LValues are direct values, too. They are reference types, though.
class Class {
	var ivar : Int
	init() { ivar = 1234 }
}

class Other : Class {
	var ovar : Int
	override init () { ovar = 112233
	    super.init()
	    ivar = 4321
	}
}

struct Struct {
	var ivar : Int
	init() { ivar = 4567 }
}

func foo (inout x : Class) {
// CHECK: \000"{{.*}}[ DW_TAG_arg_variable ] [x] [line [[@LINE-1]]]
	println(x.ivar)
	x.ivar++ // Set breakpoint here
}

func foo(inout x : Struct) {
// CHECK: \000"{{.*}}[ DW_TAG_arg_variable ] [x] [line [[@LINE-1]]]
	println(x.ivar)
	x.ivar++ // Set breakpoint here
}

func main() {
	var c : Class = Other()
	var s = Struct()
	foo(&c)
	foo(&s)
	foo(&c)
	foo(&s)
}

main()

