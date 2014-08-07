//===--- Concatenate.swift - Tests for lazy/eager concatenate -------------===//
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
// RUN: %target-build-swift -parse-stdlib -Xfrontend -disable-access-control -module-name a %s -o %t.out
// RUN: %target-run %t.out

import Swift
import StdlibUnittest

var ConcatenateTests = TestCase("ConcatenateTests")

// Help the type checker (<rdar://problem/17897413> Slow type deduction)
typealias X = (Range<Int>, [Range<Int>])

let samples: [X] = [
  (0..<8, [ 1..<1, 0..<5, 7..<7, 5..<7, 7..<8 ]),
  (0..<8, [ 0..<5, 7..<7, 5..<7, 7..<8 ]),
  (0..<8, [ 1..<1, 0..<5, 7..<7, 5..<7, 7..<8, 11..<11 ]),
  (0..<8, [ 0..<5, 7..<7, 5..<7, 7..<8, 11..<11 ]),
  (0..<0, [ 11..<11 ]),
  (0..<0, [ 3..<3, 11..<11 ]),
  (0..<0, []),
]

let expected = Array(0..<8)

for (expected, source) in samples {
  ConcatenateTests.test("forward-\(source)") {
    checkCollection(
      Array(expected),
      _lazyConcatenate(source),
      SourceLocStack().withCurrentLoc())
  }
  ConcatenateTests.test("reverse-\(source)") {
    checkCollection(
      lazy(expected).reverse().array,
      _lazyConcatenate(source).reverse(),
      SourceLocStack().withCurrentLoc())
  }
  ConcatenateTests.test("sequence-\(source)") {
    checkSequence(
      Array(expected),
      _lazyConcatenate(SequenceOf(source)),
      SourceLocStack().withCurrentLoc())
  }
}

runAllTests()

