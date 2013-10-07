//===--- Stmt.h - Swift Language Statement ASTs -----------------*- C++ -*-===//
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
// This file defines the Stmt class and subclasses.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_STMT_H
#define SWIFT_AST_STMT_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/Optional.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerUnion.h"

namespace swift {
  class ASTContext;
  class Decl;
  class Expr;
  class ASTWalker;
  class Pattern;
  class PatternBindingDecl;
  class VarDecl;
  
enum class StmtKind {
#define STMT(ID, PARENT) ID,
#include "swift/AST/StmtNodes.def"
};

/// Stmt - Base class for all statements in swift.
class alignas(8) Stmt {
  Stmt(const Stmt&) = delete;
  void operator=(const Stmt&) = delete;

  /// Kind - The subclass of Stmt that this is.
  unsigned Kind : 31;
  /// Implicit - Whether this statement is implicit.
  unsigned Implicit : 1;

protected:
  /// Return the given value for the 'implicit' flag if present, or if Nothing,
  /// return true if the location is invalid.
  bool getDefaultImplicitFlag(Optional<bool> implicit, SourceLoc keyLoc) {
    return implicit.hasValue() ? *implicit : keyLoc.isInvalid();
  }
  
public:
  Stmt(StmtKind kind, bool implicit)
    : Kind(unsigned(kind)), Implicit(unsigned(implicit)) {}

  StmtKind getKind() const { return StmtKind(Kind); }

  /// \brief Retrieve the name of the given statement kind.
  ///
  /// This name should only be used for debugging dumps and other
  /// developer aids, and should never be part of a diagnostic or exposed
  /// to the user of the compiler in any way.
  static StringRef getKindName(StmtKind kind);

  /// \brief Return the location of the start of the statement.
  SourceLoc getStartLoc() const { return getSourceRange().Start; }
  
  /// \brief Return the location of the end of the statement.
  SourceLoc getEndLoc() const { return getSourceRange().End; }
  
  SourceRange getSourceRange() const;
  SourceLoc TrailingSemiLoc;
  
  /// isImplicit - Determines whether this statement was implicitly-generated,
  /// rather than explicitly written in the AST.
  bool isImplicit() const { return bool(Implicit); }

  /// walk - This recursively walks the AST rooted at this statement.
  Stmt *walk(ASTWalker &walker);
  Stmt *walk(ASTWalker &&walker) { return walk(walker); }
  
  void dump() const;
  void print(raw_ostream &OS, unsigned Indent = 0) const;

  // Only allow allocation of Exprs using the allocator in ASTContext
  // or by doing a placement new.
  void *operator new(size_t Bytes, ASTContext &C,
                     unsigned Alignment = alignof(Stmt));
  
  // Make vanilla new/delete illegal for Stmts.
  void *operator new(size_t Bytes) throw() = delete;
  void operator delete(void *Data) throw() = delete;
  void *operator new(size_t Bytes, void *Mem) throw() = delete;
};

/// BraceStmt - A brace enclosed sequence of expressions, stmts, or decls, like
/// { var x = 10; println(10) }.
class BraceStmt : public Stmt {
public:
  typedef llvm::PointerUnion3<Expr*, Stmt*, Decl*> ExprStmtOrDecl;

private:
  unsigned NumElements;
  
  SourceLoc LBLoc;
  SourceLoc RBLoc;

  BraceStmt(SourceLoc lbloc, ArrayRef<ExprStmtOrDecl> elements,SourceLoc rbloc,
            Optional<bool> implicit);
  ExprStmtOrDecl *getElementsStorage() {
    return reinterpret_cast<ExprStmtOrDecl*>(this + 1);
  }

public:
  static BraceStmt *create(ASTContext &ctx, SourceLoc lbloc,
                           ArrayRef<ExprStmtOrDecl> elements,
                           SourceLoc rbloc,
                           Optional<bool> implicit = {});

  SourceLoc getLBraceLoc() const { return LBLoc; }
  SourceLoc getRBraceLoc() const { return RBLoc; }
  
