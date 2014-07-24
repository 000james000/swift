//===--- CSApply.cpp - Constraint Application -----------------------------===//
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
// This file implements application of a solution to a constraint
// system to a particular expression, resulting in a
// fully-type-checked expression.
//
//===----------------------------------------------------------------------===//

#include "ConstraintSystem.h"
#include "swift/AST/ArchetypeBuilder.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/Parse/Lexer.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace constraints;

/// \brief Retrieve the fixed type for the given type variable.
Type Solution::getFixedType(TypeVariableType *typeVar) const {
  auto knownBinding = typeBindings.find(typeVar);
  assert(knownBinding != typeBindings.end());
  return knownBinding->second;
}

Type Solution::computeSubstitutions(Type origType, DeclContext *dc,
                                    Type openedType,
                 SmallVectorImpl<Substitution> &substitutions) const {
  auto &tc = getConstraintSystem().getTypeChecker();
  auto &ctx = tc.Context;

  // Gather the substitutions from archetypes to concrete types, found
  // by identifying all of the type variables in the original type
  // FIXME: It's unfortunate that we're using archetypes here, but we don't
  // have another way to map from type variables back to dependent types (yet);
  TypeSubstitutionMap typeSubstitutions;
  auto type = openedType.transform([&](Type type) -> Type {
                if (auto tv = dyn_cast<TypeVariableType>(type.getPointer())) {
                  auto archetype = tv->getImpl().getArchetype();
                  auto simplified = getFixedType(tv);
                  typeSubstitutions[archetype] = simplified;
                  return SubstitutedType::get(archetype, simplified,
                                              tc.Context);
                }

                return type;
              });

  auto currentModule = getConstraintSystem().DC->getParentModule();
  ArchetypeType *currentArchetype = nullptr;
  Type currentReplacement;
  SmallVector<ProtocolConformance *, 4> currentConformances;

  ArrayRef<Requirement> requirements;
  if (auto genericFn = origType->getAs<GenericFunctionType>()) {
    requirements = genericFn->getRequirements();
  } else {
    requirements = dc->getDeclaredTypeOfContext()->getAnyNominal()
                     ->getGenericRequirements();
  }

  for (const auto &req : requirements) {
    // Drop requirements for parameters that have been constrained away to
    // concrete types.
    auto firstArchetype
      = ArchetypeBuilder::mapTypeIntoContext(dc, req.getFirstType())
        ->getAs<ArchetypeType>();
    if (!firstArchetype)
      continue;
    
    switch (req.getKind()) {
    case RequirementKind::Conformance:
      // If this is a protocol conformance requirement, get the conformance
      // and record it.
      if (auto protoType = req.getSecondType()->getAs<ProtocolType>()) {
        assert(firstArchetype == currentArchetype
               && "Archetype out-of-sync");
        ProtocolConformance *conformance = nullptr;
        Type replacement = currentReplacement;
        bool conforms = tc.conformsToProtocol(replacement,
                                              protoType->getDecl(),
                                              getConstraintSystem().DC,
                                              &conformance);
        assert((conforms ||
                replacement->isExistentialType() ||
                firstArchetype->getIsRecursive() ||
                replacement->is<GenericTypeParamType>()) &&
               "Constraint system missed a conformance?");
        (void)conforms;

        assert(conformance ||
               replacement->isExistentialType() ||
               replacement->is<ArchetypeType>() ||
               replacement->is<GenericTypeParamType>());
        currentConformances.push_back(conformance);
        break;
      }
      break;

    case RequirementKind::SameType:
      // Same-type requirements aren't recorded in substitutions.
      break;

    case RequirementKind::WitnessMarker:
      // Flush the current conformances.
      if (currentArchetype) {
        substitutions.push_back({
          currentArchetype,
          currentReplacement,
          ctx.AllocateCopy(currentConformances)
        });
        currentConformances.clear();
      }

      // Each witness marker starts a new substitution.
      currentArchetype = firstArchetype;
      currentReplacement = tc.substType(currentModule, currentArchetype,
                                        typeSubstitutions);
      break;
    }
  }
  
  // Flush the final conformances.
  if (currentArchetype) {
    substitutions.push_back({
      currentArchetype,
      currentReplacement,
      ctx.AllocateCopy(currentConformances),
    });
    currentConformances.clear();
  }

  return type;
}

/// \brief Find a particular named function witness for a type that conforms to
/// the given protocol.
///
/// \param tc The type check we're using.
///
/// \param dc The context in which we need a witness.
///
/// \param type The type whose witness to find.
///
/// \param proto The protocol to which the type conforms.
///
/// \param name The name of the requirement.
///
/// \param diag The diagnostic to emit if the protocol definition doesn't
/// have a requirement with the given name.
///
/// \returns The named witness.
template <typename DeclTy>
static DeclTy *findNamedWitnessImpl(TypeChecker &tc, DeclContext *dc, Type type,
                                    ProtocolDecl *proto, Identifier name,
                                    Diag<> diag) {
  // Find the named requirement.
  DeclTy *requirement = nullptr;
  for (auto member : proto->getMembers()) {
    auto d = dyn_cast<DeclTy>(member);
    if (!d || !d->hasName())
      continue;

    if (d->getName() == name) {
      requirement = d;
      break;
    }
  }

  if (!requirement || requirement->isInvalid()) {
    tc.diagnose(proto->getLoc(), diag);
    return nullptr;
  }

  // Find the member used to satisfy the named requirement.
  ProtocolConformance *conformance = 0;
  bool conforms = tc.conformsToProtocol(type, proto, dc, &conformance);
  if (!conforms)
    return nullptr;

  // For an archetype, just return the requirement from the protocol. There
  // are no protocol conformance tables.
  if (type->is<ArchetypeType>()) {
    return requirement;
  }

  assert(conformance && "Missing conformance information");
  // FIXME: Dropping substitutions here.
  return cast<DeclTy>(conformance->getWitness(requirement, &tc).getDecl());
}

static FuncDecl *findNamedWitness(TypeChecker &tc, DeclContext *dc, Type type,
                                  ProtocolDecl *proto, Identifier name,
                                  Diag<> diag) {
  return findNamedWitnessImpl<FuncDecl>(tc, dc, type, proto, name, diag);
}

static VarDecl *findNamedPropertyWitness(TypeChecker &tc, DeclContext *dc,
                                         Type type, ProtocolDecl *proto,
                                         Identifier name, Diag<> diag) {
  return findNamedWitnessImpl<VarDecl>(tc, dc, type, proto, name, diag);
}

/// Adjust the given type to become the self type when referring to
/// the given member.
static Type adjustSelfTypeForMember(Type baseTy, ValueDecl *member,
                                    bool IsDirectPropertyAccess,
                                    DeclContext *UseDC) {
  auto baseObjectTy = baseTy->getLValueOrInOutObjectType();
  if (auto func = dyn_cast<AbstractFunctionDecl>(member)) {
    // If 'self' is an inout type, turn the base type into an lvalue
    // type with the same qualifiers.
    auto selfTy = func->getType()->getAs<AnyFunctionType>()->getInput();
    if (selfTy->is<InOutType>()) {
      // Unless we're looking at a nonmutating existential member.  In which
      // case, the member will be modeled as an inout but ExistentialMemberRef
      // and ArchetypeMemberRef want to take the base as an rvalue.
      if (auto *fd = dyn_cast<FuncDecl>(func))
        if (!fd->isMutating() &&
            (baseObjectTy->isExistentialType() ||
             baseObjectTy->is<ArchetypeType>()))
            return baseObjectTy;
      
      return InOutType::get(baseObjectTy);
    }

    // Otherwise, return the rvalue type.
    return baseObjectTy;
  }

  // If the base of the access is mutable, then we may be invoking a getter or
  // setter and the base needs to be mutable.
  if (auto *VD = dyn_cast<VarDecl>(member)) {
    if (VD->hasAccessorFunctions() && baseTy->is<InOutType>() &&
        !IsDirectPropertyAccess)
      return InOutType::get(baseObjectTy);
   
    // If the member is immutable in this context, the base is always an
    // unqualified baseObjectTy.
    if (!VD->isSettable(UseDC))
      return baseObjectTy;
  }
  
  // If the base of the subscript is mutable, then we may be invoking a mutable
  // getter or setter.
  if (isa<SubscriptDecl>(member) && !baseTy->hasReferenceSemantics() &&
      baseTy->is<InOutType>())
    return InOutType::get(baseObjectTy);
  
  // Accesses to non-function members in value types are done through an @lvalue
  // type.
  if (baseTy->is<InOutType>())
    return LValueType::get(baseObjectTy);
  
  // Accesses to members in values of reference type (classes, metatypes) are
  // always done through a the reference to self.  Accesses to value types with
  // a non-mutable self are also done through the base type.
  return baseTy;
}

/// Return true if a MemberReferenceExpr with the specified base and member in
/// the specified DeclContext should be implicitly marked as
/// "isDirectPropertyAccess".
static bool isImplicitDirectMemberReference(Expr *base, VarDecl *member,
                                            DeclContext *DC) {
  // Properties that have storage and accessors are frequently accessed through
  // accessors.  However, in the init and destructor methods for the type
  // immediately containing the property, accesses are done direct.
  if (auto *AFD_DC = dyn_cast<AbstractFunctionDecl>(DC))
    if (member->hasStorage() &&
        // In a ctor or dtor.
        (isa<ConstructorDecl>(AFD_DC) || isa<DestructorDecl>(AFD_DC)) &&

        // Ctor or dtor are for immediate class, not a derived class.
        AFD_DC->getParent()->getDeclaredTypeOfContext()->getCanonicalType() ==
          member->getDeclContext()->getDeclaredTypeOfContext()->getCanonicalType() &&

        // Is a "self.property" reference.
        isa<DeclRefExpr>(base) &&
        AFD_DC->getImplicitSelfDecl() == cast<DeclRefExpr>(base)->getDecl()) {
      // Access this directly instead of going through (e.g.) observing or
      // trivial accessors.
      return true;
    }

  // If the value is always directly accessed from this context, do it.
  return member->isUseFromContextDirect(DC);
}

namespace {
  /// \brief Rewrites an expression by applying the solution of a constraint
  /// system to that expression.
  class ExprRewriter : public ExprVisitor<ExprRewriter, Expr *> {
  public:
    ConstraintSystem &cs;
    DeclContext *dc;
    const Solution &solution;
    
  private:
    /// \brief Coerce the given tuple to another tuple type.
    ///
    /// \param expr The expression we're converting.
    ///
    /// \param fromTuple The tuple type we're converting from, which is the same
    /// as \c expr->getType().
    ///
    /// \param toTuple The tuple type we're converting to.
    ///
    /// \param locator Locator describing where this tuple conversion occurs.
    ///
    /// \param sources The sources of each of the elements to be used in the
    /// resulting tuple, as provided by \c computeTupleShuffle.
    ///
    /// \param variadicArgs The source indices that are mapped to the variadic
    /// parameter of the resulting tuple, as provided by \c computeTupleShuffle.
    Expr *coerceTupleToTuple(Expr *expr, TupleType *fromTuple,
                             TupleType *toTuple,
                             ConstraintLocatorBuilder locator,
                             SmallVectorImpl<int> &sources,
                             SmallVectorImpl<unsigned> &variadicArgs);

    /// \brief Coerce the given scalar value to the given tuple type.
    ///
    /// \param expr The expression to be coerced.
    /// \param toTuple The tuple type to which the expression will be coerced.
    /// \param toScalarIdx The index of the scalar field within the tuple type
    /// \c toType.
    /// \param locator Locator describing where this conversion occurs.
    ///
    /// \returns The coerced expression, whose type will be equivalent to
    /// \c toTuple.
    Expr *coerceScalarToTuple(Expr *expr, TupleType *toTuple,
                              int toScalarIdx,
                              ConstraintLocatorBuilder locator);

    /// \brief Coerce the given value to existential type.
    ///
    /// \param expr The expression to be coerced.
    /// \param toType The tupe to which the expression will be coerced.
    /// \param locator Locator describing where this conversion occurs.
    ///
    /// \return The coerced expression, whose type will be equivalent to
    /// \c toType.
    Expr *coerceExistential(Expr *expr, Type toType,
                            ConstraintLocatorBuilder locator);

    /// \brief Coerce the given value to an existential metatype type.
    ///
    /// \param expr The expression to be coerced.
    /// \param toType The tupe to which the expression will be coerced.
    /// \param locator Locator describing where this conversion occurs.
    ///
    /// \return The coerced expression, whose type will be equivalent to
    /// \c toType.
    Expr *coerceExistentialMetatype(Expr *expr, Type toType,
                                    ConstraintLocatorBuilder locator);

    /// \brief Coerce the expression to another type via a user-defined
    /// conversion.
    ///
    /// \param expr The expression to be coerced.
    /// \param toType The tupe to which the expression will be coerced.
    /// \param locator Locator describing where this conversion occurs.
    ///
    /// \return The coerced expression, whose type will be equivalent to
    /// \c toType.
    Expr *coerceViaUserConversion(Expr *expr, Type toType,
                                  ConstraintLocatorBuilder locator);

    /// \brief Coerce an expression of (possibly unchecked) optional
    /// type to have a different (possibly unchecked) optional type.
    Expr *coerceOptionalToOptional(Expr *expr, Type toType,
                                   ConstraintLocatorBuilder locator);

    /// \brief Coerce an expression of implicitly unwrapped optional type to its
    /// underlying value type, in the correct way for an implicit
    /// look-through.
    Expr *coerceImplicitlyUnwrappedOptionalToValue(Expr *expr, Type objTy,
                                         ConstraintLocatorBuilder locator);

  public:
    /// \brief Build a reference to the given declaration.
    Expr *buildDeclRef(ValueDecl *decl, SourceLoc loc, Type openedType,
                       ConstraintLocatorBuilder locator,
                       bool specialized, bool implicit,
                       bool isDirectPropertyAccess) {
      // Determine the declaration selected for this overloaded reference.
      auto &ctx = cs.getASTContext();

      // If this is a member of a nominal type, build a reference to the
      // member with an implied base type.
      if (decl->getDeclContext()->isTypeContext() && isa<FuncDecl>(decl)) {
        assert(isa<FuncDecl>(decl) && "Can only refer to functions here");
        assert(cast<FuncDecl>(decl)->isOperator() && "Must be an operator");
        auto openedFnType = openedType->castTo<FunctionType>();
        auto baseTy = simplifyType(openedFnType->getInput())
                        ->getRValueInstanceType();
        Expr *base = TypeExpr::createImplicitHack(loc, baseTy, ctx);
        return buildMemberRef(base, openedType, SourceLoc(), decl,
                              loc, openedFnType->getResult(),
                              locator, implicit, isDirectPropertyAccess);
      }

      // If this is a declaration with generic function type, build a
      // specialized reference to it.
      if (auto genericFn
            = decl->getInterfaceType()->getAs<GenericFunctionType>()) {
        auto dc = decl->getPotentialGenericDeclContext();

        SmallVector<Substitution, 4> substitutions;
        auto type = solution.computeSubstitutions(genericFn, dc, openedType,
                                                  substitutions);
        return new (ctx) DeclRefExpr(ConcreteDeclRef(ctx, decl, substitutions),
                                     loc, implicit, isDirectPropertyAccess,
                                     type);
      }

      auto type = simplifyType(openedType);
      return new (ctx) DeclRefExpr(decl, loc, implicit, isDirectPropertyAccess,
                                   type);
    }

    /// Describes an opened existential that has not yet been closed.
    struct OpenedExistential {
      /// The existential value being opened.
      Expr *ExistentialValue;

      /// The opaque value (of archetype type) stored within the
      /// existential.
      OpaqueValueExpr *OpaqueValue;
    };

    /// A mapping from archetype types that resulted from opening an
    /// existential to the opened existential. This mapping captures
    /// only those existentials that have been opened, but for which
    /// we have not yet created an \c OpenExistentialExpr.
    llvm::SmallDenseMap<ArchetypeType *, OpenedExistential> OpenedExistentials;

    /// Open an existential value into a new, opaque value of
    /// archetype type.
    ///
    /// \param base An expression of existential type whose value will
    /// be opened.
    ///
    /// \returns A pair (expr, type) that provides a reference to the value
    /// stored within the expression or its metatype (if the base was a
    /// metatype) and the new archetype that describes the dynamic type stored
    /// within the existential.
    std::tuple<Expr *, ArchetypeType *>
    openExistentialReference(Expr *base) {
      auto &tc = cs.getTypeChecker();
      base = tc.coerceToRValue(base);

      auto baseTy = base->getType()->getRValueType();
      bool isMetatype = false;
      if (auto metaTy = baseTy->getAs<AnyMetatypeType>()) {
        isMetatype = true;
        baseTy = metaTy->getInstanceType();
      }
      assert(baseTy->isAnyExistentialType() && "Type must be existential");

      // Create the archetype.
      SmallVector<ProtocolDecl *, 4> protocols;
      auto &ctx = tc.Context;
      baseTy->getAnyExistentialTypeProtocols(protocols);
      auto archetype = ArchetypeType::getOpened(baseTy);

      // Create the opaque opened value. If we started with a
      // metatype, it's a metatype.
      Type opaqueType = archetype;
      if (isMetatype)
        opaqueType = MetatypeType::get(archetype);
      auto archetypeVal = new (ctx) OpaqueValueExpr(base->getLoc(), opaqueType);
      archetypeVal->setUniquelyReferenced(true);

      // Record this opened existential.
      OpenedExistentials[archetype] = { base, archetypeVal };

      return std::make_tuple(archetypeVal, archetype);
    }

    /// Is the given function a constructor of a class or protocol?
    /// Such functions are subject to DynamicSelf manipulations.
    ///
    /// We want to avoid taking the DynamicSelf paths for other
    /// constructors for two reasons:
    ///   - it's an unnecessary cost
    ///   - optionality preservation has a problem with constructors on
    ///     optional types
    static bool isPolymorphicConstructor(AbstractFunctionDecl *fn) {
      if (!isa<ConstructorDecl>(fn))
        return false;
      DeclContext *parent = fn->getParent();
      if (auto extension = dyn_cast<ExtensionDecl>(parent))
        parent = extension->getExtendedType()->getAnyNominal();
      return (isa<ClassDecl>(parent) || isa<ProtocolDecl>(parent));
    }

    /// \brief Build a new member reference with the given base and member.
    Expr *buildMemberRef(Expr *base, Type openedFullType, SourceLoc dotLoc,
                         ValueDecl *member, SourceLoc memberLoc,
                         Type openedType, ConstraintLocatorBuilder locator,
                         bool Implicit, bool IsDirectPropertyAccess) {
      auto &tc = cs.getTypeChecker();
      auto &context = tc.Context;

      bool isSuper = base->isSuperExpr();

      Type baseTy = base->getType()->getRValueType();

      // Explicit member accesses are permitted to implicitly look
      // through ImplicitlyUnwrappedOptional<T>.
      if (!Implicit) {
        if (auto objTy = cs.lookThroughImplicitlyUnwrappedOptionalType(baseTy)) {
          base = coerceImplicitlyUnwrappedOptionalToValue(base, objTy, locator);
          if (!base) return nullptr;
          baseTy = objTy;
        }
      }

      // Figure out the actual base type, and whether we have an instance of
      // that type or its metatype.
      bool baseIsInstance = true;
      if (auto baseMeta = baseTy->getAs<AnyMetatypeType>()) {
        baseIsInstance = false;
        baseTy = baseMeta->getInstanceType();
      }

      // Produce a reference to the member, the type of the container it
      // resides in, and the type produced by the reference itself.
      Type containerTy;
      ConcreteDeclRef memberRef;
      Type refTy;
      Type dynamicSelfFnType;
      if (openedFullType->hasTypeVariable()) {
        // We require substitutions. Figure out what they are.

        // Figure out the declaration context where we'll get the generic
        // parameters.
        auto dc = member->getPotentialGenericDeclContext();

        // Build a reference to the generic member.
        SmallVector<Substitution, 4> substitutions;
        refTy = solution.computeSubstitutions(member->getInterfaceType(),
                                              dc,
                                              openedFullType,
                                              substitutions);

        memberRef = ConcreteDeclRef(context, member, substitutions);

        if (auto openedFullFnType = openedFullType->getAs<FunctionType>()) {
          auto openedBaseType = openedFullFnType->getInput()
                                  ->getRValueInstanceType();
          containerTy = solution.simplifyType(tc, openedBaseType);
        }
      } else {
        // No substitutions required; the declaration reference is simple.
        containerTy = member->getDeclContext()->getDeclaredTypeOfContext();
        memberRef = member;
        refTy = tc.getUnopenedTypeOfReference(member, Type(), dc,
                                              /*wantInterfaceType=*/true);
      }

      // If this is a method whose result type is dynamic Self, or a
      // construction, replace the result type with the actual object type.
      if (auto func = dyn_cast<AbstractFunctionDecl>(member)) {
        if ((isa<FuncDecl>(func) && cast<FuncDecl>(func)->hasDynamicSelf()) ||
            isPolymorphicConstructor(func)) {
          // For a DynamicSelf method on an existential, open up the
          // existential.
          if (func->getExtensionType()->is<ProtocolType>() &&
              baseTy->isAnyExistentialType()) {
            std::tie(base, baseTy) = openExistentialReference(base);
            containerTy = baseTy;
            openedType = openedType->replaceCovariantResultType(
                           baseTy,
                           func->getNumParamPatterns()-1);

            // The member reference is a specialized declaration
            // reference that replaces the Self of the protocol with
            // the existential type; change it to refer to the opened
            // archetype type.
            // FIXME: We should do this before we create the
            // specialized declaration reference, but that requires
            // redundant hasDynamicSelf checking.
            auto oldSubstitutions = memberRef.getSubstitutions();
            SmallVector<Substitution, 4> newSubstitutions(
                                           oldSubstitutions.begin(),
                                           oldSubstitutions.end());
            auto &selfSubst = newSubstitutions.front();
            assert(selfSubst.getArchetype()->getSelfProtocol() &&
                   "Not the Self archetype for a protocol?");
            unsigned numConformances = selfSubst.getConformances().size();
            auto newConformances
              = context.Allocate<ProtocolConformance *>(numConformances);
            std::fill(newConformances.begin(), newConformances.end(), nullptr);
            selfSubst = {
              selfSubst.getArchetype(),
              baseTy,
              newConformances,
            };
            memberRef = ConcreteDeclRef(context, memberRef.getDecl(),
                                        newSubstitutions);
          }

          refTy = refTy->replaceCovariantResultType(containerTy,
                                                  func->getNumParamPatterns());
          dynamicSelfFnType = refTy->replaceCovariantResultType(
                                baseTy,
                                func->getNumParamPatterns());

          // If the type after replacing DynamicSelf with the provided base
          // type is no different, we don't need to perform a conversion here.
          if (refTy->isEqual(dynamicSelfFnType))
            dynamicSelfFnType = nullptr;
        }
      }

      // If we're referring to the member of a module, it's just a simple
      // reference.
      if (baseTy->is<ModuleType>()) {
        assert(!IsDirectPropertyAccess &&
               "Direct property access doesn't make sense for this");
        assert(!dynamicSelfFnType && "No reference type to convert to");
        Expr *ref = new (context) DeclRefExpr(memberRef, memberLoc, Implicit);
        ref->setType(refTy);
        return new (context) DotSyntaxBaseIgnoredExpr(base, dotLoc, ref);
      }

      // Otherwise, we're referring to a member of a type.

      // Is it an archetype or existential member?
      bool isArchetypeOrExistentialRef
            = isa<ProtocolDecl>(member->getDeclContext()) &&
              (baseTy->is<ArchetypeType>() || baseTy->isAnyExistentialType());

      // If we are referring to an optional member of a protocol.
      if (isArchetypeOrExistentialRef &&
          member->getAttrs().hasAttribute<OptionalAttr>()) {
        auto proto =tc.getProtocol(memberLoc, KnownProtocolKind::AnyObject);
        if (!proto)
          return nullptr;

        baseTy = proto->getDeclaredType();
      }

      // References to properties with accessors and storage usually go
      // through the accessors, but sometimes are direct.
      if (auto *VD = dyn_cast<VarDecl>(member))
        IsDirectPropertyAccess |= isImplicitDirectMemberReference(base, VD, dc);

      if (baseIsInstance) {
        // Convert the base to the appropriate container type, turning it
        // into an lvalue if required.
        Type selfTy;
        if (isArchetypeOrExistentialRef)
          selfTy = baseTy;
        else
          selfTy = containerTy;
        
        // If the base is already an lvalue with the right base type, we can
        // pass it as an inout qualified type.
        if (selfTy->isEqual(baseTy) && !selfTy->hasReferenceSemantics())
          if (base->getType()->is<LValueType>())
            selfTy = InOutType::get(selfTy);
        base = coerceObjectArgumentToType(
                 base,  selfTy, member, IsDirectPropertyAccess,
                 locator.withPathElement(ConstraintLocator::MemberRefBase));
      } else {
        // Convert the base to an rvalue of the appropriate metatype.
        base = coerceToType(base,
                            MetatypeType::get(isArchetypeOrExistentialRef
                                                ? baseTy
                                                : containerTy),
                            locator.withPathElement(
                              ConstraintLocator::MemberRefBase));
        if (!base)
          return nullptr;

        base = tc.coerceToRValue(base);
      }
      assert(base && "Unable to convert base?");

      // Handle archetype and existential references.
      if (isArchetypeOrExistentialRef) {
        assert(!IsDirectPropertyAccess &&
               "Direct property access doesn't make sense for this");
        assert(!dynamicSelfFnType && 
               "Archetype/existential DynamicSelf with extra conversion");

        Expr *ref;

        if (member->getAttrs().hasAttribute<OptionalAttr>()) {
          base = tc.coerceToRValue(base);
          if (!base) return nullptr;
          ref = new (context) DynamicMemberRefExpr(base, dotLoc, memberRef,
                                                   memberLoc);
        } else {
          assert(!dynamicSelfFnType && "Converted type doesn't make sense here");
          ref = new (context) MemberRefExpr(base, dotLoc, memberRef,
                                            memberLoc, Implicit,
                                            IsDirectPropertyAccess);
          cast<MemberRefExpr>(ref)->setIsSuper(isSuper);
        }
        
        ref->setImplicit(Implicit);
        ref->setType(simplifyType(openedType));
        
        return ref;
      }

      // For types and properties, build member references.
      if (isa<TypeDecl>(member) || isa<VarDecl>(member)) {
        assert(!dynamicSelfFnType && "Converted type doesn't make sense here");
        auto result
          = new (context) MemberRefExpr(base, dotLoc, memberRef,
                                        memberLoc, Implicit,
                                        IsDirectPropertyAccess);
        result->setIsSuper(isSuper);

        // Skip the synthesized 'self' input type of the opened type.
        result->setType(simplifyType(openedType));
        return result;
      }
      
      assert(!IsDirectPropertyAccess &&
             "Direct property access doesn't make sense for this");
      
      // Handle all other references.
      Expr *ref = new (context) DeclRefExpr(memberRef, memberLoc, Implicit);
      ref->setType(refTy);

      // If the reference needs to be converted, do so now.
      if (dynamicSelfFnType) {
        ref = new (context) CovariantFunctionConversionExpr(ref,
                                                            dynamicSelfFnType);
      }

      ApplyExpr *apply;
      if (isa<ConstructorDecl>(member)) {
        // FIXME: Provide type annotation.
        apply = new (context) ConstructorRefCallExpr(ref, base);
      } else if (!baseIsInstance && member->isInstanceMember()) {
        // Reference to an unbound instance method.
        return new (context) DotSyntaxBaseIgnoredExpr(base, dotLoc, ref);
      } else {
        assert((!baseIsInstance || member->isInstanceMember()) &&
               "can't call a static method on an instance");
        apply = new (context) DotSyntaxCallExpr(ref, dotLoc, base);
      }
      return finishApply(apply, openedType, nullptr);
    }
    
