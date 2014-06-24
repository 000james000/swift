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

struct _CountElements {}
func _countElements<Args>(a: Args) -> (_CountElements, Args) {
  return (_CountElements(), a)
}
// Default implementation of countElements for Collections
// Do not use this operator directly; call countElements(x) instead
func ~> <T: _Collection>(x:T, _:(_CountElements,()))
  -> T.IndexType.DistanceType
{
  return distance(x.startIndex, x.endIndex)
}

/// Return the number of elements in x.  O(1) if T.IndexType is
/// RandomAccessIndex; O(N) otherwise.
func countElements <T: _Collection>(x: T) -> T.IndexType.DistanceType {
  return x~>_countElements()
}

protocol _Collection : _Sequence {
  typealias IndexType : ForwardIndex

  var startIndex: IndexType {get}
  var endIndex: IndexType {get}

  // The declaration of Element and _subscript here is a trick used to
  // break a cyclic conformance/deduction that Swift can't handle.  We
  // need something other than a Collection.GeneratorType.Element that can
  // be used as IndexingGenerator<T>'s Element.  Here we arrange for the
  // Collection itself to have an Element type that's deducible from
  // its subscript.  Ideally we'd like to constrain this
  // Element to be the same as Collection.GeneratorType.Element (see
  // below), but we have no way of expressing it today.
  typealias _Element
  subscript(i: IndexType) -> _Element {get}
}

protocol Collection : _Collection, Sequence {
  subscript(i: IndexType) -> GeneratorType.Element {get}
  
  // Do not use this operator directly; call countElements(x) instead
  func ~> (_:Self, _:(_CountElements, ())) -> IndexType.DistanceType
}

// Default implementation of underestimateCount for Collections.  Do not
// use this operator directly; call underestimateCount(s) instead
func ~> <T: _Collection>(x:T,_:(_UnderestimateCount,())) -> Int {
  return numericCast(x~>_countElements())
}

// Default implementation of `preprocessingPass` for Collections.  Do not
// use this operator directly; call `_preprocessingPass(s)` instead
func ~> <T: _Collection, R>(
  s: T, args: (_PreprocessingPass, ( (T)->R ))
) -> R? {
  return args.1(s)
}


protocol MutableCollection : Collection {
  subscript(i: IndexType) -> GeneratorType.Element {get set}
}

/// A stream type that could serve for a Collection given that
/// it already had an IndexType.
struct IndexingGenerator<C: _Collection> : Generator, Sequence {
  // Because of <rdar://problem/14396120> we've had to factor _Collection out
  // of Collection to make it useful.

  init(_ seq: C) {
    self._elements = seq
    self._position = seq.startIndex
  }
  
  func generate() -> IndexingGenerator {
    return self
  }
  
  mutating func next() -> C._Element? {
    return _position == _elements.endIndex
    ? .None : .Some(_elements[_position++])
  }
  var _elements: C
  var _position: C.IndexType
}

func indices<
    Seq : Collection>(seq: Seq) -> Range<Seq.IndexType> {
  return Range(start: seq.startIndex, end: seq.endIndex)
}

struct PermutationGenerator<
  C: Collection, Indices: Sequence
  where C.IndexType == Indices.GeneratorType.Element
> : Generator, Sequence {
  var seq : C
  var indices : Indices.GeneratorType

  typealias Element = C.GeneratorType.Element

  mutating func next() -> Element? {
    var result = indices.next()
    return result ? seq[result!] : .None
  }

  // Every Generator is also a single-pass Sequence
  typealias GeneratorType = PermutationGenerator
  func generate() -> GeneratorType {
    return self
  }

  init(elements seq: C, indices: Indices) {
    self.seq = seq
    self.indices = indices.generate()
  }
}

protocol _Sliceable : Collection {}

protocol Sliceable : _Sliceable {
  // FIXME: SliceType should also be Sliceable but we can't express
  // that constraint (<rdar://problem/14375973> Include associated
  // type information in protocol witness tables) Instead we constrain
  // to _Sliceable; at least error messages will be more informative.
  typealias SliceType: _Sliceable
  subscript(_: Range<IndexType>) -> SliceType {get}
}

protocol MutableSliceable : Sliceable, MutableCollection {
  subscript(_: Range<IndexType>) -> SliceType {get set}
}

func dropFirst<Seq : Sliceable>(seq: Seq) -> Seq.SliceType {
  return seq[seq.startIndex.successor()..<seq.endIndex]
}

func dropLast<
  Seq: Sliceable 
  where Seq.IndexType: BidirectionalIndex
>(seq: Seq) -> Seq.SliceType {
  return seq[seq.startIndex..<seq.endIndex.predecessor()]
}
