//===--- ASTScopeLookup.cpp - Swift Object-Oriented AST Scope -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This file implements the lookup functionality of the ASTScopeImpl ontology.
///
//===----------------------------------------------------------------------===//
#include "swift/AST/ASTScope.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/Support/Compiler.h"
#include <algorithm>

using namespace swift;
using namespace namelookup;
using namespace ast_scope;

Optional<bool> ASTScopeImpl::unqualifiedLookup(
    SourceFile *sourceFile, const DeclName name, const SourceLoc loc,
    const DeclContext *const startingContext,
    const Optional<bool> isCascadingUseArg, DeclConsumer consumer) {
  const auto *start =
      findStartingScopeForLookup(sourceFile, name, loc, startingContext);
  if (!start)
    return isCascadingUseArg;

  return start->lookup(NullablePtr<DeclContext>(), nullptr, nullptr,
                       isCascadingUseArg, consumer);
}

const ASTScopeImpl *ASTScopeImpl::findStartingScopeForLookup(
    SourceFile *sourceFile, const DeclName name, const SourceLoc loc,
    const DeclContext *const startingContext) {
  // At present, use legacy code in unqualifiedLookup.cpp to handle module-level
  // lookups
  // TODO: implement module scope someday
  if (startingContext->getContextKind() == DeclContextKind::Module)
    return nullptr;

  auto *const fileScope = sourceFile->getScope()->impl;
  // Parser may have added decls to source file, since previous lookup
  sourceFile->getScope()->addAnyNewScopesToTree();
  if (name.isOperator())
    return fileScope; // operators always at file scope

  const auto innermost = fileScope->findInnermostEnclosingScope(loc);

  // The legacy lookup code gets passed both a SourceLoc and a starting context.
  // Someday, we might get away with just a SourceLoc.
  // For now, to ensure compatibility, start with the scope that matches the
  // starting context and includes the starting location.

  const auto *startingScope = innermost;
  for (; startingScope &&
         !startingScope->doesContextMatchStartingContext(startingContext);
       startingScope = startingScope->getParent().getPtrOrNull()) {
  }
  // Someday, just use the assertion below. For now, print out lots of info for
  // debugging.
  if (!startingScope) {
    llvm::errs() << "ASTScopeImpl: resorting to startingScope hack, file: "
                 << sourceFile->getFilename() << "\n";
    llvm::errs() << "'";
    name.print(llvm::errs());
    llvm::errs() << "' ";
    llvm::errs() << "loc: ";
    loc.dump(sourceFile->getASTContext().SourceMgr);
    llvm::errs() << "\nstarting context:\n ";
    startingContext->dumpContext();
    //    llvm::errs() << "\ninnermost: ";
    //    innermost->dump();
    //    llvm::errs() << "in: \n";
    //    fileScope->dump();
    llvm::errs() << "\n\n";
  }

  assert(startingScope && "ASTScopeImpl: could not find startingScope");
  return startingScope;
}

const ASTScopeImpl *
ASTScopeImpl::findInnermostEnclosingScope(SourceLoc loc) const {
  SourceManager &sourceMgr = getSourceManager();

  const auto *s = this;
  for (NullablePtr<const ASTScopeImpl> c;
       (c = s->findChildContaining(loc, sourceMgr)); s = c.get()) {
  }
  return s;
}

NullablePtr<const ASTScopeImpl>
ASTScopeImpl::findChildContaining(SourceLoc loc,
                                  SourceManager &sourceMgr) const {
  // Use binary search to find the child that contains this location.
  struct CompareLocs {
    SourceManager &sourceMgr;

    bool operator()(const ASTScopeImpl *scope, SourceLoc loc) {
      return sourceMgr.isBeforeInBuffer(scope->getSourceRange().End, loc);
    }
    bool operator()(SourceLoc loc, const ASTScopeImpl *scope) {
      return sourceMgr.isBeforeInBuffer(loc, scope->getSourceRange().End);
    }
  };
  auto *const *child = std::lower_bound(
      getChildren().begin(), getChildren().end(), loc, CompareLocs{sourceMgr});

  if (child != getChildren().end() &&
      sourceMgr.rangeContainsTokenLoc((*child)->getSourceRange(), loc))
    return *child;

  return nullptr;
}

