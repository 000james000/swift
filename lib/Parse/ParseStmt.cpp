//===--- ParseStmt.cpp - Swift Language Parser for Statements -------------===//
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
// Statement Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

/// isStartOfStmt - Return true if the specified token starts
/// a statement.
///
bool Parser::isStartOfStmt(const Token &Tok) {
  switch (Tok.getKind()) {
  default: return false;
  case tok::kw_return:
  case tok::kw_if:
  case tok::kw_while:
  case tok::kw_do:
  case tok::kw_for:
  case tok::kw_break:
  case tok::kw_continue:
  case tok::kw_fallthrough:
  case tok::kw_switch:
  case tok::kw_case:
  case tok::kw_default:
    return true;
  }
}

/// isStartOfDecl - Return true if this is the start of a decl or decl-import.
bool Parser::isStartOfDecl(const Token &Tok, const Token &Tok2) {
  switch (Tok.getKind()) {
  case tok::kw_static:
  case tok::kw_extension:
  case tok::kw_var:
  case tok::kw_typealias:
  case tok::kw_oneof:
  case tok::kw_case:
  case tok::kw_struct:
  case tok::kw_class:
  case tok::kw_import:
  case tok::kw_subscript:
  case tok::kw_constructor:
  case tok::kw_destructor:
  case tok::kw_func:
    return true;
  case tok::kw_protocol:
    return !(Tok2.isAnyOperator() && Tok2.getText().equals("<"));
  default:
    return isStartOfOperatorDecl(Tok, Tok2);
  }
}

bool Parser::parseExprOrStmt(ExprStmtOrDecl &Result) {
  if (Tok.is(tok::semi)) {
    diagnose(Tok.getLoc(), diag::illegal_semi_stmt)
      .fixItRemove(SourceRange(Tok.getLoc()));
    consumeToken();
    return true;
  } else if (isStartOfStmt(Tok)) {
    NullablePtr<Stmt> Res = parseStmt();
    if (Res.isNull())
      return true;
    Result = Res.get();
    return false;
  }

  if (CodeCompletion)
    CodeCompletion->setExprBeginning(getParserPosition());

  NullablePtr<Expr> ResultExpr = parseExpr(diag::expected_expr,
                                           /*usesExprBasic*/ false);
  if (ResultExpr.isNull()) {
    if (CodeCompletion) {
      CodeCompletion->completeExpr();
    }
    return true;
  }
  
  Result = ResultExpr.get();
  return false;
}

static bool isTerminatorForBraceItemListKind(const Token &Tok,
                                             BraceItemListKind Kind,
                                 ArrayRef<Parser::ExprStmtOrDecl> ParsedDecls) {
  switch (Kind) {
  case BraceItemListKind::Brace:
    return false;
  case BraceItemListKind::Property:
    return Tok.isContextualKeyword("get") || Tok.isContextualKeyword("set");
  case BraceItemListKind::Case:
    return Tok.is(tok::kw_case) || Tok.is(tok::kw_default);
  case BraceItemListKind::TopLevelCode:
    // When parsing the top level executable code for a module, if we parsed
    // some executable code, then we're done.  We want to process (name bind,
    // type check, etc) decls one at a time to make sure that there are not
    // forward type references, etc.  There is an outer loop around the parser
    // that will reinvoke the parser at the top level on each statement until
    // EOF.  In contrast, it is ok to have forward references between classes,
    // functions, etc.
    for (auto I : ParsedDecls) {
      if (isa<TopLevelCodeDecl>(I.get<Decl*>()))
        // Only bail out if the next token is at the start of a line.  If we
        // don't, then we may accidentally allow things like "a = 1 b = 4".
        // FIXME: This is really dubious.  This will reject some things, but
        // allow other things we don't want.
        if (Tok.isAtStartOfLine())
          return true;
    }
    return false;
  }
}

void Parser::consumeTopLevelDecl(TopLevelCodeDecl *TLCD,
                                 ParserPosition BeginParserPosition) {
  backtrackToPosition(BeginParserPosition);
  SourceLoc BeginLoc = Tok.getLoc();
  skipUntilDeclStmtRBrace();
  consumeIf(tok::code_complete);
  SourceLoc EndLoc = Tok.getLoc();
  State->delayTopLevelCodeDecl(TLCD, { BeginLoc, EndLoc },
                               BeginParserPosition.PreviousLoc);
}

