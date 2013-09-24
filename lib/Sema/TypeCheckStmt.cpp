//===--- TypeCheckStmt.cpp - Type Checking for Statements -----------------===//
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
// This file implements semantic analysis for statements.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "TypeChecker.h"
#include "swift/Basic/Optional.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ExprHandle.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"

using namespace swift;

namespace {
/// StmtChecker - This class implements 
class StmtChecker : public StmtVisitor<StmtChecker, Stmt*> {
public:
  TypeChecker &TC;

  /// \brief This is the current function or closure being checked.
  /// This is null for top level code.
  Optional<AnyFunctionRef> TheFunc;
  
  /// DC - This is the current DeclContext.
  DeclContext *DC;

  // Scope information for control flow statements
  // (break, continue, fallthrough).
  
  /// The level of loop nesting. 'break' and 'continue' are valid only in scopes
  /// where this is greater than one.
  unsigned LoopNestLevel;
  /// The level of 'switch' nesting. 'fallthrough' is valid only in scopes where
  /// this is greater than one.
  unsigned SwitchLevel;
  /// The destination block for a 'fallthrough' statement. Null if the switch
  /// scope depth is zero or if we are checking the final 'case' of the current
  /// switch.
  CaseStmt /*nullable*/ *FallthroughDest;

  SourceLoc EndTypeCheckLoc;

  struct AddLoopNest {
    StmtChecker &SC;
    AddLoopNest(StmtChecker &SC) : SC(SC) {
      ++SC.LoopNestLevel;
    }
    ~AddLoopNest() {
      --SC.LoopNestLevel;
    }
  };
  
  struct AddSwitchNest {
    StmtChecker &SC;
    CaseStmt *OuterFallthroughDest;
    AddSwitchNest(StmtChecker &SC)
      : SC(SC),
        OuterFallthroughDest(SC.FallthroughDest)
    {
      ++SC.SwitchLevel;
    }
    
    ~AddSwitchNest() {
      --SC.SwitchLevel;
      SC.FallthroughDest = OuterFallthroughDest;
    }
  };

  StmtChecker(TypeChecker &TC, AbstractFunctionDecl *AFD)
    : TC(TC), TheFunc(AFD),
      LoopNestLevel(0), SwitchLevel(0), FallthroughDest(nullptr) {
    if (auto *CD = dyn_cast<ConstructorDecl>(AFD)) {
      DC = CD;
      return;
    }
    if (auto *DD = dyn_cast<DestructorDecl>(AFD)) {
      DC = DD;
      return;
    }
    DC = cast<FuncDecl>(AFD);
  }

  StmtChecker(TypeChecker &TC, ClosureExpr *TheClosure)
    : TC(TC), TheFunc(TheClosure), DC(TheClosure),
      LoopNestLevel(0), SwitchLevel(0), FallthroughDest(nullptr) { }

  StmtChecker(TypeChecker &TC, DeclContext *DC)
    : TC(TC), TheFunc(), DC(DC),
      LoopNestLevel(0), SwitchLevel(0), FallthroughDest(nullptr) { }

  //===--------------------------------------------------------------------===//
  // Helper Functions.
  //===--------------------------------------------------------------------===//
  
  template<typename StmtTy>
  bool typeCheckStmt(StmtTy *&S) {
    StmtTy *S2 = cast_or_null<StmtTy>(visit(S));
    if (S2 == 0) return true;
    S = S2;
    return false;
  }
  
  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//

  Stmt *visitBraceStmt(BraceStmt *BS);

  Stmt *visitReturnStmt(ReturnStmt *RS) {
    if (!TheFunc.hasValue()) {
      TC.diagnose(RS->getReturnLoc(), diag::return_invalid_outside_func);
      return 0;
    }

    Type ResultTy = TheFunc->getBodyResultType();
    if (ResultTy->is<ErrorType>())
      return 0;

    if (!RS->hasResult()) {
      if (!ResultTy->isEqual(TupleType::getEmpty(TC.Context)))
        TC.diagnose(RS->getReturnLoc(), diag::return_expr_missing);
      return RS;
    }

    Expr *E = RS->getResult();
    if (TC.typeCheckExpression(E, DC, ResultTy, /*discardedExpr=*/false))
      return 0;
    RS->setResult(E);

    return RS;
  }
  