  SourceRange getSourceRange() const { return SourceRange(LBLoc, RBLoc); }

  /// The elements contained within the BraceStmt.
  MutableArrayRef<ExprStmtOrDecl> getElements() {
    return MutableArrayRef<ExprStmtOrDecl>(getElementsStorage(), NumElements);
  }

  /// The elements contained within the BraceStmt (const version).
  ArrayRef<ExprStmtOrDecl> getElements() const {
    return const_cast<BraceStmt*>(this)->getElements();
  }

  static bool classof(const Stmt *S) { return S->getKind() == StmtKind::Brace; }
};

/// ReturnStmt - A return statement.  The result is optional; "return" without
/// an expression is semantically equivalent to "return ()".
///    return 42
class ReturnStmt : public Stmt {
  SourceLoc ReturnLoc;
  Expr *Result;
  
public:
  ReturnStmt(SourceLoc ReturnLoc, Expr *Result,
             Optional<bool> implicit = {})
    : Stmt(StmtKind::Return, getDefaultImplicitFlag(implicit, ReturnLoc)),
      ReturnLoc(ReturnLoc), Result(Result) {}

  SourceRange getSourceRange() const;
  SourceLoc getReturnLoc() const { return ReturnLoc; }

  bool hasResult() { return Result != 0; }
  Expr *getResult() const {
    assert(Result && "ReturnStmt doesn't have a result");
    return Result;
  }
  void setResult(Expr *e) { Result = e; }
  
  static bool classof(const Stmt *S) { return S->getKind() == StmtKind::Return;}
};

/// IfStmt - if/then/else statement.  If no 'else' is specified, then the
/// ElseLoc location is not specified and the Else statement is null. After
/// type-checking, the condition is of type Builtin.Int1.
class IfStmt : public Stmt {
  SourceLoc IfLoc;
  SourceLoc ElseLoc;
  Expr *Cond;
  Stmt *Then;
  Stmt *Else;
  
public:
  IfStmt(SourceLoc IfLoc, Expr *Cond, Stmt *Then, SourceLoc ElseLoc,
         Stmt *Else, Optional<bool> implicit = {})
  : Stmt(StmtKind::If, getDefaultImplicitFlag(implicit, IfLoc)),
    IfLoc(IfLoc), ElseLoc(ElseLoc), Cond(Cond), Then(Then), Else(Else) {}

  SourceLoc getIfLoc() const { return IfLoc; }
  SourceLoc getElseLoc() const { return ElseLoc; }

  SourceRange getSourceRange() const;

  Expr *getCond() const { return Cond; }
  void setCond(Expr *e) { Cond = e; }

  Stmt *getThenStmt() const { return Then; }
  void setThenStmt(Stmt *s) { Then = s; }

  Stmt *getElseStmt() const { return Else; }
  void setElseStmt(Stmt *s) { Else = s; }
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const Stmt *S) { return S->getKind() == StmtKind::If; }
};

/// WhileStmt - while statement. After type-checking, the condition is of
/// type Builtin.Int1.
class WhileStmt : public Stmt {
  SourceLoc WhileLoc;
  Expr *Cond;
  Stmt *Body;
  
public:
  WhileStmt(SourceLoc WhileLoc, Expr *Cond, Stmt *Body,
            Optional<bool> implicit = {})
  : Stmt(StmtKind::While, getDefaultImplicitFlag(implicit, WhileLoc)),
    WhileLoc(WhileLoc), Cond(Cond), Body(Body) {}

  SourceRange getSourceRange() const;

  Expr *getCond() const { return Cond; }
  void setCond(Expr *e) { Cond = e; }

  Stmt *getBody() const { return Body; }
  void setBody(Stmt *s) { Body = s; }
  
  static bool classof(const Stmt *S) { return S->getKind() == StmtKind::While; }
};
  
