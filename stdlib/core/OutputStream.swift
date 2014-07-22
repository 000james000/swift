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

//===----------------------------------------------------------------------===//
// Input/Output interfaces
//===----------------------------------------------------------------------===//

/// Models an object into into which we can stream text.
public protocol OutputStreamType {
  mutating
  func write(string: String)
}

/// Models an object that can be written to an `OutputStreamType` in a single,
/// immediately obvious, way.
///
/// For example: `String`, `Character`, `UnicodeScalar`.
public protocol Streamable {
  func writeTo<Target : OutputStreamType>(inout target: Target)
}

/// This protocol should be adopted by types that wish to customize their
/// textual representation.  This textual representation is used when objects
/// are written to an `OutputStreamType`.
public protocol Printable {
  var description: String { get }
}

/// This protocol should be adopted by types that wish to customize their
/// textual representation used for debugging purposes.  This textual
/// representation is used when objects are written to an `OutputStreamType`.
public protocol DebugPrintable {
  var debugDescription: String { get }
}

// This protocol is adopted only by NSObject.  This is a workaround for:
// <rdar://problem/16883288> Property of type 'String!' does not satisfy
// protocol requirement of type 'String'
public protocol _PrintableNSObjectType {
  var description: String! { get }
  var debugDescription: String! { get }
}
// end workaround

//===----------------------------------------------------------------------===//
// `print`
//===----------------------------------------------------------------------===//

/// Do our best to print a value that can not be printed directly, using one of
/// its conformances to `Streamable`, `Printable` or `DebugPrintable`.
func _adHocPrint<T, TargetStream : OutputStreamType>(
    object: T, inout target: TargetStream
) {
  var mirror = reflect(object)
  // Checking the mirror kind is not a good way to implement this, but we don't
  // have a more expressive reflection API now.
  if mirror is _TupleMirror {
    print("(", &target)
    var first = true
    for i in 0..<mirror.count {
      if first {
        first = false
      } else {
        print(", ", &target)
      }
      var (label, elementMirror) = mirror[i]
      var elt = elementMirror.value
      // FIXME: uncomment for a compiler crash:
      //_adHocPrint(elt, &target)
      // workaround:
      print(elt, &target)
    }
    print(")", &target)
    return
  }
  print(mirror.summary, &target)
}

/// Writes the textual representation of `object` into the stream `target`.
///
/// The textual representation is obtained from the `object` using its protocol
/// conformances, in the following order of preference: `Streamable`,
/// `Printable`, `DebugPrintable`.
///
/// Do not overload this function for your type.  Instead, adopt one of the
/// protocols mentioned above.
public func print<T, TargetStream : OutputStreamType>(
    object: T, inout target: TargetStream
) {
  if let streamableObject =
      _stdlib_dynamicCastToExistential1(object, Streamable.self) {
    streamableObject.writeTo(&target)
    return
  }

  if var printableObject =
      _stdlib_dynamicCastToExistential1(object, Printable.self) {
    printableObject.description.writeTo(&target)
    return
  }

  if let debugPrintableObject =
      _stdlib_dynamicCastToExistential1(object, DebugPrintable.self) {
    debugPrintableObject.debugDescription.writeTo(&target)
    return
  }

  if let anNSObject =
      _stdlib_dynamicCastToExistential1(object, _PrintableNSObjectType.self) {
    anNSObject.description.writeTo(&target)
    return
  }

  _adHocPrint(object, &target)
}

/// Writes the textual representation of `object` and a newline character into
/// the stream `target`.
///
/// The textual representation is obtained from the `object` using its protocol
/// conformances, in the following order of preference: `Streamable`,
/// `Printable`, `DebugPrintable`.
///
/// Do not overload this function for your type.  Instead, adopt one of the
/// protocols mentioned above.
public func println<T, TargetStream : OutputStreamType>(
    object: T, inout target: TargetStream
) {
  print(object, &target)
  target.write("\n")
}

/// Writes the textual representation of `object` into the standard output.
///
/// The textual representation is obtained from the `object` using its protocol
/// conformances, in the following order of preference: `Streamable`,
/// `Printable`, `DebugPrintable`.
///
/// Do not overload this function for your type.  Instead, adopt one of the
/// protocols mentioned above.
public func print<T>(object: T) {
  var stdoutStream = _Stdout()
  print(object, &stdoutStream)
}

