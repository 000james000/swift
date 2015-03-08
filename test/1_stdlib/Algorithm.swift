// RUN: %target-run-simple-swift

import StdlibUnittest
import SwiftUnstable

var Algorithm = TestSuite("Algorithm")

extension String.UnicodeScalarView : Equatable {}

public func == (
  lhs: String.UnicodeScalarView, rhs: String.UnicodeScalarView) -> Bool {
  return Array(lhs) == Array(rhs)
}

Algorithm.test("split") {
  expectEqual(
    [ "foo", "  bar baz " ].map { $0.unicodeScalars },
    split("  foo   bar baz ".unicodeScalars, maxSplit: 1) { $0._isSpace() })

  expectEqual(
    [ "foo", "bar", "baz" ].map { $0.unicodeScalars },
    split(
      "  foo   bar baz ".unicodeScalars, allowEmptySlices: false) { 
      $0._isSpace() 
    })

  expectEqual(
    [ "", "", "foo", "", "", "bar", "baz", "" ].map { $0.unicodeScalars },
    split(
      "  foo   bar baz ".unicodeScalars, allowEmptySlices: true) { 
      $0._isSpace() 
    })

  expectEqual(
    [ "", "", "foo   bar baz " ].map { $0.unicodeScalars },
    split(
      "  foo   bar baz ".unicodeScalars, allowEmptySlices: true, maxSplit: 2,
      isSeparator: { $0._isSpace() }))
}

struct StartsWithTest {
  let expected: Bool
  let sequence: [Int]
  let prefix: [Int]
  let loc: SourceLoc

  init(
    _ expected: Bool, _ sequence: [Int], _ prefix: [Int],
    file: String = __FILE__, line: UWord = __LINE__
  ) {
    self.expected = expected
    self.sequence = sequence
    self.prefix = prefix
    self.loc = SourceLoc(file, line, comment: "test data")
  }
}

let startsWithTests = [
  StartsWithTest(true, [], []),
  StartsWithTest(false, [], [ 1 ]),
  StartsWithTest(true, [ 1 ], []),
  StartsWithTest(true, [ 0, 1, 3, 5 ], [ 0, 1 ]),
  StartsWithTest(true, [ 0, 1 ], [ 0, 1 ]),
  StartsWithTest(false, [ 0, 1, 3, 5 ], [ 0, 1, 4 ]),
  StartsWithTest(false, [ 0, 1 ], [ 0, 1, 4 ]),
]

func checkStartsWith(
  expected: Bool, sequence: [Int], prefix: [Int],
  stackTrace: SourceLocStack
) {
  expectEqual(expected, startsWith(sequence, prefix), stackTrace: stackTrace)
  expectEqual(
    expected, startsWith(sequence, prefix, (==)),
    stackTrace: stackTrace)
  expectEqual(
    expected, startsWith(map(sequence) { $0 * 2 }, prefix) { $0 / 2 == $1 },
    stackTrace: stackTrace)

  // Test using different types for the sequence and prefix.
  expectEqual(
    expected, startsWith(ContiguousArray(sequence), prefix),
    stackTrace: stackTrace)
  expectEqual(
    expected, startsWith(ContiguousArray(sequence), prefix, (==)),
    stackTrace: stackTrace)
}

Algorithm.test("startsWith") {
  for test in startsWithTests {
    checkStartsWith(
      test.expected, test.sequence, test.prefix, test.loc.withCurrentLoc())
  }
}

Algorithm.test("enumerate") {
  var result = [String]()
  for (i, s) in enumerate( "You will never retrieve the necronomicon!"._split(" ") ) {
    result.append("\(i) \(s)")
  }
  expectEqual(
    [ "0 You", "1 will", "2 never", "3 retrieve", "4 the", "5 necronomicon!" ],
    result)
}

Algorithm.test("equal") {
  var _0_4 = [0, 1, 2, 3]
  expectFalse(equal(_0_4, 0..<3))
  expectTrue(equal(_0_4, 0..<4))
  expectFalse(equal(_0_4, 0..<5))
  expectFalse(equal(_0_4, 1..<4))
}

Algorithm.test("equal/predicate") {
  func compare(lhs: (Int, Int), rhs: (Int, Int)) -> Bool {
    return lhs.0 == rhs.0 && lhs.1 == rhs.1
  }

  var _0_4 = [(0, 10), (1, 11), (2, 12), (3, 13)]
  expectFalse(equal(_0_4, [(0, 10), (1, 11), (2, 12)], compare))
  expectTrue(equal(_0_4, [(0, 10), (1, 11), (2, 12), (3, 13)], compare))
  expectFalse(equal(_0_4, [(0, 10), (1, 11), (2, 12), (3, 13), (4, 14)], compare))
  expectFalse(equal(_0_4, [(1, 11), (2, 12), (3, 13)], compare))
}