    /// \brief Build a new dynamic member reference with the given base and
    /// member.
    Expr *buildDynamicMemberRef(Expr *base, SourceLoc dotLoc, ValueDecl *member,
                                SourceLoc memberLoc, Type openedType,
                                ConstraintLocatorBuilder locator) {
      auto &context = cs.getASTContext();

      // If we're specializing a polymorphic function, compute the set of
      // substitutions and form the member reference.
      Optional<ConcreteDeclRef> memberRef(member);
      if (auto func = dyn_cast<FuncDecl>(member)) {
        auto resultTy = func->getType()->castTo<AnyFunctionType>()->getResult();
        (void)resultTy;
        assert(!resultTy->is<PolymorphicFunctionType>() &&
               "Polymorphic function type slipped through");
      }

      // The base must always be an rvalue.
      base = cs.getTypeChecker().coerceToRValue(base);
      if (!base) return nullptr;
      if (auto objTy = cs.lookThroughImplicitlyUnwrappedOptionalType(base->getType())) {
        base = coerceImplicitlyUnwrappedOptionalToValue(base, objTy, locator);
        if (!base) return nullptr;
      }

      auto result = new (context) DynamicMemberRefExpr(base, dotLoc, *memberRef,
                                                       memberLoc);
      result->setType(simplifyType(openedType));
      return result;
    }

    /// \brief Describes either a type or the name of a type to be resolved.
    typedef llvm::PointerUnion<Identifier, Type> TypeOrName;

    /// \brief Convert the given literal expression via a protocol pair.
    ///
    /// This routine handles the two-step literal conversion process used
    /// by integer, float, character, extended grapheme cluster, and string
    /// literals. The first step uses \c builtinProtocol while the second
    /// step uses \c protocol.
    ///
    /// \param literal The literal expression.
    ///
    /// \param type The literal type. This type conforms to \c protocol,
    /// and may also conform to \c builtinProtocol.
    ///
    /// \param openedType The literal type as it was opened in the type system.
    ///
    /// \param protocol The protocol that describes the literal requirement.
    ///
    /// \param literalType Either the name of the associated type in
    /// \c protocol that describes the argument type of the conversion function
    /// (\c literalFuncName) or the argument type itself.
    ///
    /// \param literalFuncName The name of the conversion function requirement
    /// in \c protocol.
    ///
    /// \param builtinProtocol The "builtin" form of the protocol, which
    /// always takes builtin types and can only be properly implemented
    /// by standard library types. If \c type does not conform to this
    /// protocol, it's literal type will.
    ///
    /// \param builtinLiteralType Either the name of the associated type in
    /// \c builtinProtocol that describes the argument type of the builtin
    /// conversion function (\c builtinLiteralFuncName) or the argument type
    /// itself.
    ///
    /// \param builtinLiteralFuncName The name of the conversion function
    /// requirement in \c builtinProtocol.
    ///
    /// \param isBuiltinArgType Function that determines whether the given
    /// type is acceptable as the argument type for the builtin conversion.
    ///
    /// \param brokenProtocolDiag The diagnostic to emit if the protocol
    /// is broken.
    ///
    /// \param brokenBuiltinProtocolDiag The diagnostic to emit if the builtin
    /// protocol is broken.
    ///
    /// \returns the converted literal expression.
    Expr *convertLiteral(Expr *literal,
                         Type type,
                         Type openedType,
                         ProtocolDecl *protocol,
                         TypeOrName literalType,
                         Identifier literalFuncName,
                         ProtocolDecl *builtinProtocol,
                         TypeOrName builtinLiteralType,
                         Identifier builtinLiteralFuncName,
                         bool (*isBuiltinArgType)(Type),
                         Diag<> brokenProtocolDiag,
                         Diag<> brokenBuiltinProtocolDiag);

    /// \brief Finish a function application by performing the appropriate
    /// conversions on the function and argument expressions and setting
    /// the resulting type.
    ///
    /// \param apply The function application to finish type-checking, which
    /// may be a newly-built expression.
    ///
    /// \param openedType The "opened" type this expression had during
    /// type checking, which will be used to specialize the resulting,
    /// type-checked expression appropriately.
    ///
    /// \param locator The locator for the original expression.
    Expr *finishApply(ApplyExpr *apply, Type openedType,
                      ConstraintLocatorBuilder locator);

  private:
    /// \brief Retrieve the overload choice associated with the given
    /// locator.
    SelectedOverload getOverloadChoice(ConstraintLocator *locator) {
      return *getOverloadChoiceIfAvailable(locator);
    }

    /// \brief Retrieve the overload choice associated with the given
    /// locator.
    Optional<SelectedOverload>
    getOverloadChoiceIfAvailable(ConstraintLocator *locator) {
      auto known = solution.overloadChoices.find(locator);
      if (known != solution.overloadChoices.end())
        return known->second;

      return Nothing;
    }

    /// \brief Simplify the given type by substituting all occurrences of
    /// type variables for their fixed types.
    Type simplifyType(Type type) {
      return solution.simplifyType(cs.getTypeChecker(), type);
    }

  public:
    /// \brief Coerce the given expression to the given type.
    ///
    /// This operation cannot fail.
    ///
    /// \param expr The expression to coerce.
    /// \param toType The type to coerce the expression to.
    /// \param locator Locator used to describe where in this expression we are.
    ///
    /// \returns the coerced expression, which will have type \c ToType.
    Expr *coerceToType(Expr *expr, Type toType,
                       ConstraintLocatorBuilder locator);

    /// \brief Coerce the given expression (which is the argument to a call) to
    /// the given parameter type.
    ///
    /// This operation cannot fail.
    ///
    /// \param arg The argument expression.
    /// \param paramType The parameter type.
    /// \param locator Locator used to describe where in this expression we are.
    ///
    /// \returns the coerced expression, which will have type \c ToType.
    Expr *coerceCallArguments(Expr *arg, Type paramType,
                              ConstraintLocatorBuilder locator);

    /// \brief Coerce the given object argument (e.g., for the base of a
    /// member expression) to the given type.
    ///
    /// \param expr The expression to coerce.
    ///
    /// \param baseTy The base type
    ///
    /// \param member The member being accessed.
    ///
    /// \param IsDirectPropertyAccess True if this is a direct access to
    ///        computed properties that have storage.
    ///
    /// \param locator Locator used to describe where in this expression we are.
    Expr *coerceObjectArgumentToType(Expr *expr,
                                     Type baseTy, ValueDecl *member,
                                     bool IsDirectPropertyAccess,
                                     ConstraintLocatorBuilder locator);

  private:
    /// \brief Build a new subscript.
    ///
    /// \param base The base of the subscript.
    /// \param index The index of the subscript.
    /// \param locator The locator used to refer to the subscript.
    Expr *buildSubscript(Expr *base, Expr *index,
                         ConstraintLocatorBuilder locator) {
      // Determine the declaration selected for this subscript operation.
      auto selected = getOverloadChoice(
                        cs.getConstraintLocator(
                          locator.withPathElement(
                            ConstraintLocator::SubscriptMember)));
      auto choice = selected.choice;
      auto subscript = cast<SubscriptDecl>(choice.getDecl());

      auto &tc = cs.getTypeChecker();
      auto baseTy = base->getType()->getRValueType();

      // Check whether the base is 'super'.
      bool isSuper = base->isSuperExpr();

      // Handle accesses that implicitly look through ImplicitlyUnwrappedOptional<T>.
      if (auto objTy = cs.lookThroughImplicitlyUnwrappedOptionalType(baseTy)) {
        base = coerceImplicitlyUnwrappedOptionalToValue(base, objTy, locator);
        if (!base) return nullptr;
      }

      // Figure out the index and result types.
      auto containerTy
        = subscript->getDeclContext()->getDeclaredTypeOfContext();
      auto subscriptTy = simplifyType(selected.openedType);
      auto indexTy = subscriptTy->castTo<AnyFunctionType>()->getInput();
      auto resultTy = subscriptTy->castTo<AnyFunctionType>()->getResult();

      // Coerce the index argument.
      index = coerceCallArguments(index, indexTy,
                                  locator.withPathElement(
                                    ConstraintLocator::SubscriptIndex));
      if (!index)
        return nullptr;

      // Form the subscript expression.

      // Handle dynamic lookup.
      if (selected.choice.getKind() == OverloadChoiceKind::DeclViaDynamic ||
          subscript->getAttrs().hasAttribute<OptionalAttr>()) {
        // If we've found an optional method in a protocol, the base type is
        // AnyObject.
        if (selected.choice.getKind() != OverloadChoiceKind::DeclViaDynamic) {
          auto proto = tc.getProtocol(index->getStartLoc(),
                                      KnownProtocolKind::AnyObject);
          if (!proto)
            return nullptr;

          baseTy = proto->getDeclaredType();
        }

        base = coerceObjectArgumentToType(base, baseTy, subscript, false,
                                          locator);
        if (!base)
          return nullptr;

        auto subscriptExpr = new (tc.Context) DynamicSubscriptExpr(base,
                                                                   index,
                                                                   subscript);
        subscriptExpr->setType(resultTy);
        return subscriptExpr;
      }

      // Handle subscripting of generics.
      if (subscript->getDeclContext()->isGenericContext()) {
        auto dc = subscript->getDeclContext();

        // Compute the substitutions used to reference the subscript.
        SmallVector<Substitution, 4> substitutions;
        solution.computeSubstitutions(subscript->getInterfaceType(),
                                      dc,
                                      selected.openedFullType,
                                      substitutions);

        // Convert the base.
        auto openedFullFnType = selected.openedFullType->castTo<FunctionType>();
        auto openedBaseType = openedFullFnType->getInput();
        containerTy = solution.simplifyType(tc, openedBaseType);
        base = coerceObjectArgumentToType(base, containerTy, subscript, false,
                                          locator);
                 locator.withPathElement(ConstraintLocator::MemberRefBase);
        if (!base)
          return nullptr;

        // Form the generic subscript expression.
        auto subscriptExpr
          = new (tc.Context) SubscriptExpr(base, index,
                                           ConcreteDeclRef(tc.Context,
                                                           subscript,
                                                           substitutions));
        subscriptExpr->setType(resultTy);
        subscriptExpr->setIsSuper(isSuper);
        return subscriptExpr;
      }

      Type selfTy = containerTy;
      if (selfTy->isEqual(baseTy) && !selfTy->hasReferenceSemantics())
        if (base->getType()->is<LValueType>())
          selfTy = InOutType::get(selfTy);

      // Coerce the base to the container type.
      base = coerceObjectArgumentToType(base, selfTy, subscript, false,
                                        locator);
      if (!base)
        return nullptr;

      // Form a normal subscript.
      auto *subscriptExpr
        = new (tc.Context) SubscriptExpr(base, index, subscript);
      subscriptExpr->setType(resultTy);
      subscriptExpr->setIsSuper(isSuper);
      return subscriptExpr;
    }

    /// \brief Build a new reference to another constructor.
    Expr *buildOtherConstructorRef(Type openedFullType,
                                   ConstructorDecl *ctor, SourceLoc loc,
                                   bool implicit) {
      auto &tc = cs.getTypeChecker();
      auto &ctx = tc.Context;

      // Compute the concrete reference.
      ConcreteDeclRef ref;
      Type resultTy;
      if (ctor->getInterfaceType()->is<GenericFunctionType>()) {
        // Compute the reference to the generic constructor.
        SmallVector<Substitution, 4> substitutions;
        resultTy = solution.computeSubstitutions(
                     ctor->getInterfaceType(),
                     ctor,
                     openedFullType,
                     substitutions);

        ref = ConcreteDeclRef(ctx, ctor, substitutions);

        // The constructor was opened with the allocating type, not the
        // initializer type. Map the former into the latter.
        auto resultFnTy = resultTy->castTo<FunctionType>();
        auto selfTy = resultFnTy->getInput()->getRValueInstanceType();
        if (!selfTy->hasReferenceSemantics())
          selfTy = InOutType::get(selfTy);

        resultTy = FunctionType::get(selfTy, resultFnTy->getResult(),
                                     resultFnTy->getExtInfo());
      } else {
        ref = ConcreteDeclRef(ctor);
        resultTy = ctor->getInitializerType();
      }

      // Build the constructor reference.
      Expr *refExpr = new (ctx) OtherConstructorDeclRefExpr(ref, loc, implicit,
                                                            resultTy);
      return refExpr;
    }

    /// Bridge the given value to its corresponding Objective-C object
    /// type.
    ///
    /// This routine should only be used for bridging value types.
    ///
    /// \param value The value to be bridged.
    Expr *bridgeToObjectiveC(Expr *value) {
      auto &tc = cs.getTypeChecker();

      // Find the _BridgedToObjectiveC protocol.
      auto bridgedProto
        = tc.Context.getProtocol(KnownProtocolKind::_BridgedToObjectiveCType);

      // Find the conformance of the value type to _BridgedToObjectiveC.
      Type valueType = value->getType()->getRValueType();
      ProtocolConformance *conformance = nullptr;
      bool conforms = tc.conformsToProtocol(valueType, bridgedProto, cs.DC,
                                            &conformance);
      assert(conforms && "Should already have checked the conformance");
      (void)conforms;

      // Form the call.
      return tc.callWitness(value, cs.DC, bridgedProto,
                            conformance,
                            tc.Context.Id_bridgeToObjectiveC,
                            { }, diag::broken_bridged_to_objc_protocol);
    }

    /// Bridge the given object from Objective-C to its value type.
    ///
    /// This routine should only be used for bridging value types.
    ///
    /// \param object The object, whose type should already be of the type
    /// that the value type bridges through.
    ///
    /// \param valueType The value type to which we are bridging.
    ///
    /// \returns a value of type \c valueType that stores the bridged result.
    Expr *bridgeFromObjectiveC(Expr *object, Type valueType) {
      auto &tc = cs.getTypeChecker();

      // Find the _BridgedToObjectiveC protocol.
      auto bridgedProto
        = tc.Context.getProtocol(KnownProtocolKind::_BridgedToObjectiveCType);

      // Find the conformance of the value type to _BridgedToObjectiveC.
      ProtocolConformance *conformance = nullptr;
      bool conforms = tc.conformsToProtocol(valueType, bridgedProto, cs.DC,
                                            &conformance);
      assert(conforms && "Should already have checked the conformance");
      (void)conforms;

      // Form the call.
      return tc.callWitness(TypeExpr::createImplicit(valueType, tc.Context), 
                            cs.DC, bridgedProto, conformance,
                            tc.Context.Id_bridgeFromObjectiveC,
                            { object }, 
                            diag::broken_bridged_to_objc_protocol);
    }

    /// Conditionally bridge the given object from Objective-C to its
    /// value type.
    ///
    /// This routine should only be used for bridging value types.
    ///
    /// \param object The object, whose type should already be of the type
    /// that the value type bridges through.
    ///
    /// \param valueType The value type to which we are bridging.
    ///
    /// \returns a value of type \c valueType? that stores the bridged result.
    Expr *bridgeFromObjectiveCConditional(Expr *object, Type valueType) {
      auto &tc = cs.getTypeChecker();

      // Find the _ConditionallyBridgedToObjectiveC protocol.
      auto conditionalBridgedProto
        = tc.Context.getProtocol(
            KnownProtocolKind::_ConditionallyBridgedToObjectiveCType);

      // Check whether the value type conforms to
      // _ConditionallyBridgedToObjectiveC. If so, we have a specific
      // entry point for conditional bridging.
      ProtocolConformance *conditionalConformance = nullptr;
      if (tc.conformsToProtocol(valueType, conditionalBridgedProto, cs.DC,
                                &conditionalConformance)) {
        Expr *valueMetatype = TypeExpr::createImplicit(valueType, tc.Context);
        Expr *args[1] = { object };
        return tc.callWitness(valueMetatype, cs.DC, conditionalBridgedProto,
                              conditionalConformance,
                              tc.Context.Id_bridgeFromObjectiveCConditional,
                              args, diag::broken_bridged_to_objc_protocol);
      }

      Expr *result = bridgeFromObjectiveC(object, valueType);
      if (!result)
        return nullptr;

      return new (tc.Context) InjectIntoOptionalExpr(
                                result, 
                                OptionalType::get(result->getType()));
    }

    TypeAliasDecl *MaxIntegerTypeDecl = nullptr;
    TypeAliasDecl *MaxFloatTypeDecl = nullptr;
    
  public:
    ExprRewriter(ConstraintSystem &cs, const Solution &solution)
      : cs(cs), dc(cs.DC), solution(solution) { }

    ~ExprRewriter() {
      finalize();
    }

    ConstraintSystem &getConstraintSystem() const { return cs; }

    /// \brief Simplify the expression type and return the expression.
    ///
    /// This routine is used for 'simple' expressions that only need their
    /// types simplified, with no further computation.
    Expr *simplifyExprType(Expr *expr) {
      auto toType = simplifyType(expr->getType());
      expr->setType(toType);
      return expr;
    }

    Expr *visitErrorExpr(ErrorExpr *expr) {
      // Do nothing with error expressions.
      return expr;
    }

    Expr *handleIntegerLiteralExpr(LiteralExpr *expr) {
      auto &tc = cs.getTypeChecker();
      ProtocolDecl *protocol
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::IntegerLiteralConvertible);
      ProtocolDecl *builtinProtocol
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::_BuiltinIntegerLiteralConvertible);

      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      auto type = simplifyType(expr->getType());
      if (auto defaultType = tc.getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }
      if (auto floatProtocol
            = tc.getProtocol(expr->getLoc(),
                             KnownProtocolKind::FloatLiteralConvertible)) {
        if (auto defaultFloatType = tc.getDefaultType(floatProtocol, dc)) {
          if (defaultFloatType->isEqual(type))
            type = defaultFloatType;
        }
      }

      // Find the maximum-sized builtin integer type.
      
      if(!MaxIntegerTypeDecl) {
        UnqualifiedLookup lookup(tc.Context.Id_MaxBuiltinIntegerType,
                                 tc.getStdlibModule(dc),
                                 &tc);
        MaxIntegerTypeDecl =
            dyn_cast_or_null<TypeAliasDecl>(lookup.getSingleTypeResult());
      }
      if (!MaxIntegerTypeDecl ||
          !MaxIntegerTypeDecl->getUnderlyingType()->is<BuiltinIntegerType>()) {
        tc.diagnose(expr->getLoc(), diag::no_MaxBuiltinIntegerType_found);
        return nullptr;
      }
      auto maxType = MaxIntegerTypeDecl->getUnderlyingType();

