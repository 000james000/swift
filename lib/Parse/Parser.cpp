//===--- Parser.cpp - Swift Language Parser -------------------------------===//
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
//  This file implements the Swift parser.
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Diagnostics.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/Twine.h"

using namespace swift;

static StringRef ComputeLexStart(StringRef File, unsigned Offset,
                                 unsigned EndOffset, bool IsMainModule);

namespace {
  /// To assist debugging parser crashes, tell us the location of the
  /// current token.
  class PrettyStackTraceParser : public llvm::PrettyStackTraceEntry {
    Parser &P;
  public:
    PrettyStackTraceParser(Parser &P) : P(P) {}
    void print(llvm::raw_ostream &out) const {
      out << "With parser at source location: ";
      printSourceLoc(out, P.Tok.getLoc(), P.Context);
      out << '\n';
    }
  };

/// A visitor that does delayed parsing of function bodies.
class ParseDelayedFunctionBodies : public ASTWalker {
  Parser &P;

public:
  ParseDelayedFunctionBodies(Parser &P) : P(P) {}

  virtual bool walkToDeclPre(Decl *D) {
    if (auto FD = dyn_cast<FuncDecl>(D)) {
      if (auto FE = FD->getBody()) {
        if (FE->getBodyKind() != FuncExpr::BodyKind::Unparsed)
          return false;
        P.parseDeclFuncBodyDelayed(FD);
      }
    }
    return true;
  }
};
} // unnamed namespace

/// parseIntoTranslationUnit - Entrypoint for the parser.
bool swift::parseIntoTranslationUnit(TranslationUnit *TU,
                                     unsigned BufferID,
                                     bool *Done,
                                     SILParserState *SIL,
                                     std::unique_ptr<Parser> *PersistentParser) {
  Parser *P = nullptr;
  if (PersistentParser)
    P = PersistentParser->get();

  if (!P)
    P = new Parser(BufferID, TU,
                   TU->Kind == TranslationUnit::Main ||
                   TU->Kind == TranslationUnit::REPL, SIL);

  PrettyStackTraceParser stackTrace(*P);
  bool FoundSideEffects = P->parseTranslationUnit(TU);

  const llvm::MemoryBuffer *Buffer = P->SourceMgr.getMemoryBuffer(BufferID);
  *Done = P->Tok.getLoc().Value.getPointer() ==
          Buffer->getBuffer().end();

  if (PersistentParser && !PersistentParser->get())
    PersistentParser->reset(P);

  return FoundSideEffects;
}

void swift::performDelayedParsing(
    TranslationUnit *TU, Parser *TheParser,
    CodeCompletionCallbacksFactory *CodeCompletionFactory) {
  bool NeedSecondPass = false;
  if (TU->Ctx.LangOpts.DelayFunctionBodyParsing)
    NeedSecondPass = true;

  std::unique_ptr<CodeCompletionCallbacks> CodeCompletion;
  if (TU->Ctx.LangOpts.isCodeCompletion() && CodeCompletionFactory) {
    CodeCompletion.reset(
        CodeCompletionFactory->createCodeCompletionCallbacks(*TheParser));
    TheParser->setCodeCompletion(TU->Ctx.LangOpts.CodeCompletionOffset,
                                 CodeCompletion.get());
    NeedSecondPass = true;
  }

  if (!NeedSecondPass)
    return;

  TheParser->setDelayedParsingSecondPass();
  ParseDelayedFunctionBodies Walker(*TheParser);
  for (Decl *D : TU->Decls) {
    D->walk(Walker);
  }
}

std::vector<Token> swift::tokenize(llvm::SourceMgr &SM, unsigned BufferID,
                                   unsigned Offset, unsigned EndOffset,
                                   bool KeepComments) {
  // This is mainly to check for "#!" at the beginning.
  bool IsMainModule = (Offset == 0);
  const llvm::MemoryBuffer *Buffer = SM.getMemoryBuffer(BufferID);
  std::unique_ptr<Lexer> L(
      new Lexer(ComputeLexStart(Buffer->getBuffer(),
                                Offset, EndOffset, IsMainModule),
                SM, /*Diags=*/nullptr, /*InSILMode=*/false, KeepComments));
  std::vector<Token> Tokens;
  do {
    Tokens.emplace_back();
    L->lex(Tokens.back());
  } while (Tokens.back().isNot(tok::eof));
  Tokens.pop_back(); // Remove EOF.
  return Tokens;
}

Expr *swift::parseCompletionContextExpr(TranslationUnit *TU,
                                        llvm::StringRef expr) {
  // Set up a DiagnosticEngine to swallow errors.
  DiagnosticEngine diags(TU->Ctx.SourceMgr);
  
  TU->ASTStage = TranslationUnit::Parsing;
  Parser P(TU, expr, diags, nullptr);
  // Prime the lexer.
  P.consumeToken();
  P.CurDeclContext = TU;
  
  Expr *parsed = P.parseExpr(diag::expected_expr).getPtrOrNull();
  TU->ASTStage = TranslationUnit::Parsed;
  return parsed;
}
  
//===----------------------------------------------------------------------===//
// Setup and Helper Methods
//===----------------------------------------------------------------------===//