/// DoWhileStmt - do/while statement. After type-checking, the condition is of
/// type Builtin.Int1.
class DoWhileStmt : public Stmt {
  SourceLoc DoLoc, WhileLoc;
  Stmt *Body;
  Expr *Cond;
  
public:
  DoWhileStmt(SourceLoc DoLoc, Expr *Cond, SourceLoc WhileLoc, Stmt *Body,
              Optional<bool> implicit = {})
    : Stmt(StmtKind::DoWhile, getDefaultImplicitFlag(implicit, DoLoc)),
      DoLoc(DoLoc), WhileLoc(WhileLoc), Body(Body), Cond(Cond) {}
  
  SourceRange getSourceRange() const;
  
  Stmt *getBody() const { return Body; }
  void setBody(Stmt *s) { Body = s; }

  Expr *getCond() const { return Cond; }
  void setCond(Expr *e) { Cond = e; }
  
  static bool classof(const Stmt *S) {return S->getKind() == StmtKind::DoWhile;}
};

/// ForStmt - for statement.  After type-checking, the condition is of
/// type Builtin.Int1.  Note that the condition is optional.  If not present,
/// it always evaluates to true.  The Initializer and Increment are also
/// optional.
class ForStmt : public Stmt {
  SourceLoc ForLoc, Semi1Loc, Semi2Loc;
  NullablePtr<Expr> Initializer;
  ArrayRef<Decl*> InitializerVarDecls;
  NullablePtr<Expr> Cond;
  NullablePtr<Expr> Increment;
  Stmt *Body;
  
public:
  ForStmt(SourceLoc ForLoc,
          NullablePtr<Expr> Initializer,
          ArrayRef<Decl*> InitializerVarDecls,
          SourceLoc Semi1Loc, NullablePtr<Expr> Cond, SourceLoc Semi2Loc,
          NullablePtr<Expr> Increment,
          Stmt *Body,
          Optional<bool> implicit = {})
  : Stmt(StmtKind::For, getDefaultImplicitFlag(implicit, ForLoc)),
    ForLoc(ForLoc), Semi1Loc(Semi1Loc),
    Semi2Loc(Semi2Loc), Initializer(Initializer),
    InitializerVarDecls(InitializerVarDecls),
    Cond(Cond), Increment(Increment), Body(Body) {
  }
  
  SourceRange getSourceRange() const {
    return SourceRange(ForLoc, Body->getEndLoc());
  }
  
  NullablePtr<Expr> getInitializer() const { return Initializer; }
  void setInitializer(Expr *V) { Initializer = V; }
  
  ArrayRef<Decl*> getInitializerVarDecls() const { return InitializerVarDecls; }
  void setInitializerVarDecls(ArrayRef<Decl*> D) { InitializerVarDecls = D; }

  NullablePtr<Expr> getCond() const { return Cond; }
  void setCond(NullablePtr<Expr> C) { Cond = C; }

  NullablePtr<Expr> getIncrement() const { return Increment; }
  void setIncrement(Expr *V) { Increment = V; }

  Stmt *getBody() const { return Body; }
  void setBody(Stmt *s) { Body = s; }
  
  static bool classof(const Stmt *S) { return S->getKind() == StmtKind::For; }
};

/// ForEachStmt - foreach statement that iterates over the elements in a
/// container.
///
/// Example:
/// \code
/// for i in 0..10 {
///   println(String(i))
/// }
/// \endcode
class ForEachStmt : public Stmt {
  SourceLoc ForLoc;
  SourceLoc InLoc;
  llvm::PointerUnion<Pattern *, PatternBindingDecl *> Pat;
  Expr *Container;
  BraceStmt *Body;
  
  /// Range - The range variable along with its initializer.
  PatternBindingDecl *Range = nullptr;
  /// RangeEmpty - The expression that determines whether the range is empty.
  Expr *RangeEmpty = nullptr;
  
