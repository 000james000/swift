// RUN: %swift -target x86_64-apple-macosx10.9 %s -emit-ir -g -o - | FileCheck %s

protocol PointUtils {
  func distanceFromOrigin() -> Float
}


class Point : PointUtils {
    var x : Float
    var y : Float
    init (_x : Float, _y : Float) {
        x = _x;
        y = _y;
    }

    func distanceFromOrigin() -> Float {
        //var distance = sqrt(x*x + y*y)
        var distance: Float = 1.0
        return distance
    }

}

// CHECK-DAG: define hidden i64 @_TF8protocol4mainFT_Si() {
func main() -> Int {
    var pt = Point(_x: 2.5, _y: 4.25)
// CHECK: [[LOC2D:%[a-zA-Z0-9]+]] = alloca %P8protocol10PointUtils_, align 8
// CHECK: call void @llvm.dbg.declare(metadata !{{{.*}} [[LOC2D]]}, metadata ![[LOC:.*]])
    var loc2d : protocol<PointUtils> = pt
    var distance = loc2d.distanceFromOrigin()
    print("hello") // Set breakpoint here
    return 0
}

// Self should be artificial.
// CHECK: , i32 64, i32 0} ; [ DW_TAG_arg_variable ] [self] [line 16]

// CHECK: ![[LOC]] ={{.*}}[ DW_TAG_auto_variable ] [loc2d] [line [[@LINE-9]]]

main()
