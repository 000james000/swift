//===-- MangleHack.cpp - Swift Runtime Mangle Hack for Interface Builder --===//
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
// Implementations of mangler hacks for Interface Builder
//
// We don't have the time to disentangle the real mangler from the compiler
// right now.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/MangleHack.h"
#include "cassert"
#include "cstring"
#include "Debug.h"

const char *
_swift_mangleClassForIB(const char *module, const char *class_) {
  size_t moduleLength = strlen(module);
  size_t classLength = strlen(class_);
  char *value = nullptr;
  int result = asprintf(&value, "_TtC%zu%s%zu%s", moduleLength, module,
                        classLength, class_);
  assert(result > 0);
  assert(value);
  return value;
}

const char *
_swift_mangleProtocolForIB(const char *module, const char *protocol) {
  size_t moduleLength = strlen(module);
  size_t protocolLength = strlen(protocol);
  char *value = nullptr;
  int result = asprintf(&value, "_TtP%zu%s%zu%s_", moduleLength, module,
                        protocolLength, protocol);
  assert(result > 0);
  assert(value);
  return value;
}
