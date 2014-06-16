// RUN: %swift -target x86_64-apple-darwin10 %s -emit-ir -g -o - | FileCheck %s
import Swift

func main() -> Void
{
    // I am line 6.
    var random_string = "b"
    var random_int = 5
    var out_only = 2013

    var backward_ptr  =
    // CHECK: define linkonce_odr hidden i1 @_TFF4main4mainFT_T_U_FTSSSS_Sb(
    // CHECK: %[[RHS_ADDR:.*]] = alloca %SS*, align 8
    // CHECK: store %SS* %{{.*}}, %SS** %[[RHS_ADDR]], align 8
    // The shadow copying should happen in the prologue, because the
    // stack pointer will be decremented after it.
    // CHECK-NOT: !dbg
    // CHECK-NEXT: call void @llvm.dbg.declare(metadata !{%SS** %[[RHS_ADDR]]}, metadata !{{.*}}), !dbg
    // CHECK-DAG: [ DW_TAG_arg_variable ] [lhs] [line [[@LINE+5]]]
    // CHECK-DAG: [ DW_TAG_arg_variable ] [rhs] [line [[@LINE+4]]]
    // CHECK-DAG: [ DW_TAG_arg_variable ] [random_string] [line 7]
    // CHECK-DAG: [ DW_TAG_arg_variable ] [random_int] [line 8]
    // CHECK-DAG: [ DW_TAG_arg_variable ] [out_only] [line 9]
        { (lhs : String, rhs : String) -> Bool in
            if rhs == random_string
               || countElements(rhs.unicodeScalars) == random_int
            {
            // Ensure the two local_vars are in different lexical scopes.
            // CHECK-DAG: metadata !{{{.*}}, metadata ![[THENSCOPE:.*]], metadata !"local_var", {{.*}}} ; [ DW_TAG_auto_variable ] [local_var] [line [[@LINE+2]]]
            // CHECK-DAG: ![[THENSCOPE]] = metadata !{{{.*}}, i32 [[@LINE-3]], {{.*}}} ; [ DW_TAG_lexical_block ]
                var local_var : Int = 10
                print ("I have an int here \(local_var).\n")
                return false
            }
            else
            {
            // CHECK-DAG: metadata !{{{.*}}, metadata ![[ELSESCOPE:.*]], metadata !"local_var", {{.*}}} ; [ DW_TAG_auto_variable ] [local_var] [line [[@LINE+2]]]
            // CHECK-DAG: ![[ELSESCOPE]] = metadata !{{{.*}}, i32 [[@LINE-2]], {{.*}}} ; [ DW_TAG_lexical_block ]
                var local_var : String = "g"
                print ("I have another string here \(local_var).\n")
                // Assign to all the captured variables to inhibit capture promotion.
                random_string = "c"
                random_int = -1
                out_only = 333
                return rhs < lhs
            }
        }

    var bool = backward_ptr("a" , "b")

    var my_string = ["a", "b", "c", "d"]

    var new_string = sorted (my_string, backward_ptr )

    print (new_string)
    print ("\n")
    print (random_int)
    print ("\n")
}

main()

