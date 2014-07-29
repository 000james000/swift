//===--- LookupVisibleDecls - Swift Name Lookup Routines ------------------===//
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
// This file implements the lookupVisibleDecls interface for visiting named
// declarations.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/NameLookup.h"
#include "swift/AST/AST.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include <set>

using namespace swift;

void VisibleDeclConsumer::anchor() {}
void VectorDeclConsumer::anchor() {}

namespace {
struct LookupState {
private:
  /// If \c false, an unqualified lookup of all visible decls in a
  /// DeclContext.
  ///
  /// If \c true, lookup of all visible members of a given object (possibly of
  /// metatype type).
  unsigned IsQualified : 1;

  /// Is this a qualified lookup on a metatype?
  unsigned IsOnMetatype : 1;

  /// Did we recurse into a superclass?
  unsigned IsOnSuperclass : 1;

  unsigned InheritsSuperclassInitializers : 1;

  LookupState()
      : IsQualified(0), IsOnMetatype(0), IsOnSuperclass(0),
        InheritsSuperclassInitializers(0) {}

public:
  LookupState(const LookupState &) = default;

  static LookupState makeQualified() {
    LookupState Result;
    Result.IsQualified = 1;
    return Result;
  }

  static LookupState makeUnqalified() {
    LookupState Result;
    Result.IsQualified = 0;
    return Result;
  }

  bool isQualified() const { return IsQualified; }
  bool isOnMetatype() const { return IsOnMetatype; }
  bool isOnSuperclass() const { return IsOnSuperclass; }
  bool isInheritsSuperclassInitializers() const {
    return InheritsSuperclassInitializers;
  }

  LookupState withOnMetatype() const {
    auto Result = *this;
    Result.IsOnMetatype = 1;
    return Result;
  }

  LookupState withOnSuperclass() const {
    auto Result = *this;
    Result.IsOnSuperclass = 1;
    return Result;
  }

  LookupState withInheritsSuperclassInitializers() const {
    auto Result = *this;
    Result.InheritsSuperclassInitializers = 1;
    return Result;
  }

  LookupState withoutInheritsSuperclassInitializers() const {
    auto Result = *this;
    Result.InheritsSuperclassInitializers = 0;
    return Result;
  }
};
} // unnamed namespace

static bool areTypeDeclsVisibleInLookupMode(LookupState LS) {
  // Nested type declarations can be accessed only with unqualified lookup or
  // on metatypes.
  return !LS.isQualified() || LS.isOnMetatype();
}

static bool isDeclVisibleInLookupMode(ValueDecl *Member, LookupState LS,
                                      const DeclContext *FromContext,
                                      LazyResolver *TypeResolver) {
  if (TypeResolver)
    TypeResolver->resolveDeclSignature(Member);

  // Check accessibility when relevant.
  if (!Member->getDeclContext()->isLocalContext() &&
      !isa<GenericTypeParamDecl>(Member) && !isa<ParamDecl>(Member) &&
      FromContext->getASTContext().LangOpts.EnableAccessControl) {
    if (Member->isInvalid() && !Member->hasAccessibility())
      return false;
    if (!Member->isAccessibleFrom(FromContext))
      return false;
  }

  if (auto *FD = dyn_cast<FuncDecl>(Member)) {
    // Can not call static functions on non-metatypes.
    if (!LS.isOnMetatype() && FD->isStatic())
      return false;

    // Otherwise, either call a function or curry it.
    return true;
  }
  if (auto *VD = dyn_cast<VarDecl>(Member)) {
    // Can not use static properties on non-metatypes.
    if (!(LS.isQualified() && LS.isOnMetatype()) && VD->isStatic())
      return false;

    // Can not use instance properties on metatypes.
    if (LS.isOnMetatype() && !VD->isStatic())
      return false;

    return true;
  }
  if (isa<EnumElementDecl>(Member)) {
    // Can not reference enum elements on non-metatypes.
    if (!(LS.isQualified() && LS.isOnMetatype()))
      return false;
  }
  if (auto CD = dyn_cast<ConstructorDecl>(Member)) {
    // Constructors with stub implementations can not be called in Swift.
    if (CD->hasStubImplementation())
      return false;
    if (LS.isQualified() && LS.isOnSuperclass()) {
      // Can not call initializers from a superclass, except for inherited
      // convenience initializers.
      return LS.isInheritsSuperclassInitializers() && CD->isInheritable();
    }
  }
  if (isa<TypeDecl>(Member))
    return areTypeDeclsVisibleInLookupMode(LS);

  return true;
}