///   brace-item:
///     decl
///     expr
///     stmt
///   stmt:
///     ';'
///     stmt-assign
///     stmt-if
///     stmt-for-c-style
///     stmt-for-each
///     stmt-switch
///     stmt-control-transfer
///  stmt-control-transfer:
///     stmt-return
///     stmt-break
///     stmt-continue
///     stmt-fallthrough
///   stmt-assign:
///     expr '=' expr
void Parser::parseBraceItems(SmallVectorImpl<ExprStmtOrDecl> &Entries,
                             bool IsTopLevel, BraceItemListKind Kind) {
  // This forms a lexical scope.
  Scope S(this, IsTopLevel ? ScopeKind::TopLevel : ScopeKind::Brace);

  SmallVector<Decl*, 8> TmpDecls;

  bool previousHadSemi = true;
  while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof) &&
         Tok.isNot(tok::kw_sil) &&
         !isTerminatorForBraceItemListKind(Tok, Kind, Entries)) {
    bool NeedParseErrorRecovery = false;
    ExprStmtOrDecl Result;

    // If the previous statement didn't have a semicolon and this new
    // statement doesn't start a line, complain.
    if (!previousHadSemi && !Tok.isAtStartOfLine()) {
      SourceLoc EndOfPreviousLoc = Lexer::getLocForEndOfToken(SourceMgr,
                                                              PreviousLoc);
      diagnose(EndOfPreviousLoc, diag::statement_same_line_without_semi)
        .fixItInsert(EndOfPreviousLoc, ";");
      // FIXME: Add semicolon to the AST?
    }

    // Parse the decl, stmt, or expression.
    previousHadSemi = false;
    if (isStartOfDecl(Tok, peekToken())) {
      if (parseDecl(TmpDecls, IsTopLevel ? PD_AllowTopLevel : PD_Default))
        NeedParseErrorRecovery = true;
      else {
        for (Decl *D : TmpDecls)
          Entries.push_back(D);
        if (!TmpDecls.empty())
          previousHadSemi = TmpDecls.back()->TrailingSemiLoc.isValid();
      }

      TmpDecls.clear();
    } else if (IsTopLevel && IsMainModule) {
      // If this is a statement or expression at the top level of the module,
      // Parse it as a child of a TopLevelCodeDecl.
      auto *TLCD = new (Context) TopLevelCodeDecl(CurDeclContext);
      ContextChange CC(*this, TLCD);
      SourceLoc StartLoc = Tok.getLoc();

      ParserPosition BeginParserPosition;
      if (isCodeCompletionFirstPass())
        BeginParserPosition = getParserPosition();

      bool FailedToParse = parseExprOrStmt(Result);

      if (Tok.is(tok::code_complete)) {
        consumeTopLevelDecl(TLCD, BeginParserPosition);
        return;
      }

      if (FailedToParse) {
        NeedParseErrorRecovery = true;
      } else {
        auto Brace = BraceStmt::create(Context, StartLoc, Result, Tok.getLoc());
        TLCD->setBody(Brace);
        Entries.push_back(TLCD);
      }
    } else {
      SourceLoc StartLoc = Tok.getLoc();
      if (parseExprOrStmt(Result))
        NeedParseErrorRecovery = true;
      else {

        // If this is a normal library, you can't have expressions or statements
        // outside at the top level.  Diagnose this error.
        if (IsTopLevel) {
          if (!NeedParseErrorRecovery)
            diagnose(StartLoc,
                     Result.is<Stmt*>() ? diag::illegal_top_level_stmt :
                     diag::illegal_top_level_expr);
        } else {
          Entries.push_back(Result);
        }
      }
    }

    if (!NeedParseErrorRecovery && !previousHadSemi && Tok.is(tok::semi)) {
      if (Result.is<Expr*>()) {
        Result.get<Expr*>()->TrailingSemiLoc = consumeToken(tok::semi);
      } else {
        Result.get<Stmt*>()->TrailingSemiLoc = consumeToken(tok::semi);
      }
      previousHadSemi = true;
    }

    // If we had a parse error, skip to the start of the next stmt or decl.  It
    // would be ideal to stop at the start of the next expression (e.g. "X = 4")
    // but distinguishing the start of an expression from the middle of one is
    // "hard".
    if (NeedParseErrorRecovery) {
      skipUntilDeclStmtRBrace();

      // If we have to recover, pretend that we had a semicolon; it's less
      // noisy that way.
      previousHadSemi = true;
    }
  }
}

