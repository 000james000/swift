//===--- ArrayType.swift - Protocol for Array-like types ------------------===//
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

@public protocol ArrayType
  : _ArrayType,
    ExtensibleCollection,
    MutableSliceable,
    ArrayLiteralConvertible
{
  //===--- public interface -----------------------------------------------===//
  /// Construct an empty Array
  init()

  /// Construct an array of count elements, each initialized to repeatedValue
  init(count: Int, repeatedValue: Self.GeneratorType.Element)
  
  /// How many elements the Array stores
  var count: Int {get}
  
  /// How many elements the Array can store without reallocation
  var capacity: Int {get}
  
  /// true if and only if the Array is empty
  var isEmpty: Bool {get}

  /// An object that guarantees the lifetime of this array's elements
  var _owner: AnyObject? {get}

  /// If the elements are stored contiguously, a pointer to the first
  /// element. Otherwise, nil.
  var _elementStorageIfContiguous: UnsafePointer<Element> {get}

  subscript(index: Int) -> Self.GeneratorType.Element {get set}
  
  //===--- basic mutations ------------------------------------------------===//

  /// Reserve enough space to store minimumCapacity elements in O(N).
  /// If minimumCapacity is less than count, has no effect.
  /// PostCondition: the array has mutable contiguous storage
  mutating func reserveCapacity(minimumCapacity: Int)
  
  /// Append newElement to the Array in O(1) (amortized)
  mutating func append(newElement: Self.GeneratorType.Element)

  /// Append elements from `sequence` to the Array
  mutating func extend<
      S : Sequence
      where S.GeneratorType.Element == Self.GeneratorType.Element
  >(sequence: S)

  /// Remove an element from the end of the Array in O(1).  Returns:
  /// the removed element. Requires: count > 0
  mutating func removeLast() -> Self.GeneratorType.Element
  
  /// Insert an element at the given index in O(N).  Requires: atIndex
  /// <= count
  mutating func insert(newElement: Self.GeneratorType.Element, atIndex: Int)

  /// Remove the element at the given index.  Returns: the removed
  /// element.  Worst case complexity: O(N).  Requires: count > index
  mutating func removeAtIndex(index: Int) -> Self.GeneratorType.Element

  /// Erase all the elements.  If `keepCapacity` is `true`, `capacity`
  /// will not change
  mutating func removeAll(#keepCapacity: Bool)
  
  //===--- algorithms -----------------------------------------------------===//

  func join<
      S : Sequence where S.GeneratorType.Element == Self
  >(elements: S) -> Self

  func reduce<U>(initial: U, combine: (U, Self.GeneratorType.Element) -> U) -> U

  mutating func sort(
    isOrderedBefore: (
      Self.GeneratorType.Element, Self.GeneratorType.Element
    ) -> Bool
  )
  
  //===--- implementation detail  -----------------------------------------===//

  typealias _Buffer : ArrayBufferType
  init(_ buffer: _Buffer)
  
  var _buffer: _Buffer {get set}
}

@internal struct _ArrayTypeMirror<T : ArrayType> : Mirror {
  let _value : T
  
  init(_ v : T) { _value = v }
  
  var value: Any { return (_value as Any) }

  var valueType: Any.Type { return (_value as Any).dynamicType }

  var objectIdentifier: ObjectIdentifier? { return nil }

  var count: Int { return _value.count }

  subscript(i: Int) -> (String, Mirror) {
    if (i >= 0) && (i < count) {
      return ("[\(i)]",reflect(_value[i]))
    }
    _fatalError("Mirror access out of bounds")
  }

  var summary: String {
    if count == 1 { return "1 element" }
    return "\(count) elements"
  }

  var quickLookObject: QuickLookObject? { return nil }

  var disposition: MirrorDisposition { return .IndexContainer }
}