Algorithm.test("contains") {
  let _0_4 = [0, 1, 2, 3]
  expectFalse(contains(_0_4, 7))
  expectTrue(contains(_0_4, 2))
  expectFalse(contains(_0_4, { $0 - 10 > 0  }))
  expectTrue(contains(_0_4, { $0 % 3 == 0 }))
}

Algorithm.test("min,max") {
  expectEqual(2, min(3, 2))
  expectEqual(3, min(3, 7, 5))
  expectEqual(3, max(3, 2))
  expectEqual(7, max(3, 7, 5))

  // FIXME: add tests that check that min/max return the
  // first element of the sequence (by reference equailty) that satisfy the
  // condition.
}

Algorithm.test("minElement,maxElement") {
  var arr = [Int](count: 10, repeatedValue: 0)
  for i in 0..<10 {
    arr[i] = i % 7 + 2
  }
  expectEqual([2, 3, 4, 5, 6, 7, 8, 2, 3, 4], arr)

  expectEqual(2, minElement(arr))
  expectEqual(8, maxElement(arr))

  // min and max element of a slice
  expectEqual(3, minElement(arr[1..<5]))
  expectEqual(6, maxElement(arr[1..<5]))

  // FIXME: add tests that check that minElement/maxElement return the
  // first element of the sequence (by reference equailty) that satisfy the
  // condition.
}

Algorithm.test("filter/SequenceType") {
  if true {
    let s = MinimalSequence<Int>([], underestimatedCount: .Overestimate)
    var result = filter(s) {
      (x: Int) -> Bool in
      expectUnreachable()
      return true
    }
    expectType([Int].self, &result)
    expectEqual([], result)
    expectEqual([], Array(s))
    expectEqual(0, result.capacity)
  }
  if true {
    let s = MinimalSequence(
      [ 0, 30, 10, 90 ], underestimatedCount: .Overestimate)
    let result = filter(s) { (x: Int) -> Bool in false }
    expectEqual([], result)
    expectEqual([], Array(s))
    expectEqual(0, result.capacity)
  }
  if true {
    let s = MinimalSequence(
      [ 0, 30, 10, 90 ], underestimatedCount: .Overestimate)
    let result = filter(s) { (x: Int) -> Bool in true }
    expectEqual([ 0, 30, 10, 90 ], result)
    expectEqual([], Array(s))
    expectGE(2 * result.count, result.capacity)
  }
  if true {
    let s = MinimalSequence(
      [ 0, 30, 10, 90 ], underestimatedCount: .Value(0))
    let result = filter(s) { $0 % 3 == 0 }
    expectEqual([ 0, 30, 90 ], result)
    expectEqual([], Array(s))
    expectGE(2 * result.count, result.capacity)
  }
  if true {
    let s = MinimalSequence(
      [ 0, 30, 10, 90 ], underestimatedCount: .Overestimate)
    let result = filter(s) { $0 % 3 == 0 }
    expectEqual([ 0, 30, 90 ], result)
    expectEqual([], Array(s))
    expectGE(2 * result.count, result.capacity)
  }
}

Algorithm.test("filter/CollectionType") {
  if true {
    let c = MinimalForwardCollection<Int>([])
    var result = filter(c) {
      (x: Int) -> Bool in
      expectUnreachable()
      return true
    }
    expectEqual([], result)
    expectType([Int].self, &result)
    expectEqual(0, result.capacity)
  }
  if true {
    let c = MinimalForwardCollection([ 0, 30, 10, 90 ])
    let result = filter(c) { (x: Int) -> Bool in false }
    expectEqual([], result)
    expectEqual(0, result.capacity)
  }
  if true {
    let c = MinimalForwardCollection([ 0, 30, 10, 90 ])
    let result = filter(c) { (x: Int) -> Bool in true }
    expectEqual([ 0, 30, 10, 90 ], result)
    expectGE(2 * result.count, result.capacity)
  }
  if true {
    let c = MinimalForwardCollection([ 0, 30, 10, 90 ])
    let result = filter(c) { $0 % 3 == 0 }
    expectEqual([ 0, 30, 90 ], result)
    expectGE(2 * result.count, result.capacity)
  }
}