void Parser::parseTopLevelCodeDeclDelayed() {
  auto DelayedState = State->takeDelayedDeclState();
  assert(DelayedState.get() && "should have delayed state");

  auto *TLCD = dyn_cast<TopLevelCodeDecl>(DelayedState->D);
  assert(!TLCD->getBody() && "should not have a parsed body");

  auto BeginParserPosition = getParserPosition(DelayedState->BodyPos);
  auto EndLexerState = L->getStateForEndOfTokenLoc(DelayedState->BodyEnd);

  // ParserPositionRAII needs a primed parser to restore to.
  if (Tok.is(tok::NUM_TOKENS))
    consumeToken();

  // Ensure that we restore the parser state at exit.
  ParserPositionRAII PPR(*this);

  // Create a lexer that can not go past the end state.
  Lexer LocalLex(*L, BeginParserPosition.LS, EndLexerState, SourceMgr, &Diags,
                 nullptr /*not SIL*/);

  // Temporarily swap out the parser's current lexer with our new one.
  llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

  // Rewind to '{' of the function body.
  restoreParserPosition(BeginParserPosition);

  // Re-enter the lexical scope.
  Scope S(this, DelayedState->takeScope());
  ContextChange CC(*this, TLCD);

  ExprStmtOrDecl Result;
  parseExprOrStmt(Result);
}

/// Recover from a 'case' or 'default' outside of a 'switch' by consuming up to
/// the next ':'.
static NullablePtr<Stmt> recoverFromInvalidCase(Parser &P) {
  assert(P.Tok.is(tok::kw_case) || P.Tok.is(tok::kw_default)
         && "not case or default?!");
  P.diagnose(P.Tok.getLoc(), diag::case_outside_of_switch, P.Tok.getText());
  P.skipUntil(tok::colon);
  // FIXME: Return an ErrorStmt?
  return nullptr;
}

NullablePtr<Stmt> Parser::parseStmt() {
  switch (Tok.getKind()) {
  default:
    diagnose(Tok, diag::expected_stmt);
    return 0;
  case tok::kw_return: return parseStmtReturn();
  case tok::kw_if:     return parseStmtIf();
  case tok::kw_while:  return parseStmtWhile();
  case tok::kw_do:     return parseStmtDoWhile();
  case tok::kw_for:    return parseStmtFor();
  case tok::kw_switch: return parseStmtSwitch();
  /// 'case' and 'default' are only valid at the top level of a switch.
  case tok::kw_case:
  case tok::kw_default: return recoverFromInvalidCase(*this);
  case tok::kw_break:
    return new (Context) BreakStmt(consumeToken(tok::kw_break));
  case tok::kw_continue:
    return new (Context) ContinueStmt(consumeToken(tok::kw_continue));
  case tok::kw_fallthrough:
    return new (Context) FallthroughStmt(consumeToken(tok::kw_fallthrough));
  }
}

/// parseBraceItemList - A brace enclosed expression/statement/decl list.  For
/// example { 1; 4+5; } or { 1; 2 }.  Always occurs as part of some other stmt
/// or decl.
///
///   brace-item-list:
///     '{' brace-item* '}'
///
NullablePtr<BraceStmt> Parser::parseBraceItemList(Diag<> ID) {
  if (Tok.isNot(tok::l_brace)) {
    diagnose(Tok.getLoc(), ID);
    return 0;
  }
  SourceLoc LBLoc = consumeToken(tok::l_brace);

  SmallVector<ExprStmtOrDecl, 16> Entries;
  SourceLoc RBLoc;

  parseBraceItems(Entries, false /*NotTopLevel*/);
  if (parseMatchingToken(tok::r_brace, RBLoc,
                         diag::expected_rbrace_in_brace_stmt, LBLoc))
    return 0;
  
  return BraceStmt::create(Context, LBLoc, Entries, RBLoc);
}

