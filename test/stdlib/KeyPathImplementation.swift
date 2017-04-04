// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest
#if BUILDING_OUTSIDE_STDLIB
import Swift
#endif

var keyPathImpl = TestSuite("key path implementation")

class C<T> {
  var x: Int
  var y: LifetimeTracked?
  var z: T

  init(x: Int, y: LifetimeTracked?, z: T) {
    self.x = x
    self.y = y
    self.z = z
  }
}

struct Point: Equatable {
  var x: Double
  var y: Double
  var trackLifetime = LifetimeTracked(123)
  
  init(x: Double, y: Double) {
    self.x = x
    self.y = y
  }
  
  static func ==(a: Point, b: Point) -> Bool {
    return a.x == b.x && a.y == b.y
  }
}

struct S<T: Equatable>: Equatable {
  var x: Int
  var y: LifetimeTracked?
  var z: T
  var p: Point
  var c: C<T>
  
  static func ==(a: S, b: S) -> Bool {
    return a.x == b.x
      && a.y === b.y
      && a.z == b.z
      && a.p == b.p
      && a.c === b.c
  }
}

// Helper to build keypaths with specific layouts
struct TestKeyPathBuilder {
  var buffer: UnsafeMutableRawBufferPointer
  var state: State = .header

  init(buffer: UnsafeMutableRawBufferPointer) {
    self.buffer = buffer
  }

  enum State {
    case header, component, type
    
    mutating func advance() {
      switch self {
      case .header, .type: self = .component
      case .component:     self = .type
      }
    }
  }
  
  func finish() {
    assert(buffer.count == 0,
           "Did not fill entire buffer")
    assert(state == .type, "should end expecting a type")
  }
  
  mutating func push(_ value: UInt32) {
    assert(buffer.count >= 4, "not enough room")
    buffer.storeBytes(of: value, as: UInt32.self)
    
    buffer = .init(start: buffer.baseAddress! + 4, count: buffer.count - 4)
  }
  
  mutating func addHeader(trivial: Bool, hasReferencePrefix: Bool) {
    assert(state == .header, "not expecting a header")
    let size = buffer.count - 4
    assert(buffer.count > 0 && buffer.count <= 0x3FFF_FFFF,
           "invalid buffer size")
    let header: UInt32 = UInt32(size)
      | (trivial ? 0x8000_0000 : 0)
      | (hasReferencePrefix ? 0x4000_0000 : 0)
    push(header)
    state.advance()
  }
  
  mutating func addOffsetComponent(offset: Int,
                                   kindMask: UInt32,
                                   forceOverflow: Bool,
                                   endsReferencePrefix: Bool) {
    assert(state == .component, "not expecting a component")
    assert(offset >= 0 && offset <= 0x7FFF_FFFF,
           "invalid offset")
    let referencePrefixMask: UInt32 = endsReferencePrefix ? 0x8000_0000 : 0
    if forceOverflow || offset >= 0x1FFF_FFFF {
      // Offset is overflowed into another word
      push(referencePrefixMask | kindMask | 0x1FFF_FFFF)
      push(UInt32(offset))
    } else {
      // Offset is packed in-line
      push(referencePrefixMask | kindMask | UInt32(offset))
    }
    state.advance()
  }
  
  mutating func addStructComponent(offset: Int,
                                   forceOverflow: Bool = false,
                                   endsReferencePrefix: Bool = false) {
    addOffsetComponent(offset: offset, kindMask: 0,
                       forceOverflow: forceOverflow,
                       endsReferencePrefix: endsReferencePrefix)
  }

  mutating func addClassComponent(offset: Int,
                                  forceOverflow: Bool = false,
                                  endsReferencePrefix: Bool = false) {
    addOffsetComponent(offset: offset, kindMask: 0x4000_0000,
                       forceOverflow: forceOverflow,
                       endsReferencePrefix: endsReferencePrefix)
  }
  
  mutating func addType(_ type: Any.Type) {
    assert(state == .type, "not expecting a type")
    if MemoryLayout<Int>.size == 8 {
      // Components are 4-byte aligned, but pointers are 8-byte aligned, so
      // we have to store word-by-word
      let words = unsafeBitCast(type, to: (UInt32, UInt32).self)
      push(words.0)
      push(words.1)
    } else if MemoryLayout<Int>.size == 4 {
      let word = unsafeBitCast(type, to: UInt32.self)
      push(word)
    } else {
      fatalError("unsupported architecture")
    }
    state.advance()
  }
}

