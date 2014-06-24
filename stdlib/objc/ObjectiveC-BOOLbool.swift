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

@exported
import ObjectiveC

/// The Objective-C BOOL type.
///
/// On iOS, the Objective-C BOOL type is a typedef of C/C++ bool. Clang 
/// importer imports it as ObjCBool.
///
/// The compiler has special knowledge of this type.
///
/// FIXME: It may be possible to replace this with `typealias ObjCBool = Bool`.
@public struct ObjCBool {
  var value : Bool

  @public init(_ value: Bool) {
    self.value = value
  }

  /// Allow use in a Boolean context.
  @public func getLogicValue() -> Bool {
    return value == true
  }

  /// Implicit conversion from C Boolean type to Swift Boolean type.
  @conversion @public func __conversion() -> Bool {
    return self.getLogicValue()
  }
}

extension Bool {
  /// Implicit conversion from Swift Boolean type to
  /// Objective-C Boolean type.
  @conversion @public func __conversion() -> ObjCBool {
    return ObjCBool(self ? Bool.true : Bool.false)
  }
}