      return convertLiteral(
               expr,
               type,
               expr->getType(),
               protocol,
               tc.Context.Id_IntegerLiteralType,
               tc.Context.Id_ConvertFromIntegerLiteral,
               builtinProtocol,
               maxType,
               tc.Context.Id_ConvertFromBuiltinIntegerLiteral,
               nullptr,
               diag::integer_literal_broken_proto,
               diag::builtin_integer_literal_broken_proto);
    }
    
    Expr *visitNilLiteralExpr(NilLiteralExpr *expr) {
      auto &tc = cs.getTypeChecker();
      auto *protocol = tc.getProtocol(expr->getLoc(),
                                      KnownProtocolKind::NilLiteralConvertible);
      
      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      auto type = simplifyType(expr->getType());
      if (auto defaultType = tc.getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }
      
      return convertLiteral(expr, type, expr->getType(), protocol,
                            Identifier(), tc.Context.Id_ConvertFromNilLiteral,
                            nullptr, Identifier(),
                            Identifier(),
                            [] (Type type) -> bool {
                              return false;
                            },
                            diag::nil_literal_broken_proto,
                            diag::nil_literal_broken_proto);
    }

    
    Expr *visitIntegerLiteralExpr(IntegerLiteralExpr *expr) {
      return handleIntegerLiteralExpr(expr);
    }

    Expr *visitFloatLiteralExpr(FloatLiteralExpr *expr) {
      auto &tc = cs.getTypeChecker();
      ProtocolDecl *protocol
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::FloatLiteralConvertible);
      ProtocolDecl *builtinProtocol
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::_BuiltinFloatLiteralConvertible);

      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      auto type = simplifyType(expr->getType());
      if (auto defaultType = tc.getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }

      // Find the maximum-sized builtin float type.
      // FIXME: Cache name lookup.
      if (!MaxFloatTypeDecl) {
        UnqualifiedLookup lookup(tc.Context.Id_MaxBuiltinFloatType,
                                 tc.getStdlibModule(dc),
                                 &tc);
        MaxFloatTypeDecl =
          dyn_cast_or_null<TypeAliasDecl>(lookup.getSingleTypeResult());
      }
      if (!MaxFloatTypeDecl ||
          !MaxFloatTypeDecl->getUnderlyingType()->is<BuiltinFloatType>()) {
        tc.diagnose(expr->getLoc(), diag::no_MaxBuiltinFloatType_found);
        return nullptr;
      }
      auto maxType = MaxFloatTypeDecl->getUnderlyingType();

      return convertLiteral(
               expr,
               type,
               expr->getType(),
               protocol,
               tc.Context.Id_FloatLiteralType,
               tc.Context.Id_ConvertFromFloatLiteral,
               builtinProtocol,
               maxType,
               tc.Context.Id_ConvertFromBuiltinFloatLiteral,
               nullptr,
               diag::float_literal_broken_proto,
               diag::builtin_float_literal_broken_proto);
    }

    Expr *visitBooleanLiteralExpr(BooleanLiteralExpr *expr) {
      auto &tc = cs.getTypeChecker();
      ProtocolDecl *protocol
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::BooleanLiteralConvertible);
      ProtocolDecl *builtinProtocol
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::_BuiltinBooleanLiteralConvertible);
      if (!protocol || !builtinProtocol)
        return nullptr;

      auto type = simplifyType(expr->getType());
      return convertLiteral(
               expr,
               type,
               expr->getType(),
               protocol,
               tc.Context.Id_BooleanLiteralType,
               tc.Context.Id_ConvertFromBooleanLiteral,
               builtinProtocol,
               Type(BuiltinIntegerType::get(BuiltinIntegerWidth::fixed(1), 
                                            tc.Context)),
               tc.Context.Id_ConvertFromBuiltinBooleanLiteral,
               nullptr,
               diag::boolean_literal_broken_proto,
               diag::builtin_boolean_literal_broken_proto);
    }

    Expr *visitCharacterLiteralExpr(CharacterLiteralExpr *expr) {
      auto &tc = cs.getTypeChecker();
      ProtocolDecl *protocol
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::CharacterLiteralConvertible);
      ProtocolDecl *builtinProtocol
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::_BuiltinCharacterLiteralConvertible);

      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      auto type = simplifyType(expr->getType());
      if (auto defaultType = tc.getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }

      return convertLiteral(
               expr,
               type,
               expr->getType(),
               protocol,
               tc.Context.Id_CharacterLiteralType,
               tc.Context.Id_ConvertFromCharacterLiteral,
               builtinProtocol,
               Type(BuiltinIntegerType::get(32, tc.Context)),
               tc.Context.Id_ConvertFromBuiltinCharacterLiteral,
               [] (Type type) -> bool {
                 if (auto builtinInt = type->getAs<BuiltinIntegerType>()) {
                   return builtinInt->isFixedWidth(32);
                 }
                 return false;
               },
               diag::character_literal_broken_proto,
               diag::builtin_character_literal_broken_proto);
    }

    Expr *handleStringLiteralExpr(LiteralExpr *expr) {
      auto stringLiteral = dyn_cast<StringLiteralExpr>(expr);
      auto magicLiteral = dyn_cast<MagicIdentifierLiteralExpr>(expr);
      assert(bool(stringLiteral) != bool(magicLiteral) &&
             "literal must be either a string literal or a magic literal");

      auto type = simplifyType(expr->getType());
      auto &tc = cs.getTypeChecker();

      bool isStringLiteral = true;
      ProtocolDecl *protocol = tc.getProtocol(
          expr->getLoc(), KnownProtocolKind::StringLiteralConvertible);

      if (!tc.conformsToProtocol(type, protocol, cs.DC)) {
        // If the type does not conform to StringLiteralConvertible, it should
        // be ExtendedGraphemeClusterLiteralConvertible.
        protocol = tc.getProtocol(
            expr->getLoc(),
            KnownProtocolKind::ExtendedGraphemeClusterLiteralConvertible);
        isStringLiteral = false;
      }

      assert(tc.conformsToProtocol(type, protocol, cs.DC));

      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      if (auto defaultType = tc.getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }

      // Add the first element (
      SmallVector<TupleTypeElt, 3> elements;
      elements.push_back(TupleTypeElt(tc.Context.TheRawPointerType));

/*
        TupleTypeElt(BuiltinIntegerType::getWordType(tc.Context)),
        TupleTypeElt(BuiltinIntegerType::get(1, tc.Context))
*/

      ProtocolDecl *builtinProtocol;
      Identifier literalType;
      Identifier literalFuncName;
      Identifier builtinLiteralFuncName;
      Diag<> brokenProtocolDiag;
      Diag<> brokenBuiltinProtocolDiag;

      if (isStringLiteral) {
        literalType = tc.Context.Id_StringLiteralType;
        literalFuncName = tc.Context.Id_ConvertFromStringLiteral;

        // If the type can handle UTF-16 string literals, prefer them.
        builtinProtocol = tc.getProtocol(
            expr->getLoc(),
            KnownProtocolKind::_BuiltinUTF16StringLiteralConvertible);
        if (tc.conformsToProtocol(type, builtinProtocol, cs.DC)) {
          builtinLiteralFuncName =
              tc.Context.Id_ConvertFromBuiltinUTF16StringLiteral;
          elements.push_back(
            TupleTypeElt(BuiltinIntegerType::getWordType(tc.Context),
                         tc.Context.getIdentifier("numberOfCodeUnits")));
          if (stringLiteral)
            stringLiteral->setEncoding(StringLiteralExpr::UTF16);
          else
            magicLiteral->setStringEncoding(StringLiteralExpr::UTF16);
        } else {
          // Otherwise, fall back to UTF-8.
          builtinProtocol = tc.getProtocol(
              expr->getLoc(),
              KnownProtocolKind::_BuiltinStringLiteralConvertible);
          builtinLiteralFuncName =
              tc.Context.Id_ConvertFromBuiltinStringLiteral;
          elements.push_back(
            TupleTypeElt(BuiltinIntegerType::getWordType(tc.Context),
                         tc.Context.getIdentifier("byteSize")));
          elements.push_back(
            TupleTypeElt(BuiltinIntegerType::get(1, tc.Context),
                         tc.Context.getIdentifier("isASCII")));
          if (stringLiteral)
            stringLiteral->setEncoding(StringLiteralExpr::UTF8);
          else
            magicLiteral->setStringEncoding(StringLiteralExpr::UTF8);
        }
        brokenProtocolDiag = diag::string_literal_broken_proto;
        brokenBuiltinProtocolDiag = diag::builtin_string_literal_broken_proto;
      } else {
        literalType = tc.Context.Id_ExtendedGraphemeClusterLiteralType;
        literalFuncName =
            tc.Context.Id_ConvertFromExtendedGraphemeClusterLiteral;
        builtinLiteralFuncName =
            tc.Context.Id_ConvertFromBuiltinExtendedGraphemeClusterLiteral;
        builtinProtocol = tc.getProtocol(
            expr->getLoc(),
            KnownProtocolKind::_BuiltinExtendedGraphemeClusterLiteralConvertible);
        elements.push_back(
          TupleTypeElt(BuiltinIntegerType::getWordType(tc.Context),
                       tc.Context.getIdentifier("byteSize")));
        elements.push_back(
          TupleTypeElt(BuiltinIntegerType::get(1, tc.Context),
                       tc.Context.getIdentifier("isASCII")));
        brokenProtocolDiag =
            diag::extended_grapheme_cluster_literal_broken_proto;
        brokenBuiltinProtocolDiag =
            diag::builtin_extended_grapheme_cluster_literal_broken_proto;
      }

      return convertLiteral(expr,
                            type,
                            expr->getType(),
                            protocol,
                            literalType,
                            literalFuncName,
                            builtinProtocol,
                            TupleType::get(elements, tc.Context),
                            builtinLiteralFuncName,
                            nullptr,
                            brokenProtocolDiag,
                            brokenBuiltinProtocolDiag);
    }
    
    Expr *visitStringLiteralExpr(StringLiteralExpr *expr) {
      return handleStringLiteralExpr(expr);
    }

    Expr *
    visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *expr) {
      // Figure out the string type we're converting to.
      auto openedType = expr->getType();
      auto type = simplifyType(openedType);
      expr->setType(type);

      // Find the string interpolation protocol we need.
      auto &tc = cs.getTypeChecker();
      auto &C = tc.Context;
      auto interpolationProto
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::StringInterpolationConvertible);
      assert(interpolationProto && "Missing string interpolation protocol?");

      // FIXME: Cache name,
      auto member =
          findNamedWitness(tc, dc, type, interpolationProto,
                           C.Id_ConvertFromStringInterpolation,
                           diag::interpolation_broken_proto);
      auto segmentMember =
         findNamedWitness(tc, dc, type, interpolationProto,
                          C.Id_ConvertFromStringInterpolationSegment,
                          diag::interpolation_broken_proto);
      if (!member || !segmentMember)
        return nullptr;

      // Build a reference to the convertFromStringInterpolation member.
      // FIXME: This location info is bogus.
      auto typeRef = TypeExpr::createImplicitHack(expr->getStartLoc(),
                                                  type, tc.Context);
      Expr *memberRef = new (tc.Context) MemberRefExpr(typeRef,
                                                       expr->getStartLoc(),
                                                       member,
                                                       expr->getStartLoc(),
                                                       /*Implicit=*/true);
      bool failed = tc.typeCheckExpressionShallow(memberRef, cs.DC);
      assert(!failed && "Could not reference string interpolation witness");
      (void)failed;

      // Create a tuple containing all of the segments.
      SmallVector<Expr *, 4> segments;

      unsigned index = 0;
      ConstraintLocatorBuilder locatorBuilder(cs.getConstraintLocator(expr));
      for (auto segment : expr->getSegments()) {
        auto locator = cs.getConstraintLocator(
                      locatorBuilder.withPathElement(
                          LocatorPathElt::getInterpolationArgument(index++)));

        // Find the conversion method we chose.
        auto choice = getOverloadChoice(locator);

        auto memberRef = buildMemberRef(
            typeRef, choice.openedFullType,
            segment->getStartLoc(), choice.choice.getDecl(),
            segment->getStartLoc(), choice.openedType,
            locatorBuilder, /*Implicit=*/true, /*directPropertyAccess=*/false);
        ApplyExpr *apply =
            new (tc.Context) CallExpr(memberRef, segment, /*Implicit=*/true);
        segment = finishApply(apply, openedType, locatorBuilder);
        segments.push_back(segment);
      }

      Expr *argument = nullptr;
      if (segments.size() == 1)
        argument = segments.front();
      else {
        SmallVector<TupleTypeElt, 4> tupleElements(segments.size(),
                                                   TupleTypeElt(type));
        argument = TupleExpr::create(tc.Context,
                                     expr->getStartLoc(),
                                     segments,
                                     { }, 
                                     { },
                                     expr->getStartLoc(),
                                     /*hasTrailingClosure=*/false,
                                     /*Implicit=*/true,
                                     TupleType::get(tupleElements,tc.Context));
      }

      // Call the convertFromStringInterpolation member with the arguments.
      ApplyExpr *apply = new (tc.Context) CallExpr(memberRef, argument,
                                                   /*Implicit=*/true);
      expr->setSemanticExpr(finishApply(apply, openedType, locatorBuilder));
      return expr;
    }
    
    Expr *visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *expr) {
      switch (expr->getKind()) {
      case MagicIdentifierLiteralExpr::File:
      case MagicIdentifierLiteralExpr::Function:
        return handleStringLiteralExpr(expr);

      case MagicIdentifierLiteralExpr::Line:
      case MagicIdentifierLiteralExpr::Column:
        return handleIntegerLiteralExpr(expr);
      }
    }

    /// \brief Retrieve the type of a reference to the given declaration.
    Type getTypeOfDeclReference(ValueDecl *decl, bool isSpecialized) {
      if (auto typeDecl = dyn_cast<TypeDecl>(decl)) {
        // Resolve the reference to this type declaration in our
        // current context.
        auto type = cs.getTypeChecker().resolveTypeInContext(typeDecl, dc,
                                                             isSpecialized);
        if (!type)
          return nullptr;

        // Refer to the metatype of this type.
        return MetatypeType::get(type);
      }

      return cs.TC.getUnopenedTypeOfReference(decl, Type(), dc,
                                              /*wantInterfaceType=*/true);
    }

    Expr *visitDeclRefExpr(DeclRefExpr *expr) {
      auto locator = cs.getConstraintLocator(expr);

      // Find the overload choice used for this declaration reference.
      auto selected = getOverloadChoice(locator);
      auto choice = selected.choice;
      auto decl = choice.getDecl();

      // FIXME: Cannibalize the existing DeclRefExpr rather than allocating a
      // new one?
      return buildDeclRef(decl, expr->getLoc(), selected.openedFullType,
                          locator, expr->isSpecialized(), expr->isImplicit(),
                          expr->isDirectPropertyAccess());
    }

    Expr *visitSuperRefExpr(SuperRefExpr *expr) {
      simplifyExprType(expr);
      return expr;
    }

    Expr *visitTypeExpr(TypeExpr *expr) {
      auto toType = simplifyType(expr->getTypeLoc().getType());
      expr->getTypeLoc().setType(toType, /*validated=*/true);
      expr->setType(MetatypeType::get(toType));
      return expr;
    }

    Expr *visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *expr) {
      expr->setType(expr->getDecl()->getInitializerType());
      return expr;
    }

    Expr *visitUnresolvedConstructorExpr(UnresolvedConstructorExpr *expr) {
      // Resolve the callee to the constructor declaration selected.
      auto selected = getOverloadChoice(
                        cs.getConstraintLocator(
                          expr,
                          ConstraintLocator::ConstructorMember));
      auto choice = selected.choice;
      auto *ctor = cast<ConstructorDecl>(choice.getDecl());

      auto arg = expr->getSubExpr()->getSemanticsProvidingExpr();
      auto &tc = cs.getTypeChecker();

      // If the subexpression is a metatype, build a direct reference to the
      // constructor.
      if (arg->getType()->is<AnyMetatypeType>()) {
        return buildMemberRef(expr->getSubExpr(),
                              selected.openedFullType,
                              expr->getDotLoc(),
                              ctor,
                              expr->getConstructorLoc(),
                              expr->getType(),
                              ConstraintLocatorBuilder(
                                cs.getConstraintLocator(expr)),
                              expr->isImplicit(),
                              /*IsDirectPropertyAccess=*/false);
      }

      // The subexpression must be either 'self' or 'super'.
      if (!arg->isSuperExpr()) {
        // 'super' references have already been fully checked; handle the
        // 'self' case below.
        bool diagnoseBadInitRef = true;
        if (auto dre = dyn_cast<DeclRefExpr>(arg)) {
          if (dre->getDecl()->getName() == cs.getASTContext().Id_self) {
            // We have a reference to 'self'.
            diagnoseBadInitRef = false;

            // Make sure the reference to 'self' occurs within an initializer.
            if (!dyn_cast_or_null<ConstructorDecl>(
                   cs.DC->getInnermostMethodContext())) {
              tc.diagnose(expr->getDotLoc(),
                          diag::init_delegation_outside_initializer);
            }
          }
        }

        // If we need to diagnose this as a bad reference to an initializer,
        // do so now.
        if (diagnoseBadInitRef) {
          // Determine whether 'super' would have made sense as a base.
          bool hasSuper = false;
          if (auto func = cs.DC->getInnermostMethodContext()) {
            if (auto nominalType
                       = func->getDeclContext()->getDeclaredTypeOfContext()) {
              if (auto classDecl = nominalType->getClassOrBoundGenericClass()) {
                hasSuper = classDecl->hasSuperclass();
              }
            }
          }

          tc.diagnose(expr->getDotLoc(), diag::bad_init_ref_base, hasSuper);
        }
      }

      // Build a partial application of the initializer.
      Expr *ctorRef = buildOtherConstructorRef(selected.openedFullType,
                                               ctor, expr->getConstructorLoc(),
                                               expr->isImplicit());
      auto *call
        = new (cs.getASTContext()) DotSyntaxCallExpr(ctorRef,
                                                     expr->getDotLoc(),
                                                     expr->getSubExpr());
      return finishApply(call, expr->getType(),
                         ConstraintLocatorBuilder(
                           cs.getConstraintLocator(expr)));
    }

    Expr *visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *expr) {
      return simplifyExprType(expr);
    }

    Expr *visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *expr) {
      // Determine the declaration selected for this overloaded reference.
      auto locator = cs.getConstraintLocator(expr);
      auto selected = getOverloadChoice(locator);
      auto choice = selected.choice;
      auto decl = choice.getDecl();

      return buildDeclRef(decl, expr->getLoc(), selected.openedFullType,
                          locator, expr->isSpecialized(), expr->isImplicit(),
                          /*isDirectPropertyAccess*/false);
    }

    Expr *visitOverloadedMemberRefExpr(OverloadedMemberRefExpr *expr) {
      auto selected = getOverloadChoice(
                        cs.getConstraintLocator(expr,
                                                ConstraintLocator::Member));
      return buildMemberRef(expr->getBase(),
                            selected.openedFullType,
                            expr->getDotLoc(),
                            selected.choice.getDecl(), expr->getMemberLoc(),
                            selected.openedType,
                            cs.getConstraintLocator(expr),
                            expr->isImplicit(), /*direct ivar*/false);
    }

    Expr *visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *expr) {
      // FIXME: We should have generated an overload set from this, in which
      // case we can emit a typo-correction error here but recover well.
      return nullptr;
    }

    Expr *visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *expr) {
      // Our specializations should have resolved the subexpr to the right type.
      if (auto DRE = dyn_cast<DeclRefExpr>(expr->getSubExpr())) {
        assert(DRE->getGenericArgs().empty() ||
            DRE->getGenericArgs().size() == expr->getUnresolvedParams().size());
        if (DRE->getGenericArgs().empty()) {
          SmallVector<TypeRepr *, 8> GenArgs;
          for (auto TL : expr->getUnresolvedParams())
            GenArgs.push_back(TL.getTypeRepr());
          DRE->setGenericArgs(GenArgs);
        }
      }
      return expr->getSubExpr();
    }

    Expr *visitMemberRefExpr(MemberRefExpr *expr) {
      auto selected = getOverloadChoice(
                        cs.getConstraintLocator(expr,
                                                ConstraintLocator::Member));
      return buildMemberRef(expr->getBase(),
                            selected.openedFullType,
                            expr->getDotLoc(),
                            selected.choice.getDecl(), expr->getNameLoc(),
                            selected.openedType,
                            cs.getConstraintLocator(expr),
                            expr->isImplicit(),
                            expr->isDirectPropertyAccess());
    }

    Expr *visitDynamicMemberRefExpr(DynamicMemberRefExpr *expr) {
      auto selected = getOverloadChoice(
                        cs.getConstraintLocator(expr,
                                                ConstraintLocator::Member));

      return buildDynamicMemberRef(expr->getBase(), expr->getDotLoc(),
                                   selected.choice.getDecl(),
                                   expr->getNameLoc(),
                                   selected.openedType,
                                   cs.getConstraintLocator(expr));
    }

    Expr *visitUnresolvedMemberExpr(UnresolvedMemberExpr *expr) {
      // Dig out the type of the base, which will be the result
      // type of this expression.
      Type baseTy = simplifyType(expr->getType())->getRValueType();
      auto &tc = cs.getTypeChecker();

      // Find the selected member.
      auto selected = getOverloadChoice(
                        cs.getConstraintLocator(
                          expr, ConstraintLocator::UnresolvedMember));
      auto member = selected.choice.getDecl();
      
      // If the member came by optional unwrapping, then unwrap the base type.
      if (selected.choice.getKind()
                              == OverloadChoiceKind::DeclViaUnwrappedOptional) {
        baseTy = baseTy->getAnyOptionalObjectType();
        assert(baseTy
               && "got unwrapped optional decl from non-optional base?!");
      }

      // The base expression is simply the metatype of the base type.
      // FIXME: This location info is bogus.
      auto base = TypeExpr::createImplicitHack(expr->getDotLoc(),
                                               baseTy, tc.Context);

      // Build the member reference.
      auto result = buildMemberRef(base,
                                   selected.openedFullType,
                                   expr->getDotLoc(), member, 
                                   expr->getNameLoc(),
                                   selected.openedType,
                                   cs.getConstraintLocator(expr),
                                   expr->isImplicit(), /*direct ivar*/false);
      if (!result)
        return nullptr;

      // If there was an argument, apply it.
      if (auto arg = expr->getArgument()) {
        ApplyExpr *apply = new (tc.Context) CallExpr(result, arg, 
                                                     /*Implicit=*/false);
        result = finishApply(apply, Type(), cs.getConstraintLocator(expr));
      }

      return result;
    }
    
  private:
    struct MemberPartialApplication {
      unsigned level : 31;
      // Selector for the partial_application_of_method_invalid diagnostic
      // message.
      enum : unsigned {
        Struct,
        Enum,
        EnumCase,
        Archetype,
        Protocol
      };
      unsigned kind : 3;
    };
    
    // A map used to track partial applications of methods to require that they
    // be fully applied. Partial applications of value types would capture
    // 'self' as an inout and hide any mutation of 'self', which is surprising.
    llvm::SmallDenseMap<Expr*, MemberPartialApplication, 2>
      InvalidPartialApplications;
    
    /// A list of "suspicious" optional injections that come from
    /// forced downcasts.
    SmallVector<InjectIntoOptionalExpr *, 4> SuspiciousOptionalInjections;

  public:
    /// A list of optional injections that have been diagnosed.
    llvm::SmallPtrSet<InjectIntoOptionalExpr *, 4>  DiagnosedOptionalInjections;
  private:

    Expr *applyMemberRefExpr(Expr *expr,
                             Expr *base,
                             SourceLoc dotLoc,
                             SourceLoc nameLoc,
                             bool implicit) {
      // Determine the declaration selected for this overloaded reference.
      auto selected = getOverloadChoice(
                        cs.getConstraintLocator(
                          expr,
                          ConstraintLocator::MemberRefBase));

      switch (selected.choice.getKind()) {
      case OverloadChoiceKind::DeclViaBridge: {
        // Look through an implicitly unwrapped optional.
        auto baseTy = base->getType()->getRValueType();
        if (auto objTy = cs.lookThroughImplicitlyUnwrappedOptionalType(baseTy)){
          base = coerceImplicitlyUnwrappedOptionalToValue(base, objTy,
                                         cs.getConstraintLocator(base));
          if (!base) return nullptr;

          baseTy = base->getType()->getRValueType();
        }

        if (auto baseMetaTy = baseTy->getAs<MetatypeType>()) {
          auto &tc = cs.getTypeChecker();
          auto classTy = tc.getBridgedToObjC(cs.DC, 
                                             baseMetaTy->getInstanceType());
          
          // FIXME: We're dropping side effects in the base here!
          base = TypeExpr::createImplicitHack(base->getLoc(), classTy, 
                                              tc.Context);
        } else {
          // Bridge the base to its corresponding Objective-C object.
          base = bridgeToObjectiveC(base);
        }

        // Fall through to build the member reference.
        SWIFT_FALLTHROUGH;
      }

      case OverloadChoiceKind::Decl:
      case OverloadChoiceKind::DeclViaUnwrappedOptional: {
        auto member = buildMemberRef(base,
                                     selected.openedFullType,
                                     dotLoc,
                                     selected.choice.getDecl(),
                                     nameLoc,
                                     selected.openedType,
                                     cs.getConstraintLocator(expr),
                                     implicit, /*direct ivar*/false);
        
        // If this is an application of a value type method or enum constructor,
        // arrange for us to check that it gets fully applied.
        EnumElementDecl *eed = nullptr;
        FuncDecl *fn = nullptr;
        Optional<unsigned> kind;
        if (auto apply = dyn_cast<ApplyExpr>(member)) {
          auto selfTy = apply->getArg()->getType()->getRValueType();
          auto fnDeclRef = dyn_cast<DeclRefExpr>(apply->getFn());
          if (!fnDeclRef)
            goto not_value_type_member;
          fn = dyn_cast<FuncDecl>(fnDeclRef->getDecl());
          if (selfTy->getStructOrBoundGenericStruct())
            kind = MemberPartialApplication::Struct;
          else if (selfTy->getEnumOrBoundGenericEnum())
            kind = MemberPartialApplication::Enum;
          else if (auto theCase = dyn_cast<EnumElementDecl>(fnDeclRef->getDecl())) {
            if (theCase->hasArgumentType()) {
              eed = theCase;
              kind = MemberPartialApplication::EnumCase;
            } else
              goto not_value_type_member;
          } else
            goto not_value_type_member;
        } else if (auto pmRef = dyn_cast<MemberRefExpr>(member)) {
          auto baseTy = pmRef->getBase()->getType();
          if (baseTy->hasReferenceSemantics())
            goto not_value_type_member;
          if (baseTy->isAnyExistentialType()) {
            kind = MemberPartialApplication::Protocol;
          } else if (isa<FuncDecl>(pmRef->getMember().getDecl()))
            kind = MemberPartialApplication::Archetype;
          else
            goto not_value_type_member;
          fn = dyn_cast<FuncDecl>(pmRef->getMember().getDecl());
        }
        if (fn && fn->isInstanceMember())
          InvalidPartialApplications.insert({
            member,
            // We need to apply all of the non-self argument clauses.
            {fn->getNaturalArgumentCount() - 1, kind.getValue() },
          });
        else if (eed) {
          InvalidPartialApplications.insert({
            member,
            // Enum elements need to have the constructor applied.
            { 1, kind.getValue() },
          });
        }

      not_value_type_member:
        return member;
      }

      case OverloadChoiceKind::DeclViaDynamic:
        return buildDynamicMemberRef(base, dotLoc,
                                     selected.choice.getDecl(),
                                     nameLoc,
                                     selected.openedType,
                                     cs.getConstraintLocator(expr));

      case OverloadChoiceKind::TupleIndex: {
        auto baseTy = base->getType()->getRValueType();
        if (auto objTy = cs.lookThroughImplicitlyUnwrappedOptionalType(baseTy)) {
          base = coerceImplicitlyUnwrappedOptionalToValue(base, objTy,
                                         cs.getConstraintLocator(base));
          if (!base) return nullptr;
        }

        return new (cs.getASTContext()) TupleElementExpr(
                                          base,
                                          dotLoc,
                                          selected.choice.getTupleIndex(),
                                          nameLoc,
                                          simplifyType(expr->getType()));
      }

      case OverloadChoiceKind::BaseType: {
        // FIXME: Losing ".0" sugar here.
        return base;
      }

      case OverloadChoiceKind::TypeDecl:
        llvm_unreachable("Nonsensical overload choice");
      }
    }
    
  public:
    Expr *visitUnresolvedSelectorExpr(UnresolvedSelectorExpr *expr) {
      return applyMemberRefExpr(expr, expr->getBase(), expr->getDotLoc(),
                                expr->getNameRange().Start,
                                expr->isImplicit());
      llvm_unreachable("not implemented");
    }
    
    
    Expr *visitUnresolvedDotExpr(UnresolvedDotExpr *expr) {
      return applyMemberRefExpr(expr, expr->getBase(), expr->getDotLoc(),
                                expr->getNameLoc(), expr->isImplicit());
    }

    Expr *visitSequenceExpr(SequenceExpr *expr) {
      llvm_unreachable("Expression wasn't parsed?");
    }

    Expr *visitIdentityExpr(IdentityExpr *expr) {
      expr->setType(expr->getSubExpr()->getType());
      return expr;
    }

    Expr *visitParenExpr(ParenExpr *expr) {
      auto &ctx = cs.getASTContext();
      expr->setType(ParenType::get(ctx, expr->getSubExpr()->getType()));
      return expr;
    }

    Expr *visitTupleExpr(TupleExpr *expr) {
      return simplifyExprType(expr);
    }

    Expr *visitSubscriptExpr(SubscriptExpr *expr) {
      return buildSubscript(expr->getBase(), expr->getIndex(),
                            cs.getConstraintLocator(expr));
    }

    Expr *visitArrayExpr(ArrayExpr *expr) {
      Type openedType = expr->getType();
      Type arrayTy = simplifyType(openedType);
      auto &tc = cs.getTypeChecker();

      ProtocolDecl *arrayProto
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::ArrayLiteralConvertible);
      assert(arrayProto && "type-checked array literal w/o protocol?!");

      ProtocolConformance *conformance = nullptr;
      bool conforms = tc.conformsToProtocol(arrayTy, arrayProto,
                                            cs.DC, &conformance);
      (void)conforms;
      assert(conforms && "Type does not conform to protocol?");

      // Call the witness that builds the array literal.
      // FIXME: callWitness() may end up re-doing some work we already did
      // to convert the array literal elements to the element type. It would
      // be nicer to re-use them.
      // FIXME: Cache the name.

      // FIXME: This location info is bogus.
      Expr *typeRef = TypeExpr::createImplicitHack(expr->getLoc(),
                                                   arrayTy, tc.Context);
      auto name = tc.Context.Id_ConvertFromArrayLiteral;
      auto arg = expr->getSubExpr();
      Expr *result = tc.callWitness(typeRef, dc, arrayProto, conformance,
                                    name, arg, diag::array_protocol_broken);
      if (!result)
        return nullptr;

      expr->setSemanticExpr(result);
      expr->setType(arrayTy);
      return expr;
    }

    Expr *visitDictionaryExpr(DictionaryExpr *expr) {
      Type openedType = expr->getType();
      Type dictionaryTy = simplifyType(openedType);
      auto &tc = cs.getTypeChecker();

      ProtocolDecl *dictionaryProto
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::DictionaryLiteralConvertible);

      ProtocolConformance *conformance = nullptr;
      bool conforms = tc.conformsToProtocol(dictionaryTy, dictionaryProto,
                                            cs.DC, &conformance);
      if (!conforms)
        return nullptr;

      // Call the witness that builds the dictionary literal.
      // FIXME: callWitness() may end up re-doing some work we already did
      // to convert the dictionary literal elements to the (key, value) tuple.
      // It would be nicer to re-use them.
      // FIXME: Cache the name.
      // FIXME: This location info is bogus.
      Expr *typeRef = TypeExpr::createImplicitHack(expr->getLoc(),
                                                   dictionaryTy, tc.Context);
      auto name = tc.Context.Id_ConvertFromDictionaryLiteral;
      auto arg = expr->getSubExpr();
      Expr *result = tc.callWitness(typeRef, dc, dictionaryProto,
                                    conformance, name, arg,
                                    diag::dictionary_protocol_broken);
      if (!result)
        return nullptr;

      expr->setSemanticExpr(result);
      expr->setType(dictionaryTy);
      return expr;
    }

    Expr *visitDynamicSubscriptExpr(DynamicSubscriptExpr *expr) {
      return buildSubscript(expr->getBase(), expr->getIndex(),
                            cs.getConstraintLocator(expr));
    }

    Expr *visitTupleElementExpr(TupleElementExpr *expr) {
      // Handle accesses that implicitly look through ImplicitlyUnwrappedOptional<T>.
      auto base = expr->getBase();
      auto baseTy = base->getType()->getRValueType();
      if (auto objTy = cs.lookThroughImplicitlyUnwrappedOptionalType(baseTy)) {
        base = coerceImplicitlyUnwrappedOptionalToValue(base, objTy,
                                              cs.getConstraintLocator(base));
        if (!base) return nullptr;
        expr->setBase(base);
      }

      simplifyExprType(expr);
      return expr;
    }

    Expr *visitClosureExpr(ClosureExpr *expr) {
      llvm_unreachable("Handled by the walker directly");
    }

    Expr *visitAutoClosureExpr(AutoClosureExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Expr *visitModuleExpr(ModuleExpr *expr) { return expr; }

    Expr *visitInOutExpr(InOutExpr *expr) {
      auto lvTy = expr->getSubExpr()->getType()->castTo<LValueType>();

      // The type is simply inout.
      // Compute the type of the inout expression.
      expr->setType(InOutType::get(lvTy->getObjectType()));
      return expr;
    }

    Expr *visitDynamicTypeExpr(DynamicTypeExpr *expr) {
      auto &tc = cs.getTypeChecker();

      Expr *base = expr->getBase();
      base = tc.coerceToRValue(base);
      if (!base) return nullptr;
      expr->setBase(base);

      return simplifyExprType(expr);
    }

    Expr *visitOpaqueValueExpr(OpaqueValueExpr *expr) {
      return expr;
    }

    Expr *visitDefaultValueExpr(DefaultValueExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Expr *visitApplyExpr(ApplyExpr *expr) {
      
      auto result = finishApply(expr, expr->getType(),
                         ConstraintLocatorBuilder(
                           cs.getConstraintLocator(expr)));

      // See if this application advanced a partial value type application.
      auto foundApplication = InvalidPartialApplications.find(
                                   expr->getFn()->getSemanticsProvidingExpr());
      if (foundApplication != InvalidPartialApplications.end()) {
        unsigned level = foundApplication->second.level;
        assert(level > 0);
        InvalidPartialApplications.erase(foundApplication);
        if (level > 1)
          InvalidPartialApplications.insert({
            result, {
              level - 1,
              foundApplication->second.kind
            }
          });
      }
      
      return result;
    }

    Expr *visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *expr) {
      return expr;
    }

    Expr *visitIfExpr(IfExpr *expr) {
      auto resultTy = simplifyType(expr->getType());
      expr->setType(resultTy);

      // Convert the condition to a logic value.
      auto cond
        = solution.convertToLogicValue(expr->getCondExpr(),
                                       cs.getConstraintLocator(expr));
      if (!cond) {
        cond->setType(ErrorType::get(cs.getASTContext()));
      } else {
        expr->setCondExpr(cond);
      }

      // Coerce the then/else branches to the common type.
      expr->setThenExpr(coerceToType(expr->getThenExpr(), resultTy,
                                     ConstraintLocatorBuilder(
                                       cs.getConstraintLocator(expr))
                                     .withPathElement(
                                       ConstraintLocator::IfThen)));
      expr->setElseExpr(coerceToType(expr->getElseExpr(), resultTy,
                                     ConstraintLocatorBuilder(
                                       cs.getConstraintLocator(expr))
                                     .withPathElement(
                                       ConstraintLocator::IfElse)));

      return expr;
    }
    
    Expr *visitImplicitConversionExpr(ImplicitConversionExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    /// A helper function to plumb a stack of optional types.
    Type plumbOptionals(Type type, SmallVectorImpl<Type> &optionals) {
      while (auto valueType = type->getAnyOptionalObjectType()) {
        optionals.push_back(type);
        type = valueType;
      }
      return type;
    };

    Expr *visitIsaExpr(IsaExpr *expr) {
      // Turn the subexpression into an rvalue.
      auto &tc = cs.getTypeChecker();
      auto toType = simplifyType(expr->getCastTypeLoc().getType());
      auto sub = tc.coerceToRValue(expr->getSubExpr());
      if (!sub)
        return nullptr;
      
      expr->setSubExpr(sub);

      // Set the type we checked against.
      expr->getCastTypeLoc().setType(toType, /*validated=*/true);
      auto fromType = sub->getType();
      auto castKind = tc.typeCheckCheckedCast(
                        fromType, toType, cs.DC,
                        expr->getLoc(),
                        sub->getSourceRange(),
                        expr->getCastTypeLoc().getSourceRange(),
                        [&](Type commonTy) -> bool {
                          return tc.convertToType(sub, commonTy, cs.DC);
                        });

      switch (castKind) {
      case CheckedCastKind::Unresolved:
        // Invalid type check.
        return nullptr;
      case CheckedCastKind::Coercion:
        // Check is trivially true.
        tc.diagnose(expr->getLoc(), diag::isa_is_always_true,
                    expr->getSubExpr()->getType(),
                    expr->getCastTypeLoc().getType());
        expr->setCastKind(castKind);
        break;
      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::ArrayDowncastBridged:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::DictionaryDowncastBridged:
      case CheckedCastKind::Downcast:
      case CheckedCastKind::SuperToArchetype:
      case CheckedCastKind::ArchetypeToArchetype:
      case CheckedCastKind::ArchetypeToConcrete:
      case CheckedCastKind::ExistentialToArchetype:
      case CheckedCastKind::ExistentialToConcrete:
      case CheckedCastKind::ConcreteToArchetype:
      case CheckedCastKind::ConcreteToUnrelatedExistential:
        // Valid checks.
        expr->setCastKind(castKind);
        break;
      }

      // SIL-generation magically turns this into a Bool; make sure it can.
      if (!cs.getASTContext().getGetBoolDecl(&cs.getTypeChecker())) {
        tc.diagnose(expr->getLoc(), diag::bool_intrinsics_not_found);
        // Continue anyway.
      }

      // Dig through the optionals in the from/to types.
      SmallVector<Type, 2> fromOptionals;
      auto fromValueType = plumbOptionals(fromType, fromOptionals);
      SmallVector<Type, 2> toOptionals;
      auto toValueType = plumbOptionals(toType, toOptionals);

      // If we have an imbalance of optionals, a collection downcast, or
      // are bridging through an Objective-C class, handle this as a
      // checked cast followed by a getLogicValue.
      if (fromOptionals.size() != toOptionals.size() ||
          castKind == CheckedCastKind::ArrayDowncast ||
          castKind == CheckedCastKind::DictionaryDowncast ||
          tc.getDynamicBridgedThroughObjCClass(cs.DC,
                                               fromValueType,
                                               toValueType)) {
        auto toOptType = OptionalType::get(toType);
        ConditionalCheckedCastExpr *cast
          = new (tc.Context) ConditionalCheckedCastExpr(
                               sub, expr->getLoc(), SourceLoc(),
                               TypeLoc::withoutLoc(toType));
        cast->setType(toOptType);
        if (expr->isImplicit())
          cast->setImplicit();

        // Type-check this conditional case.
        Expr *result = visitConditionalCheckedCastExpr(cast);
        if (!result)
          return nullptr;

        // Extract a Bool from the resulting expression.
        return solution.convertOptionalToBool(result,
                                              cs.getConstraintLocator(expr));
      }

      return expr;
    }

    /// Handle optional operands and results in an explicit cast.
    Expr *handleOptionalBindings(ExplicitCastExpr *cast, 
                                 Type finalResultType,
                                 bool conditionalCast) {
      auto &tc = cs.getTypeChecker();

      unsigned destExtraOptionals = conditionalCast ? 1 : 0;

      // FIXME: some of this work needs to be delayed until runtime to
      // properly account for archetypes dynamically being optional
      // types.  For example, if we're casting T to NSView?, that
      // should succeed if T=NSObject? and its value is actually nil.
      Expr *subExpr = cast->getSubExpr();
      Type srcType = subExpr->getType();

      SmallVector<Type, 4> srcOptionals;
      srcType = plumbOptionals(srcType, srcOptionals);

      SmallVector<Type, 4> destOptionals;
      auto destValueType = plumbOptionals(finalResultType, destOptionals);

      // Check whether we need to bridge the source type to the
      // destination type.
      Type bridgedThroughClass
        = tc.getDynamicBridgedThroughObjCClass(cs.DC, srcType, destValueType);

      // There's nothing special to do if the operand isn't optional
      // and we don't need any bridging.
      if (srcOptionals.empty() && !bridgedThroughClass) {
        cast->setType(finalResultType);
        return cast;
      }

      // If this is a conditional cast, the result type will always
      // have at least one level of optional, which should become the
      // type of the checked-cast expression.
      if (conditionalCast) {
        assert(!destOptionals.empty() &&
               "result of checked cast is not an optional type");
        cast->setType(destOptionals.back());

        if (bridgedThroughClass)
          cast->setType(OptionalType::get(bridgedThroughClass));
      } else {
        cast->setType(bridgedThroughClass? bridgedThroughClass : destValueType);
      }

      // The result type (without the final optional) is a subtype of
      // the operand type, so it will never have a higher depth.
      assert(destOptionals.size() - destExtraOptionals <= srcOptionals.size());

      // The outermost N levels of optionals on the operand must all
      // be present or the cast fails.  The innermost M levels of
      // optionals on the operand are reflected in the requested
      // destination type, so we should map these nils into the result.
      unsigned numRequiredOptionals =
        srcOptionals.size() - (destOptionals.size() - destExtraOptionals);

      // Determine whether we require conditional bridging.
      bool requiresConditionalBridging = conditionalCast && bridgedThroughClass;

      // The number of OptionalEvaluationExprs between the point of the
      // inner cast and the enclosing OptionalEvaluationExpr (exclusive)
      // which represents failure for the entire operation.
      unsigned failureDepth = destOptionals.size() - destExtraOptionals 
                            + requiresConditionalBridging;

      // Drill down on the operand until it's non-optional.
      SourceLoc fakeQuestionLoc = subExpr->getEndLoc();
      for (unsigned i : indices(srcOptionals)) {
        Type valueType =
          (i + 1 == srcOptionals.size() ? srcType : srcOptionals[i+1]);

        // As we move into the range of mapped optionals, start
        // lowering the depth.
        unsigned depth = failureDepth - requiresConditionalBridging;
        if (i >= numRequiredOptionals) {
          depth -= (i - numRequiredOptionals) + 1;
        } else if (!conditionalCast) {
          // For a forced cast, force the required optionals.
          subExpr = new (tc.Context) ForceValueExpr(subExpr, fakeQuestionLoc);
          subExpr->setType(valueType);
          subExpr->setImplicit(true);
          continue;
        }

        subExpr = new (tc.Context) BindOptionalExpr(subExpr, fakeQuestionLoc,
                                                    depth, valueType);
        subExpr->setImplicit(true);
      }
      cast->setSubExpr(subExpr);

      // If we're casting to an optional type, we need to capture the
      // final M bindings.
      Expr *result = cast;

      // First, handle any required bridging.
      if (bridgedThroughClass) {
        // If the source type is the bridged class, we don't need the
        // actual cast, so grab it's subexpression.
        // FIXME: This loses source information.
        bool dropCast = srcType->isEqual(bridgedThroughClass);
        if (dropCast)
          result = cast->getSubExpr();

        if (requiresConditionalBridging) {
          // When conditionally bridging, we need to carry through the
          // optional.
          if (!dropCast) {
            result = new (tc.Context) BindOptionalExpr(result, 
                                                       cast->getEndLoc(),
                                                       failureDepth, 
                                                       bridgedThroughClass);
            result->setImplicit(true);
          }

          result = bridgeFromObjectiveCConditional(result, destValueType);
          if (!result)
            return nullptr;

          // Update type sugar.
          result->setType(OptionalType::get(destValueType));

          if (!dropCast) {
            result = new (tc.Context) OptionalEvaluationExpr(
                                        result,
                                        OptionalType::get(destValueType));
          }
        } else {
          result = bridgeFromObjectiveC(result, destValueType);
          if (!result)
            return nullptr;

          // Update type sugar.
          result->setType(destValueType);
        }
      }

      if (destOptionals.size() > destExtraOptionals) {
        if (conditionalCast) {
          // If the innermost cast fails, the entire expression fails.  To
          // get this behavior, we have to bind and then re-inject the result.
          // (SILGen should know how to peephole this.)
          result = new (tc.Context) BindOptionalExpr(result, cast->getEndLoc(),
                                      failureDepth-requiresConditionalBridging, 
                                      destValueType);
          result->setImplicit(true);
        }

        for (unsigned i = destOptionals.size(); i != 0; --i) {
          Type destType = destOptionals[i-1];
          result = new (tc.Context) InjectIntoOptionalExpr(result, destType);
          result = new (tc.Context) OptionalEvaluationExpr(result, destType);
        }

      // Otherwise, we just need to capture the failure-depth binding.
      } else if (conditionalCast) {
        result = new (tc.Context) OptionalEvaluationExpr(result,
                                                         finalResultType);
      }

      return result;
    }

    Expr *visitUnresolvedCheckedCastExpr(UnresolvedCheckedCastExpr *expr) {
      // Simplify the type we're casting to.
      auto toType = simplifyType(expr->getCastTypeLoc().getType());
      expr->getCastTypeLoc().setType(toType, /*validated=*/true);

      // Determine whether we performed a coercion or a downcast.
      auto locator
        = cs.getConstraintLocator(expr, ConstraintLocator::CheckedCastOperand);
      unsigned choice = solution.getDisjunctionChoice(locator);
      assert(choice <= 1 && "checked cast choices not synced with disjunction");

      auto &tc = cs.getTypeChecker();
      auto sub = tc.coerceToRValue(expr->getSubExpr());

      // Choice 0 is coercion.
      if (choice == 0) {
        // The subexpression is always an rvalue.
        if (!sub)
          return nullptr;

        // Convert the subexpression.
        bool failed = tc.convertToType(sub, toType, cs.DC);
        (void)failed;
        assert(!failed && "Not convertible?");

        // Transmute the checked cast into a coercion expression.
        Expr *result = new (tc.Context) CoerceExpr(sub, expr->getLoc(),
                                                   expr->getCastTypeLoc());

        // The result type is the type we're converting to.
        result->setType(toType);
        return result;
      }

      // Choice 1 is downcast.
      assert(choice == 1);
      auto fromType = sub->getType();
      auto castKind = tc.typeCheckCheckedCast(
                        fromType, toType, cs.DC,
                        expr->getLoc(),
                        sub->getSourceRange(),
                        expr->getCastTypeLoc().getSourceRange(),
                        [&](Type commonTy) -> bool {
                          return tc.convertToType(sub, commonTy,
                                                 cs.DC);
                        });
      switch (castKind) {
        /// Invalid cast.
      case CheckedCastKind::Unresolved:
        return nullptr;
      case CheckedCastKind::Coercion:
        llvm_unreachable("Coercions handled above");

      // Valid casts.
      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::ArrayDowncastBridged:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::DictionaryDowncastBridged:
      case CheckedCastKind::Downcast:
      case CheckedCastKind::SuperToArchetype:
      case CheckedCastKind::ArchetypeToArchetype:
      case CheckedCastKind::ArchetypeToConcrete:
      case CheckedCastKind::ExistentialToArchetype:
      case CheckedCastKind::ExistentialToConcrete:
      case CheckedCastKind::ConcreteToArchetype:
      case CheckedCastKind::ConcreteToUnrelatedExistential:
        break;
      }
      
      auto *cast = new (tc.Context) ForcedCheckedCastExpr(
                                      sub,
                                      expr->getLoc(),
                                      expr->getCastTypeLoc());
      cast->setType(toType);
      cast->setCastKind(castKind);
      if (expr->isImplicit())
        cast->setImplicit();

      return handleOptionalBindings(cast, simplifyType(expr->getType()),
                                    /*conditionalCast=*/false);
    }

    Expr *visitForcedCheckedCastExpr(ForcedCheckedCastExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Expr *visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *expr) {
      // Simplify the type we're casting to.
      auto toType = simplifyType(expr->getCastTypeLoc().getType());
      expr->getCastTypeLoc().setType(toType, /*validated=*/true);

      // The subexpression is always an rvalue.
      auto &tc = cs.getTypeChecker();
      auto sub = tc.coerceToRValue(expr->getSubExpr());
      if (!sub)
        return nullptr;
      expr->setSubExpr(sub);

      auto fromType = sub->getType();
      auto castKind = tc.typeCheckCheckedCast(
                        fromType, toType, cs.DC,
                        expr->getLoc(),
                        sub->getSourceRange(),
                        expr->getCastTypeLoc().getSourceRange(),
                        [&](Type commonTy) -> bool {
                          return tc.convertToType(sub, commonTy,
                                                 cs.DC);
                        });
      switch (castKind) {
        /// Invalid cast.
      case CheckedCastKind::Unresolved:
        return nullptr;
      case CheckedCastKind::Coercion: {
        tc.diagnose(expr->getLoc(), diag::conditional_downcast_coercion,
                    sub->getType(), toType);

        // Convert the subexpression.
        bool failed = tc.convertToType(sub, toType, cs.DC);
        (void)failed;
        assert(!failed && "Not convertible?");

        // Transmute the checked cast into a coercion expression.
        Expr *result = new (tc.Context) CoerceExpr(sub, expr->getLoc(),
                                                   expr->getCastTypeLoc());

        // The result type is the type we're converting to.
        result->setType(toType);

        // Wrap the result in an optional.
        return new (tc.Context) InjectIntoOptionalExpr(
                                  result,
                                  OptionalType::get(toType));
      }

      // Valid casts.
      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::ArrayDowncastBridged:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::DictionaryDowncastBridged:
      case CheckedCastKind::Downcast:
      case CheckedCastKind::SuperToArchetype:
      case CheckedCastKind::ArchetypeToArchetype:
      case CheckedCastKind::ArchetypeToConcrete:
      case CheckedCastKind::ExistentialToArchetype:
      case CheckedCastKind::ExistentialToConcrete:
      case CheckedCastKind::ConcreteToArchetype:
      case CheckedCastKind::ConcreteToUnrelatedExistential:
        expr->setCastKind(castKind);
        break;
      }
      
      return handleOptionalBindings(expr, simplifyType(expr->getType()),
                                    /*conditionalCast=*/true);
    }

    Expr *visitCoerceExpr(CoerceExpr *expr) {
      return expr;
    }

    Expr *visitAssignExpr(AssignExpr *expr) {
      llvm_unreachable("Handled by ExprWalker");
    }

    Expr *visitAssignExpr(AssignExpr *expr, ConstraintLocator *srcLocator) {
      // Compute the type to which the source must be converted to allow
      // assignment to the destination.
      //
      // FIXME: This is also computed when the constraint system is set up.
      auto destTy = cs.computeAssignDestType(expr->getDest(), expr->getLoc());
      if (!destTy)
        return nullptr;

      // Convert the source to the simplified destination type.
      Expr *src = solution.coerceToType(expr->getSrc(),
                                        destTy,
                                        srcLocator);
      if (!src)
        return nullptr;
      
      expr->setSrc(src);
      
      return expr;
    }
    
    Expr *visitDiscardAssignmentExpr(DiscardAssignmentExpr *expr) {
      return simplifyExprType(expr);
    }
    
    Expr *visitUnresolvedPatternExpr(UnresolvedPatternExpr *expr) {
      llvm_unreachable("should have been eliminated during name binding");
    }
    
    Expr *visitBindOptionalExpr(BindOptionalExpr *expr) {
      Type valueType = simplifyType(expr->getType());
      Type optType =
        cs.getTypeChecker().getOptionalType(expr->getQuestionLoc(), valueType);
      if (!optType) return nullptr;

      Expr *subExpr = coerceToType(expr->getSubExpr(), optType,
                                   cs.getConstraintLocator(expr));
      if (!subExpr) return nullptr;

      // Complain if the sub-expression was converted to T? via the
      // inject-into-optional implicit conversion.
      //
      // It should be the case that that's always the last conversion applied.
      if (auto injection = dyn_cast<InjectIntoOptionalExpr>(subExpr)) {
        // If the sub-expression was a forced downcast, suggest
        // turning it into a conditional downcast.
        auto &tc = cs.getTypeChecker();
        if (auto forced = findForcedDowncast(tc.Context, 
                                             injection->getSubExpr())) {
          tc.diagnose(expr->getLoc(), diag::binding_explicit_downcast,
                      injection->getSubExpr()->getType()->getRValueType())
            .highlight(forced->getLoc())
            .fixItInsert(Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                                    forced->getLoc()),
                        "?");
        } else {
          tc.diagnose(subExpr->getLoc(), diag::binding_injected_optional,
                      expr->getSubExpr()->getType()->getRValueType())
            .highlight(subExpr->getSourceRange())
            .fixItRemove(expr->getQuestionLoc());
        }

        // Don't diagnose this injection again.
        DiagnosedOptionalInjections.insert(injection);
      }

      expr->setSubExpr(subExpr);
      expr->setType(valueType);
      return expr;
    }

    Expr *visitOptionalEvaluationExpr(OptionalEvaluationExpr *expr) {
      Type optType = simplifyType(expr->getType());
      Expr *subExpr = coerceToType(expr->getSubExpr(), optType,
                                   cs.getConstraintLocator(expr));
      if (!subExpr) return nullptr;

      expr->setSubExpr(subExpr);
      expr->setType(optType);
      return expr;
    }

    Expr *visitForceValueExpr(ForceValueExpr *expr) {
      Type valueType = simplifyType(expr->getType());
      expr->setType(valueType);
      return expr;
    }

    Expr *visitOpenExistentialExpr(OpenExistentialExpr *expr) {
      llvm_unreachable("Already type-checked");
    }
    
    void finalize() {
      // Check that all value type methods were fully applied.
      auto &tc = cs.getTypeChecker();
      for (auto &unapplied : InvalidPartialApplications) {
        unsigned kind = unapplied.second.kind;
        tc.diagnose(unapplied.first->getLoc(),
                    diag::partial_application_of_method_invalid,
                    kind);
      }

      // We should have complained above if there were any
      // existentials that haven't been closed yet.
      assert((OpenedExistentials.empty() || 
              !InvalidPartialApplications.empty()) &&
             "Opened existentials have not been closed");

      // Look at all of the suspicious optional injections
      for (auto injection : SuspiciousOptionalInjections) {
        // If we already diagnosed this injection, we're done.
        if (DiagnosedOptionalInjections.count(injection)) {
          continue;
        }

        auto *cast = findForcedDowncast(tc.Context, injection->getSubExpr());
        if (!cast)
          continue;

        if (isa<ParenExpr>(injection->getSubExpr()))
          continue;

        tc.diagnose(injection->getLoc(), diag::inject_forced_downcast,
                    injection->getSubExpr()->getType()->getRValueType());
        auto questionLoc = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                                      cast->getLoc());
        tc.diagnose(questionLoc, diag::forced_to_conditional_downcast, 
                    injection->getType()->getAnyOptionalObjectType())
          .fixItInsert(questionLoc, "?");
        auto pastEndLoc = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                                     cast->getEndLoc());
        tc.diagnose(cast->getStartLoc(), diag::silence_inject_forced_downcast)
          .fixItInsert(cast->getStartLoc(), "(")
          .fixItInsert(pastEndLoc, ")");
      }
    }

    /// Diagnose an optional injection that is probably not what the
    /// user wanted, because it comes from a forced downcast.
    void diagnoseOptionalInjection(InjectIntoOptionalExpr *injection) {
      // Don't diagnose when we're injecting into 
      auto toOptionalType = injection->getType();
      if (toOptionalType->getImplicitlyUnwrappedOptionalObjectType())
        return;

      // Check whether we have a forced downcast.
      auto &tc = cs.getTypeChecker();
      auto *cast = findForcedDowncast(tc.Context, injection->getSubExpr());
      if (!cast)
        return;
      
      SuspiciousOptionalInjections.push_back(injection);
    }
  };
}