  Stmt *visitIfStmt(IfStmt *IS) {
    Expr *E = IS->getCond();
    if (TC.typeCheckCondition(E, DC)) return 0;
    IS->setCond(E);

    Stmt *S = IS->getThenStmt();
    if (typeCheckStmt(S)) return 0;
    IS->setThenStmt(S);

    if ((S = IS->getElseStmt())) {
      if (typeCheckStmt(S)) return 0;
      IS->setElseStmt(S);
    }
    
    return IS;
  }
  
  Stmt *visitWhileStmt(WhileStmt *WS) {
    Expr *E = WS->getCond();
    if (TC.typeCheckCondition(E, DC)) return 0;
    WS->setCond(E);

    AddLoopNest loopNest(*this);
    Stmt *S = WS->getBody();
    if (typeCheckStmt(S)) return 0;
    WS->setBody(S);
    
    return WS;
  }
  Stmt *visitDoWhileStmt(DoWhileStmt *WS) {
    {
      AddLoopNest loopNest(*this);
      Stmt *S = WS->getBody();
      if (typeCheckStmt(S)) return 0;
      WS->setBody(S);
    }
    
    Expr *E = WS->getCond();
    if (TC.typeCheckCondition(E, DC)) return 0;
    WS->setCond(E);
    return WS;
  }
  Stmt *visitForStmt(ForStmt *FS) {
    // Type check any var decls in the initializer.
    for (auto D : FS->getInitializerVarDecls())
      TC.typeCheckDecl(D, /*isFirstPass*/false);

    if (auto *Initializer = FS->getInitializer().getPtrOrNull()) {
      if (TC.typeCheckExpression(Initializer, DC, Type(), /*discardedExpr=*/true))
        return 0;
      FS->setInitializer(Initializer);
    }

    if (auto *Cond = FS->getCond().getPtrOrNull()) {
      if (TC.typeCheckCondition(Cond, DC))
        return 0;
      FS->setCond(Cond);
    }

    if (auto *Increment = FS->getIncrement().getPtrOrNull()) {
      if (TC.typeCheckExpression(Increment, DC, Type(), /*discardedExpr=*/true))
        return 0;
      FS->setIncrement(Increment);
    }

    AddLoopNest loopNest(*this);
    Stmt *S = FS->getBody();
    if (typeCheckStmt(S)) return 0;
    FS->setBody(S);
    
    return FS;
  }
  