static void doGlobalExtensionLookup(Type BaseType,
                                    SmallVectorImpl<ValueDecl *> &FoundDecls,
                                    const DeclContext *CurrDC,
                                    LookupState LS,
                                    DeclVisibilityKind Reason,
                                    LazyResolver *TypeResolver) {
  auto nominal = BaseType->getAnyNominal();

  // Look in each extension of this type.
  for (auto extension : nominal->getExtensions()) {
    bool validatedExtension = false;
    for (auto Member : extension->getMembers()) {
      if (auto VD = dyn_cast<ValueDecl>(Member))
        if (isDeclVisibleInLookupMode(VD, LS, CurrDC, TypeResolver)) {
          // Resolve the extension, if we haven't done so already.
          if (!validatedExtension && TypeResolver) {
            TypeResolver->resolveExtension(extension);
            validatedExtension = true;
          }

          FoundDecls.push_back(VD);
        }
    }
  }

  // Handle shadowing.
  removeShadowedDecls(FoundDecls, CurrDC->getParentModule(), TypeResolver);
}

/// \brief Enumerate immediate members of the type \c BaseType and its
/// extensions, as seen from the context \c CurrDC.
///
/// Don't do lookup into superclasses or implemented protocols.
static void lookupTypeMembers(Type BaseType, VisibleDeclConsumer &Consumer,
                              const DeclContext *CurrDC,
                              LookupState LS, DeclVisibilityKind Reason,
                              LazyResolver *TypeResolver) {
  NominalTypeDecl *D = BaseType->getAnyNominal();
  assert(D && "should have a nominal type");

  bool LookupFromChildDeclContext = false;
  const DeclContext *TempDC = CurrDC;
  while (!TempDC->isModuleContext()) {
    if (TempDC == D) {
      LookupFromChildDeclContext = true;
      break;
    }
    TempDC = TempDC->getParent();
  }

  SmallVector<ValueDecl*, 2> FoundDecls;

  if (LookupFromChildDeclContext) {
    // Current decl context is contained inside 'D', so generic parameters
    // are visible.
    if (D->getGenericParams())
      for (auto Param : *D->getGenericParams())
        if (isDeclVisibleInLookupMode(Param, LS, CurrDC, TypeResolver))
          FoundDecls.push_back(Param);
  }

  for (Decl *Member : D->getMembers()) {
    if (auto *VD = dyn_cast<ValueDecl>(Member))
      if (isDeclVisibleInLookupMode(VD, LS, CurrDC, TypeResolver))
        FoundDecls.push_back(VD);
  }
  doGlobalExtensionLookup(BaseType, FoundDecls, CurrDC, LS, Reason,
                          TypeResolver);

  // Report the declarations we found to the consumer.
  for (auto *VD : FoundDecls)
    Consumer.foundDecl(VD, Reason);
}