extension AnyKeyPath {
  static func build(capacityInBytes: Int,
                    withBuilder: (inout TestKeyPathBuilder) -> Void) -> Self {
    return _create(capacityInBytes: capacityInBytes) {
      var builder = TestKeyPathBuilder(buffer: $0)
      withBuilder(&builder)
      builder.finish()
    }
  }
}

keyPathImpl.test("struct components") {
  let intSize = MemoryLayout<Int>.size
  let stringSize = MemoryLayout<String>.size
  let s_x = WritableKeyPath<S<String>, Int>
    .build(capacityInBytes: 8) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addStructComponent(offset: 0)
    }
  let s_y = WritableKeyPath<S<String>, LifetimeTracked?>
    .build(capacityInBytes: 8) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addStructComponent(offset: intSize)
    }
  let s_z = WritableKeyPath<S<String>, String>
    .build(capacityInBytes: 8) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addStructComponent(offset: intSize*2)
    }
  let s_p = WritableKeyPath<S<String>, Point>
    .build(capacityInBytes: 8) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addStructComponent(offset: intSize*2 + stringSize)
    }
  let twoComponentSize = 12 + intSize
  let s_p_x = WritableKeyPath<S<String>, Double>
    .build(capacityInBytes: twoComponentSize) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addStructComponent(offset: intSize*2 + stringSize)
      $0.addType(Point.self)
      $0.addStructComponent(offset: 0)
    }
  let s_p_y = WritableKeyPath<S<String>, Double>
    .build(capacityInBytes: twoComponentSize) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addStructComponent(offset: intSize*2 + stringSize)
      $0.addType(Point.self)
      $0.addStructComponent(offset: MemoryLayout<Double>.size)
    }

  // \("") forces the string to be computed at runtime (and therefore allocated)
  let c = C(x: 679, y: nil, z: "buffalo\("")")
  var value = _KeyPathBase(base: S(x: 1738, y: nil, z: "bottles of beer\("")",
                                   p: .init(x: 0.5, y: -0.5), c: c))
  expectEqual(value[s_x], 1738)
  value[s_x] = 679
  expectEqual(value[s_x], 679)

  expectTrue(value[s_y] === nil)

  let object1 = LifetimeTracked(1739)
  value[s_y] = object1
  expectTrue(value[s_y] === object1)
  expectTrue(value.base.y === object1)

  let object2 = LifetimeTracked(1740)
  value[s_y] = object2
  expectTrue(value[s_y] === object2)
  expectTrue(value.base.y === object2)

  expectEqual(value[s_z], "bottles of beer")
  value[s_z] = "cans of lemonade\("")"
  expectEqual(value[s_z], "cans of lemonade")
  expectEqual(value.base.z, "cans of lemonade")
  
  expectEqual(value[s_p], Point(x: 0.5, y: -0.5))
  expectEqual(value.base.p, Point(x: 0.5, y: -0.5))
  expectEqual(value[s_p_x], 0.5)
  expectEqual(value.base.p.x, 0.5)
  expectEqual(value[s_p_y], -0.5)
  expectEqual(value.base.p.y, -0.5)

  value[s_p] = Point(x: -3.0, y: 4.0)
  expectEqual(value[s_p], Point(x: -3.0, y: 4.0))
  expectEqual(value.base.p, Point(x: -3.0, y: 4.0))
  expectEqual(value[s_p_x], -3.0)
  expectEqual(value.base.p.x, -3.0)
  expectEqual(value[s_p_y], 4.0)
  expectEqual(value.base.p.y, 4.0)
  
  value[s_p_x] = 5.0
  expectEqual(value[s_p], Point(x: 5.0, y: 4.0))
  expectEqual(value.base.p, Point(x: 5.0, y: 4.0))
  expectEqual(value[s_p_x], 5.0)
  expectEqual(value.base.p.x, 5.0)
  expectEqual(value[s_p_y], 4.0)
  expectEqual(value.base.p.y, 4.0)
  
  value[s_p_y] = -11.0
  expectEqual(value[s_p], Point(x: 5.0, y: -11.0))
  expectEqual(value.base.p, Point(x: 5.0, y: -11.0))
  expectEqual(value[s_p_x], 5.0)
  expectEqual(value.base.p.x, 5.0)
  expectEqual(value[s_p_y], -11.0)
  expectEqual(value.base.p.y, -11.0)
  
  value[s_p].x = 65.0
  expectEqual(value[s_p], Point(x: 65.0, y: -11.0))
  expectEqual(value.base.p, Point(x: 65.0, y: -11.0))
  expectEqual(value[s_p_x], 65.0)
  expectEqual(value.base.p.x, 65.0)
  expectEqual(value[s_p_y], -11.0)
  expectEqual(value.base.p.y, -11.0)
}

