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

@available(*, unavailable, message="access the 'count' property on the collection")
public func count <T : CollectionType>(x: T) -> T.Index.Distance {
  fatalError("unavailable function can't be called")
}

/// A protocol representing the minimal requirements of
/// `CollectionType`.
///
/// - Note: In most cases, it's best to ignore this protocol and use
///   `CollectionType` instead, as it has a more complete interface.
//
// This protocol is almost an implementation detail of the standard
// library; it is used to deduce things like the `SubSequence` and
// `Generator` type from a minimal collection, but it is also used in
// exposed places like as a constraint on IndexingGenerator.
public protocol Indexable {
  /// A type that represents a valid position in the collection.
  ///
  /// Valid indices consist of the position of every element and a
  /// "past the end" position that's not valid for use as a subscript.
  typealias Index : ForwardIndexType

  /// The position of the first element in a non-empty collection.
  ///
  /// In an empty collection, `startIndex == endIndex`.
  ///
  /// - Complexity: O(1)
  var startIndex: Index {get}

  /// The collection's "past the end" position.
  ///
  /// `endIndex` is not a valid argument to `subscript`, and is always
  /// reachable from `startIndex` by zero or more applications of
  /// `successor()`.
  ///
  /// - Complexity: O(1)
  var endIndex: Index {get}

  // The declaration of _Element and subscript here is a trick used to
  // break a cyclic conformance/deduction that Swift can't handle.  We
  // need something other than a CollectionType.Generator.Element that can
  // be used as IndexingGenerator<T>'s Element.  Here we arrange for the
  // CollectionType itself to have an Element type that's deducible from
  // its subscript.  Ideally we'd like to constrain this
  // Element to be the same as CollectionType.Generator.Element (see
  // below), but we have no way of expressing it today.
  typealias _Element

  /// Returns the element at the given `position`.
  ///
  /// - Complexity: O(1)
  subscript(position: Index) -> _Element {get}
}

/// A *generator* for an arbitrary *collection*.  Provided `C`
/// conforms to the other requirements of `Indexable`,
/// `IndexingGenerator<C>` can be used as the result of `C`'s
/// `generate()` method.  For example:
///
///      struct MyCollection : CollectionType {
///        struct Index : ForwardIndexType { /* implementation hidden */ }
///        subscript(i: Index) -> MyElement { /* implementation hidden */ }
///        func generate() -> IndexingGenerator<MyCollection> { // <===
///          return IndexingGenerator(self)
///        }
///      }
public struct IndexingGenerator<Elements : Indexable>
 : GeneratorType, SequenceType {
  
  /// Create a *generator* over the given collection.
  public init(_ elements: Elements) {
    self._elements = elements
    self._position = elements.startIndex
  }

  /// Advance to the next element and return it, or `nil` if no next
  /// element exists.
  ///
  /// - Requires: No preceding call to `self.next()` has returned `nil`.
  public mutating func next() -> Elements._Element? {
    return _position == _elements.endIndex
    ? .None : .Some(_elements[_position++])
  }

  internal let _elements: Elements
  internal var _position: Elements.Index
}

/// A multi-pass *sequence* with addressable positions.
///
/// Positions are represented by an associated `Index` type.  Whereas
/// an arbitrary *sequence* may be consumed as it is traversed, a
/// *collection* is multi-pass: any element may be revisited merely by
/// saving its index.
///
/// The sequence view of the elements is identical to the collection
/// view.  In other words, the following code binds the same series of
/// values to `x` as does `for x in self {}`:
///
///     for i in startIndex..<endIndex {
///       let x = self[i]
///     }
public protocol CollectionType : Indexable, SequenceType {
  /// A type that provides the *sequence*'s iteration interface and
  /// encapsulates its iteration state.
  ///
  /// By default, a `CollectionType` satisfies `SequenceType` by
  /// supplying an `IndexingGenerator` as its associated `Generator`
  /// type.
  typealias Generator: GeneratorType = IndexingGenerator<Self>

  // FIXME: Needed here so that the Generator is properly deduced from
  // a custom generate() function.  Otherwise we get an
  // IndexingGenerator. <rdar://problem/21539115>
  func generate() -> Generator
  
