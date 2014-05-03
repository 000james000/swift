//===--- Debug.h - Swift Runtime debug helpers ----------------------------===//
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
// Random debug support
//
//===----------------------------------------------------------------------===//

#ifndef _SWIFT_RUNTIME_DEBUG_HELPERS_
#define _SWIFT_RUNTIME_DEBUG_HELPERS_

#include <llvm/Support/Compiler.h>
#include <stdio.h>

#if SWIFT_HAVE_CRASHREPORTERCLIENT
#include <CrashReporterClient.h>
#else
#define CRSetCrashLogMessage(_m_) fprintf(stderr, "%s\n", (_m_))
#endif

namespace swift {

LLVM_ATTRIBUTE_NORETURN
LLVM_ATTRIBUTE_ALWAYS_INLINE // Minimize trashed registers
static void crash(const char *message) {
  CRSetCrashLogMessage(message);
  // __builtin_trap() doesn't always do the right thing due to GCC compatibility
#if defined(__i386__) || defined(__x86_64__)
  asm("int3");
#else
  __builtin_trap();
#endif
  __builtin_unreachable();
}

};

#endif // _SWIFT_RUNTIME_DEBUG_HELPERS_