/// ComputeLexStart - Compute the start of lexing; if there's an offset, take
/// that into account.  If there's a #! line in a main module, ignore it.
static StringRef ComputeLexStart(StringRef File, unsigned Offset,
                                 unsigned EndOffset, bool IsMainModule) {
  if (EndOffset)
    return File.slice(Offset, EndOffset);
  else if (Offset)
    return File.substr(Offset);

  if (IsMainModule && File.startswith("#!")) {
    StringRef::size_type Pos = File.find_first_of("\n\r");
    if (Pos != StringRef::npos)
      return File.substr(Pos);
  }

  return File;
}


Parser::Parser(unsigned BufferID, TranslationUnit *TU,
               bool IsMainModule, SILParserState *SIL)
  : SourceMgr(TU->getASTContext().SourceMgr),
    Diags(TU->getASTContext().Diags),
    TU(TU),
    L(new Lexer(ComputeLexStart(
                    SourceMgr.getMemoryBuffer(BufferID)->getBuffer(),
                    0, 0, IsMainModule),
                 SourceMgr, &Diags, SIL != nullptr)),
    SIL(SIL),
    Component(TU->getComponent()),
    Context(TU->getASTContext()),
    ScopeInfo(*this),
    IsMainModule(IsMainModule) {

  // Set the token to a sentinel so that we know the lexer isn't primed yet.
  // This cannot be tok::unknown, since that is a token the lexer could produce.
  Tok.setKind(tok::NUM_TOKENS);
}

Parser::Parser(TranslationUnit *TU,
               llvm::StringRef fragment, DiagnosticEngine &Diags,
               SILParserState *SIL)
  : SourceMgr(TU->getASTContext().SourceMgr),
    Diags(Diags),
    TU(TU),
    L(new Lexer(fragment, SourceMgr, &Diags, SIL != nullptr)),
    SIL(SIL),
    Component(TU->getComponent()),
    Context(TU->getASTContext()),
    ScopeInfo(*this),
    IsMainModule(false)
{
  // Set the token to a sentinel so that we know the lexer isn't primed yet.
  // This cannot be tok::unknown, since that is a token the lexer could produce.
  Tok.setKind(tok::NUM_TOKENS);
}


Parser::~Parser() {
  delete L;
}

/// peekToken - Return the next token that will be installed by consumeToken.
const Token &Parser::peekToken() {
  return L->peekNextToken();
}

SourceLoc Parser::consumeToken() {
  SourceLoc Loc = Tok.getLoc();
  assert(Tok.isNot(tok::eof) && "Lexing past eof!");
  L->lex(Tok);
  PreviousLoc = Loc;
  return Loc;
}

SourceLoc Parser::getEndOfPreviousLoc() {
  return Lexer::getLocForEndOfToken(SourceMgr, PreviousLoc);
}

SourceLoc Parser::consumeStartingLess() {
  assert(startsWithLess(Tok) && "Token does not start with '<'");
  
  if (Tok.getLength() == 1)
    return consumeToken();
  
  // Skip the starting '<' in the existing token.
  SourceLoc Loc = Tok.getLoc();  
  StringRef Remaining = Tok.getText().substr(1);
  Tok.setToken(L->getTokenKind(Remaining), Remaining);
  return Loc;
}

SourceLoc Parser::consumeStartingGreater() {
  assert(startsWithGreater(Tok) && "Token does not start with '>'");
  
  if (Tok.getLength() == 1)
    return consumeToken();
  
  // Skip the starting '>' in the existing token.
  SourceLoc Loc = Tok.getLoc();
  StringRef Remaining = Tok.getText().substr(1);
  Tok.setToken(L->getTokenKind(Remaining), Remaining);
  return Loc;
}

void Parser::skipSingle() {
  switch (Tok.getKind()) {
  case tok::l_paren:
    consumeToken();
    skipUntil(tok::r_paren);
    consumeIf(tok::r_paren);
    break;
  case tok::l_brace:
    consumeToken();
    skipUntil(tok::r_brace);
    consumeIf(tok::r_brace);
    break;
  case tok::l_square:
    consumeToken();
    skipUntil(tok::r_square);
    consumeIf(tok::r_square);
    break;
  default:
    consumeToken();
    break;
  }
}

void Parser::skipUntil(tok T1, tok T2) {
  // tok::unknown is a sentinel that means "don't skip".
  if (T1 == tok::unknown && T2 == tok::unknown) return;
  
  while (Tok.isNot(tok::eof) && Tok.isNot(T1) && Tok.isNot(T2))
    skipSingle();
}

void Parser::skipUntilAnyOperator() {
  while (Tok.isNot(tok::eof) && Tok.isNotAnyOperator())
    skipSingle();
}

void Parser::skipUntilDeclRBrace() {
  while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_brace) &&
         !isStartOfDecl(Tok, peekToken()))
    skipSingle();
}

void Parser::skipUntilDeclStmtRBrace() {
  while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_brace) &&
         !isStartOfStmt(Tok) &&
         !isStartOfDecl(Tok, peekToken())) {
    skipSingle();
  }
}