  Stmt *visitForEachStmt(ForEachStmt *S) {
    // Type-check the container and convert it to an rvalue.
    Expr *Container = S->getContainer();
    if (TC.typeCheckExpression(Container, DC, Type(), /*discardedExpr=*/false))
      return nullptr;
    S->setContainer(Container);

    // Retrieve the 'Enumerable' protocol.
    ProtocolDecl *EnumerableProto
      = TC.getProtocol(S->getForLoc(), KnownProtocolKind::Enumerable);
    if (!EnumerableProto) {
      return nullptr;
    }

    // Retrieve the 'Enumerator' protocol.
    ProtocolDecl *EnumeratorProto
      = TC.getProtocol(S->getForLoc(), KnownProtocolKind::Enumerator);
    if (!EnumeratorProto) {
      return nullptr;
    }
    
    // Verify that the container conforms to the Enumerable protocol, and
    // invoke getElements() on it container to retrieve the range of elements.
    Type RangeTy;
    VarDecl *Range;
    {
      Type ContainerType = Container->getType()->getRValueType();

      ProtocolConformance *Conformance = nullptr;
      if (!TC.conformsToProtocol(ContainerType, EnumerableProto, &Conformance,
                                 Container->getLoc()))
        return nullptr;

      for (auto Member : EnumerableProto->getMembers()) {
        auto Value = dyn_cast<ValueDecl>(Member);
        if (!Value)
          continue;
        
        StringRef Name = Value->getName().str();
        if (Name.equals("EnumeratorType") && isa<TypeDecl>(Value)) {
          if (Conformance) {
            RangeTy
              = Conformance->getTypeWitness(cast<AssociatedTypeDecl>(Value))
                  .Replacement;
          } else {
            RangeTy = cast<TypeDecl>(Value)->getDeclaredType();
          }
          RangeTy = TC.substMemberTypeWithBase(RangeTy, Value, ContainerType);
        }
      }

      if (!RangeTy) {
        TC.diagnose(EnumerableProto->getLoc(), diag::enumerable_protocol_broken);
        return nullptr;
      }

      Expr *GetElements
        = TC.callWitness(Container, DC, EnumerableProto, Conformance,
                         TC.Context.getIdentifier("getEnumeratorType"),
                         { },
                         diag::enumerable_protocol_broken);
      if (!GetElements) return nullptr;
      
      // Create a local variable to capture the range.
      // FIXME: Mark declaration as implicit?
      Range = new (TC.Context) VarDecl(S->getInLoc(),
                                       TC.Context.getIdentifier("__range"),
                                       RangeTy, DC);
      
      // Create a pattern binding to initialize the range and wire it into the
      // AST.
      Pattern *RangePat = new (TC.Context) NamedPattern(Range);
      S->setRange(new (TC.Context) PatternBindingDecl(S->getForLoc(),
                                                      RangePat, GetElements,
                                                      DC));
    }
    
    // FIXME: Would like to customize the diagnostic emitted in
    // conformsToProtocol().
    ProtocolConformance *Conformance = nullptr;
    if (!TC.conformsToProtocol(RangeTy, EnumeratorProto, &Conformance,
                               Container->getLoc()))
      return nullptr;
    
    // Gather the witnesses from the Range protocol conformance. These are
    // the functions we'll call.
    FuncDecl *nextFn = 0;
    Type ElementTy;
    
    for (auto Member : EnumeratorProto->getMembers()) {
      auto Value = dyn_cast<ValueDecl>(Member);
      if (!Value)
        continue;
      
      StringRef Name = Value->getName().str();
      if (Name.equals("Element") && isa<TypeDecl>(Value)) {
        if (Conformance) {
          ElementTy
            = Conformance->getTypeWitness(cast<AssociatedTypeDecl>(Value))
                .Replacement;
        } else {
          ElementTy = cast<TypeDecl>(Value)->getDeclaredType();
        }
        ElementTy = TC.substMemberTypeWithBase(ElementTy, Value, RangeTy);
      } else if (Name.equals("next") && isa<FuncDecl>(Value)) {
        if (Conformance) {
          // FIXME: Ignoring substitutions here (?).
          nextFn = cast<FuncDecl>(Conformance->getWitness(Value).getDecl());
        } else
          nextFn = cast<FuncDecl>(Value);
      }
    }
    
    if (!nextFn || !ElementTy) {
      TC.diagnose(EnumeratorProto->getLoc(), diag::range_protocol_broken);
      return nullptr;
    }
    
    // Compute the expression that determines whether the range is empty.
    Expr *Empty
      = TC.callWitness(TC.buildCheckedRefExpr(Range, S->getInLoc(),
                                              /*Implicit=*/true),
                       DC, EnumeratorProto, Conformance,
                       TC.Context.getIdentifier("isEmpty"),
                       { },
                       diag::range_protocol_broken);
    if (!Empty) return nullptr;
    if (TC.typeCheckCondition(Empty, DC)) return nullptr;
    S->setRangeEmpty(Empty);
    
    // Compute the expression that extracts a value from the range.
    Expr *GetFirstAndAdvance
      = TC.callWitness(TC.buildCheckedRefExpr(Range, S->getInLoc(),
                                              /*Implicit=*/true),
                       DC, EnumeratorProto, Conformance,
                       TC.Context.getIdentifier("next"),
                       { },
                       diag::range_protocol_broken);
    if (!GetFirstAndAdvance) return nullptr;
    
    S->setElementInit(new (TC.Context) PatternBindingDecl(S->getForLoc(),
                                                          S->getPattern(),
                                                          GetFirstAndAdvance,
                                                          DC));

    // Coerce the pattern to the element type, now that we know the element
    // type.
    if (TC.coerceToType(S->getPattern(), DC, ElementTy))
      return nullptr;
    
    // Type-check the body of the loop.
    AddLoopNest loopNest(*this);
    BraceStmt *Body = S->getBody();
    if (typeCheckStmt(Body)) return nullptr;
    S->setBody(Body);
    
    return S;
  }