#pragma mark doesContextMatchStartingContext
// Match existing UnqualifiedLookupBehavior

bool ASTScopeImpl::doesContextMatchStartingContext(
    const DeclContext *context) const {
  // Why are we not checking the loc for this--because already did binary search
  // on loc to find the start First, try MY DeclContext
  if (auto myDCForL = getDeclContext())
    return myDCForL == context;
  // If I don't have one, ask my parent.
  // (Choose innermost scope with matching loc & context.)
  if (auto p = getParent())
    return p.get()->doesContextMatchStartingContext(context);
  // Topmost scope always has a context, the SourceFile.
  llvm_unreachable("topmost scope always has a context, the SourceFile");
}

// For a SubscriptDecl with generic parameters, the call tries to do lookups
// with startingContext equal to either the get or set subscript
// AbstractFunctionDecls. Since the generic parameters are in the
// SubScriptDeclScope, and not the AbstractFunctionDecl scopes (after all how
// could one parameter be in two scopes?), GenericParamScoped intercepts the
// match query here and tests against the accessor DeclContexts.
bool GenericParamScope::doesContextMatchStartingContext(
    const DeclContext *context) const {
  if (auto *asd = dyn_cast<AbstractStorageDecl>(holder)) {
    for (auto accessor : asd->getAllAccessors()) {
      if (up_cast<DeclContext>(accessor) == context)
        return true;
    }
  }
  return false;
}

#pragma mark lookup methods that run once per scope

Optional<bool>
ASTScopeImpl::lookup(const NullablePtr<DeclContext> selfDC,
                     const NullablePtr<const ASTScopeImpl> limit,
                     const NullablePtr<const Decl> haveAlreadyLookedHere,
                     const Optional<bool> isCascadingUseArg,
                     DeclConsumer consumer) const {
#ifndef NDEBUG
  consumer.stopForDebuggingIfTargetLookup();
#endif

  // Certain illegal nestings, e.g. protocol nestled inside a struct,
  // require that lookup stop at the outer scope.
  if (this == limit.getPtrOrNull())
    return isCascadingUseArg;

  const Optional<bool> isCascadingUseForThisScope =
      resolveIsCascadingUseForThisScope(isCascadingUseArg);
  // Check local variables, etc. first.
  if (lookupLocalBindings(consumer))
    return isCascadingUseForThisScope;

  /// Because a body scope nests in a generic param scope, etc, we might look in
  /// the self type twice. That's why we pass haveAlreadyLookedHere.
  /// Look in the generics and self type only iff haven't already looked there.
  bool isDone;
  Optional<bool> isCascadingUseResult;
  std::tie(isDone, isCascadingUseResult) =
      haveAlreadyLookedHere && haveAlreadyLookedHere == getDecl().getPtrOrNull()
          ? std::make_pair(false, isCascadingUseForThisScope)
          : lookInGenericsAndSelfType(selfDC, isCascadingUseForThisScope,
                                      consumer);
  if (isDone || !getParent())
    return isCascadingUseResult;

  return lookupInParent(selfDC, limit, haveAlreadyLookedHere,
                        isCascadingUseResult, consumer);
}

std::pair<bool, Optional<bool>>
ASTScopeImpl::lookInGenericsAndSelfType(const NullablePtr<DeclContext> selfDC,
                                        const Optional<bool> isCascadingUse,
                                        DeclConsumer consumer) const {
  // Look for generics before members in violation of lexical ordering because
  // you can say "self.name" to get a name shadowed by a generic but you
  // can't do the opposite to get a generic shadowed by a name.

  if (lookInGenericParameters(consumer))
    return {true, isCascadingUse};
  // Dig out the type we're looking into.
  // Perform lookup into the type
  return lookupInSelfType(selfDC, isCascadingUse, consumer);
}