/// Enumerate AnyObject declarations as seen from context \c CurrDC.
static void doDynamicLookup(VisibleDeclConsumer &Consumer,
                            const DeclContext *CurrDC,
                            LookupState LS,
                            LazyResolver *TypeResolver) {
  class DynamicLookupConsumer : public VisibleDeclConsumer {
    VisibleDeclConsumer &ChainedConsumer;
    LookupState LS;
    const DeclContext *CurrDC;
    LazyResolver *TypeResolver;
    llvm::DenseSet<std::pair<Identifier, CanType>> FunctionsReported;
    llvm::DenseSet<CanType> SubscriptsReported;
    llvm::DenseSet<std::pair<Identifier, CanType>> PropertiesReported;

  public:
    explicit DynamicLookupConsumer(VisibleDeclConsumer &ChainedConsumer,
                                   LookupState LS, const DeclContext *CurrDC,
                                   LazyResolver *TypeResolver)
        : ChainedConsumer(ChainedConsumer), LS(LS), CurrDC(CurrDC),
          TypeResolver(TypeResolver) {}

    void foundDecl(ValueDecl *D, DeclVisibilityKind Reason) override {
      // If the declaration has an override, name lookup will also have found
      // the overridden method.  Skip this declaration, because we prefer the
      // overridden method.
      if (D->getOverriddenDecl())
        return;

      // Initializers can not be found by dynamic lookup.
      if (isa<ConstructorDecl>(D))
        return;

      // Check if we already reported a decl with the same signature.
      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        assert(FD->getImplicitSelfDecl() && "should not find free functions");
        (void)FD;

        // Get the type without the first uncurry level with 'self'.
        CanType T = D->getType()
                        ->castTo<AnyFunctionType>()
                        ->getResult()
                        ->getCanonicalType();

        auto Signature = std::make_pair(D->getName(), T);
        if (!FunctionsReported.insert(Signature).second)
          return;
      } else if (isa<SubscriptDecl>(D)) {
        auto Signature = D->getType()->getCanonicalType();
        if (!SubscriptsReported.insert(Signature).second)
          return;
      } else if (isa<VarDecl>(D)) {
        auto Signature =
            std::make_pair(D->getName(), D->getType()->getCanonicalType());
        if (!PropertiesReported.insert(Signature).second)
          return;
      } else {
        llvm_unreachable("unhandled decl kind");
      }

      if (isDeclVisibleInLookupMode(D, LS, CurrDC, TypeResolver))
        ChainedConsumer.foundDecl(D, DeclVisibilityKind::DynamicLookup);
    }
  };

  DynamicLookupConsumer ConsumerWrapper(Consumer, LS, CurrDC, TypeResolver);

  CurrDC->getParentSourceFile()->forAllVisibleModules(
      [&](Module::ImportedModule Import) {
        Import.second->lookupClassMembers(Import.first, ConsumerWrapper);
      });
}

namespace {
  typedef llvm::SmallPtrSet<TypeDecl *, 8> VisitedSet;
}

static DeclVisibilityKind getReasonForSuper(DeclVisibilityKind Reason) {
  switch (Reason) {
  case DeclVisibilityKind::MemberOfCurrentNominal:
  case DeclVisibilityKind::MemberOfProtocolImplementedByCurrentNominal:
  case DeclVisibilityKind::MemberOfSuper:
    return DeclVisibilityKind::MemberOfSuper;

  case DeclVisibilityKind::MemberOfOutsideNominal:
    return DeclVisibilityKind::MemberOfOutsideNominal;

  default:
    llvm_unreachable("should not see this kind");
  }
}