  // FIXME: should be constrained to CollectionType
  // (<rdar://problem/20715009> Implement recursive protocol
  // constraints)
  
  /// A `SequenceType` that can represent a contiguous subrange of `self`'s
  /// elements.
  ///
  /// - Note: this associated type appears as a requirement in
  ///   `SequenceType`, but is restated here with stricter
  ///   constraints: in a `CollectionType`, the `SubSequence` should
  ///   also be a `CollectionType`.
  typealias SubSequence: Indexable, SequenceType = Slice<Self>

  /// Returns the element at the given `position`.
  subscript(position: Index) -> Generator.Element {get}
  
  /// Returns a collection representing a contiguous sub-range of
  /// `self`'s elements.
  ///
  /// - Complexity: O(1)
  subscript(bounds: Range<Index>) -> SubSequence {get}

  /// Returns `true` iff `self` is empty.
  var isEmpty: Bool { get }

  /// Returns the number of elements.
  ///
  /// - Complexity: O(1) if `Index` conforms to `RandomAccessIndexType`;
  ///   O(N) otherwise.
  var count: Index.Distance { get }
  
  // The following requirement enables dispatching for indexOf when
  // the element type is Equatable.
  
  /// Returns `Optional(Optional(index))` if an element was found;
  /// `nil` otherwise.
  ///
  /// - Complexity: O(N).
  func _customIndexOfEquatableElement(element: Generator.Element) -> Index??

  /// Returns the first element of `self`, or `nil` if `self` is empty.
  var first: Generator.Element? { get }
}

/// Supply the default `generate()` method for `CollectionType` models
/// that accept the default associated `Generator`,
/// `IndexingGenerator<Self>`.
extension CollectionType where Generator == IndexingGenerator<Self> {
  public func generate() -> IndexingGenerator<Self> {
    return IndexingGenerator(self)
  }
}


/// Supply the default "slicing" `subscript`  for `CollectionType` models
/// that accept the default associated `SubSequence`, `Slice<Self>`.
extension CollectionType where SubSequence == Slice<Self> {
  public subscript(bounds: Range<Index>) -> Slice<Self> {
    return Slice(base: self, bounds: bounds)
  }
}

extension CollectionType where SubSequence == Self {
  /// If `!self.isEmpty`, remove the first element and return it, otherwise
  /// return `nil`.
  ///
  /// - Complexity: O(`self.count`)
  public mutating func popFirst() -> Generator.Element? {
    guard !isEmpty else { return nil }
    let element = first!
    self = self[startIndex.successor()..<endIndex]
    return element
  }

  /// If `!self.isEmpty`, remove the last element and return it, otherwise
  /// return `nil`.
  ///
  /// - Complexity: O(`self.count`)
  public mutating func popLast() -> Generator.Element? {
    guard !isEmpty else { return nil }
    let lastElementIndex = advance(startIndex, numericCast(count) - 1)
    let element = self[lastElementIndex]
    self = self[startIndex..<lastElementIndex]
    return element
  }
}

/// Default implementations of core requirements
extension CollectionType {
  /// Returns `true` iff `self` is empty.
  ///
  /// - Complexity: O(1)
  public var isEmpty: Bool {
    return startIndex == endIndex
  }

  /// Returns the first element of `self`, or `nil` if `self` is empty.
  ///
  /// - Complexity: O(1)
  public var first: Generator.Element? {
    return isEmpty ? nil : self[startIndex]
  }
  
  /// Returns a value less than or equal to the number of elements in
  /// `self`, *nondestructively*.
  ///
  /// - Complexity: O(N).
  public func underestimateCount() -> Int {
    return numericCast(count)
  }

  /// Returns the number of elements.
  ///
  /// - Complexity: O(1) if `Index` conforms to `RandomAccessIndexType`;
  ///   O(N) otherwise.
  public var count: Index.Distance {
    return distance(startIndex, endIndex)
  }
  
