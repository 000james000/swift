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
#include "swift/Parse/Lexer.h"

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


/// Diagnose syntactic restrictions of expressions:
///   - Module values may only occur as part of qualification.
///   - Metatype names cannot generally be used as values: they need a "T.self"
///     qualification unless used in narrow case (e.g. T() for construction).
///   - NoEscape parameters are only allowed to be called, not copied around.
///
static void diagSyntacticUseRestrictions(TypeChecker &TC, const Expr *E) {
  class DiagnoseWalker : public ASTWalker {
    SmallPtrSet<Expr*, 4> AlreadyDiagnosedMetatypes;
    SmallPtrSet<DeclRefExpr*, 4> AlreadyDiagnosedNoEscapes;
  public:
    TypeChecker &TC;

    DiagnoseWalker(TypeChecker &TC) : TC(TC) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      // Diagnose module values that don't appear as part of a qualification.
      if (auto *ME = dyn_cast<ModuleExpr>(E))
        checkUseOfModuleExpr(ME);

      // See through implicit conversions of the expression.  We want to be able
      // to associate the parent of this expression with the ultimate callee.
      auto Base = E;
      while (auto Conv = dyn_cast<ImplicitConversionExpr>(Base))
        Base = Conv->getSubExpr();

      if (auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
        // Verify metatype uses.
        if (isa<TypeDecl>(DRE->getDecl()))
          checkUseOfMetaTypeName(Base);

        // Verify noescape parameter uses.
        checkNoEscapeParameterUse(DRE, nullptr);
      }
      if (auto *MRE = dyn_cast<MemberRefExpr>(Base))
        if (isa<TypeDecl>(MRE->getMember().getDecl()))
          checkUseOfMetaTypeName(Base);
      if (isa<TypeExpr>(Base))
        checkUseOfMetaTypeName(Base);

      if (auto *CE = dyn_cast<ClosureExpr>(E))
        checkNoEscapeClosureCaptures(CE);

      // Check function calls, looking through implicit conversions on the
      // function and inspecting the arguments directly.
      if (auto *Call = dyn_cast<ApplyExpr>(E)) {
        // Check the callee.
        if (auto *DRE = dyn_cast<DeclRefExpr>(Call->getFn()))
          checkNoEscapeParameterUse(DRE, Call);

        // The argument is either a ParenExpr or TupleExpr.
        ArrayRef<Expr*> arguments;
        if (auto *PE = dyn_cast<ParenExpr>(Call->getArg()))
          arguments = PE->getSubExpr();
        else if (auto *TE = dyn_cast<TupleExpr>(Call->getArg()))
          arguments = TE->getElements();
        else
          arguments = Call->getArg();

        // Check each argument.
        for (auto arg : arguments) {
          if (auto *DRE = dyn_cast<DeclRefExpr>(arg))
            checkNoEscapeParameterUse(DRE, Call);
        }
      }

      return { true, E };
    }

    void checkUseOfModuleExpr(ModuleExpr *E) {
      // Allow module values as a part of:
      // - ignored base expressions;
      // - expressions that failed to type check.
      if (auto *ParentExpr = Parent.getAsExpr()) {
        if (isa<DotSyntaxBaseIgnoredExpr>(ParentExpr) ||
            isa<UnresolvedDotExpr>(ParentExpr))
          return;
      }

      TC.diagnose(E->getStartLoc(), diag::value_of_module_type);
    }

    /// The DRE argument is a reference to a noescape parameter.  Verify that
    /// its uses are ok.
    void checkNoEscapeParameterUse(DeclRefExpr *DRE, Expr *ParentExpr=nullptr) {
      // This only cares about declarations marked noescape.
      if (!DRE->getDecl()->getAttrs().hasAttribute<NoEscapeAttr>())
        return;

      // Only diagnose this once.  If we check and accept this use higher up in
      // the AST, don't recheck here.
      if (!AlreadyDiagnosedNoEscapes.insert(DRE).second)
        return;

      // The only valid use of the noescape parameter is an immediate call,
      // either as the callee or as an argument (in which case, the typechecker
      // validates that the noescape bit didn't get stripped off).
      if (ParentExpr && isa<ApplyExpr>(ParentExpr)) // param()
        return;

      TC.diagnose(DRE->getStartLoc(), diag::invalid_noescape_use,
                  DRE->getDecl()->getName());
    }

    /// Check the specified closure to make sure it doesn't capture a noescape
    /// value, or that it is itself noescape if so.
    void checkNoEscapeClosureCaptures(ClosureExpr *CE) {
      if (!CE->getType() || CE->getType()->is<ErrorType>())
        return; // Ignore errorneous code.
      auto Ty = CE->getType()->castTo<FunctionType>();

      // If this closure is used in a noescape context, it can do anything.
      if (Ty->isNoEscape()) return;

      // Otherwise, check the capture list to make sure it isn't escaping
      // something.
      for (auto CapVD : CE->getCaptureInfo().getCaptures())
        if (CapVD->getAttrs().hasAttribute<NoEscapeAttr>())
          TC.diagnose(CE->getStartLoc(), diag::closure_noescape_use,
                      CapVD->getName());
    }

    // Diagnose metatype values that don't appear as part of a property,
    // method, or constructor reference.
    void checkUseOfMetaTypeName(Expr *E) {
      // If we've already checked this at a higher level, we're done.
      if (!AlreadyDiagnosedMetatypes.insert(E).second)
        return;

      // Allow references to types as a part of:
      // - member references T.foo, T.Type, T.self, etc. (but *not* T.type)
      // - constructor calls T()
      if (auto *ParentExpr = Parent.getAsExpr()) {
        // Reject use of "T.dynamicType", it should be written as "T.self".
        if (auto metaExpr = dyn_cast<DynamicTypeExpr>(ParentExpr)) {
          // Add a fixit to replace '.dynamicType' with '.self'.
          TC.diagnose(E->getStartLoc(), diag::type_of_metatype)
            .fixItReplace(metaExpr->getMetatypeLoc(), "self");
          return;
        }

        // This is the white-list of accepted syntactic forms.
        if (isa<ErrorExpr>(ParentExpr) ||
            isa<DotSelfExpr>(ParentExpr) ||               // T.self
            isa<CallExpr>(ParentExpr) ||                  // T()
            isa<MemberRefExpr>(ParentExpr) ||             // T.foo
            isa<UnresolvedMemberExpr>(ParentExpr) ||
            isa<SelfApplyExpr>(ParentExpr) ||             // T.foo()  T()
            isa<UnresolvedDotExpr>(ParentExpr) ||
            isa<DotSyntaxBaseIgnoredExpr>(ParentExpr) ||
            isa<UnresolvedSelectorExpr>(ParentExpr) ||
            isa<UnresolvedSpecializeExpr>(ParentExpr)) {
          return;
        }
      }

      TC.diagnose(E->getStartLoc(), diag::value_of_metatype_type);
      // Add fixits to insert '()' or '.self'.
      auto endLoc = Lexer::getLocForEndOfToken(TC.Context.SourceMgr,
                                               E->getEndLoc());
      TC.diagnose(endLoc, diag::add_parens_to_type).fixItInsert(endLoc, "()");
      TC.diagnose(endLoc, diag::add_self_to_type).fixItInsert(endLoc, ".self");
    }
  };

  DiagnoseWalker Walker(TC);
  const_cast<Expr *>(E)->walk(Walker);
}