Algorithm.test("filter/eager") {
  // Make sure filter is eager and only calls its predicate once per element.
  var count = 0
  let one = filter(0..<10) {
    (x: Int)->Bool in ++count; return x == 1
  }
  for x in one {}
  expectEqual(10, count)
  for x in one {}
  expectEqual(10, count)
}

Algorithm.test("map/SequenceType") {
  if true {
    let s = MinimalSequence<Int>([], underestimatedCount: .Overestimate)
    var result = map(s) {
      (x: Int) -> Int16 in
      expectUnreachable()
      return 42
    }
    expectType([Int16].self, &result)
    expectEqual([], result)
    expectEqual([], Array(s))
    // FIXME: <rdar://problem/19810841> Reserve capacity when running map() over a SequenceType
    // expectLE(s._underestimatedCount, result.capacity)
  }
  if true {
    let s = MinimalSequence(
      [ 0, 30, 10, 90 ], underestimatedCount: .Value(0))
    let result = map(s) { $0 + 1 }
    expectEqual([ 1, 31, 11, 91 ], result)
    expectEqual([], Array(s))
    // FIXME: <rdar://problem/19810841> Reserve capacity when running map() over a SequenceType
    // expectLE(s._underestimatedCount, result.capacity)
  }
  if true {
    let s = MinimalSequence(
      [ 0, 30, 10, 90 ], underestimatedCount: .Overestimate)
    let result = map(s) { $0 + 1 }
    expectEqual([ 1, 31, 11, 91 ], result)
    expectEqual([], Array(s))
    // FIXME: <rdar://problem/19810841> Reserve capacity when running map() over a SequenceType
    // expectLE(s._underestimatedCount, result.capacity)
  }
}

Algorithm.test("map/CollectionType") {
  if true {
    let c = MinimalForwardCollection<Int>([])
    var result = map(c) {
      (x: Int) -> Int16 in
      expectUnreachable()
      return 42
    }
    expectType([Int16].self, &result)
    expectEqual([], result)
    // FIXME: <rdar://problem/19810841> Reserve capacity when running map() over a SequenceType
    // expectLE(c._underestimatedCount, result.capacity)
  }
  if true {
    let c = MinimalForwardCollection(
      [ 0, 30, 10, 90 ], underestimatedCount: .Value(0))
    let result = map(c) { $0 + 1 }
    expectEqual([ 1, 31, 11, 91 ], result)
    // FIXME: <rdar://problem/19810841> Reserve capacity when running map() over a SequenceType
    // expectLE(c._underestimatedCount, result.capacity)
  }
  if true {
    let c = MinimalForwardCollection(
      [ 0, 30, 10, 90 ], underestimatedCount: .Overestimate)
    let result = map(c) { $0 + 1 }
    expectEqual([ 1, 31, 11, 91 ], result)
    // FIXME: <rdar://problem/19810841> Reserve capacity when running map() over a SequenceType
    // expectLE(c._underestimatedCount, result.capacity)
  }
}

Algorithm.test("flatMap/SequenceType") {
  if true {
    let s = MinimalSequence<Int>([])
    var result = flatMap(s) {
      (x: Int) -> [Int16] in
      expectUnreachable()
      return [42]
    }
    expectType([Int16].self, &result)
    expectEqual([], result)
    expectEqual([], Array(s))
  }
  if true {
    let s = MinimalSequence([ 0, 30, 10, 90 ])
    let result = flatMap(s) { [$0 + 1] }
    expectEqual([ 1, 31, 11, 91 ], result)
    expectEqual([], Array(s))
  }
}

Algorithm.test("flatMap/CollectionType") {
  if true {
    let c = MinimalForwardCollection<Int>([])
    var result = flatMap(c) {
      (x: Int) -> [Int16] in
      expectUnreachable()
      return [42]
    }
    expectType([Int16].self, &result)
    expectEqual([], result)
  }
  if true {
    let c = MinimalForwardCollection([ 0, 30, 10, 90 ])
    let result = flatMap(c) { [$0 + 1] }
    expectEqual([ 1, 31, 11, 91 ], result)
  }
}

Algorithm.test("sorted/strings")
  .xfail(.LinuxAny(reason: "String comparison: ICU vs. Foundation"))
  .code {
  expectEqual(
    [ "Banana", "apple", "cherry" ],
    sorted([ "apple", "Banana", "cherry" ]))

  let s = sorted(["apple", "Banana", "cherry"]) {
    count($0) > count($1)
  }
  expectEqual([ "Banana", "cherry", "apple" ], s)
}

