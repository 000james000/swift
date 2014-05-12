// RUN: %target-run-simple-swift | FileCheck %s

func foo(inout x: Int) -> () -> Int {
  func bar() -> Int {
    x += 1
    return x
  }
  bar()
  return bar
}

var x = 219
var f = foo(&x)
println(x) // CHECK: 220
println(f()) // CHECK: 221
println(f()) // CHECK: 222
println(f()) // CHECK: 223
println(x) // CHECK: 220