/// \brief Given a constraint locator, find the owner of default arguments for
/// that tuple, i.e., a FuncDecl.
static ConcreteDeclRef
findDefaultArgsOwner(ConstraintSystem &cs, const Solution &solution,
                     ConstraintLocator *locator) {
  if (locator->getPath().empty() || !locator->getAnchor())
    return nullptr;

  // If the locator points to a function application, find the function itself.
  if (locator->getPath().back().getKind() == ConstraintLocator::ApplyArgument) {
    assert(locator->getPath().back().getNewSummaryFlags() == 0 &&
           "ApplyArgument adds no flags");
    SmallVector<LocatorPathElt, 4> newPath;
    newPath.append(locator->getPath().begin(), locator->getPath().end()-1);
    unsigned newFlags = locator->getSummaryFlags();

    // If we have an interpolation argument, dig out the constructor if we
    // can.
    // FIXME: This representation is actually quite awful
    if (newPath.size() == 1 &&
        newPath[0].getKind() == ConstraintLocator::InterpolationArgument) {
      newPath.push_back(ConstraintLocator::ConstructorMember);

      locator = cs.getConstraintLocator(locator->getAnchor(), newPath, newFlags);
      auto known = solution.overloadChoices.find(locator);
      if (known != solution.overloadChoices.end()) {
        auto &choice = known->second.choice;
        if (choice.getKind() == OverloadChoiceKind::Decl)
          return cast<AbstractFunctionDecl>(choice.getDecl());
      }
      return nullptr;
    } else {
      newPath.push_back(ConstraintLocator::ApplyFunction);
    }
    assert(newPath.back().getNewSummaryFlags() == 0 &&
           "added element that changes the flags?");
    locator = cs.getConstraintLocator(locator->getAnchor(), newPath, newFlags);
  }

  // Simplify the locator.
  SourceRange range1, range2;
  locator = simplifyLocator(cs, locator, range1, range2);

  // If we didn't map down to a specific expression, we can't handle a default
  // argument.
  if (!locator->getAnchor() || !locator->getPath().empty())
    return nullptr;

  if (auto resolved
        = resolveLocatorToDecl(cs, locator,
            [&](ConstraintLocator *locator) -> Optional<SelectedOverload> {
              auto known = solution.overloadChoices.find(locator);
              if (known == solution.overloadChoices.end()) {
                return Nothing;
              }

              return known->second;
            },
            [&](ValueDecl *decl,
                Type openedType) -> ConcreteDeclRef {
              if (decl->getPotentialGenericDeclContext()->isGenericContext()) {
                SmallVector<Substitution, 4> subs;
                solution.computeSubstitutions(decl->getType(),
                                              decl->getPotentialGenericDeclContext(),
                                              openedType, subs);
                return ConcreteDeclRef(cs.getASTContext(), decl, subs);
              }
              
              return decl;
            })) {
    return resolved.getDecl();
  }

  return nullptr;
}