/// parseStmtReturn
///
///   stmt-return:
///     return expr?
///   
NullablePtr<Stmt> Parser::parseStmtReturn() {
  SourceLoc ReturnLoc = consumeToken(tok::kw_return);

  // Handle the ambiguity between consuming the expression and allowing the
  // enclosing stmt-brace to get it by eagerly eating it unless the return is
  // followed by a '}', ';', or statement keyword.
  Expr *RetExpr = nullptr;
  if (Tok.isNot(tok::r_brace) && Tok.isNot(tok::semi) &&
      !isStartOfStmt(Tok)) {
    NullablePtr<Expr> Result = parseExpr(diag::expected_expr_return);
    if (Result.isNull())
      return 0;
    RetExpr = Result.get();
  }

  return new (Context) ReturnStmt(ReturnLoc, RetExpr);
}


/// 
///   stmt-if:
///     'if' expr-basic stmt-brace stmt-if-else?
///   stmt-if-else:
///    'else' stmt-brace
///    'else' stmt-if
NullablePtr<Stmt> Parser::parseStmtIf() {
  SourceLoc IfLoc = consumeToken(tok::kw_if);

  NullablePtr<Expr> Condition = parseExprBasic(diag::expected_expr_if);
  if (Condition.isNull()) return 0;
  NullablePtr<BraceStmt> NormalBody =
    parseBraceItemList(diag::expected_lbrace_after_if);
  if (NormalBody.isNull())
    return 0;
    
  NullablePtr<Stmt> ElseBody;
  SourceLoc ElseLoc = Tok.getLoc();
  if (consumeIf(tok::kw_else)) {
    if (Tok.is(tok::kw_if))
      ElseBody = parseStmtIf();
    else
      ElseBody = parseBraceItemList(diag::expected_lbrace_after_else)
        .getPtrOrNull();
    if (ElseBody.isNull())
      return 0;
  } else {
    ElseLoc = SourceLoc();
  }

  // If our condition and normal expression parsed correctly, build an AST.
  return new (Context) IfStmt(IfLoc, Condition.get(), NormalBody.get(),
                              ElseLoc, ElseBody.getPtrOrNull());
}

/// 
///   stmt-while:
///     'while' expr-basic stmt-brace
NullablePtr<Stmt> Parser::parseStmtWhile() {
  SourceLoc WhileLoc = consumeToken(tok::kw_while);
  
  NullablePtr<Expr> Condition = parseExprBasic(diag::expected_expr_while);
  if (Condition.isNull()) return 0;
  NullablePtr<BraceStmt> Body =
    parseBraceItemList(diag::expected_lbrace_after_while);
  if (Body.isNull())
    return 0;
  
  // If our normal expression parsed correctly, build an AST.
  return new (Context) WhileStmt(WhileLoc, Condition.get(), Body.get());
}

/// 
///   stmt-do-while:
///     'do' stmt-brace 'while' expr
NullablePtr<Stmt> Parser::parseStmtDoWhile() {
  SourceLoc DoLoc = consumeToken(tok::kw_do), WhileLoc;

  NullablePtr<BraceStmt> Body =
    parseBraceItemList(diag::expected_lbrace_after_do);
  if (Body.isNull()) return 0;

  if (parseToken(tok::kw_while, WhileLoc, diag::expected_while_in_dowhile))
    return 0;
  
  NullablePtr<Expr> Condition = parseExpr(diag::expected_expr_do_while);
  if (Condition.isNull()) return 0;
  
  return new (Context) DoWhileStmt(DoLoc, Condition.get(), WhileLoc, Body.get());
}


