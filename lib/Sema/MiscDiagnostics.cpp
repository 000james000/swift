//===--- MiscDiagnostics.cpp - AST-Level Diagnostics ----------------------===//
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
// This file implements AST-level diagnostics.
//
//===----------------------------------------------------------------------===//

#include "MiscDiagnostics.h"
#include "TypeChecker.h"
#include "swift/Basic/SourceManager.h"
#include "swift/AST/ASTWalker.h"

using namespace swift;

//===--------------------------------------------------------------------===//
// Diagnose assigning variable to itself.
//===--------------------------------------------------------------------===//

static Decl *findSimpleReferencedDecl(const Expr *E) {
  if (auto *LE = dyn_cast<LoadExpr>(E))
    E = LE->getSubExpr();

  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  return nullptr;
}

static std::pair<Decl *, Decl *> findReferencedDecl(const Expr *E) {
  if (auto *LE = dyn_cast<LoadExpr>(E))
    E = LE->getSubExpr();

  if (auto *D = findSimpleReferencedDecl(E))
    return std::make_pair(nullptr, D);

  if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    if (auto *BaseDecl = findSimpleReferencedDecl(MRE->getBase()))
      return std::make_pair(BaseDecl, MRE->getMember().getDecl());
  }

  return std::make_pair(nullptr, nullptr);
}

/// Diagnose assigning variable to itself.
static void diagSelfAssignment(TypeChecker &TC, const Expr *E) {
  auto *AE = dyn_cast<AssignExpr>(E);
  if (!AE)
    return;

  auto LHSDecl = findReferencedDecl(AE->getDest());
  auto RHSDecl = findReferencedDecl(AE->getSrc());
  if (LHSDecl.second && LHSDecl == RHSDecl) {
    TC.diagnose(AE->getLoc(), LHSDecl.first ? diag::self_assignment_prop
                                            : diag::self_assignment_var)
        .highlight(AE->getDest()->getSourceRange())
        .highlight(AE->getSrc()->getSourceRange());
  }
}


/// Issue a warning on code where a returned expression is on a different line
/// than the return keyword, but both have the same indentation.
///
/// \code
///   ...
///   return
///   foo()
/// \endcode
static void diagUnreachableCode(TypeChecker &TC, const Stmt *S) {
  auto *RS = dyn_cast<ReturnStmt>(S);
  if (!RS)
    return;
  if (!RS->hasResult())
    return;

  auto RetExpr = RS->getResult();
  auto RSLoc = RS->getStartLoc();
  auto RetExprLoc = RetExpr->getStartLoc();
  // FIXME: Expose getColumnNumber() in LLVM SourceMgr to make this check
  // cheaper.
  if (RSLoc.isInvalid() || RetExprLoc.isInvalid() || (RSLoc == RetExprLoc))
    return;
  SourceManager &SM = TC.Context.SourceMgr;
  if (SM.getLineAndColumn(RSLoc).second ==
      SM.getLineAndColumn(RetExprLoc).second) {
    TC.diagnose(RetExpr->getStartLoc(), diag::unindented_code_after_return);
    TC.diagnose(RetExpr->getStartLoc(), diag::indent_expression_to_silence);
    return;
  }
  return;
}


/// Diagnose use of module values outside of dot expressions.
static void diagModuleValue(TypeChecker &TC, const Expr *E) {
  class DiagnoseWalker : public ASTWalker {
  public:
    TypeChecker &TC;

    DiagnoseWalker(TypeChecker &TC) : TC(TC) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (auto *ME = dyn_cast<ModuleExpr>(E)) {
        bool Diagnose = true;
        if (auto *ParentExpr = Parent.getAsExpr()) {
          // Allow module values as a part of:
          // - ignored base expressions;
          // - expressions that failed to type check.
          if (isa<DotSyntaxBaseIgnoredExpr>(ParentExpr) ||
              isa<UnresolvedDotExpr>(ParentExpr))
            Diagnose = false;
        }
        if (Diagnose)
          TC.diagnose(ME->getStartLoc(), diag::value_of_module_type);
      }
      return { true, E };
    }
  };

  DiagnoseWalker Walker(TC);
  const_cast<Expr *>(E)->walk(Walker);
}


/// Diagnose recursive use of properties within their own accessors
static void diagRecursivePropertyAccess(TypeChecker &TC, const Expr *E,
                                        const DeclContext *DC) {
  auto fn = dyn_cast<FuncDecl>(DC);
  if (!fn || !fn->isGetterOrSetter())
    return;

  auto var = dyn_cast<VarDecl>(fn->getAccessorStorageDecl());
  if (!var)  // Ignore subscripts
    return;

  class DiagnoseWalker : public ASTWalker {
    TypeChecker &TC;
    VarDecl *Var;
    bool IsSetter;

  public:
    explicit DiagnoseWalker(TypeChecker &TC, VarDecl *var, bool isSetter)
      : TC(TC), Var(var), IsSetter(isSetter) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
        // Handle local and top-level computed variables.
        if (DRE->getDecl() == Var &&
            !DRE->isDirectPropertyAccess()) {
          bool shouldDiagnose = true;
          if (auto *ParentExpr = Parent.getAsExpr()) {
            if (isa<DotSyntaxBaseIgnoredExpr>(ParentExpr))
              shouldDiagnose = false;
            else if (IsSetter)
              shouldDiagnose = !isa<LoadExpr>(ParentExpr);
          }
          if (shouldDiagnose) {
            TC.diagnose(E->getLoc(), diag::recursive_accessor_reference,
                        Var->getName(), IsSetter);
          }
        }

      } else if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
        // Handle instance and type computed variables.
        // Find MemberRefExprs that have an implicit "self" base.
        if (MRE->getMember().getDecl() == Var &&
            isa<DeclRefExpr>(MRE->getBase()) &&
            MRE->getBase()->isImplicit() &&
            !MRE->isDirectPropertyAccess()) {
          bool shouldDiagnose = true;
          if (IsSetter)
            shouldDiagnose = !dyn_cast_or_null<LoadExpr>(Parent.getAsExpr());

          if (shouldDiagnose) {
            TC.diagnose(E->getLoc(), diag::recursive_accessor_reference,
                        Var->getName(), IsSetter);
            TC.diagnose(E->getLoc(),
                        diag::recursive_accessor_reference_silence)
              .fixItInsert(E->getStartLoc(), "self.");
          }
        }

      } else if (auto *PE = dyn_cast<IdentityExpr>(E)) {
        // Look through ParenExprs because a function argument of a single
        // rvalue will have a LoadExpr /outside/ the ParenExpr.
        return { true, PE->getSubExpr() };
      }

      return { true, E };
    }
  };

  DiagnoseWalker walker(TC, var, fn->isSetter());
  const_cast<Expr *>(E)->walk(walker);
}

//===--------------------------------------------------------------------===//
// High-level entry points.
//===--------------------------------------------------------------------===//

void swift::performExprDiagnostics(TypeChecker &TC, const Expr *E,
                                   const DeclContext *DC) {
  diagSelfAssignment(TC, E);
  diagModuleValue(TC, E);
  diagRecursivePropertyAccess(TC, E, DC);
}

void swift::performStmtDiagnostics(TypeChecker &TC, const Stmt *S) {
  return diagUnreachableCode(TC, S);
}