static void lookupDeclsFromProtocolsBeingConformedTo(
    Type BaseTy, VisibleDeclConsumer &Consumer, LookupState LS,
    const DeclContext *FromContext, DeclVisibilityKind Reason,
    LazyResolver *TypeResolver, VisitedSet &Visited) {
  NominalTypeDecl *CurrNominal = BaseTy->getAnyNominal();
  if (!CurrNominal)
    return;

  llvm::SmallPtrSet<ProtocolDecl *, 8> ProtocolsWithConformances;
  {
    SmallVector<ProtocolDecl *, 8> Worklist;
    for (const auto *Conformance : CurrNominal->getConformances()) {
      if (!Conformance->isComplete())
        continue;
      Worklist.push_back(Conformance->getProtocol());
    }

    while (!Worklist.empty()) {
      auto Proto = Worklist.pop_back_val();
      if(!ProtocolsWithConformances.insert(Proto))
        continue;

      auto Protocols = Proto->getProtocols();
      Worklist.append(Protocols.begin(), Protocols.end());
    }
  }

  auto TopProtocols = CurrNominal->getProtocols();
  SmallVector<ProtocolDecl *, 8> Worklist(TopProtocols.begin(),
                                          TopProtocols.end());
  while (!Worklist.empty()) {
    auto Proto = Worklist.pop_back_val();
    if (!Visited.insert(Proto))
      continue;
    if (TypeResolver)
      TypeResolver->resolveDeclSignature(Proto);
    if (!Proto->isAccessibleFrom(FromContext))
      continue;

    bool ShouldFindNonOptionalValueRequirements =
        !ProtocolsWithConformances.count(Proto);
    DeclVisibilityKind ReasonForThisProtocol;
    if (Reason == DeclVisibilityKind::MemberOfCurrentNominal)
      ReasonForThisProtocol =
          DeclVisibilityKind::MemberOfProtocolImplementedByCurrentNominal;
    else
      ReasonForThisProtocol = getReasonForSuper(Reason);

    for (auto Member : Proto->getMembers()) {
      if (auto *ATD = dyn_cast<AssociatedTypeDecl>(Member)) {
        if (areTypeDeclsVisibleInLookupMode(LS)) {
          Consumer.foundDecl(ATD, ReasonForThisProtocol);
        }
        continue;
      }
      if (auto *VD = dyn_cast<ValueDecl>(Member)) {
        // Skip non-optional value requirements from protocols that the type
        // correctly conforms to.  This is done so that we don't return
        // duplicate members.
        if (ShouldFindNonOptionalValueRequirements ||
            VD->getAttrs().hasAttribute<OptionalAttr>()) {
          Consumer.foundDecl(VD, ReasonForThisProtocol);
        }
      }
    }
    auto Protocols = Proto->getProtocols();
    Worklist.append(Protocols.begin(), Protocols.end());
  }
}

