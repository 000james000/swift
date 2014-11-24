// RUN: %swift -target x86_64-apple-macosx10.9 -primary-file %s -emit-ir -g -o - | FileCheck %s
// XFAIL: linux
class DeepThought {
  func query() -> Int { return 42 }
}

func foo() -> Int {
  // CHECK: call void @llvm.dbg.declare(metadata !{%C3let11DeepThought** {{.*}}}, metadata ![[A:.*]], metadata !{{[0-9]+}})
  // CHECK ![[A]] = {{.*}}i32 0} ; [ DW_TAG_auto_variable ] [machine] [line [[@LINE+1]]]
  let machine = DeepThought()
// CHECK: [ DW_TAG_auto_variable ] [a] [line [[@LINE+1]]]
  let a = machine.query()
  return a
}