/// Writes the textual representation of `object` and a newline character into
/// the standard output.
///
/// The textual representation is obtained from the `object` using its protocol
/// conformances, in the following order of preference: `Streamable`,
/// `Printable`, `DebugPrintable`.
///
/// Do not overload this function for your type.  Instead, adopt one of the
/// protocols mentioned above.
public func println<T>(object: T) {
  var stdoutStream = _Stdout()
  print(object, &stdoutStream)
  stdoutStream.write("\n")
}

/// Writes a single newline character into the standard output.
public func println() {
  var stdoutStream = _Stdout()
  stdoutStream.write("\n")
}

/// Returns the result of `debugPrint`\ 'ing `x` into a `String`
public func toString<T>(x: T) -> String {
  var result = ""
  print(x, &result)
  return result
}

/// Returns the result of `debugPrint`\ 'ing `x` into a `String`
public func toDebugString<T>(x: T) -> String {
  var result = ""
  debugPrint(x, &result)
  return result
}

//===----------------------------------------------------------------------===//
// `debugPrint`
//===----------------------------------------------------------------------===//

public func debugPrint<T, TargetStream : OutputStreamType>(
    object: T, inout target: TargetStream
) {
  if let debugPrintableObject =
      _stdlib_dynamicCastToExistential1(object, DebugPrintable.self) {
    debugPrintableObject.debugDescription.writeTo(&target)
    return
  }

  if var printableObject =
      _stdlib_dynamicCastToExistential1(object, Printable.self) {
    printableObject.description.writeTo(&target)
    return
  }

  if let anNSObject =
      _stdlib_dynamicCastToExistential1(object, _PrintableNSObjectType.self) {
    anNSObject.debugDescription.writeTo(&target)
    return
  }

  if let streamableObject =
      _stdlib_dynamicCastToExistential1(object, Streamable.self) {
    streamableObject.writeTo(&target)
    return
  }

  _adHocPrint(object, &target)
}

public func debugPrintln<T, TargetStream : OutputStreamType>(
    object: T, inout target: TargetStream
) {
  debugPrint(object, &target)
  target.write("\n")
}

public func debugPrint<T>(object: T) {
  var stdoutStream = _Stdout()
  debugPrint(object, &stdoutStream)
}

public func debugPrintln<T>(object: T) {
  var stdoutStream = _Stdout()
  debugPrint(object, &stdoutStream)
  stdoutStream.write("\n")
}

//===----------------------------------------------------------------------===//
// Conversion of primitive types to `String`
//===----------------------------------------------------------------------===//

/// A 32 byte buffer.
struct _Buffer32 {
  var x0: UInt64 = 0
  var x1: UInt64 = 0
  var x2: UInt64 = 0
  var x3: UInt64 = 0
}

/// A 72 byte buffer.
struct _Buffer72 {
  var x0: UInt64 = 0
  var x1: UInt64 = 0
  var x2: UInt64 = 0
  var x3: UInt64 = 0
  var x4: UInt64 = 0
  var x5: UInt64 = 0
  var x6: UInt64 = 0
  var x7: UInt64 = 0
  var x8: UInt64 = 0
}

@asmname("swift_doubleToString")
func _doubleToStringImpl(
    buffer: UnsafePointer<UTF8.CodeUnit>, bufferLength: UWord, value: Double
) -> UWord

internal func _doubleToString(value: Double) -> String {
  _sanityCheck(sizeof(_Buffer32.self) == 32)
  _sanityCheck(sizeof(_Buffer72.self) == 72)

  var buffer = _Buffer32()
  return withUnsafePointer(&buffer) {
    (bufferPtr) in
    let bufferUTF8Ptr = UnsafePointer<UTF8.CodeUnit>(bufferPtr)
    let actualLength = _doubleToStringImpl(bufferUTF8Ptr, 32, value)
    return String._fromWellFormedCodeUnitSequence(
        UTF8.self,
        input: UnsafeArray(start: bufferUTF8Ptr, length: Int(actualLength)))
  }
}