  /// Customization point for `SequenceType.indexOf()`.
  ///
  /// Define this method if the collection can find an element in less than
  /// O(N) by exploiting collection-specific knowledge.
  ///
  /// - Returns: `nil` if a linear search should be attempted instead,
  ///   `Optional(nil)` if the element was not found, or
  ///   `Optional(Optional(index))` if an element was found.
  ///
  /// - Complexity: O(N).
  public // dispatching
  func _customIndexOfEquatableElement(_: Generator.Element) -> Index?? {
    return nil
  }
}

/// Algorithms
extension CollectionType {
  /// Return an `Array` containing the results of mapping `transform`
  /// over `self`.
  ///
  /// - Complexity: O(N).
  public func map<T>(
    @noescape transform: (Generator.Element) -> T
  ) -> [T] {
    // Cast away @noescape.
    typealias Transform = (Generator.Element) -> T
    let escapableTransform = unsafeBitCast(transform, Transform.self)

    // The implementation looks exactly the same as
    // `SequenceType.map()`, but it is more efficient, since here we
    // statically know that `self` is a collection, and `lazy(self)`
    // returns an instance of a different type.
    return Array<T>(lazy(self).map(escapableTransform))
  }

  /// Returns an `Array` containing the elements of `self`,
  /// in order, that satisfy the predicate `includeElement`.
  public func filter(
    @noescape includeElement: (Generator.Element) -> Bool
  ) -> [Generator.Element] {
    // Cast away @noescape.
    typealias IncludeElement = (Generator.Element) -> Bool
    let escapableIncludeElement =
      unsafeBitCast(includeElement, IncludeElement.self)
    return Array(lazy(self).filter(escapableIncludeElement))
  }
}

extension SequenceType
  where Self : _ArrayType, Self.Element == Self.Generator.Element {
  // A fast implementation for when you are backed by a contiguous array.
  public func _initializeTo(ptr: UnsafeMutablePointer<Generator.Element>)
    -> UnsafeMutablePointer<Generator.Element> {
    let s = self._baseAddressIfContiguous
    if s != nil {
      let count = self.count
      ptr.initializeFrom(s, count: count)
      _fixLifetime(self._owner)
      return ptr + count
    } else {
      var p = ptr
      for x in self {
        p++.initialize(x)
      }
      return p
    }
  }
}

extension CollectionType {
  public func _preprocessingPass<R>(preprocess: (Self)->R) -> R? {
    return preprocess(self)
  }
}

/// Returns `true` iff `x` is empty.
@available(*, unavailable, message="access the 'isEmpty' property on the collection")
public func isEmpty<C: CollectionType>(x: C) -> Bool {
  fatalError("unavailable function can't be called")
}

/// Returns the first element of `x`, or `nil` if `x` is empty.
@available(*, unavailable, message="access the 'first' property on the collection")
public func first<C: CollectionType>(x: C) -> C.Generator.Element? {
  fatalError("unavailable function can't be called")
}

/// Returns the last element of `x`, or `nil` if `x` is empty.
@available(*, unavailable, message="access the 'last' property on the collection")
public func last<C: CollectionType where C.Index: BidirectionalIndexType>(
  x: C
) -> C.Generator.Element? {
  fatalError("unavailable function can't be called")
}

/// A *collection* that supports subscript assignment.
///
/// For any instance `a` of a type conforming to
/// `MutableCollectionType`, :
///
///     a[i] = x
///     let y = a[i]
///
/// is equivalent to:
///
///     a[i] = x
///     let y = x
///
public protocol MutableCollectionType : CollectionType {

  /// Access the element at `position`.
  ///
  /// - Requires: `position` indicates a valid position in `self` and
  ///   `position != endIndex`.
  ///
  /// - Complexity: O(1)
  subscript(position: Index) -> Generator.Element {get set}

  /// Call `body(p)`, where `p` is a pointer to the collection's
  /// mutable contiguous storage.  If no such storage exists, it is
  /// first created.  If the collection does not support an internal
  /// representation in a form of mutable contiguous storage, `body` is not
  /// called and `nil` is returned.
  ///
  /// Often, the optimizer can eliminate bounds- and uniqueness-checks
  /// within an algorithm, but when that fails, invoking the
  /// same algorithm on `body`\ 's argument lets you trade safety for
  /// speed.
  mutating func _withUnsafeMutableBufferPointerIfSupported<R>(
    @noescape body: (inout UnsafeMutableBufferPointer<Generator.Element>) -> R
  ) -> R?
}