/// Produce the caller-side default argument for this default argument, or
/// null if the default argument will be provided by the callee.
static Expr *getCallerDefaultArg(TypeChecker &tc, DeclContext *dc,
                                 SourceLoc loc, ConcreteDeclRef &owner,
                                 unsigned index) {
  auto ownerFn = cast<AbstractFunctionDecl>(owner.getDecl());
  auto defArg = ownerFn->getDefaultArg(index);
  MagicIdentifierLiteralExpr::Kind magicKind;
  switch (defArg.first) {
  case DefaultArgumentKind::None:
    llvm_unreachable("No default argument here?");

  case DefaultArgumentKind::Normal:
    return nullptr;

  case DefaultArgumentKind::Inherited:
    // Update the owner to reflect inheritance here.
    owner = ownerFn->getOverriddenDecl();
    return getCallerDefaultArg(tc, dc, loc, owner, index);

  case DefaultArgumentKind::Column:
    magicKind = MagicIdentifierLiteralExpr::Column;
    break;

  case DefaultArgumentKind::File:
    magicKind = MagicIdentifierLiteralExpr::File;
    break;
    
  case DefaultArgumentKind::Line:
    magicKind = MagicIdentifierLiteralExpr::Line;
    break;
      
  case DefaultArgumentKind::Function:
    magicKind = MagicIdentifierLiteralExpr::Function;
    break;
  }

  // Create the default argument, which is a converted magic identifier
  // literal expression.
  Expr *init = new (tc.Context) MagicIdentifierLiteralExpr(magicKind, loc,
                                                           /*Implicit=*/true);
  bool invalid = tc.typeCheckExpression(init, dc, defArg.second, Type(),
                                        /*discardedExpr=*/false);
  assert(!invalid && "conversion cannot fail");
  (void)invalid;
  return init;
}

/// Rebuild the ParenTypes for the given expression, whose underlying expression
/// should be set to the given type.
static Type rebuildParenType(ASTContext &ctx, Expr *expr, Type type) {
  if (auto paren = dyn_cast<ParenExpr>(expr)) {
    type = rebuildParenType(ctx, paren->getSubExpr(), type);
    paren->setType(ParenType::get(ctx, type));
    return paren->getType();
  }

  if (auto ident = dyn_cast<IdentityExpr>(expr)) {
    type = rebuildParenType(ctx, ident->getSubExpr(), type);
    ident->setType(type);
    return ident->getType();
  }

  return type;
}

Expr *ExprRewriter::coerceTupleToTuple(Expr *expr, TupleType *fromTuple,
                                       TupleType *toTuple,
                                       ConstraintLocatorBuilder locator,
                                       SmallVectorImpl<int> &sources,
                                       SmallVectorImpl<unsigned> &variadicArgs){
  auto &tc = cs.getTypeChecker();

  // Capture the tuple expression, if there is one.
  Expr *innerExpr = expr;
  while (auto paren = dyn_cast<IdentityExpr>(innerExpr))
    innerExpr = paren->getSubExpr();
  TupleExpr *fromTupleExpr = dyn_cast<TupleExpr>(innerExpr);

  /// Check each of the tuple elements in the destination.
  bool hasVarArg = false;
  bool anythingShuffled = false;
  bool hasInits = false;
  SmallVector<TupleTypeElt, 4> toSugarFields;
  SmallVector<TupleTypeElt, 4> fromTupleExprFields(
                                 fromTuple->getFields().size());
  SmallVector<Expr *, 2> callerDefaultArgs;
  ConcreteDeclRef defaultArgsOwner;

  for (unsigned i = 0, n = toTuple->getFields().size(); i != n; ++i) {
    const auto &toElt = toTuple->getFields()[i];
    auto toEltType = toElt.getType();

    // If we're default-initializing this member, there's nothing to do.
    if (sources[i] == TupleShuffleExpr::DefaultInitialize) {
      // Dig out the owner of the default arguments.
      if (!defaultArgsOwner) {
        defaultArgsOwner 
          = findDefaultArgsOwner(cs, solution,
                                 cs.getConstraintLocator(locator));
        assert(defaultArgsOwner && "Missing default arguments owner?");
      } else {
        assert(findDefaultArgsOwner(cs, solution,
                                    cs.getConstraintLocator(locator))
                 == defaultArgsOwner);
      }

      anythingShuffled = true;
      hasInits = true;
      toSugarFields.push_back(toElt);

      // Create a caller-side default argument, if we need one.
      if (auto defArg = getCallerDefaultArg(tc, dc, expr->getLoc(),
                                            defaultArgsOwner, i)) {
        callerDefaultArgs.push_back(defArg);
        sources[i] = TupleShuffleExpr::CallerDefaultInitialize;
      }
      continue;
    }

    // If this is the variadic argument, note it.
    if (sources[i] == TupleShuffleExpr::FirstVariadic) {
      assert(i == n-1 && "Vararg not at the end?");
      toSugarFields.push_back(toElt);
      hasVarArg = true;
      anythingShuffled = true;
      continue;
    }

    // If the source and destination index are different, we'll be shuffling.
    if ((unsigned)sources[i] != i) {
      anythingShuffled = true;
    }

    // We're matching one element to another. If the types already
    // match, there's nothing to do.
    const auto &fromElt = fromTuple->getFields()[sources[i]];
    auto fromEltType = fromElt.getType();
    if (fromEltType->isEqual(toEltType)) {
      // Get the sugared type directly from the tuple expression, if there
      // is one.
      if (fromTupleExpr)
        fromEltType = fromTupleExpr->getElement(sources[i])->getType();

      toSugarFields.push_back(TupleTypeElt(fromEltType,
                                           toElt.getName(),
                                           toElt.getDefaultArgKind(),
                                           toElt.isVararg()));
      fromTupleExprFields[sources[i]] = fromElt;
      hasInits |= toElt.hasInit();
      continue;
    }

    // We need to convert the source element to the destination type.
    if (!fromTupleExpr) {
      // FIXME: Lame! We can't express this in the AST.
      tc.diagnose(expr->getLoc(),
                  diag::tuple_conversion_not_expressible,
                  fromTuple, toTuple);
      return nullptr;
    }

    // Actually convert the source element.
    auto convertedElt
      = coerceToType(fromTupleExpr->getElement(sources[i]), toEltType,
                     locator.withPathElement(
                       LocatorPathElt::getTupleElement(sources[i])));
    if (!convertedElt)
      return nullptr;

    fromTupleExpr->setElement(sources[i], convertedElt);

    // Record the sugared field name.
    toSugarFields.push_back(TupleTypeElt(convertedElt->getType(),
                                         toElt.getName(),
                                         toElt.getDefaultArgKind(),
                                         toElt.isVararg()));
    fromTupleExprFields[sources[i]] = TupleTypeElt(convertedElt->getType(),
                                                   fromElt.getName(),
                                                   fromElt.getDefaultArgKind(),
                                                   fromElt.isVararg());
    hasInits |= toElt.hasInit();
  }

  // Convert all of the variadic arguments to the destination type.
  Expr *injectionFn = nullptr;
  if (hasVarArg) {
    Type toEltType = toTuple->getFields().back().getVarargBaseTy();
    for (int fromFieldIdx : variadicArgs) {
      const auto &fromElt = fromTuple->getFields()[fromFieldIdx];
      Type fromEltType = fromElt.getType();

      // If the source and destination types match, there's nothing to do.
      if (toEltType->isEqual(fromEltType)) {
        sources.push_back(fromFieldIdx);
        fromTupleExprFields[fromFieldIdx] = fromElt;
        continue;
      }

      // We need to convert the source element to the destination type.
      if (!fromTupleExpr) {
        // FIXME: Lame! We can't express this in the AST.
        tc.diagnose(expr->getLoc(),
                    diag::tuple_conversion_not_expressible,
                    fromTuple, toTuple);
        return nullptr;
      }

      // Actually convert the source element.
      auto convertedElt = coerceToType(
                            fromTupleExpr->getElement(fromFieldIdx),
                            toEltType,
                            locator.withPathElement(
                              LocatorPathElt::getTupleElement(fromFieldIdx)));
      if (!convertedElt)
        return nullptr;

      fromTupleExpr->setElement(fromFieldIdx, convertedElt);
      sources.push_back(fromFieldIdx);

      fromTupleExprFields[fromFieldIdx] = TupleTypeElt(
                                            convertedElt->getType(),
                                            fromElt.getName(),
                                            fromElt.getDefaultArgKind(),
                                            fromElt.isVararg());
    }

    // Find the appropriate injection function.
    ArraySliceType *sliceType
      = cast<ArraySliceType>(
          toTuple->getFields().back().getType().getPointer());
    Type boundType = BuiltinIntegerType::getWordType(tc.Context);
    injectionFn = tc.buildArrayInjectionFnRef(dc,
                                              sliceType, boundType,
                                              expr->getStartLoc());
    if (!injectionFn)
      return nullptr;
  }

  // Compute the updated 'from' tuple type, since we may have
  // performed some conversions in place.
  Type fromTupleType = TupleType::get(fromTupleExprFields, tc.Context);
  if (fromTupleExpr) {
    fromTupleExpr->setType(fromTupleType);

    // Update the types of parentheses around the tuple expression.
    rebuildParenType(cs.getASTContext(), expr, fromTupleType);
  }

  // Compute the re-sugared tuple type.
  Type toSugarType = hasInits? toTuple
                             : TupleType::get(toSugarFields, tc.Context);

  // If we don't have to shuffle anything, we're done.
  if (!anythingShuffled && fromTupleExpr) {
    fromTupleExpr->setType(toSugarType);

    // Update the types of parentheses around the tuple expression.
    rebuildParenType(cs.getASTContext(), expr, toSugarType);

    return expr;
  }
  
  // Create the tuple shuffle.
  ArrayRef<int> mapping = tc.Context.AllocateCopy(sources);
  auto callerDefaultArgsCopy = tc.Context.AllocateCopy(callerDefaultArgs);
  auto shuffle = new (tc.Context) TupleShuffleExpr(expr, mapping, 
                                                   defaultArgsOwner,
                                                   callerDefaultArgsCopy,
                                                   toSugarType);
  shuffle->setVarargsInjectionFunction(injectionFn);
  return shuffle;
}