static void lookupVisibleMemberDeclsImpl(
    Type BaseTy, VisibleDeclConsumer &Consumer, const DeclContext *CurrDC,
    LookupState LS, DeclVisibilityKind Reason, LazyResolver *TypeResolver,
    VisitedSet &Visited) {
  // Just look through l-valueness.  It doesn't affect name lookup.
  BaseTy = BaseTy->getRValueType();

  // Handle metatype references, as in "some_type.some_member".  These are
  // special and can't have extensions.
  if (auto MTT = BaseTy->getAs<AnyMetatypeType>()) {
    // The metatype represents an arbitrary named type: dig through to the
    // declared type to see what we're dealing with.
    Type Ty = MTT->getInstanceType();

    // Just perform normal dot lookup on the type see if we find extensions or
    // anything else.  For example, type SomeTy.SomeMember can look up static
    // functions, and can even look up non-static functions as well (thus
    // getting the address of the member).
    lookupVisibleMemberDeclsImpl(Ty, Consumer, CurrDC,
                                 LookupState::makeQualified().withOnMetatype(),
                                 Reason, TypeResolver, Visited);
    return;
  }

  // Lookup module references, as on some_module.some_member.  These are
  // special and can't have extensions.
  if (ModuleType *MT = BaseTy->getAs<ModuleType>()) {
    AccessFilteringDeclConsumer FilteringConsumer(CurrDC, Consumer,
                                                  TypeResolver);
    MT->getModule()->lookupVisibleDecls(Module::AccessPathTy(),
                                        FilteringConsumer,
                                        NLKind::QualifiedLookup);
    return;
  }

  // If the base is a protocol, enumerate its members.
  if (ProtocolType *PT = BaseTy->getAs<ProtocolType>()) {
    if (PT->getDecl()->isSpecificProtocol(KnownProtocolKind::AnyObject)) {
      // Handle AnyObject in a special way.
      doDynamicLookup(Consumer, CurrDC, LS, TypeResolver);
      return;
    }
    if (!Visited.insert(PT->getDecl()))
      return;

    for (auto Proto : PT->getDecl()->getProtocols())
      lookupVisibleMemberDeclsImpl(Proto->getDeclaredType(), Consumer, CurrDC,
                                   LS, getReasonForSuper(Reason), TypeResolver,
                                   Visited);

    lookupTypeMembers(PT, Consumer, CurrDC, LS, Reason, TypeResolver);
    return;
  }

  // If the base is a protocol composition, enumerate members of the protocols.
  if (auto PC = BaseTy->getAs<ProtocolCompositionType>()) {
    for (auto Proto : PC->getProtocols())
      lookupVisibleMemberDeclsImpl(Proto, Consumer, CurrDC, LS, Reason,
                                   TypeResolver, Visited);
    return;
  }

  // Enumerate members of archetype's requirements.
  if (ArchetypeType *Archetype = BaseTy->getAs<ArchetypeType>()) {
    for (auto Proto : Archetype->getConformsTo())
      lookupVisibleMemberDeclsImpl(Proto->getDeclaredType(), Consumer, CurrDC,
                                   LS, getReasonForSuper(Reason),
                                   TypeResolver, Visited);

    if (auto superclass = Archetype->getSuperclass())
      lookupVisibleMemberDeclsImpl(superclass, Consumer, CurrDC, LS,
                                   getReasonForSuper(Reason), TypeResolver,
                                   Visited);
    return;
  }

  do {
    NominalTypeDecl *CurNominal = BaseTy->getAnyNominal();
    if (!CurNominal)
      break;

    // Look in for members of a nominal type.
    lookupTypeMembers(BaseTy, Consumer, CurrDC, LS, Reason, TypeResolver);
    lookupDeclsFromProtocolsBeingConformedTo(BaseTy, Consumer, LS, CurrDC,
                                             Reason, TypeResolver, Visited);
    // If we have a class type, look into its superclass.
    ClassDecl *CurClass = dyn_cast<ClassDecl>(CurNominal);

    if (CurClass && CurClass->hasSuperclass()) {
      BaseTy = CurClass->getSuperclass();
      Reason = getReasonForSuper(Reason);

      bool InheritsSuperclassInitializers =
          CurClass->inheritsSuperclassInitializers(TypeResolver);
      if (LS.isOnSuperclass() && !InheritsSuperclassInitializers)
        LS = LS.withoutInheritsSuperclassInitializers();
      else if (!LS.isOnSuperclass()) {
        LS = LS.withOnSuperclass();
        if (InheritsSuperclassInitializers)
          LS = LS.withInheritsSuperclassInitializers();
      }
    } else {
      break;
    }
  } while (1);
}

namespace {
struct FoundDeclTy {
  ValueDecl *D;
  DeclVisibilityKind Reason;

  FoundDeclTy(ValueDecl *D, DeclVisibilityKind Reason)
      : D(D), Reason(Reason) {}

  friend bool operator==(const FoundDeclTy &LHS, const FoundDeclTy &RHS) {
    return LHS.D == RHS.D;
  }

  friend bool operator<(const FoundDeclTy &LHS, const FoundDeclTy &RHS) {
    return LHS.D < RHS.D;
  }
};

class OverrideFilteringConsumer : public VisibleDeclConsumer {
public:
  std::set<ValueDecl *> FoundDecls;
  llvm::DenseSet<Identifier> FoundAssociatedTypes;
  llvm::SetVector<FoundDeclTy> DeclsToReport;

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason) override {
    if (FoundDecls.count(VD))
      return;

    if (auto ATD = dyn_cast<AssociatedTypeDecl>(VD)) {
      // AssociatedTypeDecls don't track overriding. They can come from
      // multiple implemented protocols (where there is no overriding
      // relationship), so we need to track them separately.
      //
      // In any case, don't report multiple AssociatedTypeDecls with same name.
      auto Res = FoundAssociatedTypes.insert(ATD->getName());
      if (Res.second)
        DeclsToReport.insert(FoundDeclTy(ATD, Reason));
      return;
    }

    // Insert all overridden decls into FoundDecls.
    ValueDecl *OverriddenDecl = nullptr;
    while (ValueDecl *OverriddenDecl = VD->getOverriddenDecl()) {
      if (FoundDecls.count(OverriddenDecl))
        break;
      FoundDecls.insert(OverriddenDecl);
    }

    if (OverriddenDecl) {
      bool Erased = DeclsToReport.remove(
          FoundDeclTy(OverriddenDecl, DeclVisibilityKind::LocalVariable));
      assert(Erased);
      (void)Erased;
    }
    DeclsToReport.insert(FoundDeclTy(VD, Reason));
  }
};
} // unnamed namespace