Optional<bool> ASTScopeImpl::lookupInParent(
    const NullablePtr<DeclContext> selfDC,
    const NullablePtr<const ASTScopeImpl> limit,
    const NullablePtr<const Decl> haveAlreadyLookedHere,
    const Optional<bool> isCascadingUse, DeclConsumer consumer) const {
  // If this scope has an associated Decl, we have already searched its generics
  // and selfType, so no need to look again.
  NullablePtr<const Decl> haveAlreadyLookedHereForParent =
      getDecl() ? getDecl().getPtrOrNull() : haveAlreadyLookedHere;

  // If there is no limit and this scope induces one, pass that on.
  const NullablePtr<const ASTScopeImpl> limitForParent =
      limit ? limit : getLookupLimit();

  return getParent().get()->lookup(
      computeSelfDCForParent(selfDC), limitForParent,
      haveAlreadyLookedHereForParent, isCascadingUse, consumer);
}

#pragma mark lookInGenericParameters

bool ASTScopeImpl::lookInGenericParameters(ASTScopeImpl::DeclConsumer) const {
  return false;
}

bool AbstractFunctionDeclScope::lookInGenericParameters(
    ASTScopeImpl::DeclConsumer consumer) const {
  return lookInMyAndOuterGenericParameters(decl, consumer);
}
bool SubscriptDeclScope::lookInGenericParameters(
    ASTScopeImpl::DeclConsumer consumer) const {
  return lookInMyAndOuterGenericParameters(decl, consumer);
}

bool GTXScope::lookInGenericParameters(
    ASTScopeImpl::DeclConsumer consumer) const {
  // For Decls:
  // WAIT, WHAT?! Isn't this covered by the GenericParamScope
  // lookupLocalBindings? No, that's for use of generics in the body. This is
  // for generic restrictions.

  // For Bodies:
  // Sigh... These must be here so that from body, we search generics before
  // members. But they also must be on the Decl scope for lookups starting from
  // generic parameters, where clauses, etc.
  return lookInMyAndOuterGenericParameters(getGenericContext(), consumer);
}

bool ASTScopeImpl::lookInMyAndOuterGenericParameters(
    const GenericContext *const gc, ASTScopeImpl::DeclConsumer consumer) {
  for (auto *params = gc->getGenericParams(); params;
       params = params->getOuterParameters()) {
    SmallVector<ValueDecl *, 32> bindings;
    for (auto *param : params->getParams())
      bindings.push_back(param);
    if (consumer.consume(bindings, DeclVisibilityKind::GenericParameter))
      return true;
  }
  return false;
}

#pragma mark lookupInSelfType

std::pair<bool, Optional<bool>>
ASTScopeImpl::lookupInSelfType(NullablePtr<DeclContext>,
                               const Optional<bool> isCascadingUse,
                               DeclConsumer) const {
  return dontLookupInSelfType(isCascadingUse);
}

std::pair<bool, Optional<bool>>
GTXScope::lookupInSelfType(NullablePtr<DeclContext> selfDC,
                           const Optional<bool> isCascadingUse,
                           ASTScopeImpl::DeclConsumer consumer) const {
  return portion->lookupInSelfTypeOf(this, selfDC, isCascadingUse, consumer);
}

std::pair<bool, Optional<bool>> Portion::lookupInSelfTypeOf(
    const GTXScope *scope, NullablePtr<DeclContext> selfDC,
    const Optional<bool> isCascadingUse, ASTScopeImpl::DeclConsumer) const {
  return scope->dontLookupInSelfType(isCascadingUse);
}

std::pair<bool, Optional<bool>> GTXWhereOrBodyPortion::lookupInSelfTypeOf(
    const GTXScope *scope, NullablePtr<DeclContext> selfDC,
    const Optional<bool> isCascadingUse,
    ASTScopeImpl::DeclConsumer consumer) const {
  auto nt = scope->getCorrespondingNominalTypeDecl().getPtrOrNull();
  if (!nt)
    return Portion::lookupInSelfTypeOf(scope, selfDC, isCascadingUse, consumer);
  return consumer.lookupInSelfType(selfDC, scope->getDeclContext().get(), nt,
                                   isCascadingUse);
}