// A wrapper around Array<T> that disables any type-specific algorithm
// optimizations and forces bounds checking on.
struct A<T> : MutableSliceable {
  init(_ a: Array<T>) {
    impl = a
  }

  var startIndex: Int {
    return 0
  }

  var endIndex: Int {
    return impl.count
  }

  func generate() -> Array<T>.Generator {
    return impl.generate()
  }
  
  subscript(i: Int) -> T {
    get {
      expectTrue(i >= 0 && i < impl.count)
      return impl[i]
    }
    set (x) {
      expectTrue(i >= 0 && i < impl.count)
      impl[i] = x
    }
  }

  subscript(r: Range<Int>) -> Array<T>.SubSlice {
    get {
      expectTrue(r.startIndex >= 0 && r.startIndex <= impl.count)
      expectTrue(r.endIndex >= 0 && r.endIndex <= impl.count)
      return impl[r]
    }
    set (x) {
      expectTrue(r.startIndex >= 0 && r.startIndex <= impl.count)
      expectTrue(r.endIndex >= 0 && r.endIndex <= impl.count)
      impl[r] = x
    }
  }
  
  var impl: Array<T>
}

func withInvalidOrderings(body: ((Int,Int)->Bool)->Void) {
  // Test some ordering predicates that don't create strict weak orderings
  body { (_,_) in true }
  body { (_,_) in false }
  var i = 0
  body { (_,_) in i++ % 2 == 0 }
  body { (_,_) in i++ % 3 == 0 }
  body { (_,_) in i++ % 5 == 0 }
}

func randomArray() -> A<Int> {
  let count = Int(rand32(exclusiveUpperBound: 50))
  return A(randArray(count))
}

Algorithm.test("invalidOrderings") {
  withInvalidOrderings {
    var a = randomArray()
    sort(&a, $0)
  }
  withInvalidOrderings {
    var a: A<Int>
    a = randomArray()
    partition(&a, indices(a), $0)
  }
  /*
  // FIXME: Disabled due to <rdar://problem/17734737> Unimplemented:
  // abstraction difference in l-value
  withInvalidOrderings {
    var a = randomArray()
    var pred = $0
    _insertionSort(&a, indices(a), &pred)
  }
  */
  withInvalidOrderings {
    let predicate: (Int,Int)->Bool = $0
    let result = lexicographicalCompare(randomArray(), randomArray(), isOrderedBefore: predicate)
  }
}

// The routine is based on http://www.cs.dartmouth.edu/~doug/mdmspe.pdf
func makeQSortKiller(len: Int) -> [Int] {
  var candidate: Int = 0
  var keys = [Int:Int]()
  func Compare(x: Int, y : Int) -> Bool {
    if keys[x] == nil && keys[y] == nil {
      if (x == candidate) {
        keys[x] = keys.count
      } else {
        keys[y] = keys.count
      }
    }
    if keys[x] == nil {
      candidate = x
      return true
    }
    if keys[y] == nil {
      candidate = y
      return false
    }
    return keys[x]! > keys[y]!
  }

  var ary = [Int](count: len, repeatedValue:0)
  var ret = [Int](count: len, repeatedValue:0)
  for i in 0..<len { ary[i] = i }
  ary = sorted(ary, Compare)
  for i in 0..<len {
    ret[ary[i]] = i
  }
  return ret
}

Algorithm.test("sorted/complexity") {
  var ary: [Int] = []

  // Check performance of sort on array of repeating values
  var comparisons_100 = 0
  ary = [Int](count: 100, repeatedValue: 0)
  sort(&ary) { comparisons_100++; return $0 < $1 }
  var comparisons_1000 = 0
  ary = [Int](count: 1000, repeatedValue: 0)
  sort(&ary) { comparisons_1000++; return $0 < $1 }
  expectTrue(comparisons_1000/comparisons_100 < 20)

  // Try to construct 'bad' case for quicksort, on which the algorithm
  // goes quadratic.
  comparisons_100 = 0
  ary = makeQSortKiller(100)
  sort(&ary) { comparisons_100++; return $0 < $1 }
  comparisons_1000 = 0
  ary = makeQSortKiller(1000)
  sort(&ary) { comparisons_1000++; return $0 < $1 }
  expectTrue(comparisons_1000/comparisons_100 < 20)
}

Algorithm.test("sorted/return type") {
  let x: Array = sorted([5, 4, 3, 2, 1] as ArraySlice)
}
runAllTests()

