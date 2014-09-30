// RUN: %target-run-simple-swift
// XFAIL: interpret

import StdlibUnittest

//===---
// Utilities.
//===---

@asmname("random") func random() -> UInt32
@asmname("srandomdev") func srandomdev()

// Single Unicode scalars that occupy a variety of bits in UTF-8.
//
// These scalars should be "base characters" with regards to their position in
// a grapheme cluster.
let baseScalars = [
  // U+0065 LATIN SMALL LETTER E
  "\u{0065}",

  // U+006F LATIN SMALL LETTER O
  "\u{006f}",

  // U+00A9 COPYRIGHT SIGN
  "\u{00a9}",

  // U+0122 LATIN CAPITAL LETTER G WITH CEDILLA
  "\u{0122}",

  // U+0521 CYRILLIC SMALL LETTER EL WITH MIDDLE HOOK
  "\u{0521}",

  // U+0ED2 LAO DIGIT TWO
  "\u{0ed2}",

  // U+4587 CJK UNIFIED IDEOGRAPH-4587
  "\u{4587}",

  // U+B977 HANGUL SYLLABLE REUGS
  "\u{b977}",

  // U+BF01 HANGUL SYLLABLE BBENG
  "\u{bf01}",

  // U+1D452 MATHEMATICAL ITALIC SMALL E
  "\u{1d452}",

  // U+1E825 MENDE KIKAKUI SYLLABLE M163 EE
  "\u{1e825}",

  // U+10B9C4 (private use)
  "\u{10b9c4}",
]

// Single Unicode scalars that are "continuing characters" with regards to
// their position in a grapheme cluster.
let continuingScalars = [
  // U+0300 COMBINING GRAVE ACCENT
  "\u{0300}",

  // U+0308 COMBINING DIAERESIS
  "\u{0308}",

  // U+0903 DEVANAGARI SIGN VISARGA
  "\u{0903}",

  // U+200D ZERO WIDTH JOINER
  "\u{200D}",
]

let testCharacters = [
  // U+000D CARRIAGE RETURN (CR)
  // U+000A LINE FEED (LF)
  "\u{000d}\u{000a}",

  // Grapheme clusters that have UTF-8 representations of length 1..10 bytes.

  // U+0061 LATIN SMALL LETTER A
  // U+0300 COMBINING GRAVE ACCENT
  "\u{0061}", // UTF-8: 1 byte
  "\u{0061}\u{0300}", // UTF-8: 3 bytes
  "\u{0061}\u{0300}\u{0300}", // UTF-8: 5 bytes
  "\u{0061}\u{0300}\u{0300}\u{0300}", // UTF-8: 7 bytes
  "\u{0061}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 9 bytes

  // U+00A9 COPYRIGHT SIGN
  // U+0300 COMBINING GRAVE ACCENT
  "\u{00a9}", // UTF-8: 2 bytes
  "\u{00a9}\u{0300}", // UTF-8: 4 bytes
  "\u{00a9}\u{0300}\u{0300}", // UTF-8: 6 bytes
  "\u{00a9}\u{0300}\u{0300}\u{0300}", // UTF-8: 8 bytes
  "\u{00a9}\u{0300}\u{0300}\u{0300}\u{0300}", // UTF-8: 10 bytes
]

func randomGraphemeCluster(minSize: Int, maxSize: Int) -> String {
  var n = Int(random()) % (maxSize - minSize) + minSize - 1
  var result = baseScalars[Int(random()) % baseScalars.count]
  for i in 0..<n {
    result += continuingScalars[Int(random()) % continuingScalars.count]
  }
  return result
}

//===---
// Tests.
//===---

var CharacterTests = TestSuite("Character")

CharacterTests.test("literal") {
  if true {
    // U+0041 LATIN CAPITAL LETTER A
    let ch: Character = "A"
    expectEqual("\u{0041}", String(ch))
  }

  if true {
    // U+3042 HIRAGANA LETTER A
    let ch: Character = "あ"
    expectEqual("\u{3042}", String(ch))
  }

  if true {
    // U+4F8B CJK UNIFIED IDEOGRAPH-4F8B
    let ch: Character = "例"
    expectEqual("\u{4F8B}", String(ch))
  }

  if true {
    // U+304B HIRAGANA LETTER KA
    // U+3099 COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK
    let ch: Character = "\u{304b}\u{3099}"
    expectEqual("\u{304b}\u{3099}", String(ch))
  }
}

CharacterTests.test("sizeof") {
  // FIXME: should be 8.
  // <rdar://problem/16754935> sizeof(Character.self) is 9, should be 8

  let size1 = sizeof(Character.self)
  expectTrue(size1 == 8 || size1 == 9)

  var a: Character = "a"
  let size2 = sizeofValue(a)
  expectTrue(size2 == 8 || size2 == 9)

  expectEqual(size1, size2)
}

CharacterTests.test("Hashable") {
  for characters in [
    baseScalars,
    continuingScalars,
    testCharacters
  ] {
    for i in indices(characters) {
      for j in indices(characters) {
        var ci = Character(characters[i])
        var cj = Character(characters[j])
        checkHashable(i == j, ci, cj, SourceLocStack().withCurrentLoc()) {
          "i=\(i), j=\(j)"
        }
      }
    }
  }
}

/// Test that a given `String` can be transformed into a `Character` and back
/// without loss of information.
func checkRoundTripThroughCharacter(s: String) {
  var c = Character(s)
  var s2 = String(c)
  expectEqual(Array(s.unicodeScalars), Array(s2.unicodeScalars)) {
    "round-tripping error: \"\(s)\" != \"\(s2)\""
  }
}

func isSmallRepresentation(s: String) -> Bool {
  switch(Character(s)) {
    case .SmallRepresentation:
      return true
    default:
      return false
  }
}

func checkRepresentation(s: String) {
  let expectSmall = count(s.utf8) <= 8
  let isSmall = isSmallRepresentation(s)

  expectEqual(expectSmall, isSmall) {
    let expectedSize = expectSmall ? "small" : "large"
    return "expected \"\(s)\" to use the \(expectedSize) representation"
  }
}

CharacterTests.test("RoundTripping") {
  // Single Unicode Scalar Value tests
  for s in baseScalars {
    checkRepresentation(s)
    checkRoundTripThroughCharacter(s)
  }

  // Edge case tests
  for s in testCharacters {
    checkRepresentation(s)
    checkRoundTripThroughCharacter(s)
  }
}

CharacterTests.test("RoundTripping/Random") {
  // Random tests
  // Seed the random number generator
  srandomdev()
  for x in 0..<500 {
    // Character's small representation variant has 63 bits. Making
    // the maximum length 9 scalars tests both sides of the limit.
    var s = randomGraphemeCluster(1, 9)
    checkRepresentation(s)
    checkRoundTripThroughCharacter(s)
  }
}

runAllTests()