NullablePtr<Stmt> Parser::parseStmtFor() {
  SourceLoc ForLoc = consumeToken(tok::kw_for);

  // The c-style-for loop and foreach-style-for loop are conflated together into
  // a single keyword, so we have to do some lookahead to resolve what is going
  // on.

  if (Tok.is(tok::l_paren)) {
    auto SavedPosition = getParserPosition();
    consumeToken(tok::l_paren);
    skipUntil(tok::r_paren);
    bool IsCStyle = peekToken().is(tok::l_brace);
    backtrackToPosition(SavedPosition);
    if (IsCStyle)
      return parseStmtForCStyle(ForLoc);
    return parseStmtForEach(ForLoc);
  }

  // If we have a leading identifier followed by a ':' or 'in', then this is a
  // pattern, so it is foreach.
  if (isStartOfBindingName(Tok) &&
      (peekToken().is(tok::colon) || peekToken().is(tok::kw_in)))
    return parseStmtForEach(ForLoc);

  // Otherwise, this is some sort of c-style for loop.
  return parseStmtForCStyle(ForLoc);
}

static bool parseExprForCStyle(Parser &P, Parser::ExprStmtOrDecl &Result,
                               bool usesExprBasic = false) {
  NullablePtr<Expr> ResultExpr = P.parseExpr(diag::expected_expr, usesExprBasic);
  if (ResultExpr.isNull())
    return true;
  
  Result = ResultExpr.get();
  return false;
}

///   stmt-for-c-style:
///     'for' stmt-for-c-style-init? ';' expr? ';' expr-or-stmt-assign-basic?
///           stmt-brace
///   stmt-for-c-style-init:
///     decl-var
///     expr-basic-or-stmt-assign
NullablePtr<Stmt> Parser::parseStmtForCStyle(SourceLoc ForLoc) {
  SourceLoc Semi1Loc, Semi2Loc;
  SourceLoc LPLoc, RPLoc;
  bool LPLocConsumed = false;

  ExprStmtOrDecl First;
  SmallVector<Decl*, 2> FirstDecls;
  NullablePtr<Expr> Second;
  ExprStmtOrDecl Third;
  NullablePtr<BraceStmt> Body;
  
  // Introduce a new scope to contain any var decls in the init value.
  Scope S(this, ScopeKind::ForVars);
  
  if (Tok.is(tok::l_paren)) {
    LPLoc = consumeToken();
    LPLocConsumed = true;
  }
  // Parse the first part, either a var, expr, or stmt-assign.
  if (Tok.is(tok::kw_var)) {
    if (parseDeclVar(false, FirstDecls)) return 0;
  } else if ((Tok.isNot(tok::semi) && parseExprForCStyle(*this, First)))
    return 0;

  // Parse the rest of the statement.
  if (parseToken(tok::semi, Semi1Loc, diag::expected_semi_for_stmt))
    return 0;

  if ((Tok.isNot(tok::semi) && Tok.isNot(tok::l_brace) &&
        (Second = parseExpr(diag::expected_cond_for_stmt)).isNull()) ||
      parseToken(tok::semi, Semi2Loc, diag::expected_semi_for_stmt) ||
      (Tok.isNot(tok::l_brace) && parseExprForCStyle(*this, Third, true)))
    return 0;

  if (LPLocConsumed && parseMatchingToken(tok::r_paren, RPLoc,
                                          diag::expected_rparen_for_stmt,LPLoc))
    return 0;

  if ((Body = parseBraceItemList(diag::expected_lbrace_after_for)).isNull())
    return 0;

  Expr *Initializer, *Increment;
  if (First.isNull())
    Initializer = nullptr;
  else
    Initializer = First.get<Expr*>();

  if (Third.isNull())
    Increment = nullptr;
  else
    Increment = Third.get<Expr*>();

  ArrayRef<Decl*> FirstDeclsContext;
  if (!FirstDecls.empty())
    FirstDeclsContext = Context.AllocateCopy(FirstDecls);
  
  return new (Context) ForStmt(ForLoc, Initializer, FirstDeclsContext,
                               Semi1Loc, Second,
                               Semi2Loc, Increment, Body.get());
}

