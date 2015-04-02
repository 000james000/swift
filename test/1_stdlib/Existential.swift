// RUN: %target-run-simple-swift | FileCheck %s

func pipe<T>(input: AnySequence<T>, output: SinkOf<T>) {
  for x in input {
    output.put(x)
  }
}

struct Print<T : Printable> : SinkType {
  func put(x: T) {
    print(x)
    print("/")
  }
}

var z = [ 1, 2, 3 ]

func printArray<T : Printable>(x: [T]) {
  pipe(AnySequence(x), SinkOf(Print<T>()))
  println()
}

// CHECK: 1/2/3/4/
// CHECK-NEXT: 1.5/2.5/3.5/4.5/
printArray([1, 2, 3, 4])
printArray([1.5, 2.5, 3.5, 4.5])
