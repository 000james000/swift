// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir -g -o - | FileCheck %s
class Class {
// CHECK: _TtQq_FC5atype5Class8function
	func function <T> (x : T) {
		println("hello world")
	}
}

func main() {
	var v = 1
	var c = Class()
	c.function(1)
}

main()