Expr *ExprRewriter::coerceScalarToTuple(Expr *expr, TupleType *toTuple,
                                        int toScalarIdx,
                                        ConstraintLocatorBuilder locator) {
  auto &tc = solution.getConstraintSystem().getTypeChecker();

  // If the destination type is variadic, compute the injection function to use.
  Expr *injectionFn = nullptr;
  const auto &lastField = toTuple->getFields().back();

  if (lastField.isVararg()) {
    // Find the appropriate injection function.
    ArraySliceType *sliceType
      = cast<ArraySliceType>(lastField.getType().getPointer());
    Type boundType = BuiltinIntegerType::getWordType(tc.Context);
    injectionFn = tc.buildArrayInjectionFnRef(dc,
                                              sliceType, boundType,
                                              expr->getStartLoc());
    if (!injectionFn)
      return nullptr;
  }

  // If we're initializing the varargs list, use its base type.
  const auto &field = toTuple->getFields()[toScalarIdx];
  Type toScalarType;
  if (field.isVararg())
    toScalarType = field.getVarargBaseTy();
  else
    toScalarType = field.getType();

  // Coerce the expression to the type to the scalar type.
  expr = coerceToType(expr, toScalarType,
                      locator.withPathElement(
                        ConstraintLocator::ScalarToTuple));
  if (!expr)
    return nullptr;

  // Preserve the sugar of the scalar field.
  // FIXME: This doesn't work if the type has default values because they fail
  // to canonicalize.
  SmallVector<TupleTypeElt, 4> sugarFields;
  bool hasInit = false;
  int i = 0;
  for (auto &field : toTuple->getFields()) {
    if (field.hasInit()) {
      hasInit = true;
      break;
    }

    if (i == toScalarIdx) {
      if (field.isVararg()) {
        assert(expr->getType()->isEqual(field.getVarargBaseTy()) &&
               "scalar field is not equivalent to dest vararg field?!");

        sugarFields.push_back(TupleTypeElt(field.getType(),
                                           field.getName(),
                                           field.getDefaultArgKind(),
                                           true));
      }
      else {
        assert(expr->getType()->isEqual(field.getType()) &&
               "scalar field is not equivalent to dest tuple field?!");
        sugarFields.push_back(TupleTypeElt(expr->getType(),
                                           field.getName()));
      }

      // Record the
    } else {
      sugarFields.push_back(field);
    }
    ++i;
  }

  // Compute the elements of the resulting tuple.
  SmallVector<ScalarToTupleExpr::Element, 4> elements;
  ConcreteDeclRef defaultArgsOwner = nullptr;
  i = 0;
  for (auto &field : toTuple->getFields()) {
    // Use a null entry to indicate that this is the scalar field.
    if (i == toScalarIdx) {
      elements.push_back(ScalarToTupleExpr::Element());
      ++i;
      continue;
    }

    if (field.isVararg()) {
      ++i;
      continue;
    }

    assert(field.hasInit() && "Expected a default argument");

    // Dig out the owner of the default arguments.
    if (!defaultArgsOwner) {
      defaultArgsOwner
      = findDefaultArgsOwner(cs, solution,
                             cs.getConstraintLocator(locator));
      assert(defaultArgsOwner && "Missing default arguments owner?");
    } else {
      assert(findDefaultArgsOwner(cs, solution,
                                  cs.getConstraintLocator(locator))
             == defaultArgsOwner);
    }

    // Create a caller-side default argument, if we need one.
    if (auto defArg = getCallerDefaultArg(tc, dc, expr->getLoc(),
                                          defaultArgsOwner, i)) {
      // Record the caller-side default argument expression.
      // FIXME: Do we need to record what this was synthesized from?
      elements.push_back(defArg);
    } else {
      // Record the owner of the default argument.
      elements.push_back(defaultArgsOwner);
    }

    ++i;
  }

  Type destSugarTy = hasInit? toTuple
                            : TupleType::get(sugarFields, tc.Context);

  return new (tc.Context) ScalarToTupleExpr(expr, destSugarTy,
                                            tc.Context.AllocateCopy(elements),
                                            injectionFn);
}

/// Collect the conformances for all the protocols of an existential type.
static ArrayRef<ProtocolConformance*>
collectExistentialConformances(TypeChecker &tc, Type fromType, Type toType,
                               DeclContext *DC) {
  SmallVector<ProtocolDecl *, 4> protocols;
  toType->getAnyExistentialTypeProtocols(protocols);

  SmallVector<ProtocolConformance *, 4> conformances;
  for (auto proto : protocols) {
    ProtocolConformance *conformance = nullptr;
    bool conforms = tc.conformsToProtocol(fromType, proto, DC, &conformance);
    assert(conforms && "Type does not conform to protocol?");
    (void)conforms;
    conformances.push_back(conformance);
  }

  return tc.Context.AllocateCopy(conformances);
}

Expr *ExprRewriter::coerceExistential(Expr *expr, Type toType,
                                      ConstraintLocatorBuilder locator) {
  auto &tc = solution.getConstraintSystem().getTypeChecker();
  Type fromType = expr->getType();
  
  if (auto bridgedType = tc.getDynamicBridgedThroughObjCClass(cs.DC, toType, 
                                                              fromType)) {
    // Protect against "no-op" conversions. If the bridged type points back
    // to itself, the constraint solver won't have a conversion handy to
    // coerce to a user conversion, so we should avoid creating a new
    // expression node.
    if (!bridgedType->isEqual(fromType) && !bridgedType->isEqual(toType)) {
      expr = coerceViaUserConversion(expr, bridgedType, locator);
      fromType = bridgedType;
    }
  }
  
  // Handle existential coercions that implicitly look through ImplicitlyUnwrappedOptional<T>.
  if (auto ty = cs.lookThroughImplicitlyUnwrappedOptionalType(fromType)) {
    expr = coerceImplicitlyUnwrappedOptionalToValue(expr, ty, locator);
    if (!expr) return nullptr;
    
    fromType = expr->getType();
    
    // FIXME: Hack. We shouldn't try to coerce existential when there is no
    // existential upcast to perform.
    if (fromType->isEqual(toType))
      return expr;
  }

  auto conformances =
    collectExistentialConformances(tc, fromType, toType, cs.DC);
  return new (tc.Context) ErasureExpr(expr, toType, conformances);
}

Expr *ExprRewriter::coerceExistentialMetatype(Expr *expr, Type toType,
                                              ConstraintLocatorBuilder locator) {
  auto &tc = solution.getConstraintSystem().getTypeChecker();
  Type fromType = expr->getType();
  Type fromInstanceType = fromType->castTo<AnyMetatypeType>()->getInstanceType();
  Type toInstanceType =
    toType->castTo<ExistentialMetatypeType>()->getInstanceType();

  auto conformances =
    collectExistentialConformances(tc, fromInstanceType, toInstanceType, cs.DC);
  return new (tc.Context) MetatypeErasureExpr(expr, toType, conformances);
}

Expr *ExprRewriter::coerceViaUserConversion(Expr *expr, Type toType,
                                            ConstraintLocatorBuilder locator) {
  auto &tc = solution.getConstraintSystem().getTypeChecker();

  // Determine the locator that corresponds to the conversion member.
  auto storedLocator
    = cs.getConstraintLocator(
        locator.withPathElement(ConstraintLocator::ConversionMember));
  auto knownOverload = solution.overloadChoices.find(storedLocator);
  if (knownOverload != solution.overloadChoices.end()) {
    auto selected = knownOverload->second;

    // FIXME: Location information is suspect throughout.
    // Form a reference to the conversion member.
    auto memberRef = buildMemberRef(expr,
                                    selected.openedFullType,
                                    expr->getStartLoc(),
                                    selected.choice.getDecl(),
                                    expr->getEndLoc(),
                                    selected.openedType,
                                    locator,
                                    /*Implicit=*/true, /*direct ivar*/false);

    // Form an empty tuple.
    Expr *args = TupleExpr::createEmpty(tc.Context,
                                        expr->getStartLoc(),
                                        expr->getEndLoc(),
                                        /*Implicit=*/true);

    // Call the conversion function with an empty tuple.
    ApplyExpr *apply = new (tc.Context) CallExpr(memberRef, args,
                                                 /*Implicit=*/true);
    auto openedType = selected.openedType->castTo<FunctionType>()->getResult();
    expr = finishApply(apply, openedType,
                       ConstraintLocatorBuilder(
                         cs.getConstraintLocator(apply)));

    if (!expr)
      return nullptr;

    return coerceToType(expr, toType, locator);
  }

  // If there was no conversion member, look for a constructor member.
  // This is only used for handling interpolated string literals, where
  // we allow construction or conversion.
  storedLocator
    = cs.getConstraintLocator(
        locator.withPathElement(ConstraintLocator::ConstructorMember));
  knownOverload = solution.overloadChoices.find(storedLocator);
  
  // Could not find a user conversion.
  if(knownOverload == solution.overloadChoices.end()) {
    tc.diagnose(expr->getLoc(), diag::could_not_find_user_conversion,
                expr->getType(), toType);
    return nullptr;
  }

  auto selected = knownOverload->second;

  // FIXME: Location information is suspect throughout.
  // Form a reference to the constructor.

  // Form a reference to the constructor or enum declaration.
  // FIXME: Bogus location info.
  Expr *typeBase = TypeExpr::createImplicitHack(expr->getStartLoc(), toType,
                                                 tc.Context);
  Expr *declRef = buildMemberRef(typeBase,
                                 selected.openedFullType,
                                 expr->getStartLoc(),
                                 selected.choice.getDecl(),
                                 expr->getStartLoc(),
                                 selected.openedType,
                                 storedLocator,
                                 /*Implicit=*/true, /*direct ivar*/false);

  // FIXME: Lack of openedType here is an issue.
  ApplyExpr *apply = new (tc.Context) CallExpr(declRef, expr,
                                               /*Implicit=*/true);
  expr = finishApply(apply, toType, locator);
  if (!expr)
    return nullptr;

  return coerceToType(expr, toType, locator);
}

static uint getOptionalBindDepth(const BoundGenericType *bgt) {
  
  if (bgt->getDecl()->classifyAsOptionalType()) {
    auto tyarg = bgt->getGenericArgs()[0];
    
    uint innerDepth = 0;
    
    if (auto wrappedBGT = dyn_cast<BoundGenericType>(tyarg->
                                                     getCanonicalType())) {
      innerDepth = getOptionalBindDepth(wrappedBGT);
    }
    
    return 1 + innerDepth;
  }
  
  return 0;
}

static Type getOptionalBaseType(const Type &type) {
  
  if (auto bgt = dyn_cast<BoundGenericType>(type->
                                            getCanonicalType())) {
    if (bgt->getDecl()->classifyAsOptionalType()) {
      return getOptionalBaseType(bgt->getGenericArgs()[0]);
    }
  }
  
  return type;
}

Expr *ExprRewriter::coerceOptionalToOptional(Expr *expr, Type toType,
                                             ConstraintLocatorBuilder locator) {
  auto &tc = cs.getTypeChecker();
  Type fromType = expr->getType();
  
  auto fromGenericType = fromType->castTo<BoundGenericType>();
  auto toGenericType = toType->castTo<BoundGenericType>();
  assert(fromGenericType->getDecl()->classifyAsOptionalType());
  assert(toGenericType->getDecl()->classifyAsOptionalType());
  tc.requireOptionalIntrinsics(expr->getLoc());
  
  Type fromValueType = fromGenericType->getGenericArgs()[0];
  Type toValueType = toGenericType->getGenericArgs()[0];

  
  // If the option kinds are the same, and the wrapped types are the same,
  // but the arities are different, we can peephole the optional-to-optional
  // conversion into a series of nested injections.
  auto toDepth = getOptionalBindDepth(toGenericType);
  auto fromDepth = getOptionalBindDepth(fromGenericType);
  
  if (toDepth > fromDepth) {
    
    auto toBaseType = getOptionalBaseType(toGenericType);
    auto fromBaseType = getOptionalBaseType(fromGenericType);
    
    if ((toGenericType->getDecl() == fromGenericType->getDecl()) &&
        toBaseType->isEqual(fromBaseType)) {
    
      auto diff = toDepth - fromDepth;
      auto isIUO = fromGenericType->getDecl()->
                    classifyAsOptionalType() == OTK_ImplicitlyUnwrappedOptional;
      
      while (diff) {
        const Type &t = expr->getType();
        const Type &wrapped = isIUO ?
                                Type(ImplicitlyUnwrappedOptionalType::get(t)) :
                                Type(OptionalType::get(t));
        expr = new (tc.Context) InjectIntoOptionalExpr(expr, wrapped);
        diagnoseOptionalInjection(cast<InjectIntoOptionalExpr>(expr));
        diff--;
      }
      
      return expr;
    }
  }

  expr = new (tc.Context) BindOptionalExpr(expr, expr->getSourceRange().End,
                                           /*depth*/ 0, fromValueType);
  expr->setImplicit(true);
  expr = coerceToType(expr, toValueType, locator);
  if (!expr) return nullptr;
      
  expr = new (tc.Context) InjectIntoOptionalExpr(expr, toType);
      
  expr = new (tc.Context) OptionalEvaluationExpr(expr, toType);
  expr->setImplicit(true);
  return expr;
}

Expr *ExprRewriter::coerceImplicitlyUnwrappedOptionalToValue(Expr *expr, Type objTy,
                                            ConstraintLocatorBuilder locator) {
  auto optTy = expr->getType();
  // Coerce to an r-value.
  if (optTy->is<LValueType>())
    objTy = LValueType::get(objTy);

  expr = new (cs.getTypeChecker().Context) ForceValueExpr(expr, expr->getEndLoc());
  expr->setType(objTy);
  expr->setImplicit();
  return expr;
}

Expr *ExprRewriter::coerceCallArguments(Expr *arg, Type paramType,
                                        ConstraintLocatorBuilder locator) {
  // If the types match exactly, there's nothing to do.
  // FIXME: This is propping up string literals, but it feels wrong.
  if (arg->getType()->isEqual(paramType))
    return arg;

  // Determine the parameter bindings.
  MatchCallArgumentListener listener;
  TupleTypeElt paramScalar;
  ArrayRef<TupleTypeElt> paramTuple = decomposeArgParamType(paramType,
                                                            paramScalar);
  TupleTypeElt argScalar;
  ArrayRef<TupleTypeElt> argTupleElts = decomposeArgParamType(arg->getType(),
                                                              argScalar);
  SmallVector<ParamBinding, 4> parameterBindings;
  bool failed = constraints::matchCallArguments(argTupleElts, paramTuple,
                                                /*allowFixes=*/false, listener,
                                                parameterBindings);
  assert(!failed && "Call arguments did not match up?");
  (void)failed;

  // We should either have parentheses or a tuple.
  TupleExpr *argTuple = dyn_cast<TupleExpr>(arg);
  ParenExpr *argParen = dyn_cast<ParenExpr>(arg);
  // FIXME: Eventually, we want to enforce that we have either argTuple or
  // argParen here.

  // Local function to extract the ith argument expression, which papers
  // over some of the weirdness with tuples vs. parentheses.
  auto getArg = [&](unsigned i) -> Expr * {
    if (argTuple)
      return argTuple->getElements()[i];
    assert(i == 0 && "Scalar only has a single argument");

    if (argParen)
      return argParen->getSubExpr();

    return arg;
  };

  // Local function to extract the ith argument label, which papers over some
  // of the weirdndess with tuples vs. parentheses.
  auto getArgLabel = [&](unsigned i) -> Identifier {
    if (argTuple)
      return argTuple->getElementName(i);

    assert(i == 0 && "Scalar only has a single argument");
    return Identifier();
  };

  // Local function to produce a locator to refer to the ith element of the
  // argument tuple.
  auto getArgLocator = [&](unsigned argIdx, unsigned paramIdx)
                         -> ConstraintLocatorBuilder {
    return locator.withPathElement(
             LocatorPathElt::getApplyArgToParam(argIdx, paramIdx));
  };

  // Local function to set the ith argument of the argument.
  auto setArgElement = [&](unsigned i, Expr *e) {
    if (argTuple) {
      argTuple->setElement(i, e);
      return;
    }

    assert(i == 0 && "Scalar with more than one argument?");

    if (argParen) {
      argParen->setSubExpr(e);
      return;
    }

    arg = e;
  };

  auto &tc = getConstraintSystem().getTypeChecker();
  bool anythingShuffled = false;
  SmallVector<TupleTypeElt, 4> toSugarFields;
  SmallVector<TupleTypeElt, 4> fromTupleExprFields(
                                 argTuple? argTuple->getNumElements() : 1);
  SmallVector<ScalarToTupleExpr::Element, 4> scalarToTupleElements;
  SmallVector<Expr *, 2> callerDefaultArgs;
  ConcreteDeclRef defaultArgsOwner = nullptr;
  Expr *injectionFn = nullptr;
  SmallVector<int, 4> sources;
  for (unsigned paramIdx = 0, numParams = parameterBindings.size();
       paramIdx != numParams; ++paramIdx) {
    // Extract the parameter.
    const auto &param = paramTuple[paramIdx];

    // Handle variadic parameters.
    if (param.isVararg()) {
      // FIXME: TupleShuffleExpr cannot handle variadics anywhere other than
      // at the end.
      if (paramIdx != numParams-1) {
        tc.diagnose(arg->getLoc(), diag::tuple_conversion_not_expressible,
                    arg->getType(), paramType);
        return nullptr;
      }

      // Find the appropriate injection function.
      ArraySliceType *sliceType
        = cast<ArraySliceType>(param.getType().getPointer());
      Type boundType = BuiltinIntegerType::getWordType(tc.Context);
      injectionFn = tc.buildArrayInjectionFnRef(cs.DC,
                                                sliceType, boundType,
                                                arg->getStartLoc());
      if (!injectionFn)
        return nullptr;

      // Record this parameter.
      toSugarFields.push_back(param);
      anythingShuffled = true;
      sources.push_back(TupleShuffleExpr::FirstVariadic);

      // Convert the arguments.
      auto paramBaseType = param.getVarargBaseTy();
      for (auto argIdx : parameterBindings[paramIdx]) {
        auto arg = getArg(argIdx);
        auto argType = arg->getType();
        sources.push_back(argIdx);

        // If the argument type exactly matches, this just works.
        if (argType->isEqual(paramBaseType)) {
          fromTupleExprFields[argIdx] = TupleTypeElt(argType,
                                                     getArgLabel(argIdx));
          scalarToTupleElements.push_back(ScalarToTupleExpr::Element());
          continue;
        }

        // FIXME: If we're not converting directly from a tuple expression,
        // we can't express this. LAME!
        if (!argTuple && numParams > 1) {
          tc.diagnose(arg->getLoc(),
                      diag::tuple_conversion_not_expressible,
                      arg->getType(), paramType);
          return nullptr;
        }

        // Convert the argument.
        auto convertedArg = coerceToType(arg, paramBaseType,
                                         getArgLocator(argIdx, paramIdx));
        if (!convertedArg)
          return nullptr;

        // Add the converted argument.
        setArgElement(argIdx, convertedArg);
        fromTupleExprFields[argIdx] = TupleTypeElt(convertedArg->getType(),
                                                   getArgLabel(argIdx));
        scalarToTupleElements.push_back(ScalarToTupleExpr::Element());
      }

      continue;
    }

    // If we are using a default argument, handle it now.
    if (parameterBindings[paramIdx].empty()) {
      // Dig out the owner of the default arguments.
      if (!defaultArgsOwner) {
        defaultArgsOwner
        = findDefaultArgsOwner(cs, solution,
                               cs.getConstraintLocator(locator));
        assert(defaultArgsOwner && "Missing default arguments owner?");
      } else {
        assert(findDefaultArgsOwner(cs, solution,
                                    cs.getConstraintLocator(locator))
               == defaultArgsOwner);
      }

      // Note that we'll be doing a shuffle involving default arguments.
      anythingShuffled = true;
      toSugarFields.push_back(param);

      // Create a caller-side default argument, if we need one.
      if (auto defArg = getCallerDefaultArg(tc, dc, arg->getLoc(),
                                            defaultArgsOwner, paramIdx)) {
        callerDefaultArgs.push_back(defArg);
        sources.push_back(TupleShuffleExpr::CallerDefaultInitialize);
        scalarToTupleElements.push_back(defArg);
      } else {
        sources.push_back(TupleShuffleExpr::DefaultInitialize);
        scalarToTupleElements.push_back(defaultArgsOwner);
      }
      continue;
    }

    // Extract the argument used to initialize this parameter.
    assert(parameterBindings[paramIdx].size() == 1);
    unsigned argIdx = parameterBindings[paramIdx].front();
    auto arg = getArg(argIdx);
    auto argType = arg->getType();

    // If the argument and parameter indices differ, or if the names differ,
    // this is a shuffle.
    sources.push_back(argIdx);
    if (argIdx != paramIdx || getArgLabel(argIdx) != param.getName()) {
      anythingShuffled = true;
    }
    scalarToTupleElements.push_back(ScalarToTupleExpr::Element());

    // If the types exactly match, this is easy.
    auto paramType = param.getType();
    if (argType->isEqual(paramType)) {
      toSugarFields.push_back(TupleTypeElt(argType, param.getName()));
      fromTupleExprFields[argIdx] = TupleTypeElt(paramType, param.getName());
      continue;
    }

    // Convert the argument.
    auto convertedArg = coerceToType(arg, paramType, getArgLocator(argIdx,
                                                                   paramIdx));
    if (!convertedArg)
      return nullptr;

    // Add the converted argument.
    setArgElement(argIdx, convertedArg);
    fromTupleExprFields[argIdx] = TupleTypeElt(convertedArg->getType(),
                                               getArgLabel(argIdx));
    toSugarFields.push_back(TupleTypeElt(argType, param.getName()));
  }

  // Compute the updated 'from' tuple type, since we may have
  // performed some conversions in place.
  Type argTupleType = TupleType::get(fromTupleExprFields, tc.Context);
  if (argTuple) {
    argTuple->setType(anythingShuffled? argTupleType : paramType);
  } else {
    arg->setType(anythingShuffled? argTupleType : paramType);
  }

  // If we don't have to shuffle anything, we're done.
  if (!anythingShuffled)
    return arg;

  // If we came from a scalar, create a scalar-to-tuple conversion.
  if (!argTuple) {
    auto elements = tc.Context.AllocateCopy(scalarToTupleElements);
    return new (tc.Context) ScalarToTupleExpr(arg, paramType, elements,
                                              injectionFn);
  }

  // Create the tuple shuffle.
  ArrayRef<int> mapping = tc.Context.AllocateCopy(sources);
  auto callerDefaultArgsCopy = tc.Context.AllocateCopy(callerDefaultArgs);
  auto shuffle = new (tc.Context) TupleShuffleExpr(arg, mapping,
                                                   defaultArgsOwner,
                                                   callerDefaultArgsCopy,
                                                   paramType);
  shuffle->setVarargsInjectionFunction(injectionFn);
  return shuffle;
}

