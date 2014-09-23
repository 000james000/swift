//===--- Bit.swift - A 1-bit type that can be used as an Index ------------===//
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
//  Used to index SequenceOfOne<T>
//
//===----------------------------------------------------------------------===//

/// A `RandomAccessIndexType` that has two possible values.  Used as
/// the `Index` type for `SequenceOfOne<T>`.
public enum Bit : Int, RandomAccessIndexType, Reflectable {
  case Zero = 0, One = 1

  /// Return the next consecutive value in a discrete sequence of
  /// `Bit` values
  public func successor() -> Bit {
    _precondition(self == .Zero, "Can't increment past one")
    return .One
  }

  /// Return the previous consecutive value in a discrete sequence of
  /// `Bit` values.
  public func predecessor() -> Bit {
    _precondition(self == .One, "Can't decrement past zero")
    return .Zero
  }

  /// Return the minimum number of applications of `successor` or
  /// `predecessor` required to reach `other` from `self`. O(1).
  public func distanceTo(other: Bit) -> Int {
    return rawValue.distanceTo(other.rawValue)
  }

  /// If `n > 0` returns the result of `successor` to `self` `n`
  /// times.  Otherwise, if `n < 0`, returns the result of applying
  /// `predecessor` to `self` `-n` times. Otherwise, returns
  /// `self`. O(1)
  public func advancedBy(distance: Int) -> Bit {
    return rawValue.advancedBy(distance) > 0 ? One : Zero
  }

  /// Reflection
  public func getMirror() -> MirrorType {
    return _BitMirror(self)
  }
}

internal struct _BitMirror: MirrorType {
  let _value: Bit
  
  init(_ v: Bit) {
    self._value = v
  }
  
  var value: Any { return _value }

  var valueType: Any.Type { return (_value as Any).dynamicType }

  var objectIdentifier: ObjectIdentifier? { return .None }

  var count: Int { return 0 }

  subscript(i: Int) -> (String, MirrorType) {
    _preconditionFailure("MirrorType access out of bounds")
  }

  var summary: String { 
    switch _value {
      case .Zero: return ".Zero"
      case .One:  return ".One"
    }
  }

  var quickLookObject: QuickLookObject? { return .None }

  var disposition: MirrorDisposition { return .Enum }
}

public func == (lhs: Bit, rhs: Bit) -> Bool {
  return lhs.rawValue == rhs.rawValue
}

public func < (lhs: Bit, rhs: Bit) -> Bool {
  return lhs.rawValue < rhs.rawValue
}

extension Bit : IntegerArithmeticType {
  static func _withOverflow(v: (Int, overflow: Bool)) -> (Bit, overflow: Bool) {
    return (Bit(rawValue: v.0)!, v.overflow)
  }
  
  public static func addWithOverflow(
    lhs: Bit, _ rhs: Bit
  ) -> (Bit, overflow: Bool) {
    return _withOverflow(Int.addWithOverflow(lhs.rawValue, rhs.rawValue))
  }

  public static func subtractWithOverflow(
    lhs: Bit, _ rhs: Bit
  ) -> (Bit, overflow: Bool) {
    return _withOverflow(Int.subtractWithOverflow(lhs.rawValue, rhs.rawValue))
  }

  public static func multiplyWithOverflow(
    lhs: Bit, _ rhs: Bit
  ) -> (Bit, overflow: Bool) {
    return _withOverflow(Int.multiplyWithOverflow(lhs.rawValue, rhs.rawValue))
  }

  public static func divideWithOverflow(
    lhs: Bit, _ rhs: Bit
  ) -> (Bit, overflow: Bool) {
    return _withOverflow(Int.divideWithOverflow(lhs.rawValue, rhs.rawValue))
  }

  public static func remainderWithOverflow(
    lhs: Bit, _ rhs: Bit
  ) -> (Bit, overflow: Bool) {
    return _withOverflow(Int.remainderWithOverflow(lhs.rawValue, rhs.rawValue))
  }

  /// Represent this number using Swift's widest native signed integer
  /// type.
  public func toIntMax() -> IntMax {
    return IntMax(rawValue)
  }
}