  /// The generator variable along with its initializer.
  PatternBindingDecl *Generator = nullptr;
  /// The expression that advances the generator and returns an Optional with
  /// the next value or None to signal end-of-stream.
  Expr *GeneratorNext = nullptr;

public:
  ForEachStmt(SourceLoc ForLoc, Pattern *Pat, SourceLoc InLoc,
              Expr *Container, BraceStmt *Body,
              Optional<bool> implicit = {})
    : Stmt(StmtKind::ForEach, getDefaultImplicitFlag(implicit, ForLoc)),
      ForLoc(ForLoc), InLoc(InLoc), Pat(Pat),
      Container(Container), Body(Body) { }
  
  /// getForLoc - Retrieve the location of the 'for' keyword.
  SourceLoc getForLoc() const { return ForLoc; }

  /// getInLoc - Retrieve the location of the 'in' keyword.
  SourceLoc getInLoc() const { return InLoc; }
  
  /// getPattern - Retrieve the pattern describing the iteration variables.
  /// These variables will only be visible within the body of the loop.
  Pattern *getPattern() const;
  
  /// getContainer - Retrieve the container whose elements will be visited
  /// by this foreach loop, as it was written in the source code and
  /// subsequently type-checked. To determine the semantic behavior of this
  /// expression to extract a range, use \c getRangeInit().
  Expr *getContainer() const { return Container; }
  void setContainer(Expr *C) { Container = C; }
  
  /// getRange - Retrieve the pattern binding that contains the (implicit)
  /// range variable and its initialization from the container.
  PatternBindingDecl *getRange() const { return Range; }
  void setRange(PatternBindingDecl *R) { Range = R; }
  
  /// getRangeEmpty - Retrieve the expression that determines whether the
  /// given range is empty.
  Expr *getRangeEmpty() const { return RangeEmpty; }
  void setRangeEmpty(Expr *E) { RangeEmpty = E; }
  
  /// getElementInit - Retrieve the pattern binding that binds the pattern
  /// (with the iteration variables) to the initialization of that pattern
  /// from the result of getFirst().
  PatternBindingDecl *getElementInit() const {
    return Pat.dyn_cast<PatternBindingDecl *>();
  }
  void setElementInit(PatternBindingDecl *EI) { Pat = EI; }
  
  /// Retrieve the pattern binding that contains the (implicit) generator
  /// variable and its initialization from the container.
  PatternBindingDecl *getGenerator() const { return Generator; }
  void setGenerator(PatternBindingDecl *G) { Generator = G; }
  
  /// Retrieve the expression that advances the generator.
  Expr *getGeneratorNext() const { return GeneratorNext; }
  void setGeneratorNext(Expr *E) { GeneratorNext = E; }

  /// getBody - Retrieve the body of the loop.
  BraceStmt *getBody() const { return Body; }
  void setBody(BraceStmt *B) { Body = B; }
  
  SourceRange getSourceRange() const {
    return SourceRange(ForLoc, Body->getEndLoc());
  }
  
  static bool classof(const Stmt *S) {
    return S->getKind() == StmtKind::ForEach;
  }
};
  
/// A label used at the head of a 'case' block. Each 'case' label may have one
/// or more comma-separated patterns. The 'case' may also optionally have a
/// 'where' guard expression. 'default' is allowed as an alternate spelling of
/// 'case _'.
///
/// Some examples:
///
/// case 1:
/// case 2, 3:
/// case Foo(var x, var y) where x < y:
/// default:
class CaseLabel {
  SourceLoc CaseLoc;
  SourceLoc ColonLoc;
  SourceLoc WhereLoc;
  llvm::PointerIntPair<Expr *, 1, bool> GuardExprAndIsDefault;
  unsigned NumPatterns;
  
  CaseLabel(bool isDefault,
            SourceLoc caseLoc, ArrayRef<Pattern*> patterns,
            SourceLoc whereLoc, Expr *guardExpr,
            SourceLoc colonLoc);
  
  Pattern **getPatternsBuffer() {
    return reinterpret_cast<Pattern**>(this + 1);
  }
  const Pattern * const *getPatternsBuffer() const {
    return reinterpret_cast<const Pattern * const *>(this + 1);
  }
  