/// \brief Enumerate all members in \c BaseTy (including members of extensions,
/// superclasses and implemented protocols), as seen from the context \c CurrDC.
///
/// This operation corresponds to a standard "dot" lookup operation like "a.b"
/// where 'self' is the type of 'a'.  This operation is only valid after name
/// binding.
static void lookupVisibleMemberDecls(
    Type BaseTy, VisibleDeclConsumer &Consumer, const DeclContext *CurrDC,
    LookupState LS, DeclVisibilityKind Reason, LazyResolver *TypeResolver) {
  OverrideFilteringConsumer ConsumerWrapper;
  VisitedSet Visited;
  lookupVisibleMemberDeclsImpl(BaseTy, ConsumerWrapper, CurrDC, LS, Reason,
                               TypeResolver, Visited);

  // Report the declarations we found to the real consumer.
  for (const auto &DeclAndReason : ConsumerWrapper.DeclsToReport)
    Consumer.foundDecl(DeclAndReason.D, DeclAndReason.Reason);
}

namespace {

struct FindLocalVal : public StmtVisitor<FindLocalVal> {
  const SourceManager &SM;
  SourceLoc Loc;
  VisibleDeclConsumer &Consumer;

  FindLocalVal(const SourceManager &SM, SourceLoc Loc,
               VisibleDeclConsumer &Consumer)
      : SM(SM), Loc(Loc), Consumer(Consumer) {}

  bool isReferencePointInRange(SourceRange R) {
    return SM.rangeContainsTokenLoc(R, Loc);
  }

  void checkValueDecl(ValueDecl *D, DeclVisibilityKind Reason) {
    Consumer.foundDecl(D, Reason);
  }

  void checkPattern(const Pattern *Pat, DeclVisibilityKind Reason) {
    switch (Pat->getKind()) {
    case PatternKind::Tuple:
      for (auto &field : cast<TuplePattern>(Pat)->getFields())
        checkPattern(field.getPattern(), Reason);
      return;
    case PatternKind::Paren:
    case PatternKind::Typed:
    case PatternKind::Var:
      return checkPattern(Pat->getSemanticsProvidingPattern(), Reason);
    case PatternKind::Named:
      return checkValueDecl(cast<NamedPattern>(Pat)->getDecl(), Reason);

    case PatternKind::NominalType: {
      for (auto &elt : cast<NominalTypePattern>(Pat)->getElements())
        checkPattern(elt.getSubPattern(), Reason);
      return;
    }
    case PatternKind::EnumElement: {
      auto *OP = cast<EnumElementPattern>(Pat);
      if (OP->hasSubPattern())
        checkPattern(OP->getSubPattern(), Reason);
      return;
    }
    // Handle non-vars.
    case PatternKind::Isa:
    case PatternKind::Expr:
    case PatternKind::Any:
      return;
    }
  }

  void checkGenericParams(GenericParamList *Params,
                          DeclVisibilityKind Reason) {
    if (!Params)
      return;

    for (auto P : *Params)
      checkValueDecl(P, Reason);
  }

  void checkSourceFile(const SourceFile &SF) {
    for (Decl *D : SF.Decls)
      if (TopLevelCodeDecl *TLCD = dyn_cast<TopLevelCodeDecl>(D))
        visit(TLCD->getBody());
  }