#pragma mark lookupLocalBindings

bool ASTScopeImpl::lookupLocalBindings(DeclConsumer consumer) const {
  return false; // most kinds of scopes have none
}

bool GenericParamScope::lookupLocalBindings(DeclConsumer consumer) const {
  auto *param = paramList->getParams()[index];
  return consumer.consume({param}, DeclVisibilityKind::GenericParameter);
}

bool PatternEntryUseScope::lookupLocalBindings(DeclConsumer consumer) const {
  return lookupLocalBindingsInPattern(getPattern(), consumer);
}

bool StatementConditionElementPatternScope::lookupLocalBindings(
    DeclConsumer consumer) const {
  return lookupLocalBindingsInPattern(pattern, consumer);
}

bool ForEachPatternScope::lookupLocalBindings(DeclConsumer consumer) const {
  return lookupLocalBindingsInPattern(stmt->getPattern(), consumer);
}

bool CatchStmtScope::lookupLocalBindings(DeclConsumer consumer) const {
  return lookupLocalBindingsInPattern(stmt->getErrorPattern(), consumer);
}

bool CaseStmtScope::lookupLocalBindings(DeclConsumer consumer) const {
  for (auto &item : stmt->getMutableCaseLabelItems())
    if (lookupLocalBindingsInPattern(item.getPattern(), consumer))
      return true;
  return false;
}

bool AbstractFunctionBodyScope::lookupLocalBindings(
    DeclConsumer consumer) const {
  if (auto *paramList = decl->getParameters()) {
    for (auto *paramDecl : *paramList)
      if (consumer.consume({paramDecl}, DeclVisibilityKind::FunctionParameter))
        return true;
  }
  if (auto *s = decl->getImplicitSelfDecl()) {
    if (consumer.consume({s}, DeclVisibilityKind::FunctionParameter))
      return true;
  }
  return false;
}

bool PureFunctionBodyScope::lookupLocalBindings(DeclConsumer consumer) const {
  if (AbstractFunctionBodyScope::lookupLocalBindings(consumer))
    return true;

  // Consider \c var t: T { (did/will/)get/set { ... t }}
  // Lookup needs to find t, but if the var is inside of a type the baseDC needs
  // to be set. It all works fine, except: if the var is not inside of a type,
  // then t needs to be found as a local binding:
  if (auto *accessor = dyn_cast<AccessorDecl>(decl)) {
    if (auto *storage = accessor->getStorage())
      if (consumer.consume({storage}, DeclVisibilityKind::LocalVariable))
        return true;
  }
  return false;
}

bool SpecializeAttributeScope::lookupLocalBindings(
    DeclConsumer consumer) const {
  if (auto *params = whatWasSpecialized->getGenericParams())
    for (auto *param : params->getParams())
      if (consumer.consume({param}, DeclVisibilityKind::GenericParameter))
        return true;
  return false;
}

bool BraceStmtScope::lookupLocalBindings(DeclConsumer consumer) const {
  // All types and functions are visible anywhere within a brace statement
  // scope. When ordering matters (i.e. var decl) we will have split the brace
  // statement into nested scopes.
  //
  // Don't stop at the first one, there may be local funcs with same base name
  // and want them all.
  SmallVector<ValueDecl *, 32> localBindings;
  for (auto braceElement : stmt->getElements()) {
    if (auto localBinding = braceElement.dyn_cast<Decl *>()) {
      if (isa<AbstractFunctionDecl>(localBinding) ||
          isa<TypeDecl>(localBinding))
        localBindings.push_back(cast<ValueDecl>(localBinding));
    }
  }
  return consumer.consume(localBindings, DeclVisibilityKind::LocalVariable);
}