  /// CaseLabels should be allocated with an ASTContext using create.
  void *operator new(size_t) = delete;
  void operator delete(void*) = delete;
  
public:
  static CaseLabel *create(ASTContext &C,
                           bool isDefault,
                           SourceLoc caseLoc,
                           ArrayRef<Pattern*> patterns,
                           SourceLoc whereLoc,
                           Expr * /*nullable*/ guardExpr,
                           SourceLoc colonLoc);
  
  SourceLoc getLoc() const { return CaseLoc; }
  SourceLoc getCaseLoc() const { return CaseLoc; }
  SourceLoc getColonLoc() const { return ColonLoc; }
  SourceLoc getWhereLoc() const { return WhereLoc; }
  
  SourceRange getSourceRange() const {
    return {CaseLoc, ColonLoc};
  }
  
  MutableArrayRef<Pattern*> getPatterns() {
    return {getPatternsBuffer(), NumPatterns};
  }
  ArrayRef<const Pattern *> getPatterns() const {
    return {getPatternsBuffer(), NumPatterns};
  }
  
  /// Return the guard expression if present, or null if the case label has
  /// no guard.
  Expr *getGuardExpr() const { return GuardExprAndIsDefault.getPointer(); }
  void setGuardExpr(Expr *e) { GuardExprAndIsDefault.setPointer(e); }
  
  /// Returns true if this is syntactically a 'default' label.
  bool isDefault() const { return GuardExprAndIsDefault.getInt(); }
};
  
/// A 'case' or 'default' block of a switch statement. Only valid as the
/// substatement of a SwitchStmt. A case block begins either with one or more
/// CaseLabels or a single 'default' label.
class CaseStmt : public Stmt {
  llvm::PointerIntPair<Stmt *, 1, bool> BodyAndHasBoundDecls;
  unsigned NumCaseLabels;

  CaseLabel * const *getCaseLabelsBuffer() const {
    return reinterpret_cast<CaseLabel * const *>(this + 1);
  }
  CaseLabel **getCaseLabelsBuffer() {
    return reinterpret_cast<CaseLabel **>(this + 1);
  }
  
  CaseStmt(ArrayRef<CaseLabel*> Labels, bool hasBoundDecls, Stmt *Body,
           Optional<bool> Implicit);
  
public:
  static CaseStmt *create(ASTContext &C,
                          ArrayRef<CaseLabel*> Labels,
                          bool hasBoundDecls,
                          Stmt *Body,
                          Optional<bool> Implicit = {});
  
  ArrayRef<CaseLabel *> getCaseLabels() const {
    return {getCaseLabelsBuffer(), NumCaseLabels};
  }
  
  Stmt *getBody() const { return BodyAndHasBoundDecls.getPointer(); }
  void setBody(Stmt *body) { BodyAndHasBoundDecls.setPointer(body); }
  
  /// True if the case block declares any patterns with local variable bindings.
  bool hasBoundDecls() const { return BodyAndHasBoundDecls.getInt(); }
  
  /// Get the source location of the 'case' or 'default' of the first label.
  SourceLoc getLoc() const {
    return getCaseLabels()[0]->getLoc();
  }
  
  SourceRange getSourceRange() const {
    return {getLoc(), getBody()->getEndLoc()};
  }
  
  bool isDefault() {
    return getCaseLabels()[0]->isDefault();
  }
  
  static bool classof(const Stmt *S) {
    return S->getKind() == StmtKind::Case;
  }
};

/// Switch statement.
class SwitchStmt : public Stmt {
  SourceLoc SwitchLoc, LBraceLoc, RBraceLoc;
  Expr *SubjectExpr;
  unsigned CaseCount;
  
  CaseStmt * const *getCaseBuffer() const {
    return reinterpret_cast<CaseStmt * const *>(this + 1);
  }

  CaseStmt **getCaseBuffer() {
    return reinterpret_cast<CaseStmt **>(this + 1);
  }
  