Expr *ExprRewriter::coerceToType(Expr *expr, Type toType,
                                 ConstraintLocatorBuilder locator) {
  auto &tc = cs.getTypeChecker();

  // The type we're converting from.
  Type fromType = expr->getType();

  // If the types are already equivalent, we don't have to do anything.
  if (fromType->isEqual(toType))
    return expr;

  // If the solver recorded what we should do here, just do it immediately.
  auto knownRestriction = solution.ConstraintRestrictions.find(
                            { fromType->getCanonicalType(),
                              toType->getCanonicalType() });
  if (knownRestriction != solution.ConstraintRestrictions.end()) {
    switch (knownRestriction->second) {
    case ConversionRestrictionKind::TupleToTuple: {
      auto fromTuple = expr->getType()->castTo<TupleType>();
      auto toTuple = toType->castTo<TupleType>();
      SmallVector<int, 4> sources;
      SmallVector<unsigned, 4> variadicArgs;
      bool failed = computeTupleShuffle(fromTuple, toTuple, sources,
                                        variadicArgs,
                                        hasMandatoryTupleLabels(expr));
      assert(!failed && "Couldn't convert tuple to tuple?");
      (void)failed;
      return coerceTupleToTuple(expr, fromTuple, toTuple, locator, sources,
                                variadicArgs);
    }

    case ConversionRestrictionKind::ScalarToTuple: {
      auto toTuple = toType->castTo<TupleType>();
      return coerceScalarToTuple(expr, toTuple,
                                 toTuple->getFieldForScalarInit(), locator);
    }

    case ConversionRestrictionKind::TupleToScalar: {
      // If this was a single-element tuple expression, reach into that
      // subexpression.
      // FIXME: This is a hack to deal with @lvalue-ness issues. It loses
      // source information.
      if (auto fromTupleExpr = dyn_cast<TupleExpr>(expr)) {
        if (fromTupleExpr->getNumElements() == 1) {
          return coerceToType(fromTupleExpr->getElement(0), toType,
                              locator.withPathElement(
                                LocatorPathElt::getTupleElement(0)));
        }
      }

      // Extract the element.
      auto fromTuple = fromType->castTo<TupleType>();
      expr = new (cs.getASTContext()) TupleElementExpr(
                                        expr,
                                        expr->getLoc(),
                                        0,
                                        expr->getLoc(),
                                        fromTuple->getElementType(0));
      expr->setImplicit(true);

      // Coerce the element to the expected type.
      return coerceToType(expr, toType,
                          locator.withPathElement(
                            LocatorPathElt::getTupleElement(0)));
    }

    case ConversionRestrictionKind::DeepEquality:
      llvm_unreachable("Equality handled above");

    case ConversionRestrictionKind::Superclass: {
      // Coercion from archetype to its (concrete) superclass.
      if (auto fromArchetype = fromType->getAs<ArchetypeType>()) {
        expr = new (tc.Context) ArchetypeToSuperExpr(
                                  expr,
                                  fromArchetype->getSuperclass());

        // If we are done succeeded, use the coerced result.
        if (expr->getType()->isEqual(toType)) {
          return expr;
        }

        fromType = expr->getType();
      }
      
      // Coercion from subclass to superclass.
      return new (tc.Context) DerivedToBaseExpr(expr, toType);
    }

    case ConversionRestrictionKind::LValueToRValue: {
      // Load from the lvalue.
      expr = new (tc.Context) LoadExpr(expr, fromType->getRValueType());

      // Coerce the result.
      return coerceToType(expr, toType, locator);
    }

    case ConversionRestrictionKind::Existential:
      return coerceExistential(expr, toType, locator);

    case ConversionRestrictionKind::ClassMetatypeToAnyObject: {
      return new (tc.Context) ClassMetatypeToObjectExpr(expr, toType);
    }
    case ConversionRestrictionKind::ExistentialMetatypeToAnyObject: {
      return new (tc.Context) ExistentialMetatypeToObjectExpr(expr, toType);
    }
    case ConversionRestrictionKind::ProtocolMetatypeToProtocolClass: {
      return new (tc.Context) ProtocolMetatypeToObjectExpr(expr, toType);
    }
        
    case ConversionRestrictionKind::ValueToOptional: {
      auto toGenericType = toType->castTo<BoundGenericType>();
      assert(toGenericType->getDecl()->classifyAsOptionalType());
      tc.requireOptionalIntrinsics(expr->getLoc());

      Type valueType = toGenericType->getGenericArgs()[0];
      expr = coerceToType(expr, valueType, locator);
      if (!expr) return nullptr;

      auto *result = new (tc.Context) InjectIntoOptionalExpr(expr, toType);
      diagnoseOptionalInjection(result);
      return result;
    }

    case ConversionRestrictionKind::OptionalToImplicitlyUnwrappedOptional:
    case ConversionRestrictionKind::ImplicitlyUnwrappedOptionalToOptional:
    case ConversionRestrictionKind::OptionalToOptional:
      return coerceOptionalToOptional(expr, toType, locator);

    case ConversionRestrictionKind::ForceUnchecked: {
      auto valueTy = fromType->getImplicitlyUnwrappedOptionalObjectType();
      assert(valueTy);
      expr = coerceImplicitlyUnwrappedOptionalToValue(expr, valueTy, locator);
      if (!expr) return nullptr;
      return coerceToType(expr, toType, locator);
    }

    case ConversionRestrictionKind::ArrayUpcast: {
      // Look through implicitly unwrapped optionals.
      if (auto objTy
              = cs.lookThroughImplicitlyUnwrappedOptionalType(
                                                          expr->getType())) {
        expr = coerceImplicitlyUnwrappedOptionalToValue(expr, objTy, locator);
        if (!expr) return nullptr;
      }

      // Form the upcast.
      bool isBridged = !cs.getBaseTypeForArrayType(fromType.getPointer())
                          ->isBridgeableObjectType();
      return new (tc.Context) CollectionUpcastConversionExpr(expr, toType,
                                                             isBridged);
    }

    case ConversionRestrictionKind::DictionaryUpcast: {
      // Look through implicitly unwrapped optionals.
      if (auto objTy
            = cs.lookThroughImplicitlyUnwrappedOptionalType(expr->getType())) {
        expr = coerceImplicitlyUnwrappedOptionalToValue(expr, objTy, locator);
        if (!expr) return nullptr;
      }

      // If the source key and value types are object types, this is an upcast.
      // Otherwise, it's bridged.
      Type sourceKey, sourceValue;
      std::tie(sourceKey, sourceValue) = *cs.isDictionaryType(expr->getType());

      bool isBridged = !sourceKey->isBridgeableObjectType() ||
                       !sourceValue->isBridgeableObjectType();
      return new (tc.Context) CollectionUpcastConversionExpr(expr, toType,
                                                             isBridged);
    }

    case ConversionRestrictionKind::User: {
      tc.requirePointerArgumentIntrinsics(expr->getLoc());
      return coerceViaUserConversion(expr, toType, locator);
    }
    
    case ConversionRestrictionKind::InoutToPointer: {
      tc.requirePointerArgumentIntrinsics(expr->getLoc());
      return new (tc.Context) InOutToPointerExpr(expr, toType);
    }
    
    case ConversionRestrictionKind::ArrayToPointer: {
      tc.requirePointerArgumentIntrinsics(expr->getLoc());
      return new (tc.Context) ArrayToPointerExpr(expr, toType);
    }
    
    case ConversionRestrictionKind::StringToPointer: {
      tc.requirePointerArgumentIntrinsics(expr->getLoc());
      return new (tc.Context) StringToPointerExpr(expr, toType);
    }
    
    case ConversionRestrictionKind::PointerToPointer: {
      tc.requirePointerArgumentIntrinsics(expr->getLoc());
      return new (tc.Context) PointerToPointerExpr(expr, toType);
    }

    case ConversionRestrictionKind::BridgeToObjC: {
      Expr *objcExpr = bridgeToObjectiveC(expr);
      if (!objcExpr)
        return nullptr;

      return coerceToType(objcExpr, toType, locator);
    }

    case ConversionRestrictionKind::BridgeFromObjC:
      return bridgeFromObjectiveC(expr, toType);
    }
  }

  // Tuple-to-scalar conversion.
  // FIXME: Will go away when tuple labels go away.
  if (auto fromTuple = fromType->getAs<TupleType>()) {
    if (fromTuple->getNumElements() == 1 &&
        !fromTuple->getFields()[0].isVararg() &&
        !toType->is<TupleType>()) {
      expr = new (cs.getASTContext()) TupleElementExpr(
                                        expr,
                                        expr->getLoc(),
                                        0,
                                        expr->getLoc(),
                                        fromTuple->getElementType(0));
      expr->setImplicit(true);
    }
  }

  // Coercions from an lvalue: load or perform implicit address-of. We perform
  // these coercions first because they are often the first step in a multi-step
  // coercion.
  if (auto fromLValue = fromType->getAs<LValueType>()) {
    if (auto *toIO = toType->getAs<InOutType>()) {
      (void)toIO;
      // In an 'inout' operator like "++i", the operand is converted from
      // an implicit lvalue to an inout argument.
      assert(toIO->getObjectType()->isEqual(fromLValue->getObjectType()));
      return new (tc.Context) InOutExpr(expr->getStartLoc(), expr,
                                            toType, /*isImplicit*/true);
    }

    // If we're actually turning this into an lvalue tuple element, don't
    // load.
    bool performLoad = true;
    if (auto toTuple = toType->getAs<TupleType>()) {
      int scalarIdx = toTuple->getFieldForScalarInit();
      if (scalarIdx >= 0 &&
          toTuple->getElementType(scalarIdx)->is<InOutType>())
        performLoad = false;
    }

    if (performLoad) {
      // Load from the lvalue.
      expr = new (tc.Context) LoadExpr(expr, fromLValue->getObjectType());

      // Coerce the result.
      return coerceToType(expr, toType, locator);
    }
  }

  // Coercions to tuple type.
  if (auto toTuple = toType->getAs<TupleType>()) {
    // Coerce from a tuple to a tuple.
    if (auto fromTuple = fromType->getAs<TupleType>()) {
      SmallVector<int, 4> sources;
      SmallVector<unsigned, 4> variadicArgs;
      if (!computeTupleShuffle(fromTuple, toTuple, sources, variadicArgs,
                               hasMandatoryTupleLabels(expr))) {
        return coerceTupleToTuple(expr, fromTuple, toTuple,
                                  locator, sources, variadicArgs);
      }
    }

    // Coerce scalar to tuple.
    int toScalarIdx = toTuple->getFieldForScalarInit();
    if (toScalarIdx != -1) {
      return coerceScalarToTuple(expr, toTuple, toScalarIdx, locator);
    }
  }

  // Coercion from a subclass to a superclass.
  if (fromType->mayHaveSuperclass() &&
      toType->getClassOrBoundGenericClass()) {
    for (auto fromSuperClass = tc.getSuperClassOf(fromType);
         fromSuperClass;
         fromSuperClass = tc.getSuperClassOf(fromSuperClass)) {
      if (fromSuperClass->isEqual(toType)) {
        
        // Coercion from archetype to its (concrete) superclass.
        if (auto fromArchetype = fromType->getAs<ArchetypeType>()) {
          expr = new (tc.Context) ArchetypeToSuperExpr(
                                    expr,
                                    fromArchetype->getSuperclass());

          // If we succeeded, use the coerced result.
          if (expr->getType()->isEqual(toType))
            return expr;
        }

        // Coercion from subclass to superclass.
        expr = new (tc.Context) DerivedToBaseExpr(expr, toType);
        return expr;
      }
    }
  }

  // Coercions to function type.
  if (auto toFunc = toType->getAs<FunctionType>()) {
    // Coercion to an autoclosure type produces an implicit closure.
    // FIXME: The type checker is more lenient, and allows @autoclosures to
    // be subtypes of non-@autoclosures, which is bogus.
    if (toFunc->isAutoClosure()) {
      // Convert the value to the expected result type of the function.
      expr = coerceToType(expr, toFunc->getResult(),
                          locator.withPathElement(ConstraintLocator::Load));

      // We'll set discriminator values on all the autoclosures in a
      // later pass.
      auto discriminator = AutoClosureExpr::InvalidDiscriminator;
      auto Closure = new (tc.Context) AutoClosureExpr(expr, toType,
                                                      discriminator, dc);
      Pattern *pattern = TuplePattern::create(tc.Context, expr->getLoc(),
                                              ArrayRef<TuplePatternElt>(),
                                              expr->getLoc());
      pattern->setType(TupleType::getEmpty(tc.Context));
      Closure->setParams(pattern);

      // Compute the capture list, now that we have analyzed the expression.
      tc.computeCaptures(Closure);

      return Closure;
    }

    // Coercion from one function type to another.
    auto fromFunc = fromType->getAs<FunctionType>();
    if (fromFunc) {
      return new (tc.Context) FunctionConversionExpr(expr, toType);
    }
  }

  // Coercions from a type to an existential type.
  if (toType->isExistentialType()) {
    return coerceExistential(expr, toType, locator);
  }

  // Coercions to an existential metatype.
  if (toType->is<ExistentialMetatypeType>()) {
    return coerceExistentialMetatype(expr, toType, locator);
  }

  // Coercion to Optional<T>.
  if (auto toGenericType = toType->getAs<BoundGenericType>()) {
    if (toGenericType->getDecl()->classifyAsOptionalType()) {
      tc.requireOptionalIntrinsics(expr->getLoc());

      Type valueType = toGenericType->getGenericArgs()[0];
      expr = coerceToType(expr, valueType, locator);
      if (!expr) return nullptr;

      auto *result = new (tc.Context) InjectIntoOptionalExpr(expr, toType);
      diagnoseOptionalInjection(result);
      return result;
    }
  }

  // Coerce via conversion function or constructor.
  if (fromType->getNominalOrBoundGenericNominal()||
      fromType->is<ArchetypeType>() ||
      toType->getNominalOrBoundGenericNominal() ||
      toType->is<ArchetypeType>()) {
    return coerceViaUserConversion(expr, toType, locator);
  }

  // Coercion from one metatype to another.
  if (fromType->is<MetatypeType>()) {
    auto toMeta = toType->castTo<MetatypeType>();
    return new (tc.Context) MetatypeConversionExpr(expr, toMeta);
  }

  llvm_unreachable("Unhandled coercion");
}

Expr *
ExprRewriter::coerceObjectArgumentToType(Expr *expr,
                                         Type baseTy, ValueDecl *member,
                                         bool IsDirectPropertyAccess,
                                         ConstraintLocatorBuilder locator) {
  Type toType = adjustSelfTypeForMember(baseTy, member, IsDirectPropertyAccess,
                                        dc);

  // If our expression already has the right type, we're done.
  Type fromType = expr->getType();
  if (fromType->isEqual(toType))
    return expr;

  // If we're coercing to an rvalue type, just do it.
  if (!toType->is<InOutType>())
    return coerceToType(expr, toType, locator);

  assert(fromType->is<LValueType>() && "Can only convert lvalues to inout");

  auto &ctx = cs.getTypeChecker().Context;

  // Use InOutExpr to convert it to an explicit inout argument for the
  // receiver.
  return new (ctx) InOutExpr(expr->getStartLoc(), expr,
                                 toType, /*isImplicit*/true);
}

Expr *ExprRewriter::convertLiteral(Expr *literal,
                                   Type type,
                                   Type openedType,
                                   ProtocolDecl *protocol,
                                   TypeOrName literalType,
                                   Identifier literalFuncName,
                                   ProtocolDecl *builtinProtocol,
                                   TypeOrName builtinLiteralType,
                                   Identifier builtinLiteralFuncName,
                                   bool (*isBuiltinArgType)(Type),
                                   Diag<> brokenProtocolDiag,
                                   Diag<> brokenBuiltinProtocolDiag) {
  auto &tc = cs.getTypeChecker();

  // Check whether this literal type conforms to the builtin protocol.
  ProtocolConformance *builtinConformance = nullptr;
  if (builtinProtocol &&
      tc.conformsToProtocol(type, builtinProtocol, cs.DC, &builtinConformance)){
    // Find the builtin argument type we'll use.
    Type argType;
    if (builtinLiteralType.is<Type>())
      argType = builtinLiteralType.get<Type>();
    else
      argType = tc.getWitnessType(type, builtinProtocol,
                                  builtinConformance,
                                  builtinLiteralType.get<Identifier>(),
                                  brokenBuiltinProtocolDiag);

    if (!argType)
      return nullptr;

    // Make sure it's of an appropriate builtin type.
    if (isBuiltinArgType && !isBuiltinArgType(argType)) {
      tc.diagnose(builtinProtocol->getLoc(), brokenBuiltinProtocolDiag);
      return nullptr;
    }

    // The literal expression has this type.
    literal->setType(argType);

    // Call the builtin conversion operation.
    // FIXME: Bogus location info.
    Expr *base = TypeExpr::createImplicitHack(literal->getLoc(), type,
                                              tc.Context);
    Expr *result = tc.callWitness(base, dc,
                                  builtinProtocol, builtinConformance,
                                  builtinLiteralFuncName,
                                  literal,
                                  brokenBuiltinProtocolDiag);
    if (result)
      result->setType(type);
    return result;
  }

  // This literal type must conform to the (non-builtin) protocol.
  assert(protocol && "requirements should have stopped recursion");
  ProtocolConformance *conformance = nullptr;
  bool conforms = tc.conformsToProtocol(type, protocol, cs.DC, &conformance);
  assert(conforms && "must conform to literal protocol");
  (void)conforms;

  // Figure out the (non-builtin) argument type if there is one.
  Type argType;
  if (literalType.is<Identifier>() &&
      literalType.get<Identifier>().empty()) {
    // If there is no argument to the constructor function, then just pass in
    // the empty tuple.
    literal = TupleExpr::createEmpty(tc.Context, literal->getLoc(),
                                     literal->getLoc(), /*implicit*/true);
  } else {
    // Otherwise, figure out the type of the constructor function and coerce to
    // it.
    if (literalType.is<Type>())
      argType = literalType.get<Type>();
    else
      argType = tc.getWitnessType(type, protocol, conformance,
                                  literalType.get<Identifier>(),
                                  brokenProtocolDiag);
    if (!argType)
      return nullptr;

    // Convert the literal to the non-builtin argument type via the
    // builtin protocol, first.
    // FIXME: Do we need an opened type here?
    literal = convertLiteral(literal, argType, argType, nullptr, Identifier(),
                             Identifier(), builtinProtocol,
                             builtinLiteralType, builtinLiteralFuncName,
                             isBuiltinArgType, brokenProtocolDiag,
                             brokenBuiltinProtocolDiag);
    if (!literal)
      return nullptr;
  }

  // Convert the resulting expression to the final literal type.
  // FIXME: Bogus location info.
  Expr *base = TypeExpr::createImplicitHack(literal->getLoc(), type,
                                            tc.Context);
  literal = tc.callWitness(base, dc,
                           protocol, conformance, literalFuncName,
                           literal, brokenProtocolDiag);
  if (literal)
    literal->setType(type);
  return literal;
}

Expr *ExprRewriter::finishApply(ApplyExpr *apply, Type openedType,
                                ConstraintLocatorBuilder locator) {
  TypeChecker &tc = cs.getTypeChecker();
  
  auto fn = apply->getFn();
  
  // The function is always an rvalue.
  fn = tc.coerceToRValue(fn);
  assert(fn && "Rvalue conversion failed?");
  if (!fn)
    return nullptr;
  
  // Handle applications that implicitly look through ImplicitlyUnwrappedOptional<T>.
  if (auto fnTy = cs.lookThroughImplicitlyUnwrappedOptionalType(fn->getType())) {
    fn = coerceImplicitlyUnwrappedOptionalToValue(fn, fnTy, locator);
    if (!fn) return nullptr;
  }

  // If we're applying a function that resulted from a covariant
  // function conversion, strip off that conversion.
  // FIXME: It would be nicer if we could build the ASTs properly in the
  // first shot.
  Type covariantResultType;
  if (auto covariant = dyn_cast<CovariantFunctionConversionExpr>(fn)) {
    // Strip off one layer of application from the covariant result.
    covariantResultType
      = covariant->getType()->castTo<AnyFunctionType>()->getResult();
   
    // Use the subexpression as the function.
    fn = covariant->getSubExpr();
  }

  apply->setFn(fn);

  // Check whether the argument is 'super'.
  bool isSuper = apply->getArg()->isSuperExpr();

  // For function application, convert the argument to the input type of
  // the function.
  if (auto fnType = fn->getType()->getAs<FunctionType>()) {
    auto origArg = apply->getArg();

    Expr *arg = coerceCallArguments(origArg, fnType->getInput(),
                                    locator.withPathElement(
                                      ConstraintLocator::ApplyArgument));
    if (!arg) {
      return nullptr;
    }

    apply->setArg(arg);
    apply->setType(fnType->getResult());
    apply->setIsSuper(isSuper);

    assert(!apply->getType()->is<PolymorphicFunctionType>() &&
           "Polymorphic function type slipped through");
    Expr *result = tc.substituteInputSugarTypeForResult(apply);

    // If the result is an archetype from an opened existential, erase
    // the existential and create the OpenExistentialExpr.
    // FIXME: This is a localized form of a much more general rule for
    // placement of open existential expressions. It only works for 
    // DynamicSelf.
    OptionalTypeKind optKind;
    auto resultTy = result->getType();
    if (auto optValueTy = resultTy->getAnyOptionalObjectType(optKind)) {
      resultTy = optValueTy;
    }
    if (auto archetypeTy = resultTy->getAs<ArchetypeType>()) {
      auto opened = OpenedExistentials.find(archetypeTy);
      if (opened != OpenedExistentials.end()) {
        // Erase the archetype to its corresponding existential:
        auto openedTy = archetypeTy->getOpenedExistentialType();

        //   - Drill down to the optional value (if necessary).
        if (optKind) {
          result = new (tc.Context) BindOptionalExpr(result,
                                                     result->getEndLoc(),
                                                     0, archetypeTy);
          result->setImplicit(true);
        }

        //   - Coerce to an existential value.
        result = coerceToType(result, openedTy, locator);
        if (!result)
          return result;

        //   - Bind up the result back up as an optional (if necessary).
        if (optKind) {
          Type optOpenedTy = OptionalType::get(optKind, openedTy);
          result = new (tc.Context) InjectIntoOptionalExpr(result, optOpenedTy);
          result = new (tc.Context) OptionalEvaluationExpr(result, optOpenedTy);
        }

        // Create the expression that opens the existential.
        result = new (tc.Context) OpenExistentialExpr(
                                    opened->second.ExistentialValue,
                                    opened->second.OpaqueValue,
                                    result);        

        // Remove this from the set of opened existentials.
        OpenedExistentials.erase(opened);
      }
    }

    // If we have a covariant result type, perform the conversion now.
    if (covariantResultType) {
      if (covariantResultType->is<FunctionType>())
        result = new (tc.Context) CovariantFunctionConversionExpr(
                                    result,
                                    covariantResultType);
      else
        result = new (tc.Context) CovariantReturnConversionExpr(
                                    result, 
                                    covariantResultType);
    }

    return result;
  }

  // We have a type constructor.
  auto metaTy = fn->getType()->castTo<AnyMetatypeType>();
  auto ty = metaTy->getInstanceType();

  // If we're "constructing" a tuple type, it's simply a conversion.
  if (auto tupleTy = ty->getAs<TupleType>()) {
    // FIXME: Need an AST to represent this properly.
    return coerceToType(apply->getArg(), tupleTy, locator);
  }

  // We're constructing a value of nominal type. Look for the constructor or
  // enum element to use.
  assert(ty->getNominalOrBoundGenericNominal() || ty->is<DynamicSelfType>() ||
         ty->is<ArchetypeType>() || ty->isExistentialType());
  auto selected = getOverloadChoiceIfAvailable(
                    cs.getConstraintLocator(
                      locator.withPathElement(
                        ConstraintLocator::ConstructorMember)));

  // We have the constructor.
  auto choice = selected->choice;
  auto decl = choice.getDecl();

  // Consider the constructor decl reference expr 'implicit', but the
  // constructor call expr itself has the apply's 'implicitness'.
  Expr *declRef = buildMemberRef(fn,
                                 selected->openedFullType,
                                 /*DotLoc=*/SourceLoc(),
                                 decl, fn->getEndLoc(),
                                 selected->openedType, locator,
                                 /*Implicit=*/true, /*direct ivar*/false);
  declRef->setImplicit(apply->isImplicit());
  apply->setFn(declRef);

  // If we're constructing a class object, either the metatype must be
  // statically derived (rather than an arbitrary value of metatype type) or
  // the referenced constructor must be abstract.
  if ((ty->getClassOrBoundGenericClass() || ty->is<DynamicSelfType>()) &&
      !fn->isStaticallyDerivedMetatype() &&
      !decl->hasClangNode() &&
      !cast<ConstructorDecl>(decl)->isRequired()) {
    tc.diagnose(apply->getLoc(), diag::dynamic_construct_class, ty)
      .highlight(fn->getSourceRange());
    auto ctor = cast<ConstructorDecl>(decl);
    // FIXME: Better description of the initializer than just it's type.
    if (ctor->isImplicit())
      tc.diagnose(decl, diag::note_nonrequired_implicit_initializer,
                  ctor->getArgumentType());
    else
      tc.diagnose(decl, diag::note_nonrequired_initializer);
  } else if (isa<ConstructorDecl>(decl) && ty->isExistentialType() &&
             fn->isStaticallyDerivedMetatype()) {
    tc.diagnose(apply->getLoc(), diag::static_construct_existential, ty)
      .highlight(fn->getSourceRange());
  }

  // Tail-recur to actually call the constructor.
  return finishApply(apply, openedType, locator);
}

