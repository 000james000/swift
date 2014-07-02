// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir -g -o - | FileCheck %s
func main() {
  // CHECK: call void @llvm.dbg.declare(metadata !{%"protocol<>"* {{.*}}}, metadata ![[S:.*]]), !dbg ![[DBG:.*]]
  // CHECK: ![[S]] ={{.*}}[ DW_TAG_auto_variable ] [s] [line [[@LINE+3]]]
  // CHECK: ![[SCOPE:.*]] = {{.*}}i32 2, i32 13, i32 0, i32 {{[0-9]+}}} ; [ DW_TAG_lexical_block ]
  // CHECK: ![[DBG]] = metadata !{i32 [[@LINE+1]], i32 6, metadata ![[SCOPE]], null}
	var s : Any = "hello world"
	var n : Any = 12
	var t : Any = (1,2)
	println("hello world")
}

main()
