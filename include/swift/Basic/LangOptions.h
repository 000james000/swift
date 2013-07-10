//===--- LangOptions.h - Language & configuration options -------*- C++ -*-===//
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
//  This file defines the LangOptions class, which provides various
//  language and configuration flags.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LANGOPTIONS_H
#define SWIFT_LANGOPTIONS_H

namespace swift {
  /// \brief A collection of options that affect the language dialect and
  /// provide compiler debugging facilities.
  class LangOptions {
  public:
    /// \brief Whether we are debugging the constraint solver.
    ///
    /// This option enables verbose debugging output from the constraint
    /// solver.
    bool DebugConstraintSolver = false;

    /// \brief Perform all dynamic allocations using malloc/free instead of
    /// optimized custom allocator, so that memory debugging tools can be used.
    bool UseMalloc = false;

    /// \brief If true, the parser will not parse function bodies.  Tokens for
    /// the function body will be saved in the AST node.
    bool DelayFunctionBodyParsing = false;

    /// \brief Code completion offset in bytes from the beginning of the main
    /// source file.  Valid only if \c isCodeCompletion() == true.
    unsigned CodeCompletionOffset = ~0U;

    /// \returns true if we are doing code completion.
    bool isCodeCompletion() const {
      return CodeCompletionOffset != ~0U;
    }
  };
}

#endif // LLVM_SWIFT_LANGOPTIONS_H

