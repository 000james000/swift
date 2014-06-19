//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

struct RangeGenerator<T: ForwardIndex> : Generator, Sequence {
  typealias Element = T

  @transparent
  init(_ bounds: Range<T>) {
    self.startIndex = bounds.startIndex
    self.endIndex = bounds.endIndex
  }

  mutating func next() -> Element? {
    if startIndex == endIndex {
      return .None
    }
    return startIndex++
  }

  // Every Generator is also a single-pass Sequence
  typealias GeneratorType = RangeGenerator<T>
  func generate() -> GeneratorType {
    return self
  }

  var startIndex: T
  var endIndex: T
}

struct StridedRangeGenerator<T: ForwardIndex> : Generator, Sequence {
  typealias Element = T

  @transparent
  init(_ bounds: Range<T>, stride: T.DistanceType) {
    self._bounds = bounds
    self._stride = stride
  }

  mutating func next() -> Element? {
    if !_bounds {
      return .None
    }
    let ret = _bounds.startIndex
    _bounds.startIndex = advance(_bounds.startIndex, _stride, _bounds.endIndex)
    return ret
  }

  // Every Generator is also a single-pass Sequence
  typealias GeneratorType = StridedRangeGenerator
  func generate() -> GeneratorType {
    return self
  }

  var _bounds: Range<T>
  var _stride: T.DistanceType
}

struct Range<T: ForwardIndex> : LogicValue, Sliceable {  
  @transparent
  init(start: T, end: T) {
    _startIndex = start
    _endIndex = end
  }

  var isEmpty : Bool {
    return startIndex == endIndex
  }

  func getLogicValue() -> Bool {
    return !isEmpty
  }

  subscript(i: T) -> T {
    return i
  }

  subscript(x: Range<T>) -> Range {
    return Range(start: x.startIndex, end: x.endIndex)
  }

  typealias GeneratorType = RangeGenerator<T>
  func generate() -> RangeGenerator<T> {
    return GeneratorType(self)
  }

  func by(stride: T.DistanceType) -> StridedRangeGenerator<T> {
    return StridedRangeGenerator(self, stride: stride)
  }

  var startIndex: T {
    get {
      return _startIndex
    }
    set(newValue) {
      _startIndex = newValue
    }
  }

  var endIndex: T {
    get {
      return _endIndex
    }
    set(newValue) {
      _endIndex = newValue
    }
  }
  
  var _startIndex: T
  var _endIndex: T
}

func count<I: RandomAccessIndex>(r: Range<I>) -> I.DistanceType {
  return r.startIndex.distanceTo(r.endIndex)
}


/// Forms a half open range including the minimum value but excluding the
/// maximum value.
@transparent
@availability(*, unavailable, message="half-open range operator .. has been renamed to ..<")
func .. <Pos : ForwardIndex> (min: Pos, max: Pos) -> Range<Pos> {
  return Range(start: min, end: max)
}

@transparent
func ..< <Pos : ForwardIndex> (min: Pos, max: Pos) -> Range<Pos> {
  return Range(start: min, end: max)
}


/// Forms a closed range including both the minimum and maximum values.
@transparent
func ... <Pos : ForwardIndex> (min: Pos, max: Pos) -> Range<Pos> {
  return Range(start: min, end: max.succ())
}

struct ReverseRangeGenerator<T: BidirectionalIndex> : Generator, Sequence {
  typealias Element = T

  @transparent
  init(start: T, pastEnd: T) {
    self._bounds = (start,pastEnd)
  }

  mutating func next() -> Element? {
    if _bounds.0 == _bounds.1 { return .None }
    _bounds.1 = _bounds.1.pred()
    return _bounds.1
  }

  // Every Generator is also a single-pass Sequence
  typealias GeneratorType = ReverseRangeGenerator<T>
  func generate() -> GeneratorType {
    return self
  }

  var _bounds: (T, T)
}

struct ReverseRange<T: BidirectionalIndex> : Sequence {
  init(start: T, pastEnd: T) {
    self._bounds = (start, pastEnd)
  }

  init(range fwd: Range<T>) {
    self._bounds = (fwd.startIndex, fwd.endIndex)
  }

  func isEmpty() -> Bool {
    return _bounds.0 == _bounds.1
  }

  func bounds() -> (T, T) {
    return _bounds
  }

  typealias GeneratorType = ReverseRangeGenerator<T>
  func generate() -> GeneratorType {
    return GeneratorType(start: _bounds.0, pastEnd: _bounds.1)
  }

  var _bounds: (T, T)
}

//
// Pattern matching support for ranges
//
// Ranges can be used to match values contained within the range, e.g.:
// switch x {
// case 0...10:
//   println("single digit")
// case _:
//   println("too big")
// }

@infix func ~= <T: RandomAccessIndex where T.DistanceType : SignedInteger>(x: Range<T>, y: T) -> Bool {
  let a = x.startIndex.distanceTo(y) >= 0
  let b = y.distanceTo(x.endIndex) > 0
  return a && b
}


extension Range {
  /// Return a ${Self} containing the results of calling
  /// `transform(x)` on each element `x` of `self`.
  func map<U>(transform: (T)->U) -> U[] {
    return U[](Swift.map(self, transform))
  }
}


