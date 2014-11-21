// RUN: rm -rf %t/clang-module-cache
// RUN: %swift %clang-importer-sdk -parse -verify -module-cache-path %t/clang-module-cache -target x86_64-apple-macosx10.9 %s
// RUN: ls -lR %t/clang-module-cache | FileCheck %s
// CHECK: ctypes{{.*}}.pcm

import ctypes
import CoreGraphics
import Foundation
import CoreFoundation

var cgPointVar: CGPoint

func testColor() {
  var c: Color = red
  c = blue
  let _ = c.value
}

func testTribool() {
  var b = Indeterminate
  b = True
  let _ = b.value
}

func testAnonEnum() {
  var a = AnonConst1
  a = AnonConst2
  var a2: CUnsignedLong = a
}

func testAnonEnumSmall() {
  var a = AnonConstSmall1
  a = AnonConstSmall2
  var a2: Int = a
}

func testPoint() -> Float {
  var p: Point
  p.x = 1.0
  return p.y
}

func testAnonStructs() {
  var a_s: AnonStructs
  a_s.a = 5
  a_s.b = 3.14
  a_s.c = 7.5
}

func testBitfieldMembers() {
  var a: StructWithBitfields
  // TODO: Expose the bitfields as properties.
}

// FIXME: Import arrays as real array-looking things.

func testArrays() {
  var fes: NSFastEnumerationState
  var ulong: CUnsignedLong
  var pulong: UnsafeMutablePointer<CUnsignedLong>

  ulong = fes.state
  pulong = fes.mutationsPtr
  ulong = fes.extra.0
  ulong = fes.extra.1
  ulong = fes.extra.2
  ulong = fes.extra.3
  ulong = fes.extra.4
}

// FIXME: Import pointers to opaque types as unique types.

func testPointers() {
  var hWnd: HWND = HWND.null()
  var cfstr: CFString? = nil
  var cfty: CFTypeRef? = cfstr
}

// Ensure that imported structs can be extended, even if typedef'ed on the C
// side.

func sqrt(x: Float) -> Float {}
func atan2(x: Float, y: Float) -> Float {}

extension Point {
  func asPolar() -> (rho: Float, theta: Float) {
    return (sqrt(x*x + y*y), atan2(x, y))
  }
}

extension AnonStructs {
  func frob() -> Double {
    return Double(a) + Double(b) + c
  }
}

extension NSFastEnumerationState {
  func reset() {}
}

extension CGRectTy {
  init(x: Double, y: Double, w: Double, h: Double) {
    origin.x = CGFloat(x)
    origin.y = CGFloat(y)
    size.width = CGFloat(w)
    size.height = CGFloat(h)
  }
}

extension CGRect {
  func printAsX11Geometry() {
    print("\(size.width)x\(size.height)+\(origin.x)+\(origin.y)")
  }
}

func testFuncStructDisambiguation() {
  var a : funcOrStruct
  var i = funcOrStruct()
  i = 5
  var a2 = funcOrStruct(i: 5)
  a2 = a
}

func testVoid() {
  var x: MyVoid // expected-error{{use of undeclared type 'MyVoid'}}
  returnsMyVoid()
}

func testImportMacTypes() {
  // Test that we import integer and floating-point types as swift stdlib
  // types.
  var a : UInt32 = UInt32_test
  a = a + 1

  var b : Float64 = Float64_test
  b = b + 1

  var t9_unqual : Float32 = Float32_test
  var t10_unqual : Float64 = Float64_test

  var t9_qual : ctypes.Float32 = 0.0  // expected-error {{no type named 'Float32' in module 'ctypes'}}
  var t10_qual : ctypes.Float64 = 0.0 // expected-error {{no type named 'Float64' in module 'ctypes'}}

  // FIXME: this should work.  We cannot map Float80 to 'long double' because
  // 'long double' has size of 128 bits on SysV ABI, and 80 != 128.  'long
  // double' is currently not handled by the importer, so it cannot be
  // imported in normal way either.
  var t11_unqual : Float80 = Float80_test // expected-error {{use of unresolved identifier 'Float80_test'}}
  var t11_qual : ctypes.Float80 = 0.0 // expected-error {{no type named 'Float80' in module 'ctypes'}}
}

var word: Word = 0
var uword: UWord = 0

func testImportStdintTypes() {
  var t9_unqual : Int = intptr_t_test
  var t10_unqual : UInt = uintptr_t_test
  t9_unqual = word
  t10_unqual = uword

  var t9_qual : intptr_t = 0 // no-warning
  var t10_qual : uintptr_t = 0 // no-warning
  t9_qual = word
  t10_qual = uword
}

func testImportStddefTypes() {
  var t1_unqual: Int = ptrdiff_t_test
  var t2_unqual: UInt = size_t_test

  var t1_qual: ctypes.ptrdiff_t = t1_unqual
  var t2_qual: ctypes.size_t = t2_unqual
}

func testImportSysTypesTypes() {
  var t1_unqual: Int = ssize_t_test
  var t1_qual: ctypes.ssize_t = t1_unqual
}

func testImportCFTypes() {
  var t1_unqual: Int = CFIndex_test
  var t1_qual: CoreFoundation.CFIndex = t1_unqual
}

func testImportOSTypesTypes() {
  var t1_unqual: CInt = SInt_test
  var t2_unqual: CUnsignedInt = UInt_test

  var t1_qual: ctypes.SInt = t1_unqual // expected-error {{no type named 'SInt' in module 'ctypes'}}
  var t2_qual: ctypes.UInt = t2_unqual // expected-error {{no type named 'UInt' in module 'ctypes'}}
}

func testImportSEL() {
  var t1 : SEL // expected-error {{use of undeclared type 'SEL'}}
  var t2 : ctypes.SEL // expected-error {{no type named 'SEL' in module 'ctypes'}}
}

func testImportTagDeclsAndTypedefs() {
  var t1 = FooStruct1(x: 0, y: 0.0)
  t1.x = 0
  t1.y = 0.0

  var t2 = FooStruct2(x: 0, y: 0.0)
  t2.x = 0
  t2.y = 0.0

  var t3 = FooStruct3(x: 0, y: 0.0)
  t3.x = 0
  t3.y = 0.0

  var t4 = FooStruct4(x: 0, y: 0.0)
  t4.x = 0
  t4.y = 0.0

  var t5 = FooStruct5(x: 0, y: 0.0)
  t5.x = 0
  t5.y = 0.0

  var t6 = FooStruct6(x: 0, y: 0.0)
  t6.x = 0
  t6.y = 0.0
}


func testNoReturnStuff() {
  couldReturnFunction()  // not dead
  couldReturnFunction()  // not dead
  noreturnFunction()

  couldReturnFunction()  // dead
}

func testFunctionPointersAsOpaquePointers() {
  let fp = getFunctionPointer()
  useFunctionPointer(fp)

  let explicitFP: CFunctionPointer<(CInt) -> CInt> = fp
  let opaque = COpaquePointer(explicitFP)
  let reExplicitFP = CFunctionPointer<(CInt) -> CInt>(opaque)

  let wrapper = FunctionPointerWrapper(a: nil, b: nil)
  useFunctionPointer(wrapper.a)
  let _: CFunctionPointer<(CInt) -> CInt> = wrapper.b

  var anotherFP: CFunctionPointer<
    (CInt, CLong, UnsafeMutablePointer<Void>) -> Void
  > = getFunctionPointer2()

  useFunctionPointer2(anotherFP)
  anotherFP = fp // expected-error {{'(CInt, CLong, UnsafeMutablePointer<Void>)' is not identical to 'Int32'}}
}