/// 
///   stmt-for-each:
///     'for' pattern 'in' expr-basic stmt-brace
NullablePtr<Stmt> Parser::parseStmtForEach(SourceLoc ForLoc) {
  NullablePtr<Pattern> Pattern = parsePattern();

  if (!Tok.is(tok::kw_in)) {
    if (Pattern.isNonNull())
      diagnose(Tok.getLoc(), diag::expected_foreach_in);
    return nullptr;
  }
  SourceLoc InLoc = consumeToken();
  
  // expr
  NullablePtr<Expr> Container
    = parseExprBasic(diag::expected_foreach_container);

  // Introduce a new scope and place the variables in the pattern into that
  // scope.
  // FIXME: We may want to merge this scope with the scope introduced by
  // the stmt-brace, as in C++.
  Scope S(this, ScopeKind::ForeachVars);
  if (Pattern.isNonNull()) {
    SmallVector<Decl *, 2> Decls;
    DeclAttributes Attributes;
    addVarsToScope(Pattern.get(), Decls, Attributes);
  }
  // stmt-brace
  NullablePtr<BraceStmt> Body =
    parseBraceItemList(diag::expected_foreach_lbrace);
  
  if (Pattern.isNull() || Container.isNull() || Body.isNull())
    return nullptr;

  return new (Context) ForEachStmt(ForLoc, Pattern.get(), InLoc,
                                   Container.get(), Body.get());
}

///
///    stmt-switch:
///      'switch' expr-basic '{' stmt-case* '}'
NullablePtr<Stmt> Parser::parseStmtSwitch() {
  SourceLoc switchLoc = consumeToken(tok::kw_switch);
  NullablePtr<Expr> subjectExpr = parseExprBasic(diag::expected_switch_expr);
  
  if (subjectExpr.isNull())
    return nullptr;
  
  if (!Tok.is(tok::l_brace)) {
    diagnose(Tok.getLoc(), diag::expected_lbrace_after_switch);
    return nullptr;
  }
  
  SourceLoc lBraceLoc = consumeToken(tok::l_brace);
  SourceLoc rBraceLoc;
  
  // If there are non-case-label statements at the start of the switch body,
  // raise an error and recover by parsing and discarding them.
  ExprStmtOrDecl uncoveredStmt;
  while (!Tok.is(tok::kw_case) && !Tok.is(tok::kw_default)
         && !Tok.is(tok::r_brace) && !Tok.is(tok::eof)) {
    if (!uncoveredStmt)
      diagnose(Tok.getLoc(), diag::stmt_in_switch_not_covered_by_case);
    if (parseExprOrStmt(uncoveredStmt))
      return nullptr;
  }
  
  llvm::SmallVector<CaseStmt*, 8> cases;
  bool parsedDefault = false;
  bool parsedBlockAfterDefault = false;
  while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
    // We cannot have additional cases after a default clause. Complain on
    // the first offender.
    if (parsedDefault && !parsedBlockAfterDefault) {
      parsedBlockAfterDefault = true;
      diagnose(Tok.getLoc(), diag::case_after_default);
    }
    
    NullablePtr<CaseStmt> c = parseStmtCase();
    if (c.isNull())
      return nullptr;
    cases.push_back(c.get());
    if (c.get()->isDefault())
      parsedDefault = true;
  }
  
  if (parseMatchingToken(tok::r_brace, rBraceLoc,
                         diag::expected_rbrace_switch, lBraceLoc))
    return nullptr;
  
  return SwitchStmt::create(switchLoc,
                            subjectExpr.get(),
                            lBraceLoc,
                            cases,
                            rBraceLoc,
                            Context);
}