  void visitBreakStmt(BreakStmt *) {}
  void visitContinueStmt(ContinueStmt *) {}
  void visitFallthroughStmt(FallthroughStmt *) {}
  void visitReturnStmt(ReturnStmt *) {}
  void visitIfStmt(IfStmt * S) {
    if (auto *PBD = S->getCond().dyn_cast<PatternBindingDecl *>())
      checkPattern(PBD->getPattern(), DeclVisibilityKind::LocalVariable);
    visit(S->getThenStmt());
    if (S->getElseStmt())
      visit(S->getElseStmt());
  }
  void visitIfConfigStmt(IfConfigStmt * S) {
    // Active members are attached to the enclosing declaration, so there's no
    // need to walk anything within.
  }
  void visitWhileStmt(WhileStmt *S) {
    if (auto *PBD = S->getCond().dyn_cast<PatternBindingDecl *>())
      checkPattern(PBD->getPattern(), DeclVisibilityKind::LocalVariable);
    visit(S->getBody());
  }
  void visitDoWhileStmt(DoWhileStmt *S) {
    visit(S->getBody());
  }

  void visitForStmt(ForStmt *S) {
    if (!isReferencePointInRange(S->getSourceRange()))
      return;
    visit(S->getBody());
    for (Decl *D : S->getInitializerVarDecls()) {
      if (ValueDecl *VD = dyn_cast<ValueDecl>(D))
        checkValueDecl(VD, DeclVisibilityKind::LocalVariable);
    }
  }
  void visitForEachStmt(ForEachStmt *S) {
    if (!isReferencePointInRange(S->getSourceRange()))
      return;
    visit(S->getBody());
    checkPattern(S->getPattern(), DeclVisibilityKind::LocalVariable);
  }
  void visitBraceStmt(BraceStmt *S) {
    if (!isReferencePointInRange(S->getSourceRange()))
      return;
    for (auto elem : S->getElements()) {
      if (Stmt *S = elem.dyn_cast<Stmt*>())
        visit(S);
    }
    for (auto elem : S->getElements()) {
      if (Decl *D = elem.dyn_cast<Decl*>()) {
        if (ValueDecl *VD = dyn_cast<ValueDecl>(D))
          checkValueDecl(VD, DeclVisibilityKind::LocalVariable);
      }
    }
  }
  
  void visitSwitchStmt(SwitchStmt *S) {
    if (!isReferencePointInRange(S->getSourceRange()))
      return;
    for (CaseStmt *C : S->getCases()) {
      visit(C);
    }
  }
  
  void visitCaseStmt(CaseStmt *S) {
    if (!isReferencePointInRange(S->getSourceRange()))
      return;
    for (const auto &CLI : S->getCaseLabelItems()) {
      checkPattern(CLI.getPattern(), DeclVisibilityKind::LocalVariable);
    }
    visit(S->getBody());
  }
};
  
} // end anonymous namespace