bool PatternEntryInitializerScope::lookupLocalBindings(
    DeclConsumer consumer) const {
  // 'self' is available within the pattern initializer of a 'lazy' variable.
  auto *initContext = cast_or_null<PatternBindingInitializer>(
      decl->getPatternList()[0].getInitContext());
  if (initContext) {
    if (auto *selfParam = initContext->getImplicitSelfDecl()) {
      return consumer.consume({selfParam},
                              DeclVisibilityKind::FunctionParameter);
    }
  }
  return false;
}

bool ClosureParametersScope::lookupLocalBindings(DeclConsumer consumer) const {
  if (auto *cl = captureList.getPtrOrNull()) {
    CaptureListExpr *mutableCL =
        const_cast<CaptureListExpr *>(captureList.get());
    for (auto &e : mutableCL->getCaptureList()) {
      if (consumer.consume(
              {e.Var},
              DeclVisibilityKind::LocalVariable)) // or FunctionParamter??
        return true;
    }
  }
  for (auto param : *closureExpr->getParameters())
    if (consumer.consume({param}, DeclVisibilityKind::FunctionParameter))
      return true;
  return false;
}

bool ASTScopeImpl::lookupLocalBindingsInPattern(Pattern *p,
                                                DeclConsumer consumer) {
  if (!p)
    return false;
  bool isDone = false;
  p->forEachVariable([&](VarDecl *var) {
    if (!isDone)
      isDone = consumer.consume({var}, DeclVisibilityKind::LocalVariable);
  });
  return isDone;
}

#pragma mark getLookupLimit

NullablePtr<const ASTScopeImpl> ASTScopeImpl::getLookupLimit() const {
  return nullptr;
}

NullablePtr<const ASTScopeImpl> GTXScope::getLookupLimit() const {
  return portion->getLookupLimitFor(this);
}

NullablePtr<const ASTScopeImpl>
Portion::getLookupLimitFor(const GTXScope *) const {
  return nullptr;
}
NullablePtr<const ASTScopeImpl>
GTXWholePortion::getLookupLimitFor(const GTXScope *scope) const {
  return scope->getLookupLimitForDecl();
}

NullablePtr<const ASTScopeImpl> GTXScope::getLookupLimitForDecl() const {
  return nullptr;
}

NullablePtr<const ASTScopeImpl>
NominalTypeScope::getLookupLimitForDecl() const {
  if (isa<ProtocolDecl>(decl)) {
    // ProtocolDecl can only be legally nested in a SourceFile,
    // so any other kind of Decl is illegal
    return parentIfNotChildOfTopScope();
  }
  // AFAICT, a struct, decl, or enum can be nested inside anything
  // but a ProtocolDecl.
  return ancestorWithDeclSatisfying(
      [&](const Decl *const d) { return isa<ProtocolDecl>(d); });
}

NullablePtr<const ASTScopeImpl> ASTScopeImpl::ancestorWithDeclSatisfying(
    function_ref<bool(const Decl *)> predicate) const {
  for (NullablePtr<const ASTScopeImpl> s = getParent(); s;
       s = s.get()->getParent()) {
    if (Decl *d = s.get()->getDecl().getPtrOrNull()) {
      if (predicate(d))
        return s;
    }
  }
  return nullptr;
}

#pragma mark computeSelfDCForParent

// If the lookup depends on implicit self, selfDC is its context.
// (Names in extensions never depend on self.)
// Lookup can propagate it up from, say a method to the enclosing type body.

// By default, propagate the selfDC up to a NomExt decl, body,
// or where clause
NullablePtr<DeclContext>
ASTScopeImpl::computeSelfDCForParent(NullablePtr<DeclContext> selfDC) const {
  return selfDC;
}

// Forget the "self" declaration:
NullablePtr<DeclContext>
GTXScope::computeSelfDCForParent(NullablePtr<DeclContext>) const {
  return nullptr;
}