@asmname("swift_int64ToString")
func _int64ToStringImpl(
    buffer: UnsafePointer<UTF8.CodeUnit>, bufferLength: UWord, value: Int64,
    radix: Int64, uppercase: Bool
) -> UWord

internal func _int64ToString(
    value: Int64, radix: Int64 = 10, uppercase: Bool = false
) -> String {
  if radix >= 10 {
    var buffer = _Buffer32()
    return withUnsafePointer(&buffer) {
      (bufferPtr) in
      let bufferUTF8Ptr = UnsafePointer<UTF8.CodeUnit>(bufferPtr)
      let actualLength =
          _int64ToStringImpl(bufferUTF8Ptr, 32, value, radix, uppercase)
      return String._fromWellFormedCodeUnitSequence(
          UTF8.self,
          input: UnsafeArray(start: bufferUTF8Ptr, length: Int(actualLength)))
    }
  } else {
    var buffer = _Buffer72()
    return withUnsafePointer(&buffer) {
      (bufferPtr) in
      let bufferUTF8Ptr = UnsafePointer<UTF8.CodeUnit>(bufferPtr)
      let actualLength =
          _int64ToStringImpl(bufferUTF8Ptr, 72, value, radix, uppercase)
      return String._fromWellFormedCodeUnitSequence(
          UTF8.self,
          input: UnsafeArray(start: bufferUTF8Ptr, length: Int(actualLength)))
    }
  }
}

@asmname("swift_uint64ToString")
func _uint64ToStringImpl(
    buffer: UnsafePointer<UTF8.CodeUnit>, bufferLength: UWord, value: UInt64,
    radix: Int64, uppercase: Bool
) -> UWord

func _uint64ToString(
    value: UInt64, radix: Int64 = 10, uppercase: Bool = false
) -> String {
  if radix >= 10 {
    var buffer = _Buffer32()
    return withUnsafePointer(&buffer) {
      (bufferPtr) in
      let bufferUTF8Ptr = UnsafePointer<UTF8.CodeUnit>(bufferPtr)
      let actualLength =
          _uint64ToStringImpl(bufferUTF8Ptr, 32, value, radix, uppercase)
      return String._fromWellFormedCodeUnitSequence(
          UTF8.self,
          input: UnsafeArray(start: bufferUTF8Ptr, length: Int(actualLength)))
    }
  } else {
    var buffer = _Buffer72()
    return withUnsafePointer(&buffer) {
      (bufferPtr) in
      let bufferUTF8Ptr = UnsafePointer<UTF8.CodeUnit>(bufferPtr)
      let actualLength =
          _uint64ToStringImpl(bufferUTF8Ptr, 72, value, radix, uppercase)
      return String._fromWellFormedCodeUnitSequence(
          UTF8.self,
          input: UnsafeArray(start: bufferUTF8Ptr, length: Int(actualLength)))
    }
  }
}

func _rawPointerToString(value: Builtin.RawPointer) -> String {
  var result = _uint64ToString(reinterpretCast(value) as UInt64,
      radix: 16, uppercase: false)
  for i in 0..<(2 * sizeof(Builtin.RawPointer) - countElements(result)) {
    result = "0" + result
  }
  return "0x" + result
}

//===----------------------------------------------------------------------===//
// OutputStreams
//===----------------------------------------------------------------------===//

internal struct _Stdout : OutputStreamType {
  mutating func write(string: String) {
    // FIXME: buffering?
    // It is important that we use stdio routines in order to correctly
    // interoperate with stdio buffering.
    for c in string.utf8 {
      _putchar(Int32(c))
    }
  }
}

extension String : OutputStreamType {
  public mutating
  func write(string: String) {
    self += string
  }
}

//===----------------------------------------------------------------------===//
// Streamables
//===----------------------------------------------------------------------===//

extension String : Streamable {
  public func writeTo<Target : OutputStreamType>(inout target: Target) {
    target.write(self)
  }
}

extension Character : Streamable {
  public func writeTo<Target : OutputStreamType>(inout target: Target) {
    target.write(String(self))
  }
}

extension UnicodeScalar : Streamable {
  public func writeTo<Target : OutputStreamType>(inout target: Target) {
    target.write(String(Character(self)))
  }
}