void swift::lookupVisibleDecls(VisibleDeclConsumer &Consumer,
                               const DeclContext *DC,
                               LazyResolver *TypeResolver,
                               bool IncludeTopLevel,
                               SourceLoc Loc) {
  const Module &M = *DC->getParentModule();
  const SourceManager &SM = DC->getASTContext().SourceMgr;
  auto Reason = DeclVisibilityKind::MemberOfCurrentNominal;

  // If we are inside of a method, check to see if there are any ivars in scope,
  // and if so, whether this is a reference to one of them.
  while (!DC->isModuleScopeContext()) {
    const ValueDecl *BaseDecl = nullptr;
    const ValueDecl *MetaBaseDecl = nullptr;
    GenericParamList *GenericParams = nullptr;
    Type ExtendedType;
    auto LS = LookupState::makeUnqalified();

    // Skip initializer contexts, we will not find any declarations there.
    if (isa<Initializer>(DC)) {
      DC = DC->getParent();
      LS = LS.withOnMetatype();
    }

    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
      // Look for local variables; normally, the parser resolves these
      // for us, but it can't do the right thing inside local types.
      // FIXME: when we can parse and typecheck the function body partially for
      // code completion, AFD->getBody() check can be removed.
      if (Loc.isValid() && AFD->getBody()) {
        FindLocalVal(SM, Loc, Consumer).visit(AFD->getBody());
      }

      for (auto *P : AFD->getBodyParamPatterns())
        FindLocalVal(SM, Loc, Consumer)
            .checkPattern(P, DeclVisibilityKind::FunctionParameter);

      // Constructors and destructors don't have 'self' in parameter patterns.
      if (isa<ConstructorDecl>(AFD) || isa<DestructorDecl>(AFD))
        Consumer.foundDecl(AFD->getImplicitSelfDecl(),
                           DeclVisibilityKind::FunctionParameter);

      if (AFD->getExtensionType()) {
        ExtendedType = AFD->getExtensionType();
        BaseDecl = AFD->getImplicitSelfDecl();
        MetaBaseDecl = ExtendedType->getAnyNominal();
        DC = DC->getParent();

        if (auto *FD = dyn_cast<FuncDecl>(AFD))
          if (FD->isStatic())
            ExtendedType = MetatypeType::get(ExtendedType);
      }

      // Look in the generic parameters after checking our local declaration.
      GenericParams = AFD->getGenericParams();
    } else if (auto ACE = dyn_cast<AbstractClosureExpr>(DC)) {
      if (Loc.isValid()) {
        auto CE = cast<ClosureExpr>(ACE);
        FindLocalVal(SM, Loc, Consumer).visit(CE->getBody());
      }
    } else if (auto ED = dyn_cast<ExtensionDecl>(DC)) {
      ExtendedType = ED->getExtendedType();
      BaseDecl = ExtendedType->getNominalOrBoundGenericNominal();
      MetaBaseDecl = BaseDecl;
    } else if (auto ND = dyn_cast<NominalTypeDecl>(DC)) {
      ExtendedType = ND->getDeclaredType();
      BaseDecl = ND;
      MetaBaseDecl = BaseDecl;
    }

    if (BaseDecl) {
      ::lookupVisibleMemberDecls(ExtendedType, Consumer, DC, LS, Reason,
                                 TypeResolver);
    }

    // Check the generic parameters for something with the given name.
    if (GenericParams) {
      FindLocalVal(SM, Loc, Consumer)
          .checkGenericParams(GenericParams,
                              DeclVisibilityKind::GenericParameter);
    }

    DC = DC->getParent();
    Reason = DeclVisibilityKind::MemberOfOutsideNominal;
  }

  SmallVector<Module::ImportedModule, 8> extraImports;
  if (auto SF = dyn_cast<SourceFile>(DC)) {
    if (Loc.isValid()) {
      // Look for local variables in top-level code; normally, the parser
      // resolves these for us, but it can't do the right thing for
      // local types.
      FindLocalVal(SM, Loc, Consumer).checkSourceFile(*SF);
    }

    if (IncludeTopLevel) {
      auto &cached = SF->getCachedVisibleDecls();
      if (!cached.empty()) {
        for (auto result : cached)
          Consumer.foundDecl(result, DeclVisibilityKind::VisibleAtTopLevel);
        return;
      }

      for (auto importPair : SF->getImports())
        if (!importPair.second)
          extraImports.push_back(importPair.first);
    }
  }

  if (IncludeTopLevel) {
    using namespace namelookup;
    SmallVector<ValueDecl *, 0> moduleResults;
    auto &mutableM = const_cast<Module&>(M);
    lookupVisibleDeclsInModule(&mutableM, {}, moduleResults,
                               NLKind::QualifiedLookup,
                               ResolutionKind::Overloadable,
                               TypeResolver, DC, extraImports);
    for (auto result : moduleResults)
      Consumer.foundDecl(result, DeclVisibilityKind::VisibleAtTopLevel);

    if (auto SF = dyn_cast<SourceFile>(DC))
      SF->cacheVisibleDecls(std::move(moduleResults));
  }
}

void swift::lookupVisibleMemberDecls(VisibleDeclConsumer &Consumer, Type BaseTy,
                                     const DeclContext *CurrDC,
                                     LazyResolver *TypeResolver) {
  ::lookupVisibleMemberDecls(BaseTy, Consumer, CurrDC,
                             LookupState::makeQualified(),
                             DeclVisibilityKind::MemberOfCurrentNominal,
                             TypeResolver);
}