/// Diagnose recursive use of properties within their own accessors
static void diagRecursivePropertyAccess(TypeChecker &TC, const Expr *E,
                                        const DeclContext *DC) {
  auto fn = dyn_cast<FuncDecl>(DC);
  if (!fn || !fn->isAccessor())
    return;

  auto var = dyn_cast<VarDecl>(fn->getAccessorStorageDecl());
  if (!var)  // Ignore subscripts
    return;

  class DiagnoseWalker : public ASTWalker {
    TypeChecker &TC;
    VarDecl *Var;
    const FuncDecl *Accessor;

  public:
    explicit DiagnoseWalker(TypeChecker &TC, VarDecl *var,
                            const FuncDecl *Accessor)
      : TC(TC), Var(var), Accessor(Accessor) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
        // Handle local and top-level computed variables.
        if (DRE->getDecl() == Var &&
            DRE->getAccessSemantics() != AccessSemantics::DirectToStorage &&
            Accessor->getAccessorKind() != AccessorKind::IsMaterializeForSet) {
          bool shouldDiagnose = true;
          if (auto *ParentExpr = Parent.getAsExpr()) {
            if (isa<DotSyntaxBaseIgnoredExpr>(ParentExpr))
              shouldDiagnose = false;
            else if (Accessor->isSetter())
              shouldDiagnose = !isa<LoadExpr>(ParentExpr);
          }
          if (shouldDiagnose) {
            TC.diagnose(E->getLoc(), diag::recursive_accessor_reference,
                        Var->getName(), Accessor->isSetter());
          }
        }
        
        // If this is a direct store in a "willSet", we reject this because
        // it is about to get overwritten.
        if (DRE->getDecl() == Var &&
            DRE->getAccessSemantics() == AccessSemantics::DirectToStorage &&
            !dyn_cast_or_null<LoadExpr>(Parent.getAsExpr()) &&
            Accessor->getAccessorKind() == AccessorKind::IsWillSet) {
          TC.diagnose(E->getLoc(), diag::store_in_willset, Var->getName());
        }


      } else if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
        // Handle instance and type computed variables.
        // Find MemberRefExprs that have an implicit "self" base.
        if (MRE->getMember().getDecl() == Var &&
            isa<DeclRefExpr>(MRE->getBase()) &&
            MRE->getBase()->isImplicit()) {
          
          if (MRE->getAccessSemantics() != AccessSemantics::DirectToStorage) {
            bool shouldDiagnose = false;
            // Warn about any property access in the getter.
            if (Accessor->isGetter())
              shouldDiagnose = true;
            // Warn about stores in the setter, but allow loads.
            if (Accessor->isSetter())
              shouldDiagnose = !dyn_cast_or_null<LoadExpr>(Parent.getAsExpr());

            if (shouldDiagnose) {
              TC.diagnose(E->getLoc(), diag::recursive_accessor_reference,
                          Var->getName(), Accessor->isSetter());
              TC.diagnose(E->getLoc(),
                          diag::recursive_accessor_reference_silence)
              .fixItInsert(E->getStartLoc(), "self.");
            }
          } else {
            // If this is a direct store in a "willSet", we reject this because
            // it is about to get overwritten.
            if (!dyn_cast_or_null<LoadExpr>(Parent.getAsExpr()) &&
                Accessor->getAccessorKind() == AccessorKind::IsWillSet) {
              TC.diagnose(E->getLoc(), diag::store_in_willset, Var->getName());
            }
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

  DiagnoseWalker walker(TC, var, fn);
  const_cast<Expr *>(E)->walk(walker);
}

/// Look for any property references in closures that lack a "self." qualifier.
/// Within a closure, we require that the source code contain "self." explicitly
/// because 'self' is captured, not the property value.  This is a common source
/// of confusion, so we force an explicit self.
static void diagnoseImplicitSelfUseInClosure(TypeChecker &TC, const Expr *E) {
  class DiagnoseWalker : public ASTWalker {
    TypeChecker &TC;
    unsigned InClosure = 0;
  public:
    explicit DiagnoseWalker(TypeChecker &TC) : TC(TC) {}

    /// Return true if this is an implicit reference to self.
    static bool isImplicitSelfUse(Expr *E) {
      auto *DRE = dyn_cast<DeclRefExpr>(E);
      return DRE && DRE->isImplicit() && DRE->getDecl()->hasName() &&
             DRE->getDecl()->getName().str() == "self";
    }

    /// Return true if this is a closure expression that will require "self."
    /// qualification of member references.
    static bool isClosureRequiringSelfQualification(const Expr *E) {
      if (!isa<ClosureExpr>(E)) return false;

      // If the closure's type was inferred to be noescape, then it doesn't
      // need qualification.
      if (E->getType())
        if (auto *FT = E->getType()->getAs<FunctionType>())
          return !FT->isNoEscape();
      return true;
    }


    // Don't walk into nested decls.
    bool walkToDeclPre(Decl *D) override {
      return false;
    }

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {

      // If this is an explicit closure expression - not an autoclosure - then
      // we keep track of the fact that recursive walks are within the closure.
      if (isClosureRequiringSelfQualification(E))
        ++InClosure;

      // If we aren't in a closure, no diagnostics will be produced.
      if (!InClosure)
        return { true, E };

      // If we see a property reference with an implicit base from within a
      // closure, then reject it as requiring an explicit "self." qualifier.  We
      // do this in explicit closures, not autoclosures, because otherwise the
      // transparence of autoclosures is lost.
      if (auto *MRE = dyn_cast<MemberRefExpr>(E))
        if (isImplicitSelfUse(MRE->getBase())) {
          TC.diagnose(MRE->getLoc(),
                      diag::property_use_in_closure_without_explicit_self,
                      MRE->getMember().getDecl()->getName())
            .fixItInsert(MRE->getLoc(), "self.");
          return { false, E };
        }

      // Handle method calls with a specific diagnostic + fixit.
      if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(E))
        if (isImplicitSelfUse(DSCE->getBase()) &&
            isa<DeclRefExpr>(DSCE->getFn())) {
          auto MethodExpr = cast<DeclRefExpr>(DSCE->getFn());
          TC.diagnose(DSCE->getLoc(),
                      diag::method_call_in_closure_without_explicit_self,
                      MethodExpr->getDecl()->getName())
            .fixItInsert(DSCE->getLoc(), "self.");
          return { false, E };
        }

      // Catch any other implicit uses of self with a generic diagnostic.
      if (isImplicitSelfUse(E))
        TC.diagnose(E->getLoc(), diag::implicit_use_of_self_in_closure);

      return { true, E };
    }
    
    Expr *walkToExprPost(Expr *E) {
      if (isClosureRequiringSelfQualification(E)) {
        assert(InClosure);
        --InClosure;
      }
      
      return E;
    }
  };
  
  const_cast<Expr *>(E)->walk(DiagnoseWalker(TC));
}

//===--------------------------------------------------------------------===//
// Diagnose availability.
//===--------------------------------------------------------------------===//

/// Diagnose uses of unavailable declarations.
static void diagAvailability(TypeChecker &TC, const ValueDecl *D,
                             SourceRange R, const DeclContext *DC) {
  if (!D)
    return;

  if (auto Attr = AvailabilityAttr::isUnavailable(D)) {
    auto Name = D->getFullName();
    SourceLoc Loc = R.Start;

    if (!Attr->Rename.empty()) {
      TC.diagnose(Loc, diag::availability_decl_unavailable_rename, Name,
                  Attr->Rename)
        .fixItReplace(R, Attr->Rename);
    } else if (Attr->Message.empty()) {
      TC.diagnose(Loc, diag::availability_decl_unavailable, Name)
        .highlight(R);
    } else {
      TC.diagnose(Loc, diag::availability_decl_unavailable_msg,
                  Name, Attr->Message)
        .highlight(SourceRange(Loc, Loc));
    }

    switch (Attr->getMinVersionAvailability(
              TC.Context.LangOpts.MinPlatformVersion)) {
      case MinVersionComparison::Available:
      case MinVersionComparison::PotentiallyUnavailable:
        llvm_unreachable("These aren't considered unavailable");

      case MinVersionComparison::Unavailable:
        TC.diagnose(D, diag::availability_marked_unavailable, Name)
          .highlight(Attr->getRange());
        break;

      case MinVersionComparison::Obsoleted: {
        // FIXME: Use of the platformString here is non-awesome for application
        // extensions.
        TC.diagnose(D, diag::availability_obsoleted, Name,
                    Attr->prettyPlatformString(), *Attr->Obsoleted)
          .highlight(Attr->getRange());
        break;
      }

    }
  }
}


namespace {
class AvailabilityWalker : public ASTWalker {
  TypeChecker &TC;
  const DeclContext *DC;
public:
  AvailabilityWalker(TypeChecker &TC, const DeclContext *DC)
    : TC(TC), DC(DC) {}

  virtual Expr *walkToExprPost(Expr *E) override {
    if (auto DR = dyn_cast<DeclRefExpr>(E))
      diagAvailability(TC, DR->getDecl(), DR->getSourceRange(), DC);
    if (auto MR = dyn_cast<MemberRefExpr>(E))
      diagAvailability(TC, MR->getMember().getDecl(), MR->getNameLoc(), DC);
    if (auto OCDR = dyn_cast<OtherConstructorDeclRefExpr>(E))
      diagAvailability(TC, OCDR->getDecl(), OCDR->getConstructorLoc(), DC);
    if (auto DMR = dyn_cast<DynamicMemberRefExpr>(E))
      diagAvailability(TC, DMR->getMember().getDecl(), DMR->getNameLoc(), DC);
    if (auto DS = dyn_cast<DynamicSubscriptExpr>(E))
      diagAvailability(TC, DS->getMember().getDecl(), DS->getSourceRange(), DC);
    if (auto S = dyn_cast<SubscriptExpr>(E)) {
      if (S->hasDecl())
        diagAvailability(TC, S->getDecl().getDecl(), S->getSourceRange(), DC);
    }
    return E;
  }
};
}

/// Diagnose uses of unavailable declarations.
static void diagAvailability(TypeChecker &TC, const Expr *E,
                             const DeclContext *DC) {
  AvailabilityWalker walker(TC, DC);
  const_cast<Expr*>(E)->walk(walker);
}

//===--------------------------------------------------------------------===//
// High-level entry points.
//===--------------------------------------------------------------------===//

void swift::performExprDiagnostics(TypeChecker &TC, const Expr *E,
                                   const DeclContext *DC) {
  diagSelfAssignment(TC, E);
  diagSyntacticUseRestrictions(TC, E);
  diagRecursivePropertyAccess(TC, E, DC);
  diagnoseImplicitSelfUseInClosure(TC, E);
  diagAvailability(TC, E, DC);
}

void swift::performStmtDiagnostics(TypeChecker &TC, const Stmt *S) {
  return diagUnreachableCode(TC, S);
}

//===--------------------------------------------------------------------===//
// Utility functions
//===--------------------------------------------------------------------===//

void swift::fixItAccessibility(InFlightDiagnostic &diag, ValueDecl *VD,
                               Accessibility desiredAccess, bool isForSetter) {
  StringRef fixItString;
  switch (desiredAccess) {
  case Accessibility::Private:  fixItString = "private ";  break;
  case Accessibility::Internal: fixItString = "internal "; break;
  case Accessibility::Public:   fixItString = "public ";   break;
  }

  DeclAttribute *attr;
  if (isForSetter) {
    attr = VD->getAttrs().getAttribute<SetterAccessibilityAttr>();
    cast<AbstractStorageDecl>(VD)->overwriteSetterAccessibility(desiredAccess);
  } else {
    attr = VD->getAttrs().getAttribute<AccessibilityAttr>();
    VD->overwriteAccessibility(desiredAccess);
  }

  if (isForSetter && VD->getAccessibility() == desiredAccess) {
    assert(attr);
    attr->setInvalid();
    if (!attr->Range.isValid())
      return;

    // Remove the setter attribute along with a possible single trailing space.
    SourceManager &sourceMgr = VD->getASTContext().SourceMgr;
    SourceLoc nextCharLoc = Lexer::getLocForEndOfToken(sourceMgr,
                                                       attr->Range.End);
    StringRef nextChar = sourceMgr.extractText({ nextCharLoc, 1 });
    if (nextChar == " ")
      diag.fixItRemoveChars(attr->Range.Start, nextCharLoc.getAdvancedLoc(1));
    else
      diag.fixItRemove(attr->Range);

  } else if (attr) {
    // This uses getLocation() instead of getRange() because we don't want to
    // replace the "(set)" part of a setter attribute.
    diag.fixItReplace(attr->getLocation(), fixItString.drop_back());
    attr->setInvalid();

  } else if (auto var = dyn_cast<VarDecl>(VD)) {
    if (auto PBD = var->getParentPattern())
      diag.fixItInsert(PBD->getStartLoc(), fixItString);

  } else {
    diag.fixItInsert(VD->getStartLoc(), fixItString);
  }
}
