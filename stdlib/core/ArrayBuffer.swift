//===--- ArrayBuffer.swift - Dynamic storage for Swift Array --------------===//
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
//  This is the class that implements the storage and object management for
//  Swift Array.
//
//===----------------------------------------------------------------------===//

import SwiftShims

enum _ArrayCastKind { case Up, Down, DeferredDown }

@final
class IndirectArrayBuffer {
  
  init<T>(
    nativeBuffer: ContiguousArrayBuffer<T>,
    isMutable: Bool,
    needsElementTypeCheck: Bool
  ) {
    self.buffer = nativeBuffer._storage
    self.isMutable = isMutable
    self.isCocoa = false
    self.needsElementTypeCheck = needsElementTypeCheck
  }
    
  init(cocoa: _CocoaArray, needsElementTypeCheck: Bool) {
    self.buffer = cocoa
    self.isMutable = false
    self.isCocoa = true
    self.needsElementTypeCheck = needsElementTypeCheck
  }

  init<Target>(
    castFrom source: IndirectArrayBuffer,
    toElementType _: Target.Type
  ) {
    self.buffer = source.buffer
    self.isCocoa = source.isCocoa
    
    if source.isCocoa {
      self.isMutable = false
    }
    else {
      self.isMutable = source
        .getNativeBufferOf(AnyObject.self)
        .canStoreElementsOfDynamicType(Target.self)
    }
    
    self.needsElementTypeCheck = source.needsElementTypeCheck
      ? !(AnyObject.self is Target.Type)
      : false
  }
  
  // When this buffer has immutable storage and it is modified, the
  // storage is replaced with mutable storage.
  func replaceStorage<T>(newBuffer: ContiguousArrayBuffer<T>) {
    self.buffer = newBuffer._storage
    self.isMutable = true
    self.isCocoa = false
    self.needsElementTypeCheck = false
  }

  var buffer: AnyObject?
  var isMutable: Bool
  var isCocoa: Bool
  var needsElementTypeCheck: Bool
  
  func getNativeBufferOf<T>(_: T.Type) -> ContiguousArrayBuffer<T> {
    _sanityCheck(!isCocoa)
    return ContiguousArrayBuffer(
      buffer ? reinterpretCast(buffer) as ContiguousArrayStorage<T> : nil)
  }

  func getCocoa() -> _CocoaArray {
    _sanityCheck(isCocoa)
    return reinterpretCast(buffer!) as _CocoaArray
  }
}

let _emptyIndirectArrayBuffer = IndirectArrayBuffer(
  nativeBuffer: _emptyContiguousArrayBuffer,
  isMutable: false,
  needsElementTypeCheck: true
)

enum ArrayBufferStorage<T> {
case Direct(ContiguousArrayStorage<T>)
case Indirect(IndirectArrayBuffer)

  init() {
    _sanityCheck(sizeof(ArrayBufferStorage<T>.self) == sizeof(Word.self))
    if !_isClassOrObjCExistential(T.self) {
      let s: ContiguousArrayStorage = _emptyContiguousArrayBuffer._storage!
      self = Direct(reinterpretCast(s) as ContiguousArrayStorage<T>)
    }
    else {
      self = Indirect(_emptyIndirectArrayBuffer)
    }
  }

  init(_ source: ContiguousArrayBuffer<T>) {
    if !_isClassOrObjCExistential(T.self) {
      self = source ? Direct(source._storage!) : ArrayBufferStorage()
    }
    else {
      self = Indirect(
        IndirectArrayBuffer(
          nativeBuffer: source, isMutable: true, needsElementTypeCheck: false))
    }
  }

  mutating func isUniquelyReferenced() -> Bool {
    // Extract the buffer without increasing its refcount
    var x = self.bufferAsWord()
    return Swift.isUniquelyReferenced(&x)
  }

  func bufferAsWord() -> Word {
    switch self {
    case Direct(let x):
      return reinterpretCast(x) as Word
    case Indirect(let x):
      return reinterpretCast(x) as Word
    }
  }
}

struct ArrayBuffer<T> : ArrayBufferType {
  var storage: ArrayBufferStorage<T> = ArrayBufferStorage()

  var indirect: IndirectArrayBuffer {
    _sanityCheck(_isClassOrObjCExistential(T.self))
    switch storage {
    case .Indirect(let x):
      return x
    default:
      _fatalError("Array of Class or ObjC existential without indirect buffer")
    }
  }
  
  typealias Element = T

  /// create an empty buffer
  init() {}

  init(_ cocoa: _CocoaArray) {
    _sanityCheck(_isClassOrObjCExistential(T.self))
    storage = .Indirect(
      IndirectArrayBuffer(
        cocoa: cocoa,
        // FIXME: it may be possible to avoid a deferred check if we can
        // verify that source is backed by a ContiguousArray<T>.
        needsElementTypeCheck: !(AnyObject.self is T.Type)))
  }