  Stmt *visitBreakStmt(BreakStmt *S) {
    if (!LoopNestLevel) {
      TC.diagnose(S->getLoc(), diag::break_outside_loop);
      return nullptr;
    }
    return S;
  }

  Stmt *visitContinueStmt(ContinueStmt *S) {
    if (!LoopNestLevel) {
      TC.diagnose(S->getLoc(), diag::continue_outside_loop);
      return nullptr;
    }
    return S;
  }
  
  Stmt *visitFallthroughStmt(FallthroughStmt *S) {
    if (!SwitchLevel) {
      TC.diagnose(S->getLoc(), diag::fallthrough_outside_switch);
      return nullptr;
    }
    if (!FallthroughDest) {
      TC.diagnose(S->getLoc(), diag::fallthrough_from_last_case);
      return nullptr;
    }
    if (FallthroughDest->hasBoundDecls())
      TC.diagnose(S->getLoc(), diag::fallthrough_into_case_with_var_binding);
    S->setFallthroughDest(FallthroughDest);
    return S;
  }
  
  Stmt *visitSwitchStmt(SwitchStmt *S) {
    // Type-check the subject expression.
    Expr *subjectExpr = S->getSubjectExpr();
    if (TC.typeCheckExpression(subjectExpr, DC, Type(),
                               /*discardedExpr=*/false))
      return nullptr;
    subjectExpr = TC.coerceToMaterializable(subjectExpr);
    if (!subjectExpr)
      return nullptr;
    S->setSubjectExpr(subjectExpr);
    Type subjectType = subjectExpr->getType();

    // Type-check the case blocks.
    AddSwitchNest switchNest(*this);
    bool hadTypeError = false;
    for (unsigned i = 0, e = S->getCases().size(); i < e; ++i) {
      auto *caseBlock = S->getCases()[i];
      // Fallthrough transfers control to the next case block. In the
      // final case block, it is invalid.
      FallthroughDest = i+1 == e ? nullptr : S->getCases()[i+1];

      for (auto *caseLabel : caseBlock->getCaseLabels()) {
        // Resolve the patterns in the label.
        for (auto *&pattern : caseLabel->getPatterns()) {
          if (auto *newPattern = TC.resolvePattern(pattern, DC)) {
            pattern = newPattern;
          } else {
            hadTypeError = true;
            continue;
          }

          // Coerce the pattern to the subject's type.
          hadTypeError |= TC.coerceToType(pattern, DC, subjectType);
        }
        
        // Check the guard expression, if present.
        if (auto *guard = caseLabel->getGuardExpr()) {
          if (TC.typeCheckCondition(guard, DC))
            hadTypeError = true;
          else
            caseLabel->setGuardExpr(guard);
        }
      }
      
      // Type-check the body statements.
      Stmt *body = caseBlock->getBody();
      if (typeCheckStmt(body))
        hadTypeError = true;
      caseBlock->setBody(body);
    }
    
    return hadTypeError ? nullptr : S;
  }

  Stmt *visitCaseStmt(CaseStmt *S) {
    // Cases are handled in visitSwitchStmt.
    llvm_unreachable("case stmt outside of switch?!");
  }
};
  
} // end anonymous namespace
  
