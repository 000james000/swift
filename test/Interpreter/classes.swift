// RUN: %target-run-simple-swift | FileCheck %s

class Interval {
  var lo, hi : Int

  init(_ lo:Int, _ hi:Int) {
    self.lo = lo
    self.hi = hi
  }

  func show() {
    println("[\(lo), \(hi)]")
  }
  
  class func like(lo: Int, _ hi: Int) -> Interval {
    return Interval(lo, hi)
  }
}

class OpenInterval : Interval {
  init(_ lo:Int, _ hi:Int) {
    super.init(lo, hi)
  }

  override func show() {
    println("(\(lo), \(hi))")
  }

  override class func like(lo:Int, _ hi:Int) -> Interval {
    return OpenInterval(lo, hi)
  }
}

func +(a: Interval, b: Interval) -> Interval {
  return Interval(a.lo + b.lo, a.hi + b.hi)
}

func -(a: Interval, b: Interval) -> Interval {
  return Interval(a.lo - b.hi, a.hi - b.lo)
}

prefix func -(a: Interval) -> Interval {
  return a.dynamicType.like(-a.hi, -a.lo)
}

// CHECK: [-2, -1]
(-Interval(1,2)).show()
// CHECK: [4, 6]
(Interval(1,2) + Interval(3,4)).show()
// CHECK: [1, 3]
(Interval(3,4) - Interval(1,2)).show()
// CHECK: (-1, 1)
(OpenInterval(-1,1)).show()
// CHECK: (-3, 2)
(-OpenInterval(-2,3)).show()

// CHECK: false
println(Interval(1,2) is OpenInterval)
// CHECK: true
var i12 : Interval = OpenInterval(1,2)
println(i12 is OpenInterval)

class RDar16563763_A {}
class RDar16563763_B : RDar16563763_A {}
println("self is Type = \(RDar16563763_A.self is RDar16563763_B.Type)")
// CHECK: self is Type = false