keyPathImpl.test("class components") {
  let intSize = MemoryLayout<Int>.size
  let classHeaderSize = intSize * 2
  
  let c_x = ReferenceWritableKeyPath<C<String>, Int>
    .build(capacityInBytes: 8) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addClassComponent(offset: classHeaderSize)
    }
  let c_y = ReferenceWritableKeyPath<C<String>, LifetimeTracked?>
    .build(capacityInBytes: 8) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addClassComponent(offset: classHeaderSize + intSize)
    }
  let c_z = ReferenceWritableKeyPath<C<String>, String>
    .build(capacityInBytes: 8) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addClassComponent(offset: classHeaderSize + 2*intSize)
    }

  let c = C(x: 679, y: nil, z: "buffalo\("")")
  let value = _KeyPathBase(base: c)
  
  expectEqual(value[c_x], 679)
  value[c_x] = 1738
  expectEqual(value[c_x], 1738)
  expectEqual(value.base.x, 1738)

  expectTrue(value[c_y] === nil)

  let object1 = LifetimeTracked(680)
  value[c_y] = object1
  expectTrue(value[c_y] === object1)
  expectTrue(value.base.y === object1)

  let object2 = LifetimeTracked(681)
  value[c_y] = object2
  expectTrue(value[c_y] === object2)
  expectTrue(value.base.y === object2)

  expectEqual(value[c_z], "buffalo")
  value[c_z] = "water buffalo\("")"
  expectEqual(value[c_z], "water buffalo")
  expectEqual(value.base.z, "water buffalo")
  
  var mutValue = value
  let value_c_x: WritableKeyPath = c_x
  let value_c_y: WritableKeyPath = c_y
  
  expectEqual(value[value_c_x], 1738)
  mutValue[value_c_x] = 86
  expectTrue(value.base === mutValue.base)
  expectEqual(value[c_x], 86)
  expectEqual(value[value_c_x], 86)
  expectEqual(value.base.x, 86)

  expectTrue(value[value_c_y] === object2)
  mutValue[value_c_y] = object1
  expectTrue(value.base === mutValue.base)
  expectTrue(value[c_y] === object1)
  expectTrue(value[value_c_y] === object1)
  expectTrue(value.base.y === object1)
}