bool Parser::parseStmtCaseLabels(llvm::SmallVectorImpl<CaseLabel *> &labels,
                                 llvm::SmallVectorImpl<Decl *> &boundDecls) {
  // We must have at least one case label.
  assert(Tok.is(tok::kw_case) || Tok.is(tok::kw_default));
  
  bool parsedDefault = false;
  bool parsedOtherLabelWithDefault = false;
  do {
    // 'default' should label a block by itself.
    if (parsedDefault && !parsedOtherLabelWithDefault) {
      diagnose(Tok.getLoc(), diag::default_with_other_labels);
      parsedOtherLabelWithDefault = true;
    }

    // case-label ::= 'case' matching-pattern (',' matching-pattern)*
    //                ('where' expr)? ':'
    if (Tok.is(tok::kw_case)) {
      SourceLoc caseLoc = consumeToken();
      
      // Parse comma-separated patterns.
      SmallVector<Pattern *, 2> patterns;
      do {
        NullablePtr<Pattern> pattern = parseMatchingPattern();
        if (pattern.isNull())
          return true;
        // Add variable bindings from the pattern to the case scope.
        DeclAttributes defaultAttributes;
        addVarsToScope(pattern.get(), boundDecls, defaultAttributes);
        
        patterns.push_back(pattern.get());
      } while (consumeIf(tok::comma));
      
      // Parse an optional 'where' guard.
      SourceLoc whereLoc;
      Expr *guardExpr = nullptr;
      
      if (Tok.is(tok::kw_where)) {
        whereLoc = consumeToken();
        NullablePtr<Expr> guard = parseExpr(diag::expected_case_where_expr);
        if (guard.isNull())
          return true;
        guardExpr = guard.get();
      }
      
      SourceLoc colonLoc = Tok.getLoc();
      if (!Tok.is(tok::colon))
        diagnose(Tok.getLoc(), diag::expected_case_colon, "case");
      else
        colonLoc = consumeToken();
      
      auto label = CaseLabel::create(Context, /*isDefault*/false,
                                     caseLoc, patterns, whereLoc, guardExpr,
                                     colonLoc);
      labels.push_back(label);
      continue;
    }
    
    // case-label ::= 'default' ':'
    
    // 'default' should label a block by itself.
    if (!labels.empty() && !parsedOtherLabelWithDefault) {
      diagnose(Tok.getLoc(), diag::default_with_other_labels);
      parsedOtherLabelWithDefault = true;
    }
    
    parsedDefault = true;
    SourceLoc defaultLoc = consumeToken(tok::kw_default);
    
    // We don't allow 'where' guards on a 'default' block. For recovery
    // parse one if present.
    SourceLoc whereLoc;
    Expr *guardExpr = nullptr;
    if (Tok.is(tok::kw_where)) {
      diagnose(Tok.getLoc(), diag::default_with_where);
      whereLoc = consumeToken();
      NullablePtr<Expr> guard = parseExpr(diag::expected_case_where_expr);
      if (guard.isNull())
        return true;
      guardExpr = guard.get();
    }
    
    SourceLoc colonLoc = Tok.getLoc();
    if (!Tok.is(tok::colon))
      diagnose(Tok.getLoc(), diag::expected_case_colon, "default");
    colonLoc = consumeToken();
    
    // Create an implicit AnyPattern to represent the default match.
    auto any = new (Context) AnyPattern(defaultLoc);
    auto label = CaseLabel::create(Context, /*isDefault*/true,
                                   defaultLoc, any, whereLoc, guardExpr,
                                   colonLoc);
    labels.push_back(label);
  } while (Tok.is(tok::kw_case) || Tok.is(tok::kw_default));
  return false;
}

// stmt-case ::= case-label+ brace-item*
NullablePtr<CaseStmt> Parser::parseStmtCase() {
  // A case block has its own scope for variables bound out of the pattern.
  Scope scope(this, ScopeKind::CaseVars);
  
  llvm::SmallVector<CaseLabel*, 2> labels;
  llvm::SmallVector<Decl*, 4> boundDecls;
  if (parseStmtCaseLabels(labels, boundDecls))
    return nullptr;
  assert(!labels.empty() && "did not parse any labels?!");
  
  // Case blocks with multiple patterns cannot bind variables.
  if (!boundDecls.empty()
      && (labels.size() > 1 || labels[0]->getPatterns().size() > 1))
    diagnose(boundDecls[0]->getLoc(),
             diag::var_binding_with_multiple_case_patterns);
  
  llvm::SmallVector<ExprStmtOrDecl, 8> bodyItems;

  SourceLoc startOfBody = Tok.getLoc();
  parseBraceItems(bodyItems, /*isTopLevel*/ false, BraceItemListKind::Case);
  BraceStmt *body = BraceStmt::create(Context,
                                      startOfBody, bodyItems, Tok.getLoc());

  return CaseStmt::create(Context, labels, !boundDecls.empty(), body);
}