Stmt *StmtChecker::visitBraceStmt(BraceStmt *BS) {
  const SourceManager &SM = TC.Context.SourceMgr;
  for (auto &elem : BS->getElements()) {
    if (Expr *SubExpr = elem.dyn_cast<Expr*>()) {
      SourceLoc Loc = SubExpr->getStartLoc();
      if (EndTypeCheckLoc.isValid() &&
          (Loc == EndTypeCheckLoc || SM.isBeforeInBuffer(EndTypeCheckLoc, Loc)))
        break;

      // Type check the expression.
      bool isDiscarded
        = TC.TU.Kind != TranslationUnit::REPL || !isa<TopLevelCodeDecl>(DC);
      if (TC.typeCheckExpression(SubExpr, DC, Type(), isDiscarded))
        continue;
      
      if (isDiscarded)
        TC.typeCheckIgnoredExpr(SubExpr);
      elem = SubExpr;
      continue;
    }

    if (Stmt *SubStmt = elem.dyn_cast<Stmt*>()) {
      SourceLoc Loc = SubStmt->getStartLoc();
      if (EndTypeCheckLoc.isValid() &&
          (Loc == EndTypeCheckLoc || SM.isBeforeInBuffer(EndTypeCheckLoc, Loc)))
        break;

      if (!typeCheckStmt(SubStmt))
        elem = SubStmt;
      continue;
    }

    Decl *SubDecl = elem.get<Decl *>();
    SourceLoc Loc = SubDecl->getStartLoc();
    if (EndTypeCheckLoc.isValid() &&
        (Loc == EndTypeCheckLoc || SM.isBeforeInBuffer(EndTypeCheckLoc, Loc)))
      break;

    TC.typeCheckDecl(SubDecl, /*isFirstPass*/false);
  }
  
  return BS;
}

/// Check an expression whose result is not being used at all.
void TypeChecker::typeCheckIgnoredExpr(Expr *E) {
  // Complain about l-values that are neither loaded nor stored.
  if (E->getType()->is<LValueType>()) {
    diagnose(E->getLoc(), diag::expression_unused_lvalue)
      .highlight(E->getSourceRange());
    return;
  }

  // Complain about functions that aren't called.
  // TODO: What about tuples which contain functions by-value that are
  // dead?
  if (E->getType()->is<AnyFunctionType>()) {
    diagnose(E->getLoc(), diag::expression_unused_function)
      .highlight(E->getSourceRange());
    return;
  }

  // FIXME: Complain about literals
}

/// Check the default arguments that occur within this pattern.
static void checkDefaultArguments(TypeChecker &tc, Pattern *pattern,
                                  DeclContext *dc) {
  switch (pattern->getKind()) {
  case PatternKind::Tuple:
    for (auto &field : cast<TuplePattern>(pattern)->getFields()) {
      if (field.getInit()) {
        assert(!field.getInit()->alreadyChecked() &&
               "Expression already checked");
        Expr *e = field.getInit()->getExpr();
        if (tc.typeCheckExpression(e, dc, field.getPattern()->getType(),
                                   /*discardedExpr=*/false))
          field.getInit()->setExpr(field.getInit()->getExpr(), true);
        else
          field.getInit()->setExpr(e, true);
      }
    }
    return;
  case PatternKind::Paren:
    return checkDefaultArguments(tc,
                                 cast<ParenPattern>(pattern)->getSubPattern(),
                                 dc);
  case PatternKind::Typed:
  case PatternKind::Named:
  case PatternKind::Any:
    return;

#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) case PatternKind::Id:
#include "swift/AST/PatternNodes.def"
    llvm_unreachable("pattern can't appear in argument list!");
  }
  llvm_unreachable("bad pattern kind!");
}

bool TypeChecker::typeCheckAbstractFunctionBodyUntil(AbstractFunctionDecl *AFD,
                                                     SourceLoc EndTypeCheckLoc) {
  if (auto *FD = dyn_cast<FuncDecl>(AFD))
    return typeCheckFunctionBodyUntil(FD, EndTypeCheckLoc);

  if (auto *CD = dyn_cast<ConstructorDecl>(AFD))
    return typeCheckConstructorBodyUntil(CD, EndTypeCheckLoc);

  auto *DD = cast<DestructorDecl>(AFD);
  return typeCheckDestructorBodyUntil(DD, EndTypeCheckLoc);
}