//===----------------------------------------------------------------------===//
// Primitive Parsing
//===----------------------------------------------------------------------===//

/// parseIdentifier - Consume an identifier (but not an operator) if
/// present and return its name in Result.  Otherwise, emit an error and
/// return true.
bool Parser::parseIdentifier(Identifier &Result, SourceLoc &Loc,
                             const Diagnostic &D) {
  switch (Tok.getKind()) {
#define IDENTIFIER_KEYWORD(kw) case tok::kw_##kw:
#include "swift/Parse/Tokens.def"
  case tok::identifier:
    Result = Context.getIdentifier(Tok.getText());
    Loc = Tok.getLoc();
    consumeToken();
    return false;
  default:
    Diags.diagnose(Tok.getLoc(), D);
    return true;
  }
}

/// parseAnyIdentifier - Consume an identifier or operator if present and return
/// its name in Result.  Otherwise, emit an error and return true.
bool Parser::parseAnyIdentifier(Identifier &Result, SourceLoc &Loc,
                                const Diagnostic &D) {
  if (Tok.is(tok::identifier) || Tok.isAnyOperator()) {
    Result = Context.getIdentifier(Tok.getText());
    Loc = Tok.getLoc();
    consumeToken();
    return false;
  }
  
  Diags.diagnose(Tok.getLoc(), D);
  return true;
}

/// parseToken - The parser expects that 'K' is next in the input.  If so, it is
/// consumed and false is returned.
///
/// If the input is malformed, this emits the specified error diagnostic.
bool Parser::parseToken(tok K, SourceLoc &TokLoc, const Diagnostic &D) {
  if (Tok.is(K)) {
    TokLoc = consumeToken(K);
    return false;
  }
  
  diagnose(Tok.getLoc(), D);
  return true;
}

/// parseMatchingToken - Parse the specified expected token and return its
/// location on success.  On failure, emit the specified error diagnostic, and a
/// note at the specified note location.
bool Parser::parseMatchingToken(tok K, SourceLoc &TokLoc, Diag<> ErrorDiag,
                                SourceLoc OtherLoc) {
  Diag<> OtherNote;
  switch (K) {
  case tok::r_paren:  OtherNote = diag::opening_paren;    break;
  case tok::r_square: OtherNote = diag::opening_bracket;  break;
  case tok::r_brace:  OtherNote = diag::opening_brace;    break;
  default:            assert(!"unknown matching token!"); break;
  }
  if (parseToken(K, TokLoc, ErrorDiag)) {
    diagnose(OtherLoc, OtherNote);
    return true;
  }

  return false;
}

/// parseList - Parse the list of statements, expressions, or declarations.
bool Parser::parseList(tok RightK, SourceLoc LeftLoc, SourceLoc &RightLoc,
                       tok SeparatorK, bool OptionalSep, Diag<> ErrorDiag,
                       std::function<bool()> callback) {
  assert(SeparatorK == tok::comma || SeparatorK == tok::semi);

  if (Tok.is(RightK)) {
    RightLoc = consumeToken(RightK);
    return false;
  }

  bool Invalid = false;
  while (true) {
    while (Tok.is(SeparatorK)) {
      diagnose(Tok, diag::unexpected_separator,
               SeparatorK == tok::comma ? "," : ";")
        .fixItRemove(SourceRange(Tok.getLoc()));
      consumeToken();
    }
    SourceLoc StartLoc = Tok.getLoc();
    Invalid |= callback();
    if (Tok.is(RightK))
      break;
    // If the lexer stopped with an EOF token whose spelling is ')', then this
    // is actually the tuple that is a string literal interpolation context.
    // Just accept the ) and build the tuple as we usually do.
    if (Tok.is(tok::eof) && Tok.getText()[0] == ')') {
      RightLoc = Tok.getLoc();
      return Invalid;
    }
    if (consumeIf(SeparatorK))
      continue;
    if (!OptionalSep) {
      SourceLoc InsertLoc = Lexer::getLocForEndOfToken(SourceMgr, PreviousLoc);
      StringRef Separator = (SeparatorK == tok::comma ? "," : ";");
      diagnose(Tok, diag::expected_separator, Separator)
        .fixItInsert(InsertLoc, Separator);
      Invalid = true;
    }
    // If we haven't made progress, skip ahead
    if (Tok.getLoc() == StartLoc) {
      skipUntil(RightK, SeparatorK);
      if (Tok.is(RightK))
        break;
      if (Tok.is(tok::eof))
        return true;
      consumeIf(SeparatorK);
    }
  }

  Invalid |= parseMatchingToken(RightK, RightLoc, ErrorDiag, LeftLoc);
  return Invalid;
}

/// diagnoseRedefinition - Diagnose a redefinition error, with a note
/// referring back to the original definition.

void Parser::diagnoseRedefinition(ValueDecl *Prev, ValueDecl *New) {
  assert(New != Prev && "Cannot conflict with self");
  diagnose(New->getLoc(), diag::decl_redefinition, New->isDefinition());
  diagnose(Prev->getLoc(), diag::previous_decldef, Prev->isDefinition(),
             Prev->getName());
}