  init(_ buffer: IndirectArrayBuffer) {
    storage = .Indirect(buffer)
  }
  
  /// Returns an `ArrayBuffer<U>` containing the same elements.
  /// Requires: the elements actually have dynamic type `U`, and `U`
  /// is a class or `@objc` existential.
  func castToBufferOf<U>(_: U.Type) -> ArrayBuffer<U> {
    _sanityCheck(_isClassOrObjCExistential(T.self))
    _sanityCheck(_isClassOrObjCExistential(U.self))
    return ArrayBuffer<U>(
      IndirectArrayBuffer(castFrom: self.indirect, toElementType: U.self))
  }
}

extension ArrayBuffer {
  /// Adopt the storage of source
  init(_ source: NativeBuffer) {
    storage = ArrayBufferStorage(source)
  }
  
  /// Return true iff this buffer's storage is uniquely-referenced.
  mutating func isUniquelyReferenced() -> Bool {
    return storage.isUniquelyReferenced()
  }

  /// Convert to an NSArray.
  /// Precondition: isBridgedToObjectiveC(Element.self)
  /// O(1) if the element type is bridged verbatim, O(N) otherwise
  func _asCocoaArray() -> _CocoaArray {
    _sanityCheck(
      isBridgedToObjectiveC(T.self),
      "Array element type is not bridged to ObjectiveC")

    return _fastPath(_isNative) ? _native._asCocoaArray() : _nonNative!
  }

  var _hasMutableBuffer: Bool {
    if !_isClassOrObjCExistential(T.self) {
      return true
    }
    return indirect.isMutable && Swift.isUniquelyReferenced(&indirect.buffer)
  }

  /// If this buffer is backed by a uniquely-referenced mutable
  /// ContiguousArrayBuffer that can be grown in-place to allow the self
  /// buffer store minimumCapacity elements, returns that buffer.
  /// Otherwise, returns nil
  mutating func requestUniqueMutableBuffer(minimumCapacity: Int)
    -> NativeBuffer?
  {
    if _fastPath(storage.isUniquelyReferenced() && _hasMutableBuffer) {
      let b = _native
      return b.capacity >= minimumCapacity ? b : nil
    }
    return nil
  }

  mutating func isMutableAndUniquelyReferenced() -> Bool {
    return storage.isUniquelyReferenced() && _hasMutableBuffer
  }
  
  /// If this buffer is backed by a ContiguousArrayBuffer, return it.
  /// Otherwise, return nil.  Note: the result's elementStorage may
  /// not match ours, if we are a SliceBuffer.
  func requestNativeBuffer() -> NativeBuffer? {
    let result = self._native
    if result { return result }
    return nil
  }

  func _typeCheck(subRange: Range<Int>) {
    if !_isClassOrObjCExistential(T.self) {
      return
    }
    if _slowPath(indirect.needsElementTypeCheck) {
      if _fastPath(_isNative) {
        for x in _native[subRange] {
          _precondition(
            reinterpretCast(x) as AnyObject is T,
            "NSArray element failed to match the Swift Array Element type")
        }
      }
      else if (subRange) {
        let ns = _nonNative!
        // Could be sped up, e.g. by using
        // enumerateObjectsAtIndexes:options:usingBlock:
        for i in subRange {
          _precondition(ns.objectAtIndex(i) is T,
            "NSArray element failed to match the Swift Array Element type")
        }
      }
    }
  }
  
  /// Copy the given subRange of this buffer into uninitialized memory
  /// starting at target.  Return a pointer past-the-end of the
  /// just-initialized memory.
  func _uninitializedCopy(subRange: Range<Int>, target: UnsafePointer<T>)
         -> UnsafePointer<T> {
    _typeCheck(subRange)
    if _fastPath(_isNative) {
      return _native._uninitializedCopy(subRange, target: target)
    }

    let nonNative = _nonNative!

    let nsSubRange = SwiftShims._SwiftNSRange(
      location:subRange.startIndex,
      length: subRange.endIndex - subRange.startIndex)

    let buffer = reinterpretCast(target) as UnsafePointer<AnyObject>
    
    // Copies the references out of the NSArray without retaining them
    nonNative.getObjects(buffer, range: nsSubRange)
    
    // Make another pass to retain the copied objects
    var result = target
    for i in subRange {
      result.initialize(result.memory)
      ++result
    }
    return result
  }
  