/// Diagnose a relabel-tuple 
///
/// \returns true if we successfully diagnosed the issue.
static bool diagnoseRelabel(TypeChecker &tc, Expr *expr, 
                            ArrayRef<Identifier> newNames,
                            bool isSubscript) {
  auto tuple = dyn_cast<TupleExpr>(expr);
  if (!tuple) {
    if (newNames[0].empty()) {
      // This is probably a conversion from a value of labeled tuple type to
      // a scalar.
      // FIXME: We want this issue to disappear completely when single-element
      // labelled tuples go away.
      if (auto tupleTy = expr->getType()->getRValueType()->getAs<TupleType>()) {
        int scalarFieldIdx = tupleTy->getFieldForScalarInit();
        if (scalarFieldIdx >= 0) {
          auto &field = tupleTy->getFields()[scalarFieldIdx];
          if (field.hasName()) {
            llvm::SmallString<16> str;
            str = ".";
            str += field.getName().str();
            auto insertLoc = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                                        expr->getEndLoc());
            tc.diagnose(expr->getStartLoc(),
                        diag::extra_named_single_element_tuple,
                        field.getName().str())
              .fixItInsert(insertLoc, str);
            return true;
          }
        }
      }

      // We don't know what to do with this.
      return false;
    }

    // This is a scalar-to-tuple conversion. Add the name.  We "know"
    // that we're inside a ParenExpr, because ParenExprs are required
    // by the syntax and locator resolution looks through on level of
    // them.

    // Look through the paren expression, if there is one.
    if (auto parenExpr = dyn_cast<ParenExpr>(expr))
      expr = parenExpr->getSubExpr();

    llvm::SmallString<16> str;
    str += newNames[0].str();
    str += ": ";
    tc.diagnose(expr->getStartLoc(), diag::missing_argument_labels, false,
                str.substr(0, str.size()-1), isSubscript)
      .fixItInsert(expr->getStartLoc(), str);
    return true;
  }

  // Figure out how many extraneous, missing, and wrong labels are in
  // the call.
  unsigned numExtra = 0, numMissing = 0, numWrong = 0;
  unsigned n = std::max(tuple->getNumElements(), (unsigned)newNames.size());

  llvm::SmallString<16> missingBuffer;
  llvm::SmallString<16> extraBuffer;
  for (unsigned i = 0; i != n; ++i) {
    Identifier oldName;
    if (i < tuple->getNumElements())
      oldName = tuple->getElementName(i);
    Identifier newName;
    if (i < newNames.size())
      newName = newNames[i];

    if (oldName == newName)
      continue;

    if (oldName.empty()) {
      ++numMissing;
      missingBuffer += newName.str();
      missingBuffer += ":";
    } else if (newName.empty()) {
      ++numExtra;
      extraBuffer += oldName.str();
      extraBuffer += ':';
    } else
      ++numWrong;
  }

  // Emit the diagnostic.
  assert(numMissing > 0 || numExtra > 0 || numWrong > 0);
  llvm::SmallString<16> haveBuffer; // note: diagOpt has references to this
  llvm::SmallString<16> expectedBuffer; // note: diagOpt has references to this
  Optional<InFlightDiagnostic> diagOpt;

  // If we had any wrong labels, or we have both missing and extra labels,
  // emit the catch-all "wrong labels" diagnostic.
  bool plural = (numMissing + numExtra + numWrong) > 1;
  if (numWrong > 0 || (numMissing > 0 && numExtra > 0)) {
    for(unsigned i = 0, n = tuple->getNumElements(); i != n; ++i) {
      auto haveName = tuple->getElementName(i);
      if (haveName.empty())
        haveBuffer += '_';
      else
        haveBuffer += haveName.str();
      haveBuffer += ':';
    }

    for (auto expected : newNames) {
      if (expected.empty())
        expectedBuffer += '_';
      else
        expectedBuffer += expected.str();
      expectedBuffer += ':';
    }

    StringRef haveStr = haveBuffer;
    StringRef expectedStr = expectedBuffer;
    diagOpt.emplace(tc.diagnose(expr->getLoc(), diag::wrong_argument_labels,
                                plural, haveStr, expectedStr, isSubscript));
  } else if (numMissing > 0) {
    StringRef missingStr = missingBuffer;
    diagOpt.emplace(tc.diagnose(expr->getLoc(), diag::missing_argument_labels,
                                plural, missingStr, isSubscript));
  } else {
    assert(numExtra > 0);
    StringRef extraStr = extraBuffer;
    diagOpt.emplace(tc.diagnose(expr->getLoc(), diag::extra_argument_labels,
                                plural, extraStr, isSubscript));
  }

  // Emit Fix-Its to correct the names.
  auto &diag = *diagOpt;
  for (unsigned i = 0, n = tuple->getNumElements(); i != n; ++i) {
    Identifier oldName = tuple->getElementName(i);
    Identifier newName;
    if (i < newNames.size())
      newName = newNames[i];

    if (oldName == newName)
      continue;

    if (newName.empty()) {
      // Delete the old name.
      diag.fixItRemoveChars(tuple->getElementNameLocs()[i],
                            tuple->getElements()[i]->getStartLoc());
      continue;
    }

    if (oldName.empty()) {
      // Insert the name.
      llvm::SmallString<16> str;
      str += newName.str();
      str += ": ";
      diag.fixItInsert(tuple->getElements()[i]->getStartLoc(), str);
      continue;
    }
    
    // Change the name.
    diag.fixItReplace(tuple->getElementNameLocs()[i], newName.str());
  }

  // FIXME: Fix AST.

  return true;
}

/// \brief Apply a given solution to the expression, producing a fully
/// type-checked expression.
Expr *ConstraintSystem::applySolution(Solution &solution, Expr *expr) {
  // If any fixes needed to be applied to arrive at this solution, resolve
  // them to specific expressions.
  if (!solution.Fixes.empty()) {
    bool diagnosed = false;
    for (auto fix : solution.Fixes) {
      // Some fixes need more information from the locator itself, including
      // tweaking the locator. Deal with those now.
      ConstraintLocator *locator = fix.second;

      // Removing a nullary call to a non-function requires us to have an
      // 'ApplyFunction', which we strip.
      if (fix.first.getKind() == FixKind::RemoveNullaryCall) {
        auto anchor = locator->getAnchor();
        auto path = locator->getPath();
        if (!path.empty() &&
            path.back().getKind() == ConstraintLocator::ApplyFunction) {
          locator = getConstraintLocator(anchor, path.slice(0, path.size()-1),
                                         locator->getSummaryFlags());
        } else {
          continue;
        }
      }

      // Resolve the locator to a specific expression.
      SourceRange range1, range2;
      ConstraintLocator *resolved
        = simplifyLocator(*this, locator, range1, range2);

      // If we didn't manage to resolve directly to an expression, we don't
      // have a great diagnostic to give, so continue.
      if (!resolved || !resolved->getAnchor() || !resolved->getPath().empty())
        continue;

      Expr *affected = resolved->getAnchor();

      switch (fix.first.getKind()) {
      case FixKind::None:
        llvm_unreachable("no-fix marker should never make it into solution");

      case FixKind::NullaryCall: {
        // Dig for the function we want to call.
        auto type = solution.simplifyType(TC, affected->getType())
                      ->getRValueType();
        if (auto tupleTy = type->getAs<TupleType>()) {
          if (auto tuple = dyn_cast<TupleExpr>(affected))
            affected = tuple->getElement(0);
          type = tupleTy->getFields()[0].getType()->getRValueType();
        }

        if (auto optTy = type->getAnyOptionalObjectType())
          type = optTy;

        if (type->is<AnyFunctionType>()) {
          type = type->castTo<AnyFunctionType>()->getResult();
        }
        
        SourceLoc afterAffectedLoc
          = Lexer::getLocForEndOfToken(TC.Context.SourceMgr,
                                       affected->getEndLoc());
        TC.diagnose(affected->getLoc(), diag::missing_nullary_call, type)
          .fixItInsert(afterAffectedLoc, "()");
        diagnosed = true;
        break;
      }

      case FixKind::RemoveNullaryCall: {
        if (auto apply = dyn_cast<ApplyExpr>(affected)) {
          auto type = solution.simplifyType(TC, apply->getFn()->getType())
                        ->getRValueObjectType();
          TC.diagnose(affected->getLoc(), diag::extra_call_nonfunction, type)
            .fixItRemove(apply->getArg()->getSourceRange());
          diagnosed = true;
        }
        break;
      }

      case FixKind::ForceOptional: {
        auto type = solution.simplifyType(TC, affected->getType())
                      ->getRValueObjectType();
        SourceLoc afterAffectedLoc
          = Lexer::getLocForEndOfToken(TC.Context.SourceMgr,
                                       affected->getEndLoc());
        TC.diagnose(affected->getLoc(), diag::missing_unwrap_optional, type)
          .fixItInsert(afterAffectedLoc, "!");
        diagnosed = true;
        break;
      }

      case FixKind::ForceDowncast: {
        auto fromType = solution.simplifyType(TC, affected->getType())
                          ->getRValueObjectType();
        Type toType = solution.simplifyType(TC,
                                            fix.first.getTypeArgument(*this));
        SourceLoc afterAffectedLoc
          = Lexer::getLocForEndOfToken(TC.Context.SourceMgr,
                                       affected->getEndLoc());

        llvm::SmallString<32> asCastStr;
        asCastStr += " as ";
        asCastStr += toType.getString();
        TC.diagnose(affected->getLoc(), diag::missing_forced_downcast, fromType,
                    toType)
          .fixItInsert(afterAffectedLoc, asCastStr);
        diagnosed = true;

        // FIXME: Add parentheses if we now need them.
        break;
      }

      case FixKind::AddressOf: {
        auto type = solution.simplifyType(TC, affected->getType())
                      ->getRValueObjectType();
        TC.diagnose(affected->getLoc(), diag::missing_address_of, type)
          .fixItInsert(affected->getStartLoc(), "&");
        diagnosed = true;
        break;
      }

      case FixKind::TupleToScalar: 
      case FixKind::ScalarToTuple:
      case FixKind::RelabelCallTuple: {
        if (diagnoseRelabel(TC,affected,fix.first.getRelabelTupleNames(*this),
                            /*isSubscript=*/locator->getPath().back().getKind()
                              == ConstraintLocator::SubscriptIndex))
          diagnosed = true;
        break;
      }
      }

      // FIXME: It would be really nice to emit a follow-up note showing where
      // we got the other type information from, e.g., the parameter we're
      // initializing.
    }

    if (diagnosed)
      return nullptr;

    // We didn't manage to diagnose anything well, so fall back to
    // diagnosing mining the system to construct a reasonable error message.
    this->diagnoseFailureFromConstraints(expr);

    return nullptr;
  }

  class ExprWalker : public ASTWalker {
    ExprRewriter &Rewriter;
    unsigned LeftSideOfAssignment = 0;

  public:
    ExprWalker(ExprRewriter &Rewriter) : Rewriter(Rewriter) { }

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      // For a default-value expression, do nothing.
      if (isa<DefaultValueExpr>(expr))
        return { false, expr };

      // For closures, update the parameter types and check the body.
      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        Rewriter.simplifyExprType(expr);
        auto &cs = Rewriter.getConstraintSystem();
        auto &tc = cs.getTypeChecker();

        // Coerce the pattern, in case we resolved something.
        auto fnType = closure->getType()->castTo<FunctionType>();
        Pattern *params = closure->getParams();
        TypeResolutionOptions TROptions;
        TROptions |= TR_OverrideType;
        TROptions |= TR_FromNonInferredPattern;
        if (tc.coercePatternToType(params, closure, fnType->getInput(),
                                   TROptions))
          return { false, nullptr };
        closure->setParams(params);

        // If this is a single-expression closure, convert the expression
        // in the body to the result type of the closure.
        if (closure->hasSingleExpressionBody()) {
          // Enter the context of the closure when type-checking the body.
          llvm::SaveAndRestore<DeclContext *> savedDC(Rewriter.dc, closure);
          Expr *body = closure->getSingleExpressionBody()->walk(*this);
          if (body)
            body = Rewriter.coerceToType(body,
                                         fnType->getResult(),
                                         cs.getConstraintLocator(
                                           closure,
                                           ConstraintLocator::ClosureResult));
          if (!body)
            return { false, nullptr } ;

          closure->setSingleExpressionBody(body);
        } else {
          // For other closures, type-check the body.
          tc.typeCheckClosureBody(closure);
        }

        // Compute the capture list, now that we have type-checked the body.
        tc.computeCaptures(closure);
        return { false, closure };
      }

      // Track whether we're in the left-hand side of an assignment...
      if (auto assign = dyn_cast<AssignExpr>(expr)) {
        ++LeftSideOfAssignment;
        
        if (auto dest = assign->getDest()->walk(*this))
          assign->setDest(dest);
        else
          return { false, nullptr };
        
        --LeftSideOfAssignment;

        auto &cs = Rewriter.getConstraintSystem();
        auto srcLocator = cs.getConstraintLocator(
                            assign,
                            ConstraintLocator::AssignSource);

        if (auto src = assign->getSrc()->walk(*this))
          assign->setSrc(src);
        else
          return { false, nullptr };
        
        expr = Rewriter.visitAssignExpr(assign, srcLocator);
        return { false, expr };
      }
      
      // ...so we can verify that '_' only appears there.
      if (isa<DiscardAssignmentExpr>(expr) && LeftSideOfAssignment == 0)
        Rewriter.getConstraintSystem().getTypeChecker()
          .diagnose(expr->getLoc(), diag::discard_expr_outside_of_assignment);
      
      return { true, expr };
    }

    Expr *walkToExprPost(Expr *expr) override {
      return Rewriter.visit(expr);
    }

    /// \brief Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }

    /// \brief Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }
  };

  ExprRewriter rewriter(*this, solution);
  ExprWalker walker(rewriter);
  auto result = expr->walk(walker);
  return result;
}

Expr *ConstraintSystem::applySolutionShallow(const Solution &solution,
                                             Expr *expr) {
  ExprRewriter rewriter(*this, solution);
  return rewriter.visit(expr);
}

Expr *Solution::coerceToType(Expr *expr, Type toType,
                             ConstraintLocator *locator,
                             bool ignoreTopLevelInjection) const {
  auto &cs = getConstraintSystem();
  ExprRewriter rewriter(cs, *this);
  Expr *result = rewriter.coerceToType(expr, toType, locator);
  if (!result)
    return nullptr;

  // If we were asked to ignore top-level optional injections, mark
  // the top-level injection (if any) as "diagnosed".
  if (ignoreTopLevelInjection) {
    if (auto injection = dyn_cast<InjectIntoOptionalExpr>(
                           result->getSemanticsProvidingExpr())) {
      rewriter.DiagnosedOptionalInjections.insert(injection);
    }
  }

  return result;
}

Expr *TypeChecker::callWitness(Expr *base, DeclContext *dc,
                               ProtocolDecl *protocol,
                               ProtocolConformance *conformance,
                               Identifier name,
                               MutableArrayRef<Expr *> arguments,
                               Diag<> brokenProtocolDiag) {
  // Construct an empty constraint system and solution.
  ConstraintSystem cs(*this, dc, ConstraintSystemOptions());

  // Find the witness we need to use.
  auto type = base->getType();
  if (auto metaType = type->getAs<AnyMetatypeType>())
    type = metaType->getInstanceType();
  
  auto witness = findNamedWitness(*this, dc, type->getRValueType(), protocol,
                                  name, brokenProtocolDiag);
  if (!witness)
    return nullptr;

  // Form a reference to the witness itself.
  Type openedFullType, openedType;
  std::tie(openedFullType, openedType)
    = cs.getTypeOfMemberReference(base->getType(), witness,
                                  /*isTypeReference=*/false,
                                  /*isDynamicResult=*/false);
  auto locator = cs.getConstraintLocator(base);

  // Form the call argument.
  Expr *arg;
  if (arguments.size() == 1)
    arg = arguments[0];
  else {
    SmallVector<TupleTypeElt, 4> elementTypes;
    for (auto elt : arguments)
      elementTypes.push_back(TupleTypeElt(elt->getType()));

    arg = TupleExpr::create(Context,
                            base->getStartLoc(),
                            arguments,
                            witness->getFullName().getArgumentNames(),
                            { },
                            base->getEndLoc(),
                            /*hasTrailingClosure=*/false,
                            /*Implicit=*/true,
                            TupleType::get(elementTypes, Context));
  }

  // Add the conversion from the argument to the function parameter type.
  cs.addConstraint(ConstraintKind::ArgumentTupleConversion, arg->getType(),
                   openedType->castTo<FunctionType>()->getInput(),
                   cs.getConstraintLocator(arg,
                                           ConstraintLocator::ApplyArgument));

  // Solve the system.
  SmallVector<Solution, 1> solutions;
  bool failed = cs.solve(solutions);
  (void)failed;
  assert(!failed && "Unable to solve for call to witness?");

  Solution &solution = solutions.front();
  ExprRewriter rewriter(cs, solution);

  auto memberRef = rewriter.buildMemberRef(base, openedFullType,
                                           base->getStartLoc(),
                                           witness, base->getEndLoc(),
                                           openedType, locator,
                                           /*Implicit=*/true,
                                           /*direct ivar*/false);

  // Call the witness.
  ApplyExpr *apply = new (Context) CallExpr(memberRef, arg, /*Implicit=*/true);
  return rewriter.finishApply(apply, openedType,
                              cs.getConstraintLocator(arg));
}

/// \brief Convert an expression via a builtin protocol.
///
/// \param solution The solution to the expression's constraint system,
/// which must have included a constraint that the expression's type
/// conforms to the give \c protocol.
/// \param expr The expression to convert.
/// \param locator The locator describing where the conversion occurs.
/// \param protocol The protocol to use for conversion.
/// \param generalName The name of the protocol method to use for the
/// conversion.
/// \param builtinName The name of the builtin method to use for the
/// last step of the conversion.
/// \param brokenProtocolDiag Diagnostic to emit if the protocol
/// definition is missing.
/// \param brokenBuiltinDiag Diagnostic to emit if the builtin definition
/// is broken.
///
/// \returns the converted expression.
static Expr *convertViaBuiltinProtocol(const Solution &solution,
                                       Expr *expr,
                                       ConstraintLocator *locator,
                                       ProtocolDecl *protocol,
                                       Identifier generalName,
                                       Identifier builtinName,
                                       Diag<> brokenProtocolDiag,
                                       Diag<> brokenBuiltinDiag) {
  auto &cs = solution.getConstraintSystem();
  ExprRewriter rewriter(cs, solution);

  // FIXME: Cache name.
  auto &tc = cs.getTypeChecker();
  auto &ctx = tc.Context;
  auto type = expr->getType();

  // Look for the builtin name. If we don't have it, we need to call the
  // general name via the witness table.
  auto witnesses = tc.lookupMember(type->getRValueType(), builtinName, cs.DC);
  if (!witnesses) {
    // Find the witness we need to use.
    auto witness =
        findNamedPropertyWitness(tc, cs.DC, type -> getRValueType(), protocol,
                                 generalName, brokenProtocolDiag);
    // Form a reference to this member.
    Expr *memberRef = new (ctx) MemberRefExpr(expr, expr->getStartLoc(),
                                              witness, expr->getEndLoc(),
                                              /*Implicit=*/true);
    bool failed = tc.typeCheckExpressionShallow(memberRef, cs.DC);
    if (failed) {
      // If the member reference expression failed to type check, the Expr's
      // type does not conform to the given protocol.
      tc.diagnose(expr->getLoc(),
                  diag::type_does_not_conform,
                  type,
                  protocol->getType());
      return nullptr;
    }
    expr = memberRef;

    // At this point, we must have a type with the builtin member.
    type = expr->getType();
    witnesses = tc.lookupMember(type->getRValueType(), builtinName, cs.DC);
    if (!witnesses) {
      tc.diagnose(protocol->getLoc(), brokenProtocolDiag);
      return nullptr;
    }
  }

  // Find the builtin method.
  if (witnesses.size() != 1) {
    tc.diagnose(protocol->getLoc(), brokenBuiltinDiag);
    return nullptr;
  }
  FuncDecl *builtinMethod = dyn_cast<FuncDecl>(witnesses[0]);
  if (!builtinMethod) {
    tc.diagnose(protocol->getLoc(), brokenBuiltinDiag);
    return nullptr;

  }

  // Form a reference to the builtin method.
  Expr *memberRef = new (ctx) MemberRefExpr(expr, SourceLoc(),
                                            builtinMethod, expr->getLoc(),
                                            /*Implicit=*/true);
  bool failed = tc.typeCheckExpressionShallow(memberRef, cs.DC);
  assert(!failed && "Could not reference witness?");
  (void)failed;

  // Call the builtin method.
  Expr *arg = TupleExpr::createEmpty(ctx, expr->getStartLoc(), 
                                     expr->getEndLoc(), /*Implicit=*/true);
  expr = new (ctx) CallExpr(memberRef, arg, /*Implicit=*/true);
  failed = tc.typeCheckExpressionShallow(expr, cs.DC);
  assert(!failed && "Could not call witness?");
  (void)failed;
  return expr;
}

Expr *
Solution::convertToLogicValue(Expr *expr, ConstraintLocator *locator) const {
  auto &tc = getConstraintSystem().getTypeChecker();

  // Special case: already a builtin logic value.
  if (expr->getType()->getRValueType()->isBuiltinIntegerType(1)) {
    return tc.coerceToRValue(expr);
  }

  // FIXME: Cache names.
  auto result = convertViaBuiltinProtocol(
                  *this, expr, locator,
                  tc.getProtocol(expr->getLoc(),
                                 KnownProtocolKind::BooleanType),
                  tc.Context.Id_BoolValue,
                  tc.Context.Id_GetBuiltinLogicValue,
                  diag::condition_broken_proto,
                  diag::broken_bool);
  if (result && !result->getType()->isBuiltinIntegerType(1)) {
    tc.diagnose(expr->getLoc(), diag::broken_bool);
    return nullptr;
  }

  return result;
}

Expr *
Solution::convertOptionalToBool(Expr *expr, ConstraintLocator *locator) const {
  auto &cs = getConstraintSystem();
  ExprRewriter rewriter(cs, *this);
  auto &tc = cs.getTypeChecker();

  auto proto = tc.getProtocol(
    expr->getLoc(), KnownProtocolKind::BooleanType);

  // Find the witness we need to use.
  Type type = expr->getType();
  auto witness = findNamedPropertyWitness(tc, cs.DC, type -> getRValueType(),
                                          proto, tc.Context.Id_BoolValue,
                                          diag::condition_broken_proto);

  // Form a reference to this member.
  auto &ctx = tc.Context;
  Expr *memberRef = new (ctx) MemberRefExpr(expr, expr->getStartLoc(),
                                            witness, expr->getEndLoc(),
                                            /*Implicit=*/true);
  bool failed = tc.typeCheckExpressionShallow(memberRef, cs.DC);
  if (failed) {
    // If the member reference expression failed to type check, the Expr's
    // type does not conform to the given protocol.
    tc.diagnose(expr->getLoc(),
                diag::type_does_not_conform,
                type,
                proto->getType());
    return nullptr;
  }

  return memberRef;
}

Expr *
Solution::convertToArrayBound(Expr *expr, ConstraintLocator *locator) const {
  auto &tc = getConstraintSystem().getTypeChecker();
  auto result = convertViaBuiltinProtocol(
                  *this, expr, locator,
                  tc.getProtocol(expr->getLoc(),
                                 KnownProtocolKind::ArrayBoundType),
                  tc.Context.Id_ArrayBoundValue,
                  tc.Context.Id_GetBuiltinArrayBoundValue,
                  diag::broken_array_bound_proto,
                  diag::broken_builtin_array_bound);
  if (result && !result->getType()->is<BuiltinIntegerType>()) {
    tc.diagnose(expr->getLoc(), diag::broken_builtin_array_bound);
    return nullptr;
  }

  return result;
}
