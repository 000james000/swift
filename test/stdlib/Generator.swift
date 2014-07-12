// RUN: %target-run-simple-swift | FileCheck %s

// Check to make sure we are actually getting Optionals out of this
// GeneratorType
var w = (1..<2).generate()
var maybe_one = w.next()

var is_ = "is"
var is_not = "is not"

println("maybe_one \(!maybe_one ? is_ : is_not) None")
// CHECK: maybe_one is not None

switch maybe_one {
case .Some(var one):
  println("one \(one == 1 ? is_ : is_not) 1")
  // CHECK: one is 1
case .None:
  ()
}

println("w.next() \(!w.next() ? is_ : is_not) None")
// CHECK: w.next() is None

// Test SequenceType protocol
w = (1..<2).generate()
for x in w { 
  println("x is \(x)") 
}

// Test round-trip GeneratorType/GeneratorType adaptation
var x = (1..<7).generate()
var y = x
var z = ZipGenerator2(y, (1..<7).generate())

for a in GeneratorSequence(z) {
  println("\(a.0), \(a.1)")
}
// CHECK:      1, 1
// CHECK-NEXT: 2, 2
// CHECK-NEXT: 3, 3
// CHECK-NEXT: 4, 4
// CHECK-NEXT: 5, 5
// CHECK-NEXT: 6, 6

println("done.")
// CHECK: done.
