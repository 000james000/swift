//===--- Subsystems.h - Swift Compiler Subsystem Entrypoints ----*- C++ -*-===//
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
//  This file declares the main entrypoints to the various subsystems.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SUBSYSTEMS_H
#define SWIFT_SUBSYSTEMS_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"

#include <memory>

namespace llvm {
  class SourceMgr;
  class MemoryBuffer;
  class Module;
  class FunctionPass;
}

namespace swift {
  class TranslationUnit;
  class Component;
  class Expr;
  class SILModule;
  struct TypeLoc;
  class SILParserTUState;
  class Parser;
  class Token;
  class CodeCompletionCallbacksFactory;
  class PersistentParserState;
  class DelayedParsingCallbacks;

  namespace irgen {
    class Options;
  }
  
  /// SILParserState - This is a context object used to optionally maintain SIL
  /// parsing context for the parser.
  class SILParserState {
  public:
    SILModule *M;
    SILParserTUState *S;

    explicit SILParserState(SILModule *M);
    ~SILParserState();
  };
  

  /// verify - Check that the translation unit is well formed (i.e. following
  /// the invariants of the AST, not that the code written by the user makes
  /// sense), aborting and spewing errors if not.
  void verify(TranslationUnit *TUnit);

  /// \brief Parse a single buffer into the given taranslation unit.  If the
  /// translation unit is the main module, stop parsing after the next
  /// stmt-brace-item with side-effects.
  ///
  /// \param Done set to \c true if end of the buffer was reached.
  ///
  /// \param SIL if non-null, we're parsing a SIL file.
  ///
  /// \param PersistentState if non-null the same PersistentState object can
  /// be used to resume parsing or parse delayed function bodies.
  ///
  /// \param DelayedParseCB if non-null enables delayed parsing for function
  /// bodies.
  bool parseIntoTranslationUnit(TranslationUnit *TU, unsigned BufferID,
                                bool *Done,
                                unsigned CodeCompletionOffset = ~0U,
                                SILParserState *SIL = nullptr,
                              PersistentParserState *PersistentState = nullptr,
                             DelayedParsingCallbacks *DelayedParseCB = nullptr);

  /// \brief Finish the parsing by going over the nodes that were delayed
  /// during the first parsing pass.
  void
  performDelayedParsing(TranslationUnit *TU,
                        PersistentParserState &PersistentState,
                        CodeCompletionCallbacksFactory *CodeCompletionFactory,
                        unsigned CodeCompletionOffset);

  /// \brief Lex and return a vector of tokens for the given buffer.
  std::vector<Token> tokenize(llvm::SourceMgr &SM, unsigned BufferID,
                              unsigned Offset = 0, unsigned EndOffset = 0,
                              bool KeepComments = true);

  /// Attempt to parse a StringRef as a code completion context expression.
  /// Returns the parsed expression if successful, or null if an expression
  /// could not be parsed.
  Expr *parseCompletionContextExpr(TranslationUnit *TU, StringRef expr);

  /// performAutoImport - When a translation unit is first set up, this handles
  /// setting up any auto imports of the standard library.
  void performAutoImport(TranslationUnit *TU);

  /// performNameBinding - Once parsing is complete, this walks the AST to
  /// resolve names and do other top-level validation.  StartElem indicates
  /// where to start for incremental name binding in the main module.
  void performNameBinding(TranslationUnit *TU, unsigned StartElem = 0);
  
  /// performTypeChecking - Once parsing and namebinding are complete, this
  /// walks the AST to resolve types and diagnose problems therein. StartElem
  /// indicates where to start for incremental type checking in the
  /// main module.
  void performTypeChecking(TranslationUnit *TU, unsigned StartElem = 0);

  /// performTypeLocChecking - recursively validate the specified type.  This is
  /// used when dealing with partial translation units (e.g. SIL parsing).
  bool performTypeLocChecking(TranslationUnit *TU, TypeLoc &T);
  
  /// typeCheckCompletionContextExpr - Typecheck an expression parsed as a
  /// completion context.
  bool typeCheckCompletionContextExpr(TranslationUnit *TU,
                                      Expr *&parsedExpr);
  
  /// performCaptureAnalysis - Analyse the AST and mark local declarations
  /// and expressions which can capture them so they can be emitted more
  /// efficiently.  StartElem indicates where to start for incremental capture
  /// analysis in the main module.
  void performCaptureAnalysis(TranslationUnit *TU, unsigned StartElem = 0);

  /// Turn the given translation unit into SIL IR. The returned SILModule must
  /// be deleted by the caller.
  SILModule *performSILGeneration(TranslationUnit *TU,
                                  unsigned StartElem = 0);

  /// performSILAllocBoxToStackPromotion - Promote alloc_box into stack
  /// allocations.
  void performSILAllocBoxToStackPromotion(SILModule *M);

  /// performSILStackToSSAPromotion - Promote alloc_stack instructions into SSA
  /// registers.
  void performSILStackToSSAPromotion(SILModule *M);

  /// \brief Analyze the SIL module for correcntess and generate user
  /// diagnostics if any.
  void emitSILDataflowDiagnostics(const SILModule *M);

  /// Serializes a translation unit to the given output file.
  ///
  /// This interface is still prone to change!
  void serialize(const TranslationUnit *TU, const char *outputPath,
                 ArrayRef<unsigned> inputFileBufferIDs = {});

  /// Turn the given translation unit into either LLVM IR or native code.
  ///
  /// \param SILMod  A SIL module to translate to LLVM IR. If null, IRGen works
  ///   directly from the AST.
  /// \param StartElem  Indicates where to start for incremental IRGen in the
  ///   main module.
  void performIRGeneration(irgen::Options &Opts, llvm::Module *Module,
                           TranslationUnit *TU, SILModule *SILMod,
                           unsigned StartElem = 0);
  
  // Optimization passes.
  llvm::FunctionPass *createSwiftARCOptPass();
  llvm::FunctionPass *createSwiftARCExpandPass();

  /// The extension for serialized modules.
  static const char * const SERIALIZED_MODULE_EXTENSION = "swiftmodule";
} // end namespace swift

#endif
