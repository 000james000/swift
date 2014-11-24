// RUN: %target-run-stdlib-swift | FileCheck %s
// XFAIL: linux

import Foundation
import Swift

// ==== Tests =====

func hexAddrVal<T>(x: T) -> String {
  return "@0x" + String(UInt64(unsafeBitCast(x, Word.self)), radix: 16)
}

func hexAddr(x: AnyObject?) -> String {
  if let owner: AnyObject = x {
    if let y = owner as? _StringBuffer._Storage.Storage {
      return ".Native\(hexAddrVal(y))"
    }
    if let y = owner as? NSString {
      return ".Cocoa\(hexAddrVal(y))"
    }
    else {
      return "?Uknown?\(hexAddrVal(owner))"
    }
  }
  return "null"
}

func repr(x: NSString) -> String {
  return "\(NSStringFromClass(object_getClass(x)))\(hexAddrVal(x)) = \"\(x)\""
}

func repr(x: _StringCore) -> String {
  if x.hasContiguousStorage {
    if let b = x.nativeBuffer {
    var offset = x.elementWidth == 2
      ? UnsafeMutablePointer(b.start) - x.startUTF16
      : UnsafeMutablePointer(b.start) - x.startASCII
      return "Contiguous(owner: "
      + "\(hexAddr(x._owner))[\(offset)...\(x.count + offset)]"
      + ", capacity = \(b.capacity))"
    }
    return "Contiguous(owner: \(hexAddr(x._owner)), count: \(x.count))"
  }
  else if let b2 = x.cocoaBuffer {
    return "Opaque(buffer: \(hexAddr(b2))[0...\(x.count)])"
  }
  return "?????"
}

func repr(x: String) -> String {
  return "String(\(repr(x._core))) = \"\(x)\""
}

// CHECK: Testing
println("Testing...")

//===--------- Native Strings ---------===

// Force the string literal representation into a Native, heap-allocated buffer
var nsb = "🏂☃❅❆❄︎⛄️❄️"
// CHECK-NEXT: Hello, snowy world: 🏂☃❅❆❄︎⛄️❄️
println("Hello, snowy world: \(nsb)")
// CHECK-NEXT: String(Contiguous(owner: null, count: 11))
println("  \(repr(nsb))")

var empty = String()
// CHECK-NEXT: These are empty: <>
println("These are empty: <\(empty)>")
// CHECK-NEXT: String(Contiguous(owner: null, count: 0))
println("  \(repr(empty))")


//===--------- non-ASCII ---------===

func nonASCII() {
  // Cocoa stores non-ASCII in a UTF-16 buffer
  // Code units in each character: 2 1 1 1 2 2 2
  // Offset of each character:     0 2 3 4 5 7 9 11
  var nsUTF16 = NSString(UTF8String: "🏂☃❅❆❄︎⛄️❄️")!
  // CHECK-NEXT: has UTF-16: true
  println("has UTF-16: \(CFStringGetCharactersPtr(unsafeBitCast(nsUTF16, CFString.self)) != nil)")

  // CHECK: --- UTF-16 basic round-tripping ---
  println("--- UTF-16 basic round-tripping ---")

  // check that no extraneous objects are created
  // CHECK-NEXT: __NSCFString@[[utf16address:[x0-9a-f]+]] = "🏂☃❅❆❄︎⛄️❄️"
  println("  \(repr(nsUTF16))")

  // CHECK-NEXT: String(Contiguous(owner: .Cocoa@[[utf16address]], count: 11))
  var newNSUTF16 = nsUTF16 as String
  println("  \(repr(newNSUTF16))")

  // CHECK-NEXT: __NSCFString@[[utf16address]] = "🏂☃❅❆❄︎⛄️❄️"
  var nsRoundTripUTF16: NSString = newNSUTF16
  println("  \(repr(nsRoundTripUTF16))")

  // CHECK: --- UTF-16 slicing ---
  println("--- UTF-16 slicing ---")

  // Slicing the String does not allocate
  // CHECK-NEXT: String(Contiguous(owner: .Cocoa@[[utf16address]], count: 6))
  let i2 = advance(newNSUTF16.startIndex, 2)
  let i8 = advance(newNSUTF16.startIndex, 6)
  println("  \(repr(newNSUTF16[i2..<i8]))")

  // Representing a slice as an NSString requires a new object
  // CHECK-NOT: NSString@[[utf16address]] = "❅❆❄︎⛄️"
  // CHECK-NEXT: _NSContiguousString@[[nsContiguousStringAddress:[x0-9a-f]+]] = "❅❆❄︎⛄️"
  var nsSliceUTF16: NSString = newNSUTF16[i2..<i8]
  println("  \(repr(nsSliceUTF16))")

  // Check that we can recover the original buffer
  // CHECK-NEXT: String(Contiguous(owner: .Cocoa@[[utf16address]], count: 6))
  println("  \(repr(nsSliceUTF16 as String))")
}
nonASCII()

//===--------- ASCII ---------===

