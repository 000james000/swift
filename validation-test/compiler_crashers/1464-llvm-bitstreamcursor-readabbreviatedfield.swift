// RUN: not --crash %target-swift-frontend %s -parse
// XFAIL: no_asserts
// XFAIL: asan

// Distributed under the terms of the MIT license
// Test case submitted to project by https://github.com/practicalswift (practicalswift)
// Test case found by fuzzing

if true {
protocol b : A {
protocol A : a {
typealias b : b