  /// Return a SliceBuffer containing the given subRange of values
  /// from this buffer.
  subscript(subRange: Range<Int>) -> SliceBuffer<T> {
    _typeCheck(subRange)
    
    if _fastPath(_isNative) {
      return _native[subRange]
    }

    let nonNative = self._nonNative

    let subRangeCount = countElements(subRange)
    
    // Look for contiguous storage in the NSArray
    let cocoa = _CocoaArrayWrapper(nonNative!)
    let start = cocoa.contiguousStorage(subRange)
    if start != nil {
      return SliceBuffer(owner: nonNative, start: UnsafePointer(start),
                         count: subRangeCount, hasNativeBuffer: false)
    }
    
    // No contiguous storage found; we must allocate
    var result = ContiguousArrayBuffer<T>(count: subRangeCount, minimumCapacity: 0)

    // Tell Cocoa to copy the objects into our storage
    cocoa.buffer.getObjects(
      UnsafePointer(result.elementStorage),
      range: _SwiftNSRange(location: subRange.startIndex, length: subRangeCount)
    )
    
    return SliceBuffer(result)
  }

  /// If the elements are stored contiguously, a pointer to the first
  /// element. Otherwise, nil.
  var elementStorage: UnsafePointer<T> {
    if (_fastPath(_isNative)) {
      return _native.elementStorage
    }
    return nil
  }
  
  /// How many elements the buffer stores
  var count: Int {
    get {
      return _fastPath(_isNative) ? _native.count : _nonNative!.count
    }
    set {
      // Allow zero here for the case where elements have been moved
      // out of the buffer during reallocation
      _sanityCheck(
        newValue == 0 || newValue >= count,
        "We don't yet know how to shrink an array")
      
      _sanityCheck(_isNative, "attempting to update count of Cocoa array")
      _native.count = newValue
    }
  }
  
  /// How many elements the buffer can store without reallocation
  var capacity: Int {
    return _fastPath(_isNative) ? _native.capacity : _nonNative!.count
  }

  /// Get/set the value of the ith element
  subscript(i: Int) -> T {
    get {
      _typeCheck(i..i)
      if _fastPath(_isNative) {
        return _native[i]
      }
      return reinterpretCast(_nonNative!.objectAtIndex(i))
    }
    
    nonmutating set {
      if _fastPath(_hasMutableBuffer) {
        _native[i] = newValue
      }
      else {
        indirect.replaceStorage(_copyCollectionToNativeArrayBuffer(self))
        _native[i] = newValue
      }
    }
  }

  /// Call body(p), where p is a pointer to the underlying contiguous storage
  /// Requires: such contiguous storage exists or the buffer is empty
  func withUnsafePointerToElements<R>(body: (UnsafePointer<T>)->R) -> R {
    _precondition(
      elementStorage != nil || count == 0,
      "Array is bridging an opaque NSArray; can't get a pointer to the elements"
    )
    let ret = body(elementStorage)
    _fixLifetime(self)
    return ret
  }
  
  /// An object that keeps the elements stored in this buffer alive
  var owner: AnyObject? {
    return _fastPath(_isNative) ? _native._storage : _nonNative!
  }
  
  /// A value that identifies first mutable element, if any.  Two
  /// arrays compare === iff they are both empty or if their buffers
  /// have the same identity and count.
  var identity: Word {
    if count == 0 {
      return 0
    }
    return reinterpretCast(elementStorage)
  }
  
  //===--- Collection conformance -----------------------------------------===//
  var startIndex: Int {
    return 0
  }
  
  var endIndex: Int {
    return count
  }

  func generate() -> IndexingGenerator<ArrayBuffer> {
    return IndexingGenerator(self)
  }
  
  //===--- private --------------------------------------------------------===//
  typealias Storage = ContiguousArrayStorage<T>
  typealias NativeBuffer = ContiguousArrayBuffer<T>

  func _invariantCheck() -> Bool {
    return true
  }

  var _isNative: Bool {
    if !_isClassOrObjCExistential(T.self) {
      return true
    }
    else {
      return !indirect.isCocoa
    }
  }

  /// Our native representation, if any.  If there's no native
  /// representation, the result is an empty buffer.
  var _native: NativeBuffer {
    if !_isClassOrObjCExistential(T.self) {
      switch storage {
      case .Direct(let x):
        return reinterpretCast(x) as Int
          == reinterpretCast(_emptyContiguousArrayBuffer)
          ? NativeBuffer() : NativeBuffer(x)
      default:
        _fatalError("Array of non-class or -ObjC existential is indirect")
      }
    }
    else {
      let i = indirect
      return _fastPath(!i.isCocoa)
        ? i.getNativeBufferOf(T.self) 
        : NativeBuffer()
    }
  }

  var _nonNative: _CocoaArray? {
    if !_isClassOrObjCExistential(T.self) {
      return nil
    }
    else {
      let i = indirect
      return _fastPath(!i.isCocoa) ? nil : i.getCocoa()
    }
  }
}