func ascii() {
  // Cocoa stores ASCII in a buffer of bytes.  This is an important case
  // because it doesn't provide a contiguous array of UTF-16, so we'll be
  // treating it as an opaque NSString.
  var nsASCII = NSString(UTF8String: "foobar")!
  // CHECK-NEXT: has UTF-16: false
  println("has UTF-16: \(CFStringGetCharactersPtr(unsafeBitCast(nsASCII, CFString.self)) != nil)")

  // CHECK: --- ASCII basic round-tripping ---
  println("--- ASCII basic round-tripping ---")

  // CHECK-NEXT: [[nsstringclass:(__NSCFString|NSTaggedPointerString)]]@[[asciiaddress:[x0-9a-f]+]] = "foobar"
  println("  \(repr(nsASCII))")

  // CHECK-NEXT NO: String(Opaque(buffer: @[[asciiaddress]][0...6]))
  var newNSASCII = nsASCII as String
  // println("  \(repr(newNSASCII))")

  // CHECK-NEXT: [[nsstringclass]]@[[asciiaddress]] = "foobar"
  var nsRoundTripASCII: NSString = newNSASCII
  println("  \(repr(nsRoundTripASCII))")

  // CHECK: --- ASCII slicing ---
  println("--- ASCII slicing ---")

  let i3 = advance(newNSASCII.startIndex, 3)
  let i6 = advance(newNSASCII.startIndex, 6)
  
  // Slicing the String does not allocate
  // XCHECK-NEXT: String(Opaque(buffer: @[[asciiaddress]][3...6]))
  println("  \(repr(newNSASCII[i3..<i6]))")

  // Representing a slice as an NSString requires a new object
  // XCHECK-NOT: NSString@[[asciiaddress]] = "bar"
  // XCHECK-NEXT: _NSOpaqueString@[[nsOpaqueSliceAddress:[x0-9a-f]+]] = "bar"
  var nsSliceASCII: NSString = newNSASCII[i3..<i6]
  println("  \(repr(nsSliceASCII))")

  // When round-tripped back to Swift, the _NSOpaqueString object is the new owner
  // XCHECK-NEXT: String(Opaque(buffer: @[[nsOpaqueSliceAddress]][0...3]))
  println("  \(repr(nsSliceASCII as String))")
}
ascii()

//===-------- Literals --------===

// String literals default to UTF-16.

// CHECK: --- Literals ---
println("--- Literals ---")

// CHECK-NEXT: String(Contiguous(owner: null, count: 6)) = "foobar"
// CHECK-NEXT: true
var asciiLiteral: String = "foobar"
println("  \(repr(asciiLiteral))")
println("  \(asciiLiteral._core.isASCII)")

// CHECK-NEXT: String(Contiguous(owner: null, count: 11)) = "🏂☃❅❆❄︎⛄️❄️"
// CHECK-NEXT: false
var nonASCIILiteral: String = "🏂☃❅❆❄︎⛄️❄️"
println("  \(repr(nonASCIILiteral))")
println("  \(!asciiLiteral._core.isASCII)")

// ===------- Appending -------===

// These tests are in NewStringAppending.swift.

// ===---------- Comparison --------===

var s = "ABCDEF"
var s1 = s + "G"

// CHECK-NEXT: true
println("\(s) == \(s) => \(s == s)")

// CHECK-NEXT: false
println("\(s) == \(s1) => \(s == s1)")

// CHECK-NEXT: true
let abcdef: String = "ABCDEF"
println("\(s) == \"\(abcdef)\" => \(s == abcdef)")

let so: String = "so"
let sox: String = "sox"
let tocks: String = "tocks"

// CHECK-NEXT: false
println("so < so => \(so < so)")
// CHECK-NEXT: true
println("so < sox => \(so < sox)")
// CHECK-NEXT: true
println("so < tocks => \(so < tocks)")
// CHECK-NEXT: true
println("sox < tocks => \(sox < tocks)")

let qqq = nonASCIILiteral.hasPrefix("🏂☃")
let rrr = nonASCIILiteral.hasPrefix("☃")
let zz = (
  nonASCIILiteral.hasPrefix("🏂☃"), nonASCIILiteral.hasPrefix("☃"),
  nonASCIILiteral.hasSuffix("⛄️❄️"), nonASCIILiteral.hasSuffix("☃"))

// CHECK-NEXT: <true, false, true, false>
println("<\(zz.0), \(zz.1), \(zz.2), \(zz.3)>")

// ===---------- Interpolation --------===

// CHECK-NEXT: {{.*}}"interpolated: foobar 🏂☃❅❆❄︎⛄️❄️ 42 3.14 true"
s = "interpolated: \(asciiLiteral) \(nonASCIILiteral) \(42) \(3.14) \(true)"
println("\(repr(s))")

// ===---------- Views --------===

let winter = "🏂☃❅❆❄︎⛄️❄️"
let summer = "school's out!"

func printHexSequence<
  S:SequenceType where S.Generator.Element : IntegerType
>(s: S) {
  print("[")
  var prefix = ""
  for x in s {
    print(prefix);
    print(String(x.toIntMax(), radix: 16))
    prefix = " "
  }
  println("]")
}

// CHECK-NEXT: [f0 9f 8f 82 e2 98 83 e2 9d 85 e2 9d 86 e2 9d 84 ef b8 8e e2 9b 84 ef b8 8f e2 9d 84 ef b8 8f]
printHexSequence(winter.utf8)
// CHECK-NEXT: [d83c dfc2 2603 2745 2746 2744 fe0e 26c4 fe0f 2744 fe0f]
printHexSequence(winter.utf16)
// CHECK-NEXT: [73 63 68 6f 6f 6c 27 73 20 6f 75 74 21]
printHexSequence(summer.utf8)
// CHECK-NEXT: [73 63 68 6f 6f 6c 27 73 20 6f 75 74 21]
printHexSequence(summer.utf16)

// ===---------- Done --------===
// CHECK-NEXT: Done.
println("Done.")