bool TypeChecker::typeCheckAbstractFunctionBody(AbstractFunctionDecl *AFD) {
  return typeCheckAbstractFunctionBodyUntil(AFD, SourceLoc());
}

// Type check a function body (defined with the func keyword) that is either a
// named function or an anonymous func expression.
bool TypeChecker::typeCheckFunctionBodyUntil(FuncDecl *FD,
                                             SourceLoc EndTypeCheckLoc) {
  if (FD->isInvalid())
    return true;

  // Check the default argument definitions.
  for (auto pattern : FD->getBodyParamPatterns()) {
    checkDefaultArguments(*this, pattern, FD->getParent());
  }

  BraceStmt *BS = FD->getBody();
  assert(BS && "Should have a body");

  StmtChecker SC(*this, static_cast<AbstractFunctionDecl *>(FD));
  SC.EndTypeCheckLoc = EndTypeCheckLoc;
  bool HadError = SC.typeCheckStmt(BS);

  FD->setBody(BS);
  return HadError;
}

/// \brief Given a pattern declaring some number of member variables, build an
/// expression that references the variables relative to 'self' with the same
/// structure as the pattern.
///
/// \param tc The type checker.
/// \param selfDecl The 'self' declaration.
/// \param pattern The pattern.
static Expr *createPatternMemberRefExpr(TypeChecker &tc, VarDecl *selfDecl,
                                        Pattern *pattern) {
  switch (pattern->getKind()) {
  case PatternKind::Any:
    // FIXME: Unfortunate case. We have no way to represent 'forget this value'
    // in the AST.
    return nullptr;

  case PatternKind::Named:
    return new (tc.Context) UnresolvedDotExpr(
             tc.buildRefExpr(selfDecl, SourceLoc(), /*Implicit=*/true),
             SourceLoc(), 
             cast<NamedPattern>(pattern)->getDecl()->getName(), 
             SourceLoc(), /*Implicit=*/true);

  case PatternKind::Paren:
    return createPatternMemberRefExpr(
             tc, selfDecl,
             cast<ParenPattern>(pattern)->getSubPattern());

  case PatternKind::Tuple: {
    auto tuple = cast<TuplePattern>(pattern);
    SmallVector<Expr *, 4> elements;
    for (auto elt : tuple->getFields()) {
      auto sub = createPatternMemberRefExpr(tc, selfDecl, elt.getPattern());
      if (!sub)
        return nullptr;

      elements.push_back(sub);
    }

    if (elements.size() == 1)
      return elements[0];
    return new (tc.Context) TupleExpr(SourceLoc(),
                                      tc.Context.AllocateCopy(elements),
                                      nullptr,
                                      SourceLoc(),
                                      /*hasTrailingClosure=*/false,
                                      /*Implicit=*/true);
  }

  case PatternKind::Typed:
    return createPatternMemberRefExpr(
             tc,
             selfDecl,
             cast<TypedPattern>(pattern)->getSubPattern());
      
#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) case PatternKind::Id:
#include "swift/AST/PatternNodes.def"
    llvm_unreachable("pattern can't appear in constructor decl!");
  }
}