  SwitchStmt(SourceLoc SwitchLoc,
             Expr *SubjectExpr,
             SourceLoc LBraceLoc,
             unsigned CaseCount,
             SourceLoc RBraceLoc,
             Optional<bool> implicit = {})
    : Stmt(StmtKind::Switch, getDefaultImplicitFlag(implicit, SwitchLoc)),
      SwitchLoc(SwitchLoc), LBraceLoc(LBraceLoc), RBraceLoc(RBraceLoc),
      SubjectExpr(SubjectExpr), CaseCount(CaseCount)
  {}

public:
  /// Allocate a new SwitchStmt in the given ASTContext.
  static SwitchStmt *create(SourceLoc SwitchLoc,
                            Expr *SubjectExpr,
                            SourceLoc LBraceLoc,
                            ArrayRef<CaseStmt*> Cases,
                            SourceLoc RBraceLoc,
                            ASTContext &C);
  
  /// Get the source location of the 'switch' keyword.
  SourceLoc getSwitchLoc() const { return SwitchLoc; }
  /// Get the source location of the opening brace.
  SourceLoc getLBraceLoc() const { return LBraceLoc; }
  /// Get the source location of the closing brace.
  SourceLoc getRBraceLoc() const { return RBraceLoc; }
  
  SourceLoc getLoc() const { return SwitchLoc; }
  SourceRange getSourceRange() const { return {SwitchLoc, RBraceLoc}; }
  
  /// Get the subject expression of the switch.
  Expr *getSubjectExpr() const { return SubjectExpr; }
  void setSubjectExpr(Expr *e) { SubjectExpr = e; }
  
  /// Get the list of case clauses.
  ArrayRef<CaseStmt*> getCases() const {
    return {getCaseBuffer(), CaseCount};
  }
  
  static bool classof(const Stmt *S) {
    return S->getKind() == StmtKind::Switch;
  }
};

/// BreakStmt - The keyword "break".
class BreakStmt : public Stmt {
  SourceLoc Loc;
  
public:
  BreakStmt(SourceLoc Loc, Optional<bool> implicit = {})
    : Stmt(StmtKind::Break, getDefaultImplicitFlag(implicit, Loc)), Loc(Loc)
  {}

  SourceLoc getLoc() const { return Loc; }
  
  SourceRange getSourceRange() const { return Loc; }

  static bool classof(const Stmt *S) {
    return S->getKind() == StmtKind::Break;
  }
};

/// ContinueStmt - The keyword "continue".
class ContinueStmt : public Stmt {
  SourceLoc Loc;
  
public:
  ContinueStmt(SourceLoc Loc, Optional<bool> implicit = {})
    : Stmt(StmtKind::Continue, getDefaultImplicitFlag(implicit, Loc)), Loc(Loc)
  {}

  SourceLoc getLoc() const { return Loc; }
  
  SourceRange getSourceRange() const { return Loc; }

  static bool classof(const Stmt *S) {
    return S->getKind() == StmtKind::Continue;
  }
};

/// FallthroughStmt - The keyword "fallthrough".
class FallthroughStmt : public Stmt {
  SourceLoc Loc;
  CaseStmt *FallthroughDest;
  
public:
  FallthroughStmt(SourceLoc Loc, Optional<bool> implicit = {})
    : Stmt(StmtKind::Fallthrough, getDefaultImplicitFlag(implicit, Loc)),
      Loc(Loc), FallthroughDest(nullptr)
  {}
  
  SourceLoc getLoc() const { return Loc; }
  
  SourceRange getSourceRange() const { return Loc; }
  
  /// Get the CaseStmt block to which the fallthrough transfers control.
  /// Set during Sema.
  CaseStmt *getFallthroughDest() const {
    assert(FallthroughDest && "fallthrough dest is not set until Sema");
    return FallthroughDest;
  }
  void setFallthroughDest(CaseStmt *C) {
    assert(!FallthroughDest && "fallthrough dest already set?!");
    FallthroughDest = C;
  }
  
  static bool classof(const Stmt *S) {
    return S->getKind() == StmtKind::Fallthrough;
  }
};

} // end namespace swift

#endif
