//===--- Index.swift - A position in a Collection -------------------------===//
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
//
//  ForwardIndex, BidirectionalIndex, and RandomAccessIndex
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//===--- Dispatching advance and distance functions -----------------------===//
// These generic functions are for user consumption; they dispatch to the
// appropriate implementation for T.

/// Measure the distance between start and end.
///
/// If T models RandomAccessIndex, requires that start and end are
/// part of the same sequence and executes in O(1).
///
/// Otherwise, requires that end is reachable from start by
/// incrementation, and executes in O(N), where N is the function's
/// result.
public func distance<T: ForwardIndex>(start: T, end: T) -> T.DistanceType {
  return start~>_distanceTo(end)
}

/// Return the result of moving start by n positions.  If T models
/// RandomAccessIndex, executes in O(1).  Otherwise, executes in
/// O(abs(n)).  If T does not model BidirectionalIndex, requires that n
/// is non-negative.
public func advance<T: ForwardIndex>(start: T, n: T.DistanceType) -> T {
  return start~>_advance(n)
}

/// Return the result of moving start by n positions, or until it
/// equals end.  If T models RandomAccessIndex, executes in O(1).
/// Otherwise, executes in O(abs(n)).  If T does not model
/// BidirectionalIndex, requires that n is non-negative.
public func advance<T: ForwardIndex>(start: T, n: T.DistanceType, end: T) -> T {
  return start~>_advance(n, end)
}

/// Operation tags for distance and advance
///
/// Operation tags allow us to use a single operator (~>) for
/// dispatching every generic function with a default implementation.
/// Only authors of specialized distance implementations need to touch
/// this tag.
public struct _Distance {}
public func _distanceTo<I>(end: I) -> (_Distance, (I)) {
  return (_Distance(), (end))
}

public struct _Advance {}
public func _advance<D>(n: D) -> (_Advance, (D)) {
  return (_Advance(), (n: n))
}
public func _advance<D, I>(n: D, end: I) -> (_Advance, (D, I)) {
  return (_Advance(), (n, end))
}

//===----------------------------------------------------------------------===//
//===--- ForwardIndex -----------------------------------------------------===//

// Protocols with default implementations are broken into two parts, a
// base and a more-refined part.  From the user's point-of-view,
// however, _ForwardIndex and ForwardIndex should look like a single
// protocol.  This technique gets used throughout the standard library
// to break otherwise-cyclic protocol dependencies, which the compiler
// isn't yet smart enough to handle.

public protocol _Incrementable : Equatable {
  func successor() -> Self
}

//===----------------------------------------------------------------------===//
// A dummy type that we can use when we /don't/ want to create an
// ambiguity indexing Range<T> outside a generic context.  See the
// implementation of Range for details.
public struct _DisabledRangeIndex_ {
  private init() {
    _fatalError("Nobody should ever create one.")
  }
}

//===----------------------------------------------------------------------===//

public protocol _ForwardIndex : _Incrementable {
  typealias DistanceType : _SignedInteger = Int

  // See the implementation of Range for an explanation of these
  // associated types.
  typealias _DisabledRangeIndex = _DisabledRangeIndex_
}

@prefix @assignment @transparent public
func ++ <T : _Incrementable> (inout x: T) -> T {
  x = x.successor()
  return x
}

@postfix @assignment @transparent public
func ++ <T : _Incrementable> (inout x: T) -> T {
  var ret = x
  x = x.successor()
  return ret
}

public protocol ForwardIndex : _ForwardIndex {
  // This requirement allows generic distance() to find default
  // implementations.  Only the author of F and the author of a
  // refinement of F having a non-default distance implementation need
  // to know about it.  These refinements are expected to be rare
  // (which is why defaulted requirements are a win)

  // Do not use these operators directly; call distance(start, end)
  // and advance(start, n) instead
  func ~> (start:Self, _ : (_Distance, Self)) -> DistanceType
  func ~> (start:Self, _ : (_Advance, DistanceType)) -> Self
  func ~> (start:Self, _ : (_Advance, (DistanceType, Self))) -> Self
}

// advance and distance implementations

/// Do not use this operator directly; call distance(start, end) instead
public
func ~> <T: _ForwardIndex>(start:T, rest: (_Distance, T)) -> T.DistanceType {
  var p = start
  var count: T.DistanceType = 0
  let end = rest.1
  while p != end {
    ++count
    ++p
  }
  return count
}

