// RUN: rm -rf %t/clang-module-cache
// RUN: %swift -i -parse-stdlib -module-cache-path %t/clang-module-cache -sdk %sdk %s | FileCheck %s
// REQUIRES: swift_interpreter
// REQUIRES: sdk

// FIXME: iOS fails: target-run-stdlib-swift gets 'unknown identifier VarArgs'

import Swift

@asmname("vprintf")
func c_vprintf(format: ConstUnsafePointer<Int8>, args: CVaListPointer)

func printf(format: String, arguments: CVarArg...) {
  withVaList(arguments) {
    c_vprintf(format, $0)
  }
}

func test_varArgs0() {
  // CHECK: The answer to life and everything is 42, 42, -42, 3.14
  VarArgs.printf(
    "The answer to life and everything is %ld, %u, %d, %f\n",
    42, UInt32(42), Int16(-42), 3.14159279)
}
test_varArgs0()

func test_varArgs1() {
  var args = [CVarArg]()

  var format = "dig it: "
  for i in 0..<12 {
    args.append(Int16(-i))
    args.append(Float(i))
    format += "%d %2g "
  }
  
  // CHECK: dig it: 0  0 -1  1 -2  2 -3  3 -4  4 -5  5 -6  6 -7  7 -8  8 -9  9 -10 10 -11 11
  withVaList(args) {
    c_vprintf(format + "\n", $0)
  }
}
test_varArgs1()

// CHECK: done.
println("done.")
