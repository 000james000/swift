//===--- RangeReplaceableCollectionType.swift -----------------*- swift -*-===//
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
//  A Collection protocol with replaceRange
//
//===----------------------------------------------------------------------===//

public protocol _RangeReplaceableCollectionDefaultsType
  : ExtensibleCollectionType {

  /// Append `x` to `self`.
  ///
  /// Applying `successor()` to the index of the new element yields
  /// `self.endIndex`.
  ///
  /// - complexity: amortized O(1).
  mutating func append(x: Self.Generator.Element)
}

extension _RangeReplaceableCollectionDefaultsType {
  /// Return a collection of the same type, containing the elements
  /// of `self`, in order, that satisfy the predicate `includeElement`.
  final public func _prext_filter(
    @noescape includeElement: (Generator.Element) -> Bool
  ) -> Self {
    var result = Self()
    for e in self {
      if includeElement(e) {
        result.append(e)
      }
    }
    return result
  }
}


/// A *collection* that supports replacement of an arbitrary subRange
/// of elements with the elements of another collection.
public protocol RangeReplaceableCollectionType
  : ExtensibleCollectionType, _RangeReplaceableCollectionDefaultsType {

  //===--- Fundamental Requirements ---------------------------------------===//

  /// Replace the given `subRange` of elements with `newElements`.
  ///
  /// Invalidates all indices with respect to `self`.
  ///
  /// - complexity: O(`count(subRange)`) if
  /// `subRange.endIndex == self.endIndex` and `isEmpty(newElements)`,
  /// O(`count(self)` + `count(newElements)`) otherwise.
  mutating func replaceRange<
    C : CollectionType where C.Generator.Element == Self.Generator.Element
  >(
    subRange: Range<Index>, with newElements: C
  )

  //===--- Derivable Requirements (see free functions below) --------------===//
  /// Insert `newElement` at index `i`.
  ///
  /// Invalidates all indices with respect to `self`.
  ///
  /// - complexity: O(`count(self)`).
  ///
  /// Can be implemented as:
  ///
  ///     Swift.insert(&self, newElement, atIndex: i)
  mutating func insert(newElement: Generator.Element, atIndex i: Index)

  /// Insert `newElements` at index `i`
  ///
  /// Invalidates all indices with respect to `self`.
  ///
  /// - complexity: O(`count(self) + count(newElements)`).
  ///
  /// Can be implemented as:
  ///
  ///     Swift.splice(&self, newElements, atIndex: i)
  mutating func splice<
    S : CollectionType where S.Generator.Element == Generator.Element
  >(newElements: S, atIndex i: Index)

  /// Remove the element at index `i`
  ///
  /// Invalidates all indices with respect to `self`.
  ///
  /// - complexity: O(`count(self)`).
  ///
  /// Can be implemented as:
  ///
  ///     Swift.removeAtIndex(&self, i)
  mutating func removeAtIndex(i: Index) -> Generator.Element

  /// Remove the indicated `subRange` of elements
  ///
  /// Invalidates all indices with respect to `self`.
  ///
  /// - complexity: O(`count(self)`).
  ///
  /// Can be implemented as:
  ///
  ///     Swift.removeRange(&self, subRange)
  mutating func removeRange(subRange: Range<Index>)

  /// Remove all elements
  ///
  /// Invalidates all indices with respect to `self`.
  ///
  /// - parameter keepCapacity: if `true`, is a non-binding request to
  ///    avoid releasing storage, which can be a useful optimization
  ///    when `self` is going to be grown again.
  ///
  /// - complexity: O(`count(self)`).
  ///
  /// Can be implemented as:
  ///
  ///     Swift.removeAll(&self, keepCapacity: keepCapacity)
  mutating func removeAll(keepCapacity keepCapacity: Bool /*= false*/)

  /// Return a collection of the same type, containing the elements
  /// of `self`, in order, that satisfy the predicate `includeElement`.
  func _prext_filter(
    @noescape includeElement: (Generator.Element) -> Bool
  ) -> Self
}

/// Insert `newElement` into `x` at index `i`.
///
/// Invalidates all indices with respect to `x`.
///
/// - complexity: O(`count(x)`).
public func insert<
  C: RangeReplaceableCollectionType
>(inout x: C, _ newElement: C.Generator.Element, atIndex i: C.Index) {
    x.replaceRange(i..<i, with: CollectionOfOne(newElement))
}

/// Insert `newElements` into `x` at index `i`
///
/// Invalidates all indices with respect to `x`.
///
/// - complexity: O(`count(x) + count(newElements)`).
public func splice<
  C: RangeReplaceableCollectionType,
  S : CollectionType where S.Generator.Element == C.Generator.Element
>(inout x: C, _ newElements: S, atIndex i: C.Index) {
  x.replaceRange(i..<i, with: newElements)
}

// FIXME: Trampoline helper to make the typechecker happy.  For some
// reason we can't call x.replaceRange directly in the places where
// this is used.  <rdar://problem/17863882>
internal func _replaceRange<
  C0: RangeReplaceableCollectionType, C1: CollectionType
    where C0.Generator.Element == C1.Generator.Element
>(
  inout x: C0, _ subRange: Range<C0.Index>, with newElements: C1
) {
  x.replaceRange(subRange, with: newElements)
}

/// Remove from `x` and return the element at index `i`
///
/// Invalidates all indices with respect to `x`.
///
/// - complexity: O(`count(x)`).
public func removeAtIndex<
  C: RangeReplaceableCollectionType
>(inout x: C, _ index: C.Index) -> C.Generator.Element {
  _precondition(!isEmpty(x), "can't remove from an empty collection")
  let result: C.Generator.Element = x[index]
  _replaceRange(&x, index...index, with: EmptyCollection())
  return result
}

/// Remove from `x` the indicated `subRange` of elements
///
/// Invalidates all indices with respect to `x`.
///
/// - complexity: O(`count(x)`).
public func removeRange<
  C: RangeReplaceableCollectionType
>(inout x: C, _ subRange: Range<C.Index>) {
  _replaceRange(&x, subRange, with: EmptyCollection())
}

/// Remove all elements from `x`
///
/// Invalidates all indices with respect to `x`.
///
/// - parameter keepCapacity: if `true`, is a non-binding request to
///    avoid releasing storage, which can be a useful optimization
///    when `x` is going to be grown again.
///
/// - complexity: O(`count(x)`).
public func removeAll<
  C: RangeReplaceableCollectionType
>(inout x: C, keepCapacity: Bool = false) {
  if !keepCapacity {
    x = C()
  }
  else {
    _replaceRange(&x, indices(x), with: EmptyCollection())
  }
}

/// Append elements from `newElements` to `x`.  - complexity:
/// O(N)
public func extend<
  C: RangeReplaceableCollectionType,
  S : CollectionType where S.Generator.Element == C.Generator.Element
>(inout x: C, _ newElements: S) {
  x.replaceRange(x.endIndex..<x.endIndex, with: newElements)
}

/// Remove an element from the end of `x`  in O(1).
/// Requires: `x` is nonempty
public func removeLast<
  C: RangeReplaceableCollectionType where C.Index : BidirectionalIndexType
>(
  inout x: C
) -> C.Generator.Element {
  _precondition(!isEmpty(x), "can't removeLast from an empty collection")
  return removeAtIndex(&x, x.endIndex.predecessor())
}