keyPathImpl.test("reference prefix") {
  let intSize = MemoryLayout<Int>.size
  let stringSize = MemoryLayout<String>.size
  let pointSize = MemoryLayout<Point>.size
  let classHeaderSize = intSize * 2

  let s_c_x = ReferenceWritableKeyPath<S<String>, Int>
    .build(capacityInBytes: 12 + intSize) {
      $0.addHeader(trivial: true, hasReferencePrefix: true)
      $0.addStructComponent(offset: intSize*2 + stringSize + pointSize,
                            endsReferencePrefix: true)
      $0.addType(C<String>.self)
      $0.addClassComponent(offset: classHeaderSize)
    }
  let s_c_y = ReferenceWritableKeyPath<S<String>, LifetimeTracked?>
    .build(capacityInBytes: 12 + intSize) {
      $0.addHeader(trivial: true, hasReferencePrefix: true)
      $0.addStructComponent(offset: intSize*2 + stringSize + pointSize,
                            endsReferencePrefix: true)
      $0.addType(C<String>.self)
      $0.addClassComponent(offset: classHeaderSize + intSize)
    }
  let s_c_z = ReferenceWritableKeyPath<S<String>, String>
    .build(capacityInBytes: 12 + intSize) {
      $0.addHeader(trivial: true, hasReferencePrefix: true)
      $0.addStructComponent(offset: intSize*2 + stringSize + pointSize,
                            endsReferencePrefix: true)
      $0.addType(C<String>.self)
      $0.addClassComponent(offset: classHeaderSize + 2*intSize)
    }
  
  let c = C(x: 679, y: nil, z: "buffalo\("")")
  let value = _KeyPathBase(base: S(x: 1738, y: nil, z: "bottles of beer\("")",
    p: .init(x: 0.5, y: -0.5), c: c))

  expectEqual(value[s_c_x], 679)
  value[s_c_x] = 6
  expectEqual(value[s_c_x], 6)
  expectEqual(value.base.c.x, 6)
  expectTrue(value.base.c.y === nil)
  expectEqual(value.base.c.z, "buffalo")

  let object1 = LifetimeTracked(7)
  let object2 = LifetimeTracked(8)
  expectTrue(value[s_c_y] === nil)

  value[s_c_y] = object1
  expectTrue(value[s_c_y] === object1)
  expectTrue(value.base.c.y === object1)
  expectEqual(value.base.c.x, 6)
  expectEqual(value.base.c.z, "buffalo")

  value[s_c_y] = object2
  expectTrue(value[s_c_y] === object2)
  expectTrue(value.base.c.y === object2)
  expectEqual(value.base.c.x, 6)
  expectEqual(value.base.c.z, "buffalo")
  
  expectEqual(value[s_c_z], "buffalo")
  value[s_c_z] = "antelope"
  expectTrue(value[s_c_z] == "antelope")
  expectTrue(value.base.c.z == "antelope")
  expectEqual(value.base.c.x, 6)
  expectTrue(value.base.c.y === object2)
  
  let value_s_c_x: WritableKeyPath = s_c_x
  let value_s_c_y: WritableKeyPath = s_c_y
  let value_s_c_z: WritableKeyPath = s_c_z
  
  var mutValue = value
  
  mutValue[value_s_c_x] = 7
  expectEqual(value.base, mutValue.base)
  expectEqual(value[s_c_x], 7)
  expectEqual(value.base.c.x, 7)
  expectTrue(value.base.c.y === object2)
  expectEqual(value.base.c.z, "antelope")

  mutValue[value_s_c_y] = object1
  expectEqual(value.base, mutValue.base)
  expectTrue(value[s_c_y] === object1)
  expectTrue(value.base.c.y === object1)
  expectEqual(value.base.c.x, 7)
  expectEqual(value.base.c.z, "antelope")
  
  mutValue[value_s_c_z] = "elk"
  expectEqual(value.base, mutValue.base)
  expectTrue(value[s_c_z] == "elk")
  expectTrue(value.base.c.z == "elk")
  expectEqual(value.base.c.x, 7)
  expectTrue(value.base.c.y === object1)
}

keyPathImpl.test("overflowed offsets") {
  let intSize = MemoryLayout<Int>.size
  let stringSize = MemoryLayout<String>.size
  let classHeaderSize = intSize * 2

  let s_p = WritableKeyPath<S<String>, Point>
    .build(capacityInBytes: 12) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addStructComponent(offset: intSize*2 + stringSize,
                            forceOverflow: true)
    }
  let c_z = ReferenceWritableKeyPath<C<String>, String>
    .build(capacityInBytes: 12) {
      $0.addHeader(trivial: true, hasReferencePrefix: false)
      $0.addClassComponent(offset: classHeaderSize + 2*intSize,
                           forceOverflow: true)
    }
  
  let c = C(x: 679, y: LifetimeTracked(42), z: "buffalo\("")")
  var sValue = _KeyPathBase(base: S(x: 1738, y: LifetimeTracked(43),
                                    z: "bottles of beer\("")",
                                    p: .init(x: 0.5, y: -0.5), c: c))
  let cValue = _KeyPathBase(base: c)
  
  expectEqual(sValue[s_p], Point(x: 0.5, y: -0.5))
  sValue[s_p] = Point(x: 1.0, y: -1.0)
  expectEqual(sValue.base.p, Point(x: 1.0, y: -1.0))
  expectEqual(sValue[s_p], Point(x: 1.0, y: -1.0))
  
  expectEqual(cValue[c_z], "buffalo")
  cValue[c_z] = "dik dik"
  expectEqual(cValue.base.z, "dik dik")
  expectEqual(cValue[c_z], "dik dik")
}

runAllTests()