extension MutableCollectionType {
  public mutating func _withUnsafeMutableBufferPointerIfSupported<R>(
  @noescape body: (inout UnsafeMutableBufferPointer<Generator.Element>) -> R
  ) -> R? {
    return nil
  }
}

/// Returns the range of `x`'s valid index values.
///
/// The result's `endIndex` is the same as that of `x`.  Because
/// `Range` is half-open, iterating the values of the result produces
/// all valid subscript arguments for `x`, omitting its `endIndex`.
@available(*, unavailable, message="access the 'indices' property on the collection")
public func indices<
    C : CollectionType>(x: C) -> Range<C.Index> {
  fatalError("unavailable function can't be called")
}

/// A *generator* that adapts a *collection* `C` and any *sequence* of
/// its `Index` type to present the collection's elements in a
/// permuted order.
public struct PermutationGenerator<
  C: CollectionType, Indices: SequenceType
  where C.Index == Indices.Generator.Element
> : GeneratorType, SequenceType {
  var seq : C
  var indices : Indices.Generator

  /// The type of element returned by `next()`.
  public typealias Element = C.Generator.Element

  /// Advance to the next element and return it, or `nil` if no next
  /// element exists.
  ///
  /// - Requires: No preceding call to `self.next()` has returned `nil`.
  public mutating func next() -> Element? {
    let result = indices.next()
    return result != nil ? seq[result!] : .None
  }

  /// Construct a *generator* over a permutation of `elements` given
  /// by `indices`.
  ///
  /// - Requires: `elements[i]` is valid for every `i` in `indices`.
  public init(elements: C, indices: Indices) {
    self.seq = elements
    self.indices = indices.generate()
  }
}

/// A *collection* with mutable slices.
///
/// For example,
///
///      x[i..<j] = someExpression
///      x[i..<j].mutatingMethod()
public protocol MutableSliceable : CollectionType, MutableCollectionType {
  subscript(_: Range<Index>) -> SubSequence { get set }
}

/// Returns a slice containing all but the first element of `s`.
///
/// - Requires: `s` is non-empty.
public func dropFirst<Seq : CollectionType>(s: Seq) -> Seq.SubSequence {
  return s[s.startIndex.successor()..<s.endIndex]
}

/// Returns a slice containing all but the last element of `s`.
///
/// - Requires: `s` is non-empty.
public func dropLast<
  S : CollectionType
  where S.Index: BidirectionalIndexType
>(s: S) -> S.SubSequence {
  return s[s.startIndex..<s.endIndex.predecessor()]
}

/// Returns a slice, up to `maxLength` in length, containing the
/// initial elements of `s`.
///
/// If `maxLength` exceeds `s.count`, the result contains all
/// the elements of `s`.
///
/// - Complexity: O(1)+K when `S.Index` conforms to
///   `RandomAccessIndexType` and O(N)+K otherwise, where K is the cost
///   of slicing `s`.
public func prefix<S : CollectionType>(s: S, _ maxLength: Int) -> S.SubSequence {
  let index = advance(s.startIndex, max(0, numericCast(maxLength)), s.endIndex)
  return s[s.startIndex..<index]
}

/// Returns a slice, up to `maxLength` in length, containing the
/// final elements of `s`.
///
/// If `maxLength` exceeds `s.count`, the result contains all
/// the elements of `s`.
///
/// - Complexity: O(1)+K when `S.Index` conforms to
///   `RandomAccessIndexType` and O(N)+K otherwise, where K is the cost
///   of slicing `s`.
public func suffix<
  S : CollectionType where S.Index: BidirectionalIndexType
>(s: S, _ maxLength: Int) -> S.SubSequence {
  let index = advance(s.endIndex, -max(0, numericCast(maxLength)), s.startIndex)
  return s[index..<s.endIndex]
}

@available(*, unavailable, renamed="CollectionType")
public struct Sliceable {}

