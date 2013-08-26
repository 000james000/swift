//===-- Immediate.h - Entry point for swift immediate mode -----*- C++ -*--===//
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
// This is the entry point to the swift immediate mode, which takes a
// TranslationUnit, and runs it immediately using the JIT.
//
//===----------------------------------------------------------------------===//

#include <string>
#include <vector>

namespace swift {
  class ASTContext;
  class CompilerInstance;
  class TranslationUnit;
  class SILModule;

  // Using LLVM containers to store command-line arguments turns out
  // to be a lose, because LLVM's execution engine demands this vector
  // type.  We can flip the typedef if/when the LLVM interface
  // supports LLVM containers.
  using ProcessCmdLine = std::vector<std::string>;

  namespace irgen {
    class Options;
  }
  
  /// Publicly available REPL state information.
  class REPLContext {
  public:
    /// The SourceMgr buffer ID of the REPL input.
    unsigned CurBufferID;
    
    /// The index into the TranslationUnit's Decls at which to start
    /// typechecking the next REPL input.
    unsigned CurTUElem;

    /// The index into the TranslationUnit's Decls at which to start
    /// irgenning the next REPL input.
    unsigned CurIRGenElem;

    /// \brief Whether we have run replApplicationMain().
    bool RanREPLApplicationMain;
  };

  void RunImmediately(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
                      irgen::Options &Options);

  void REPL(CompilerInstance &CI, const ProcessCmdLine &CmdLine);
  void REPLRunLoop(CompilerInstance &CI, const ProcessCmdLine &CmdLine);
}
