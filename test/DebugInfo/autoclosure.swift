// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir -g -o - | FileCheck %s
// CHECK: define linkonce_odr hidden void @_TFF11autoclosure7call_meFSiT_u_KT_PSs11BooleanType_
// CHECK: call void @llvm.dbg.declare{{.*}}, !dbg
// CHECK: , !dbg ![[DBG:.*]]

func get_truth(input: Int) -> Int {
    return input % 2
}


// Since this is an autoclosure test, don't use &&, which is transparent.
infix operator &&&&& {
  associativity left
  precedence 120
}

func &&&&&(lhs: BooleanType, @autoclosure rhs: ()->BooleanType) -> Bool {
  return lhs.boolValue ? rhs().boolValue : false
}

func call_me(var input: Int) -> Void {
// rdar://problem/14627460
// An autoclosure should have a line number in the debug info and a scope line of 0.
// CHECK-DAG: \00_TFF11autoclosure7call_meFSiT_u_KT_PSs11BooleanType_{{.*}} [ DW_TAG_subprogram ] [line [[@LINE+3]]] [def] [scope 0]
// But not in the line table.
// CHECK-DAG: ![[DBG]] = !{i32 [[@LINE+1]], i32
    if input != 0 &&&&& ( get_truth (input * 2 + 1) > 0 )
    {
        println ("Whew, passed that test.")
    }

}

call_me(5)
