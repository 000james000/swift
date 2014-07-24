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

// Swift's String bridges NSString via this protocol and these
// variables, allowing the core stdlib to remain decoupled from
// Foundation.

/// Effectively a proxy for NSString that doesn't mention it by
/// name.  NSString's conformance to this protocol is declared in
/// Foundation.
@objc public protocol _CocoaStringType {}

/// Loading Foundation initializes these function variables
/// with useful values

/// Produces a `_StringBuffer` from a given subrange of a source
/// `_CocoaStringType`, having the given minimum capacity.
public var _cocoaStringToContiguous: (
  source: _CocoaStringType, range: Range<Int>, minimumCapacity: Int
) -> _StringBuffer = _cocoaStringToContiguousNotInitialized

func _cocoaStringToContiguousNotInitialized(
  source: _CocoaStringType, range: Range<Int>, minimumCapacity: Int
) -> _StringBuffer {
  _fatalError("_cocoaStringToContiguous not initialized")
}

/// Reads the entire contents of a _CocoaStringType into contiguous
/// storage of sufficient capacity.
public var _cocoaStringReadAll: (
  source: _CocoaStringType, destination: UnsafeMutablePointer<UTF16.CodeUnit>
) -> Void = _cocoaStringReadAllNotInitialized

func _cocoaStringReadAllNotInitialized(
  source: _CocoaStringType, destination: UnsafeMutablePointer<UTF16.CodeUnit>
) -> Void {
  _fatalError("_cocoaStringReadAll not initialized")
}

public var _cocoaStringLength: (
  source: _CocoaStringType
) -> Int = _cocoaStringLengthNotInitialized

func _cocoaStringLengthNotInitialized(
  source: _CocoaStringType
) -> Int {
  _fatalError("_cocoaStringLength not initialized")
}

public var _cocoaStringSlice: (
  target: _StringCore, subRange: Range<Int>
) -> _StringCore = _cocoaStringSliceNotInitialized

func _cocoaStringSliceNotInitialized(
  target: _StringCore, subRange: Range<Int>
) -> _StringCore {
  _fatalError("_cocoaStringSlice not initialized")
}

public var _cocoaStringSubscript: (
  target: _StringCore, position: Int
) -> UTF16.CodeUnit = _cocoaStringSubscriptNotInitialized

func _cocoaStringSubscriptNotInitialized(
  target: _StringCore, position: Int
) -> UTF16.CodeUnit {
  _fatalError("_cocoaStringSubscript not initialized")
}

import SwiftShims

/// This class is derived from `_NSSwiftStringBase` (through runtime magic),
/// which is derived from `NSString`.
///
/// This allows us to subclass an Objective-C class and use the fast Swift
/// memory allocator.
@objc
public class _NSSwiftString {}

@objc
public protocol _SwiftNSStringRequiredOverridesType :
    _SwiftNSCopyingType, _SwiftNSFastEnumerationType {

  // The following methods should be overridden when implementing an
  // NSString subclass.

  func length() -> Int

  func characterAtIndex(index: Int) -> UInt16

  // We also override the following methods for efficiency.
}

@objc
public protocol _SwiftNSStringType :
    _SwiftNSStringRequiredOverridesType {
}

/// An NSString built around a slice of contiguous Swift String storage
public final class _NSContiguousString : _NSSwiftString {
  public init(_ _core: _StringCore) {
    _sanityCheck(
      _core.hasContiguousStorage,
      "_NSContiguousString requires contiguous storage")
    self._core = _core
    super.init()
  }

  init(coder aDecoder: AnyObject) {
    fatalError("init(coder:) not implemented for _NSContiguousString")
  }

  func length() -> Int {
    return _core.count
  }

  func characterAtIndex(index: Int) -> UInt16 {
    return _core[index]
  }

  func getCharacters(
    buffer: UnsafeMutablePointer<UInt16>,
    range aRange: _SwiftNSRange) {

    _precondition(aRange.location + aRange.length <= Int(_core.count))

    if _core.elementWidth == 2 {
      UTF16.copy(
        _core.startUTF16 + aRange.location,
        destination: UnsafeMutablePointer<UInt16>(buffer),
        count: aRange.length)
    }
    else {
      UTF16.copy(
        _core.startASCII + aRange.location,
        destination: UnsafeMutablePointer<UInt16>(buffer),
        count: aRange.length)
    }
  }

  @objc
  func _fastCharacterContents() -> UnsafeMutablePointer<UInt16> {
    return _core.elementWidth == 2
      ? UnsafeMutablePointer(_core.startUTF16) : nil
  }

  //
  // Implement sub-slicing without adding layers of wrapping
  //
  func substringFromIndex(start: Int) -> _NSContiguousString {
    return _NSContiguousString(_core[Int(start)..<Int(_core.count)])
  }

  func substringToIndex(end: Int) -> _NSContiguousString {
    return _NSContiguousString(_core[0..<Int(end)])
  }

  func substringWithRange(aRange: _SwiftNSRange) -> _NSContiguousString {
    return _NSContiguousString(
      _core[Int(aRange.location)..<Int(aRange.location + aRange.length)])
  }

  func copy() -> AnyObject {
    // Since this string is immutable we can just return ourselves.
    return self
  }

  public let _core: _StringCore
}

extension String {
  /// Same as `_bridgeToObjectiveC()`, but located inside the core standard
  /// library.
  public func _bridgeToObjectiveCImpl() -> AnyObject {
    if let ns = _core.cocoaBuffer {
      if _cocoaStringLength(source: ns) == _core.count {
        return ns
      }
    }
    _sanityCheck(_core.hasContiguousStorage)
    return _NSContiguousString(_core)
  }
}