NullablePtr<DeclContext> PatternEntryInitializerScope::computeSelfDCForParent(
    NullablePtr<DeclContext> selfDC) const {
  // Pattern binding initializers are only interesting insofar as they
  // affect lookup in an enclosing nominal type or extension thereof.
  if (auto *ic = getPatternEntry().getInitContext()) {
    if (auto *bindingInit = dyn_cast<PatternBindingInitializer>(ic)) {
      // Lazy variable initializer contexts have a 'self' parameter for
      // instance member lookup.
      if (bindingInit->getImplicitSelfDecl()) {
        assert((selfDC.isNull() || selfDC == bindingInit) &&
               "Would lose information");
        return bindingInit;
      }
    }
  }
  return selfDC;
}

NullablePtr<DeclContext>
MethodBodyScope::computeSelfDCForParent(NullablePtr<DeclContext> selfDC) const {
  assert(!selfDC && "Losing selfDC");
  return decl;
}
NullablePtr<DeclContext> PureFunctionBodyScope::computeSelfDCForParent(
    NullablePtr<DeclContext> selfDC) const {
  return selfDC;
}

#pragma mark resolveIsCascadingUseForThisScope helpers
// TODO: rename and comment

static bool isCascadingUseAccordingTo(const DeclContext *const dc) {
  return dc->isCascadingContextForLookup(false);
}

static bool ifUnknownIsCascadingUseAccordingTo(Optional<bool> isCascadingUse,
                                               const DeclContext *const dc) {
  return isCascadingUse.getValueOr(isCascadingUseAccordingTo(dc));
}

#pragma mark resolveIsCascadingUseForThisScope

Optional<bool> ASTScopeImpl::resolveIsCascadingUseForThisScope(
    Optional<bool> isCascadingUse) const {
  return isCascadingUse;
}

Optional<bool> GenericParamScope::resolveIsCascadingUseForThisScope(
    Optional<bool> isCascadingUse) const {
  if (auto *dc = getDeclContext().getPtrOrNull())
    return ifUnknownIsCascadingUseAccordingTo(isCascadingUse, dc);
  llvm_unreachable("generic what?");
}

Optional<bool> AbstractFunctionDeclScope::resolveIsCascadingUseForThisScope(
    Optional<bool> isCascadingUse) const {
  return decl->isCascadingContextForLookup(false) &&
         isCascadingUse.getValueOr(true);
}

Optional<bool> AbstractFunctionBodyScope::resolveIsCascadingUseForThisScope(
    Optional<bool> isCascadingUse) const {
  return false;
}

Optional<bool> GTXScope::resolveIsCascadingUseForThisScope(
    Optional<bool> isCascadingUse) const {
  return ifUnknownIsCascadingUseAccordingTo(isCascadingUse,
                                            getDeclContext().get());
}

Optional<bool>
DefaultArgumentInitializerScope::resolveIsCascadingUseForThisScope(
    Optional<bool>) const {
  return false;
}

Optional<bool> ClosureParametersScope::resolveIsCascadingUseForThisScope(
    Optional<bool> isCascadingUse) const {
  return ifUnknownIsCascadingUseAccordingTo(isCascadingUse, closureExpr);
}
Optional<bool> ClosureBodyScope::resolveIsCascadingUseForThisScope(
    Optional<bool> isCascadingUse) const {
  return ifUnknownIsCascadingUseAccordingTo(isCascadingUse, closureExpr);
}

Optional<bool> PatternEntryInitializerScope::resolveIsCascadingUseForThisScope(
    Optional<bool> isCascadingUse) const {
  auto *const initContext = getPatternEntry().getInitContext();
  auto *PBI = cast_or_null<PatternBindingInitializer>(initContext);
  auto *isd = PBI ? PBI->getImplicitSelfDecl() : nullptr;

  // 'self' is available within the pattern initializer of a 'lazy' variable.
  if (isd)
    return ifUnknownIsCascadingUseAccordingTo(isCascadingUse, PBI);

  // initializing stored property of a type
  auto *const patternDeclContext = decl->getDeclContext();
  if (patternDeclContext->isTypeContext())
    return isCascadingUseAccordingTo(PBI->getParent());

  // initializing global or local
  if (PBI)
    return ifUnknownIsCascadingUseAccordingTo(isCascadingUse, PBI);

  return isCascadingUse;
}