bool TypeChecker::typeCheckConstructorBodyUntil(ConstructorDecl *ctor,
                                                SourceLoc EndTypeCheckLoc) {
  if (auto allocSelf = ctor->getAllocSelfExpr()) {
    if (!typeCheckExpression(allocSelf, ctor, Type(), /*discardedExpr=*/false))
      ctor->setAllocSelfExpr(allocSelf);
  }

  // Check the default argument definitions.
  checkDefaultArguments(*this, ctor->getArgParams(), ctor->getDeclContext());

  BraceStmt *body = ctor->getBody();
  if (!body)
    return true;

  // Type-check the body.
  StmtChecker SC(*this, static_cast<AbstractFunctionDecl *>(ctor));
  SC.EndTypeCheckLoc = EndTypeCheckLoc;
  bool HadError = SC.typeCheckStmt(body);

  // Figure out which members already have initializers. We don't
  // default-initialize those members.
  // FIXME: This traversal is quite simplistic and quite stupid. It should
  // use dataflow analysis to determine which members are guaranteed to
  // be (manually) initialized before they are used.
  bool allOfThisInitialized = false;
  auto nominalDecl = ctor->getDeclContext()->getDeclaredTypeInContext()
                       ->getNominalOrBoundGenericNominal();
  llvm::SmallPtrSet<VarDecl *, 4> initializedMembers;
  for (auto elt : body->getElements()) {
    auto expr = elt.dyn_cast<Expr *>();
    if (!expr)
      continue;

    auto assign = dyn_cast<AssignExpr>(expr);
    if (!assign)
      continue;

    // We have an assignment. Check whether the left-hand side refers to
    // a member of our class.
    // FIXME: Also look into TupleExpr destinations.
    VarDecl *member = nullptr;
    auto dest = assign->getDest()->getSemanticsProvidingExpr();
    if (auto memberRef = dyn_cast<MemberRefExpr>(dest))
      member = dyn_cast<VarDecl>(memberRef->getMember().getDecl());
    else if (auto memberRef = dyn_cast<ExistentialMemberRefExpr>(dest))
      member = dyn_cast<VarDecl>(memberRef->getDecl());
    else if (auto memberRef = dyn_cast<ArchetypeMemberRefExpr>(dest))
      member = dyn_cast<VarDecl>(memberRef->getDecl());
    else if (auto memberRef = dyn_cast<UnresolvedDotExpr>(dest)) {
      if (auto base = dyn_cast<DeclRefExpr>(
                        memberRef->getBase()->getSemanticsProvidingExpr())) {
        if (base->getDecl()->getName().str().equals("self")) {
          // Look for the member within this type.
          auto memberDecls
            = lookupMember(nominalDecl->getDeclaredTypeInContext(),
                           memberRef->getName(),
                           /*allowDynamicLookup=*/false);
          if (memberDecls.size() == 1)
            member = dyn_cast<VarDecl>(memberDecls[0]);
        }
      }
    } else if (auto declRef = dyn_cast<DeclRefExpr>(dest)) {
      // If the left-hand side is 'self', we're initializing the
      // whole object.
      if (declRef->getDecl()->getName().str().equals("self")) {
        allOfThisInitialized = true;
        break;
      }
    }

    if (member)
      initializedMembers.insert(member);
  }

  SmallVector<BraceStmt::ExprStmtOrDecl, 4> defaultInits;

  // If this is the implicit default constructor for a class with a superclass,
  // call the superclass constructor.
  if (ctor->isImplicit() && isa<ClassDecl>(ctor->getDeclContext()) &&
      cast<ClassDecl>(ctor->getDeclContext())->getSuperclass()) {
    Expr *superRef = new (Context) SuperRefExpr(ctor->getImplicitSelfDecl(),
                                                SourceLoc(), /*Implicit=*/true);
    Expr *result = new (Context) UnresolvedConstructorExpr(superRef,
                                                           SourceLoc(),
                                                           SourceLoc(),
                                                           /*Implicit=*/true);
    Expr *args = new (Context) TupleExpr(SourceLoc(), { }, nullptr, SourceLoc(),
                                         /*hasTrailingClosure=*/false,
                                         /*Implicit=*/true);
    result = new (Context) CallExpr(result, args, /*Implicit=*/true);
    if (!typeCheckExpression(result, ctor, Type(), /*discardedExpr=*/true))
      defaultInits.push_back(result);
  }

  // Default-initialize all of the members.
  if (!allOfThisInitialized) {
    for (auto member : nominalDecl->getMembers()) {
      // We only care about pattern bindings.
      auto patternBind = dyn_cast<PatternBindingDecl>(member);
      if (!patternBind)
        continue;

      // If the pattern has an initializer, use it.
      // FIXME: Implement this.
      if (auto initializer = patternBind->getInit()) {
        // Create a tuple expression with the same structure as the
        // pattern.
        if (Expr *dest = createPatternMemberRefExpr(
                           *this,
                           ctor->getImplicitSelfDecl(),
                           patternBind->getPattern())) {
          initializer = new (Context) DefaultValueExpr(initializer);
          Expr *assign = new (Context) AssignExpr(dest, SourceLoc(),
                                                  initializer,
                                                  /*Implicit=*/true);
          typeCheckExpression(assign, ctor, Type(), /*discardedExpr=*/false);
          defaultInits.push_back(assign);
          continue;
        }

        diagnose(body->getLBraceLoc(), diag::decl_no_default_init_ivar_hole);
        diagnose(patternBind->getLoc(), diag::decl_init_here);
      }

      // Find the variables in the pattern. They'll each need to be
      // default-initialized.
      SmallVector<VarDecl *, 4> variables;
      patternBind->getPattern()->collectVariables(variables);

      // Initialize the variables.
      for (auto var : variables) {
        if (var->isProperty())
          continue;

        // If we already saw an initializer for this member, don't
        // initialize it.
        if (!initializedMembers.insert(var))
          continue;

        // If this variable is not default-initializable, we're done: we can't
        // add the default constructor because it will be ill-formed.
        auto varType = getTypeOfRValue(var);

        // Don't complain about variables with ErrorType; an error was
        // already emitted alsewhere.
        if (varType->is<ErrorType>())
          continue;

        Expr *initializer = nullptr;
        if (!isDefaultInitializable(varType, &initializer)) {
          diagnose(body->getLBraceLoc(), diag::decl_no_default_init_ivar,
                   var->getName(), varType);
          diagnose(var->getLoc(), diag::decl_declared_here, var->getName());
          continue;
        }

        // Create the assignment.
        auto selfDecl = ctor->getImplicitSelfDecl();
        Expr *dest
          = new (Context) UnresolvedDotExpr(
              new (Context) DeclRefExpr(selfDecl, SourceLoc(),
                                        /*Implicit=*/true),
              SourceLoc(), 
              var->getName(),
              SourceLoc(),
              /*Implicit=*/true);
        Expr *assign = new (Context) AssignExpr(dest, SourceLoc(),
                                                initializer, /*Implicit=*/true);
        typeCheckExpression(assign, ctor, Type(), /*discardedExpr=*/false);
        defaultInits.push_back(assign);
      }
    }
  }
  
  // If we added any default initializers, update the body.
  if (!defaultInits.empty()) {
    defaultInits.append(body->getElements().begin(),
                        body->getElements().end());

    body = BraceStmt::create(Context, body->getLBraceLoc(), defaultInits,
                             body->getRBraceLoc());
  }

  ctor->setBody(body);
  return HadError;
}

bool TypeChecker::typeCheckDestructorBodyUntil(DestructorDecl *DD,
                                               SourceLoc EndTypeCheckLoc) {
  StmtChecker SC(*this, static_cast<AbstractFunctionDecl *>(DD));
  SC.EndTypeCheckLoc = EndTypeCheckLoc;
  BraceStmt *Body = DD->getBody();
  if (!Body)
    return false;

  bool HadError = SC.typeCheckStmt(Body);

  DD->setBody(Body);
  return HadError;
}

void TypeChecker::typeCheckClosureBody(ClosureExpr *closure) {
  BraceStmt *body = closure->getBody();
  StmtChecker(*this, closure).typeCheckStmt(body);
  if (body) {
    closure->setBody(body, closure->hasSingleExpressionBody());
  }
}

void TypeChecker::typeCheckTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
  BraceStmt *Body = TLCD->getBody();
  StmtChecker(*this, TLCD).typeCheckStmt(Body);
  TLCD->setBody(Body);
}