/// Do not use this operator directly; call advance(start, n) instead
@transparent public
func ~> <T: _ForwardIndex>(
  start: T, rest: (_Advance, T.DistanceType)
) -> T {
  let n = rest.1
  return _advanceForward(start, n)
}

internal
func _advanceForward<T: _ForwardIndex>(start: T, n: T.DistanceType) -> T {
  _precondition(n >= 0,
      "Only BidirectionalIndex can be advanced by a negative amount")
  var p = start
  for var i: T.DistanceType = 0; i != n; ++i {
    ++p
  }
  return p
}

/// Do not use this operator directly; call advance(start, n, end) instead
@transparent public
func ~> <T: _ForwardIndex>(
  start:T, rest: ( _Advance, (T.DistanceType, T))
) -> T {
  return _advanceForward(start, rest.1.0, rest.1.1)
}

internal
func _advanceForward<T: _ForwardIndex>(
  start: T, n: T.DistanceType, end: T
) -> T {
  _precondition(n >= 0,
      "Only BidirectionalIndex can be advanced by a negative amount")
  var p = start
  for var i: T.DistanceType = 0; i != n && p != end; ++i {
    ++p
  }
  return p
}

//===----------------------------------------------------------------------===//
//===--- BidirectionalIndex -----------------------------------------------===//
public protocol _BidirectionalIndex : _ForwardIndex {
  func predecessor() -> Self
}

public protocol BidirectionalIndex : ForwardIndex, _BidirectionalIndex {
}

@prefix @assignment @transparent public
func -- <T: _BidirectionalIndex> (inout x: T) -> T {
  x = x.predecessor()
  return x
}


@postfix @assignment @transparent public
func -- <T: _BidirectionalIndex> (inout x: T) -> T {
  var ret = x
  x = x.predecessor()
  return ret
}

// advance implementation

/// Do not use this operator directly; call advance(start, n) instead
@transparent public
func ~> <T: _BidirectionalIndex>(
  start:T , rest: (_Advance, T.DistanceType)
) -> T {
  let n = rest.1
  if n >= 0 {
    return _advanceForward(start, n)
  }
  var p = start
  for var i: T.DistanceType = n; i != 0; ++i {
    --p
  }
  return p
}

/// Do not use this operator directly; call advance(start, n, end) instead
@transparent public
func ~> <T: _BidirectionalIndex>(
  start:T, rest: (_Advance, (T.DistanceType, T))
) -> T {
  let n = rest.1.0
  let end = rest.1.1

  if n >= 0 {
    return _advanceForward(start, n, end)
  }
  var p = start
  for var i: T.DistanceType = n; i != 0 && p != end; ++i {
    --p
  }
  return p
}

//===----------------------------------------------------------------------===//
//===--- RandomAccessIndex ------------------------------------------------===//
public protocol _RandomAccessIndex : _BidirectionalIndex, Strideable {
  func distanceTo(Self) -> DistanceType
  func advancedBy(DistanceType) -> Self
}

public protocol RandomAccessIndex
  : BidirectionalIndex, _RandomAccessIndex, Comparable {
  /* typealias DistanceType : IntegerArithmetic*/
}

public
func < <T: _RandomAccessIndex>(x: T, y: T) -> Bool {
  return x.distanceTo(y) > 0
}

// advance and distance implementations

/// Do not use this operator directly; call distance(start, end) instead
@transparent public
func ~> <T: _RandomAccessIndex>(start:T, rest:(_Distance, (T)))
-> T.DistanceType {
  let end = rest.1
  return start.distanceTo(end)
}

/// Do not use this operator directly; call advance(start, n) instead
@transparent public
func ~> <T: _RandomAccessIndex>(
  start:T, rest:(_Advance, (T.DistanceType))
) -> T {
  let n = rest.1
  return start.advancedBy(n)
}

/// Do not use this operator directly; call advance(start, n, end) instead
@transparent public
func ~> <T: _RandomAccessIndex>(
  start:T, rest:(_Advance, (T.DistanceType, T))
) -> T {
  let n = rest.1.0
  let end = rest.1.1

  let d = start.distanceTo(end)
  return (n > 0 ? d < n : d > n) ? end : start.advancedBy(n)
}

