//===--- Type.cpp - Swift Language Type ASTs ------------------------------===//
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
//  This file implements the Type class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/AST.h"
#include "swift/AST/TypeLoc.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <iterator>
using namespace swift;

bool TypeLoc::isError() const {
  assert(wasValidated() && "Type not yet validated");
  return !getType()->is<ErrorType>();
}

SourceRange TypeLoc::getSourceRange() const {
  if (TyR)
    return TyR->getSourceRange();
  return SourceRange();
}

// Only allow allocation of Types using the allocator in ASTContext.
void *TypeBase::operator new(size_t bytes, const ASTContext &ctx,
                             AllocationArena arena, unsigned alignment) {
  return ctx.Allocate(bytes, alignment, arena);
}

bool CanType::isActuallyCanonicalOrNull() const {
  return getPointer() == 0 || 
         getPointer() == llvm::DenseMapInfo<TypeBase*>::getTombstoneKey() ||
         getPointer()->isCanonical();
}



//===----------------------------------------------------------------------===//
// Various Type Methods.
//===----------------------------------------------------------------------===//

/// isEqual - Return true if these two types are equal, ignoring sugar.
bool TypeBase::isEqual(Type Other) {
  return getCanonicalType() == Other.getPointer()->getCanonicalType();
}

bool TypeBase::isError() {
  TypeBase *T = this;
  if (auto *LVT = T->getAs<LValueType>())
    T = LVT->getObjectType().getPointer();

  return T->is<ErrorType>();
}

/// isMaterializable - Is this type 'materializable' according to the
/// rules of the language?  Basically, does it not contain any l-value
/// types?
bool TypeBase::isMaterializable() {
  // Tuples are materializable if all their elements are.
  if (TupleType *tuple = getAs<TupleType>()) {
    for (auto &field : tuple->getFields())
      if (!field.getType()->isMaterializable())
        return false;
    return true;
  }

  // Some l-values may be materializable someday.
  if (LValueType *lvalue = getAs<LValueType>())
    return lvalue->isMaterializable();

  // Everything else is materializable.
  return true;
}

/// hasReferenceSemantics - Does this type have reference semantics?
bool TypeBase::hasReferenceSemantics() {
  return getCanonicalType().hasReferenceSemantics();
}

bool CanType::hasReferenceSemanticsImpl(CanType type) {
  // At the moment, Builtin.ObjectPointer, class types, and function types.
  switch (type->getKind()) {
#define SUGARED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
    return false;

  case TypeKind::Error:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinVector:
  case TypeKind::Tuple:
  case TypeKind::Union:
  case TypeKind::Struct:
  case TypeKind::MetaType:
  case TypeKind::Module:
  case TypeKind::Array:
  case TypeKind::LValue:
  case TypeKind::TypeVariable:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct:
    return false;

  case TypeKind::ReferenceStorage:
    return false; // This might seem non-obvious.

  case TypeKind::Archetype:
    return cast<SubstitutableType>(type)->requiresClass();
  case TypeKind::Protocol:
    return cast<ProtocolType>(type)->requiresClass();
  case TypeKind::ProtocolComposition:
    return cast<ProtocolCompositionType>(type)->requiresClass();
      
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::Class:
  case TypeKind::BoundGenericClass:
  case TypeKind::Function:
  case TypeKind::PolymorphicFunction:
    return true;

  case TypeKind::UnboundGeneric:
    return isa<ClassDecl>(cast<UnboundGenericType>(type)->getDecl());

  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    llvm_unreachable("Dependent types can't answer reference-semantics query");
  }

  llvm_unreachable("Unhandled type kind!");
}

/// hasOwnership - Are variables of this type permitted to have
/// ownership attributes?
///
/// This includes:
///   - class types, generic or not
///   - archetypes with class or class protocol bounds
///   - existentials with class or class protocol bounds
/// But not:
///   - function types
bool TypeBase::allowsOwnership() {
  CanType canonical = getCanonicalType();
  return (!isa<AnyFunctionType>(canonical)
          && canonical.hasReferenceSemantics());
}

bool CanType::isExistentialTypeImpl(CanType type) {
  return isa<ProtocolType>(type) || isa<ProtocolCompositionType>(type);
}

bool TypeBase::isExistentialType(SmallVectorImpl<ProtocolDecl *> &Protocols) {
  CanType T = getCanonicalType();
  if (auto Proto = dyn_cast<ProtocolType>(T)) {
    Protocols.push_back(Proto->getDecl());
    return true;
  }
  
  if (auto PC = dyn_cast<ProtocolCompositionType>(T)) {
    std::transform(PC->getProtocols().begin(), PC->getProtocols().end(),
                   std::back_inserter(Protocols),
                   [](Type T) { return T->castTo<ProtocolType>()->getDecl(); });
    return true;
  }

  assert(!T.isExistentialType());
  return false;
}

bool TypeBase::isSpecialized() {
  CanType CT = getCanonicalType();
  if (CT.getPointer() != this)
    return CT->isSpecialized();

  switch (getKind()) {
#define SUGARED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Error:
  case TypeKind::TypeVariable:
      return false;

  case TypeKind::UnboundGeneric:
    if (auto parentTy = cast<UnboundGenericType>(this)->getParent())
      return parentTy->isSpecialized();

    return false;

  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct:
    return true;

  case TypeKind::Function:
  case TypeKind::PolymorphicFunction: {
    auto funcTy = cast<AnyFunctionType>(this);
    return funcTy->getInput()->isSpecialized() ||
           funcTy->getResult()->isSpecialized();
  }

  case TypeKind::Class:
  case TypeKind::Struct:
  case TypeKind::Union:
    if (auto parentTy = cast<NominalType>(this)->getParent())
      return parentTy->isSpecialized();
    return false;

  case TypeKind::MetaType:
    return cast<MetaTypeType>(this)->getInstanceType()->isSpecialized();

  case TypeKind::LValue:
    return cast<LValueType>(this)->getObjectType()->isSpecialized();

  case TypeKind::Tuple: {
    auto tupleTy = cast<TupleType>(this);
    for (auto &Elt : tupleTy->getFields())
      if (Elt.getType()->isSpecialized())
        return true;
    
    return false;
  }

  case TypeKind::ReferenceStorage:
    return cast<ReferenceStorageType>(this)->getReferentType()->isSpecialized();

  case TypeKind::Archetype:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinVector:
  case TypeKind::Module:
  case TypeKind::Protocol:
  case TypeKind::ProtocolComposition:
    return false;

  case TypeKind::Array:
    return cast<ArrayType>(this)->getBaseType()->isSpecialized();

  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    return false;
  }
}

bool TypeBase::isUnspecializedGeneric() {
  CanType CT = getCanonicalType();
  if (CT.getPointer() != this)
    return CT->isUnspecializedGeneric();

  switch (getKind()) {
#define SUGARED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
    return false;

  case TypeKind::Error:
  case TypeKind::TypeVariable:
    llvm_unreachable("querying invalid type");

  case TypeKind::UnboundGeneric:
    return true;

  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct:
    return true;

  case TypeKind::Function:
  case TypeKind::PolymorphicFunction: {
    auto funcTy = cast<AnyFunctionType>(this);
    return funcTy->getInput()->isUnspecializedGeneric() ||
           funcTy->getResult()->isUnspecializedGeneric();
  }

  case TypeKind::Class:
  case TypeKind::Struct:
  case TypeKind::Union:
    if (auto parentTy = cast<NominalType>(this)->getParent())
      return parentTy->isUnspecializedGeneric();
    return false;

  case TypeKind::MetaType:
    return cast<MetaTypeType>(this)->getInstanceType()
             ->isUnspecializedGeneric();

  case TypeKind::ReferenceStorage:
    return cast<ReferenceStorageType>(this)->getReferentType()
             ->isUnspecializedGeneric();

  case TypeKind::LValue:
    return cast<LValueType>(this)->getObjectType()->isUnspecializedGeneric();

  case TypeKind::Tuple: {
    auto tupleTy = cast<TupleType>(this);
    for (auto &Elt : tupleTy->getFields())
      if (Elt.getType()->isUnspecializedGeneric())
        return true;

    return false;
  }

  case TypeKind::Archetype:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinVector:
  case TypeKind::Module:
  case TypeKind::Protocol:
  case TypeKind::ProtocolComposition:
    return false;
    
  case TypeKind::Array:
    return cast<ArrayType>(this)->getBaseType()->isUnspecializedGeneric();

  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    return false;
  }
}

/// \brief Gather the type variables in the given type, recursively.
static void gatherTypeVariables(Type wrappedTy,
                          SmallVectorImpl<TypeVariableType *> &typeVariables) {
  auto ty = wrappedTy.getPointer();
  if (!ty)
    return;

  switch (ty->getKind()) {
  case TypeKind::Error:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinVector:
  case TypeKind::NameAlias:
  case TypeKind::Module:
  case TypeKind::Protocol:
  case TypeKind::Archetype:
  case TypeKind::GenericTypeParam:
  case TypeKind::AssociatedType:
  case TypeKind::ProtocolComposition:
    // None of these types ever have type variables.
    return;

  case TypeKind::Paren:
    return gatherTypeVariables(cast<ParenType>(ty)->getUnderlyingType(),
                               typeVariables);

  case TypeKind::Tuple: {
    const TupleType *tupleTy = cast<TupleType>(ty);
    // FIXME: Always walk default arguments.
    for (const auto &field : tupleTy->getFields()) {
      gatherTypeVariables(field.getType(), typeVariables);
    }
    return;
  }

  case TypeKind::Union:
  case TypeKind::Struct:
  case TypeKind::Class:
    return gatherTypeVariables(cast<NominalType>(ty)->getParent(),
                               typeVariables);

  case TypeKind::MetaType:
    return gatherTypeVariables(cast<MetaTypeType>(ty)->getInstanceType(),
                               typeVariables);

  case TypeKind::ReferenceStorage:
    return gatherTypeVariables(
                             cast<ReferenceStorageType>(ty)->getReferentType(),
                               typeVariables);

  case TypeKind::Substituted:
    return gatherTypeVariables(cast<SubstitutedType>(ty)->getReplacementType(),
                               typeVariables);

  case TypeKind::Function:
  case TypeKind::PolymorphicFunction: {
    auto fnType = cast<AnyFunctionType>(ty);
    gatherTypeVariables(fnType->getInput(), typeVariables);
    gatherTypeVariables(fnType->getResult(), typeVariables);
    return;
  }

  case TypeKind::Array:
    return gatherTypeVariables(cast<ArrayType>(ty)->getBaseType(),
                               typeVariables);

  case TypeKind::ArraySlice:
  case TypeKind::Optional:
    gatherTypeVariables(cast<SyntaxSugarType>(ty)->getImplementationType(),
                        typeVariables);
    return;

  case TypeKind::LValue:
    return gatherTypeVariables(cast<LValueType>(ty)->getObjectType(),
                               typeVariables);

  case TypeKind::UnboundGeneric:
    return gatherTypeVariables(cast<UnboundGenericType>(ty)->getParent(),
                               typeVariables);

  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct: {
    auto boundTy = cast<BoundGenericType>(ty);
    gatherTypeVariables(boundTy->getParent(), typeVariables);
    for (auto arg : boundTy->getGenericArgs())
      gatherTypeVariables(arg, typeVariables);
    return;
  }

  case TypeKind::TypeVariable:
    typeVariables.push_back(cast<TypeVariableType>(ty));
    return;

  case TypeKind::DependentMember:
    gatherTypeVariables(cast<DependentMemberType>(ty)->getBase(),
                        typeVariables);
    return;
  }

  llvm_unreachable("Unhandling type kind");
}

void
TypeBase::getTypeVariables(SmallVectorImpl<TypeVariableType *> &typeVariables) {
  // If we know we don't have any type variables, we're done.
  if (hasTypeVariable()) {
    gatherTypeVariables(this, typeVariables);
    assert(!typeVariables.empty() && "Did not find type variables!");
  }
}

bool TypeBase::isVoid() {
  return isEqual(getASTContext().TheEmptyTupleType);
}

ClassDecl *TypeBase::getClassOrBoundGenericClass() {
  if (auto classTy = getAs<ClassType>())
    return classTy->getDecl();

  if (auto boundTy = getAs<BoundGenericType>())
    return dyn_cast<ClassDecl>(boundTy->getDecl());

  return nullptr;
}

UnionDecl *TypeBase::getUnionOrBoundGenericUnion() {
  if (auto oofTy = getAs<UnionType>())
    return oofTy->getDecl();
  
  if (auto boundTy = getAs<BoundGenericType>())
    return dyn_cast<UnionDecl>(boundTy->getDecl());
  
  return nullptr;
}

NominalTypeDecl *TypeBase::getNominalOrBoundGenericNominal() {
  if (auto nominalTy = getAs<NominalType>())
    return nominalTy->getDecl();

  if (auto boundTy = getAs<BoundGenericType>())
    return boundTy->getDecl();

  return nullptr;
}

NominalTypeDecl *TypeBase::getAnyNominal() {
  if (auto nominalTy = getAs<NominalType>())
    return nominalTy->getDecl();

  if (auto boundTy = getAs<BoundGenericType>())
    return boundTy->getDecl();

  if (auto unboundTy = getAs<UnboundGenericType>())
    return unboundTy->getDecl();

  return nullptr;
}

static Type getStrippedType(const ASTContext &context, Type type,
                            bool stripLabels, bool stripDefaultArgs) {
  switch (type->getKind()) {
  case TypeKind::Error: 
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinVector:
  case TypeKind::Union:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::MetaType:
  case TypeKind::Module:
  case TypeKind::Protocol:
  case TypeKind::Archetype:
  case TypeKind::AssociatedType:
  case TypeKind::GenericTypeParam:
  case TypeKind::ProtocolComposition:
  case TypeKind::UnboundGeneric:
  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct:
  case TypeKind::TypeVariable:
  case TypeKind::ReferenceStorage:
    return type;

  case TypeKind::NameAlias:
    if (TypeAliasDecl *D = cast<NameAliasType>(type.getPointer())->getDecl()) {
      Type UnderlingTy = getStrippedType(context, D->getUnderlyingType(),
                                         stripLabels, stripDefaultArgs);
      if (UnderlingTy.getPointer() != D->getUnderlyingType().getPointer())
        return UnderlingTy;
    }
    
    return type;
  
  case TypeKind::Paren: {
    ParenType *ParenTy = cast<ParenType>(type.getPointer());
    Type UnderlyingTy = getStrippedType(context, ParenTy->getUnderlyingType(),
                                        stripLabels, stripDefaultArgs);
    if (UnderlyingTy.getPointer() != ParenTy->getUnderlyingType().getPointer())
      return ParenType::get(context, UnderlyingTy);
    return type;
  }
      
  case TypeKind::Tuple: {
    TupleType *TupleTy = cast<TupleType>(type.getPointer());
    SmallVector<TupleTypeElt, 4> Elements;
    bool Rebuild = false;
    unsigned Idx = 0;
    for (const TupleTypeElt &Elt : TupleTy->getFields()) {
      Type EltTy = getStrippedType(context, Elt.getType(),
                                   stripLabels, stripDefaultArgs);
      if (Rebuild || EltTy.getPointer() != Elt.getType().getPointer() ||
          (Elt.hasInit() && stripDefaultArgs) ||
          (!Elt.getName().empty() && stripLabels)) {
        if (!Rebuild) {
          Elements.reserve(TupleTy->getFields().size());
          for (unsigned I = 0; I != Idx; ++I) {
            const TupleTypeElt &Elt = TupleTy->getFields()[I];
            Identifier newName = stripLabels? Identifier() : Elt.getName();
            DefaultArgumentKind newDefArg
              = stripDefaultArgs? DefaultArgumentKind::None
                                : Elt.getDefaultArgKind();
            Elements.push_back(TupleTypeElt(Elt.getType(), newName, newDefArg,
                                            Elt.isVararg()));
          }
          Rebuild = true;
        }

        Identifier newName = stripLabels? Identifier() : Elt.getName();
        DefaultArgumentKind newDefArg
          = stripDefaultArgs? DefaultArgumentKind::None
                            : Elt.getDefaultArgKind();
        Elements.push_back(TupleTypeElt(EltTy, newName, newDefArg,
                                        Elt.isVararg()));
      }
      ++Idx;
    }
    
    if (!Rebuild)
      return type;
    
    // An unlabeled 1-element tuple type is represented as a parenthesized
    // type.
    if (Elements.size() == 1 && !Elements[0].isVararg() && 
        Elements[0].getName().empty())
      return ParenType::get(context, Elements[0].getType());
    
    return TupleType::get(Elements, context);
  }
      
  case TypeKind::Function:
  case TypeKind::PolymorphicFunction: {
    AnyFunctionType *FunctionTy = cast<AnyFunctionType>(type.getPointer());
    Type InputTy = getStrippedType(context, FunctionTy->getInput(),
                                   stripLabels, stripDefaultArgs);
    Type ResultTy = getStrippedType(context, FunctionTy->getResult(),
                                    stripLabels, stripDefaultArgs);
    if (InputTy.getPointer() != FunctionTy->getInput().getPointer() ||
        ResultTy.getPointer() != FunctionTy->getResult().getPointer()) {
      if (auto monoFn = dyn_cast<FunctionType>(FunctionTy)) {
        auto Info = FunctionType::ExtInfo()
                      .withIsAutoClosure(monoFn->isAutoClosure());
        return FunctionType::get(InputTy, ResultTy,
                                 Info, context);
      } else {
        auto polyFn = cast<PolymorphicFunctionType>(FunctionTy);
        return PolymorphicFunctionType::get(InputTy, ResultTy,
                                            &polyFn->getGenericParams(),
                                            context);
      }
    }
    
    return type;
  }
      
  case TypeKind::Array: {
    ArrayType *ArrayTy = cast<ArrayType>(type.getPointer());
    Type BaseTy = getStrippedType(context, ArrayTy->getBaseType(),
                                  stripLabels, stripDefaultArgs);
    if (BaseTy.getPointer() != ArrayTy->getBaseType().getPointer())
      return ArrayType::get(BaseTy, ArrayTy->getSize(), context);
    
    return type;
  }

  case TypeKind::ArraySlice: {
    ArraySliceType *sliceTy = cast<ArraySliceType>(type.getPointer());
    Type baseTy = getStrippedType(context, sliceTy->getBaseType(),
                                  stripLabels, stripDefaultArgs);
    if (baseTy.getPointer() != sliceTy->getBaseType().getPointer()) {
      ArraySliceType *newSliceTy = ArraySliceType::get(baseTy, context);
      if (!newSliceTy->hasImplementationType())
        newSliceTy->setImplementationType(sliceTy->getImplementationType());
      return newSliceTy;
    }
    
    return type;
  }

  case TypeKind::Optional: {
    auto optionalTy = cast<OptionalType>(type.getPointer());
    Type baseTy = getStrippedType(context, optionalTy->getBaseType(),
                                  stripLabels, stripDefaultArgs);
    if (baseTy.getPointer() != optionalTy->getBaseType().getPointer()) {
      auto newOptTy = OptionalType::get(baseTy, context);
      if (!newOptTy->hasImplementationType())
        newOptTy->setImplementationType(optionalTy->getImplementationType());
      return newOptTy;
    }
    
    return type;
  }

  case TypeKind::LValue: {
    LValueType *LValueTy = cast<LValueType>(type.getPointer());
    Type ObjectTy = getStrippedType(context, LValueTy->getObjectType(),
                                    stripLabels, stripDefaultArgs);
    if (ObjectTy.getPointer() != LValueTy->getObjectType().getPointer())
      return LValueType::get(ObjectTy, LValueTy->getQualifiers(), context);
    return type;
  }
      
  case TypeKind::Substituted: {
    SubstitutedType *SubstTy = cast<SubstitutedType>(type.getPointer());
    Type NewSubstTy = getStrippedType(context, SubstTy->getReplacementType(),
                                      stripLabels, stripDefaultArgs);
    if (NewSubstTy.getPointer() != SubstTy->getReplacementType().getPointer())
      return SubstitutedType::get(SubstTy->getOriginal(), NewSubstTy,
                                  context);
    return type;
  }

  case TypeKind::DependentMember: {
    auto dependent = cast<DependentMemberType>(type.getPointer());
    auto base = getStrippedType(context, dependent->getBase(), stripLabels,
                                stripDefaultArgs);
    if (base.getPointer() == dependent->getBase().getPointer())
      return type;

    return DependentMemberType::get(base, dependent->getName(), context);
  }
  }
}

Type TypeBase::getUnlabeledType(ASTContext &Context) {
  return getStrippedType(Context, Type(this), /*labels=*/true,
                         /*defaultArgs=*/true);
}

Type TypeBase::getWithoutDefaultArgs(const ASTContext &Context) {
  return getStrippedType(Context, Type(this), /*labels=*/false,
                         /*defaultArgs=*/true);
}

/// \brief Collect the protocols in the existential type T into the given
/// vector.
static void addProtocols(Type T, SmallVectorImpl<ProtocolDecl *> &Protocols) {
  if (auto Proto = T->getAs<ProtocolType>()) {
    Protocols.push_back(Proto->getDecl());
  } else if (auto PC = T->getAs<ProtocolCompositionType>()) {
    for (auto P : PC->getProtocols())
      addProtocols(P, Protocols);
  }
}

/// \brief Add the protocol (or protocols) in the type T to the stack of
/// protocols, checking whether any of the protocols had already been seen and
/// zapping those in the original list that we find again.
static void addMinimumProtocols(Type T,
                                SmallVectorImpl<ProtocolDecl *> &Protocols,
                           llvm::SmallDenseMap<ProtocolDecl *, unsigned> &Known,
                                llvm::SmallPtrSet<ProtocolDecl *, 16> &Visited,
                                SmallVector<ProtocolDecl *, 16> &Stack,
                                bool &ZappedAny) {
  if (auto Proto = T->getAs<ProtocolType>()) {
    auto KnownPos = Known.find(Proto->getDecl());
    if (KnownPos != Known.end()) {
      // We've come across a protocol that is in our original list. Zap it.
      Protocols[KnownPos->second] = nullptr;
      ZappedAny = true;
    }

    if (Visited.insert(Proto->getDecl())) {
      for (auto Inherited : Proto->getDecl()->getProtocols())
        addProtocols(Inherited->getDeclaredType(), Stack);
    }
    return;
  }
  
  if (auto PC = T->getAs<ProtocolCompositionType>()) {
    for (auto C : PC->getProtocols()) {
      addMinimumProtocols(C, Protocols, Known, Visited, Stack, ZappedAny);
    }
  }
}

/// \brief 'Minimize' the given set of protocols by eliminating any mentions of
/// protocols that are already covered by inheritance due to other entries in
/// the protocol list.
static void minimizeProtocols(SmallVectorImpl<ProtocolDecl *> &Protocols) {
  llvm::SmallDenseMap<ProtocolDecl *, unsigned> Known;
  llvm::SmallPtrSet<ProtocolDecl *, 16> Visited;
  SmallVector<ProtocolDecl *, 16> Stack;
  bool ZappedAny = false;

  // Seed the stack with the protocol declarations in the original list.
  // Zap any obvious duplicates along the way.
  for (unsigned I = 0, N = Protocols.size(); I != N; ++I) {
    // Check whether we've seen this protocol before.
    auto KnownPos = Known.find(Protocols[I]);
    
    // If we have not seen this protocol before, record it's index.
    if (KnownPos == Known.end()) {
      Known[Protocols[I]] = I;
      Stack.push_back(Protocols[I]);
      continue;
    }
    
    // We have seen this protocol before; zap this occurrance.
    Protocols[I] = 0;
    ZappedAny = true;
  }
  
  // Walk the inheritance hierarchies of all of the protocols. If we run into
  // one of the known protocols, zap it from the original list.
  while (!Stack.empty()) {
    ProtocolDecl *Current = Stack.back();
    Stack.pop_back();
    
    // Add the protocols we inherited.
    for (auto Inherited : Current->getProtocols()) {
      addMinimumProtocols(Inherited->getDeclaredType(), Protocols, Known,
                          Visited, Stack, ZappedAny);
    }
  }
  
  if (ZappedAny)
    Protocols.erase(std::remove(Protocols.begin(), Protocols.end(), nullptr),
                    Protocols.end());
}

/// \brief Compare two protocols to establish an ordering between them.
static int compareProtocols(const void *V1, const void *V2) {
  const ProtocolDecl *P1 = *reinterpret_cast<const ProtocolDecl *const *>(V1);
  const ProtocolDecl *P2 = *reinterpret_cast<const ProtocolDecl *const *>(V2);

  Module *M1 = P1->getParentModule();
  Module *M2 = P2->getParentModule();

  // Try ordering based on module name, first.
  if (int result = M1->Name.str().compare(M2->Name.str()))
    return result;
  
  // Order based on protocol name.
  return P1->getName().str().compare(P2->getName().str());
}

/// getCanonicalType - Return the canonical version of this type, which has
/// sugar from all levels stripped off.
CanType TypeBase::getCanonicalType() {
  assert(this != 0 &&
         "Cannot call getCanonicalType before name binding is complete");

  // If the type is itself canonical, return it.
  if (isCanonical())
    return CanType(this);
  // If the canonical type was already computed, just return what we have.
  if (TypeBase *CT = CanonicalType.get<TypeBase*>())
    return CanType(CT);
  
  // Otherwise, compute and cache it.
  TypeBase *Result = 0;
  switch (getKind()) {
#define ALWAYS_CANONICAL_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Error:
  case TypeKind::TypeVariable:
    llvm_unreachable("these types are always canonical");

#define SUGARED_TYPE(id, parent) \
  case TypeKind::id: \
    Result = cast<id##Type>(this)-> \
             getDesugaredType()->getCanonicalType().getPointer(); \
    break;
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"

  case TypeKind::Union:
  case TypeKind::Struct:
  case TypeKind::Class: {
    auto nominalTy = cast<NominalType>(this);
    auto parentTy = nominalTy->getParent()->getCanonicalType();
    Result = NominalType::get(nominalTy->getDecl(), parentTy,
                              parentTy->getASTContext());
    break;
  }

  case TypeKind::Tuple: {
    TupleType *TT = cast<TupleType>(this);
    assert(!TT->getFields().empty() && "Empty tuples are always canonical");

    SmallVector<TupleTypeElt, 8> CanElts;
    CanElts.reserve(TT->getFields().size());
    for (const TupleTypeElt &field : TT->getFields()) {
      assert(!field.getType().isNull() &&
             "Cannot get canonical type of un-typechecked TupleType!");
      CanElts.push_back(TupleTypeElt(field.getType()->getCanonicalType(),
                                     field.getName(),
                                     field.getDefaultArgKind(),
                                     field.isVararg()));
    }

    const ASTContext &C = CanElts[0].getType()->getASTContext();
    Result = TupleType::get(CanElts, C)->castTo<TupleType>();
    break;
  }

  case TypeKind::GenericTypeParam: {
    // FIXME: Actually canonicalize to a sensible representation that doesn't
    // contain the declaration.
    Result = this;
    break;
  }

  case TypeKind::DependentMember: {
    auto dependent = cast<DependentMemberType>(this);
    auto base = dependent->getBase()->getCanonicalType();
    const ASTContext &ctx = base->getASTContext();
    Result = DependentMemberType::get(base, dependent->getName(), ctx);
    break;
  }

  case TypeKind::ReferenceStorage: {
    auto ref = cast<ReferenceStorageType>(this);
    Type referentType = ref->getReferentType()->getCanonicalType();
    Result = ReferenceStorageType::get(referentType, ref->getOwnership(),
                                       referentType->getASTContext());
    break;
  }
  case TypeKind::LValue: {
    LValueType *lvalue = cast<LValueType>(this);
    Type objectType = lvalue->getObjectType();
    objectType = objectType->getCanonicalType();
    Result = LValueType::get(objectType, lvalue->getQualifiers(),
                             objectType->getASTContext());
    break;
  }
  case TypeKind::PolymorphicFunction: {
    PolymorphicFunctionType *FT = cast<PolymorphicFunctionType>(this);
    Type In = FT->getInput()->getCanonicalType();
    Type Out = FT->getResult()->getCanonicalType();
    Result = PolymorphicFunctionType::get(In, Out, &FT->getGenericParams(),
                                          FT->getExtInfo(),
                                          In->getASTContext());
    break;
  }
  case TypeKind::Function: {
    FunctionType *FT = cast<FunctionType>(this);
    Type In = FT->getInput()->getCanonicalType();
    Type Out = FT->getResult()->getCanonicalType();
    Result = FunctionType::get(In, Out,
                               FT->getExtInfo(),
                               In->getASTContext());
    break;
  }
  case TypeKind::Array: {
    ArrayType *AT = cast<ArrayType>(this);
    Type EltTy = AT->getBaseType()->getCanonicalType();
    Result = ArrayType::get(EltTy, AT->getSize(), EltTy->getASTContext());
    break;
  }
  case TypeKind::ProtocolComposition: {
    SmallVector<Type, 4> CanProtos;
    for (Type t : cast<ProtocolCompositionType>(this)->getProtocols())
      CanProtos.push_back(t->getCanonicalType());
    assert(!CanProtos.empty() && "Non-canonical empty composition?");
    const ASTContext &C = CanProtos[0]->getASTContext();
    Type Composition = ProtocolCompositionType::get(C, CanProtos);
    Result = Composition.getPointer();
    break;
  }
  case TypeKind::MetaType: {
    MetaTypeType *MT = cast<MetaTypeType>(this);
    Type InstanceTy = MT->getInstanceType()->getCanonicalType();
    Result = MetaTypeType::get(InstanceTy, InstanceTy->getASTContext());
    break;
  }
  case TypeKind::UnboundGeneric: {
    auto unbound = cast<UnboundGenericType>(this);
    Type parentTy = unbound->getParent()->getCanonicalType();
    Result = UnboundGenericType::get(unbound->getDecl(), parentTy,
                                     parentTy->getASTContext());
    break;
  }
  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct: {
    BoundGenericType *BGT = cast<BoundGenericType>(this);
    Type parentTy;
    if (BGT->getParent())
      parentTy = BGT->getParent()->getCanonicalType();
    SmallVector<Type, 4> CanGenericArgs;
    for (Type Arg : BGT->getGenericArgs())
      CanGenericArgs.push_back(Arg->getCanonicalType());
    Result = BoundGenericType::get(BGT->getDecl(), parentTy, CanGenericArgs);
    break;
  }
  }
    
  
  // Cache the canonical type for future queries.
  assert(Result && "Case not implemented!");
  CanonicalType = Result;
  return CanType(Result);
}


TypeBase *TypeBase::getDesugaredType() {
  switch (getKind()) {
#define ALWAYS_CANONICAL_TYPE(id, parent) case TypeKind::id:
#define UNCHECKED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Tuple:
  case TypeKind::Function:
  case TypeKind::PolymorphicFunction:
  case TypeKind::Array:
  case TypeKind::LValue:
  case TypeKind::ProtocolComposition:
  case TypeKind::MetaType:
  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct:
  case TypeKind::Union:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::ReferenceStorage:
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    // None of these types have sugar at the outer level.
    return this;
  case TypeKind::Paren:
    return cast<ParenType>(this)->getDesugaredType();
  case TypeKind::NameAlias:
    return cast<NameAliasType>(this)->getDesugaredType();
  case TypeKind::AssociatedType:
    return cast<AssociatedTypeType>(this)->getDesugaredType();
  case TypeKind::ArraySlice:
  case TypeKind::Optional:
    return cast<SyntaxSugarType>(this)->getDesugaredType();
  case TypeKind::Substituted:
    return cast<SubstitutedType>(this)->getDesugaredType();
  }

  llvm_unreachable("Unknown type kind");
}

TypeBase *ParenType::getDesugaredType() {
  return getUnderlyingType()->getDesugaredType();
}

TypeBase *NameAliasType::getDesugaredType() {
  return getDecl()->getUnderlyingType()->getDesugaredType();
}

TypeBase *SyntaxSugarType::getDesugaredType() {
  return getImplementationType()->getDesugaredType();
}

TypeBase *SubstitutedType::getDesugaredType() {
  return getReplacementType()->getDesugaredType();
}

unsigned GenericTypeParamType::getDepth() const {
  return Param->getDepth();
}

unsigned GenericTypeParamType::getIndex() const {
  return Param->getIndex();
}

TypeBase *AssociatedTypeType::getDesugaredType() {
  return getDecl()->getArchetype()->getDesugaredType();
}

const llvm::fltSemantics &BuiltinFloatType::getAPFloatSemantics() const {
  switch (getFPKind()) {
  case BuiltinFloatType::IEEE16:  return APFloat::IEEEhalf;
  case BuiltinFloatType::IEEE32:  return APFloat::IEEEsingle;
  case BuiltinFloatType::IEEE64:  return APFloat::IEEEdouble;
  case BuiltinFloatType::IEEE80:  return APFloat::x87DoubleExtended;
  case BuiltinFloatType::IEEE128: return APFloat::IEEEquad;
  case BuiltinFloatType::PPC128:  return APFloat::PPCDoubleDouble;
  }
  assert(0 && "Unknown FP semantics");
  return APFloat::IEEEhalf;
}

bool TypeBase::isSpelledLike(Type other) {
  TypeBase *me = this;
  TypeBase *them = other.getPointer();
  
  if (me == them)
    return true;
  
  if (me->getKind() != them->getKind())
    return false;

  switch (me->getKind()) {
#define ALWAYS_CANONICAL_TYPE(id, parent) case TypeKind::id:
#define UNCHECKED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Union:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::NameAlias:
  case TypeKind::Substituted:
  case TypeKind::AssociatedType:
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    return false;

  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct: {
    auto bgMe = cast<BoundGenericType>(me);
    auto bgThem = cast<BoundGenericType>(them);
    if (bgMe->getDecl() != bgThem->getDecl())
      return false;
    if (bgMe->getGenericArgs().size() != bgThem->getGenericArgs().size())
      return false;
    for (size_t i = 0, sz = bgMe->getGenericArgs().size(); i < sz; ++i)
      if (!bgMe->getGenericArgs()[i]->isSpelledLike(bgThem->getGenericArgs()[i]))
        return false;
    return true;
  }

  case TypeKind::Tuple: {
    auto tMe = cast<TupleType>(me);
    auto tThem = cast<TupleType>(them);
    if (tMe->getFields().size() != tThem->getFields().size())
      return false;
    for (size_t i = 0, sz = tMe->getFields().size(); i < sz; ++i) {
      auto &myField = tMe->getFields()[i], &theirField = tThem->getFields()[i];
      if (myField.hasInit() != theirField.hasInit())
        return false;
      
      if (myField.getName() != theirField.getName())
        return false;
      
      if (myField.isVararg() != theirField.isVararg())
        return false;
      if (!myField.getType()->isSpelledLike(theirField.getType()))
        return false;
    }
    return true;
  }

  case TypeKind::PolymorphicFunction: {
    // Polymorphic function types should never be explicitly spelled.
    return false;
  }

  // TODO: change this to is same ExtInfo.
  case TypeKind::Function: {
    auto fMe = cast<FunctionType>(me);
    auto fThem = cast<FunctionType>(them);
    if (fMe->isAutoClosure() != fThem->isAutoClosure())
      return false;
    if (fMe->isBlock() != fThem->isBlock())
      return false;
    if (fMe->isThin() != fThem->isThin())
      return false;
    if (fMe->isNoReturn() != fThem->isNoReturn())
      return false;
    if (!fMe->getInput()->isSpelledLike(fThem->getInput()))
      return false;
    if (!fMe->getResult()->isSpelledLike(fThem->getResult()))
      return false;
    return true;
  }

  case TypeKind::Array: {
    auto aMe = cast<ArrayType>(me);
    auto aThem = cast<ArrayType>(them);
    if (aMe->getSize() != aThem->getSize())
      return false;
    return aMe->getBaseType()->isSpelledLike(aThem->getBaseType());
  }
      
  case TypeKind::LValue: {
    auto lMe = cast<LValueType>(me);
    auto lThem = cast<LValueType>(them);
    return lMe->getObjectType()->isSpelledLike(lThem->getObjectType());
  }
  case TypeKind::ProtocolComposition: {
    auto pMe = cast<ProtocolCompositionType>(me);
    auto pThem = cast<ProtocolCompositionType>(them);
    if (pMe->getProtocols().size() != pThem->getProtocols().size())
      return false;
    for (size_t i = 0, sz = pMe->getProtocols().size(); i < sz; ++i)
      if (!pMe->getProtocols()[i]->isSpelledLike(pThem->getProtocols()[i]))
        return false;
    return true;
  }
  case TypeKind::MetaType: {
    auto mMe = cast<MetaTypeType>(me);
    auto mThem = cast<MetaTypeType>(them);
    return mMe->getInstanceType()->isSpelledLike(mThem->getInstanceType());
  }
  case TypeKind::Paren: {
    auto pMe = cast<ParenType>(me);
    auto pThem = cast<ParenType>(them);
    return pMe->getUnderlyingType()->isSpelledLike(pThem->getUnderlyingType());
  }
  case TypeKind::ArraySlice:
  case TypeKind::Optional: {
    auto aMe = cast<SyntaxSugarType>(me);
    auto aThem = cast<SyntaxSugarType>(them);
    return aMe->getBaseType()->isSpelledLike(aThem->getBaseType());
  }
  case TypeKind::ReferenceStorage: {
    auto rMe = cast<ReferenceStorageType>(me);
    auto rThem = cast<ReferenceStorageType>(them);
    return rMe->getReferentType()->isSpelledLike(rThem->getReferentType());
  }
  }

  llvm_unreachable("Unknown type kind");

}

TupleType::TupleType(ArrayRef<TupleTypeElt> fields, const ASTContext *CanCtx,
                     bool hasTypeVariable)
  : TypeBase(TypeKind::Tuple, CanCtx, hasTypeVariable), Fields(fields) { }

/// hasAnyDefaultValues - Return true if any of our elements has a default
/// value.
bool TupleType::hasAnyDefaultValues() const {
  for (const TupleTypeElt &Elt : Fields)
    if (Elt.hasInit())
      return true;
  return false;
}

/// getNamedElementId - If this tuple has a field with the specified name,
/// return the field index, otherwise return -1.
int TupleType::getNamedElementId(Identifier I) const {
  for (unsigned i = 0, e = Fields.size(); i != e; ++i) {
    if (Fields[i].getName() == I)
      return i;
  }

  // Otherwise, name not found.
  return -1;
}

/// getFieldForScalarInit - If a tuple of this type can be initialized with a
/// scalar, return the field number that the scalar is assigned to.  If not,
/// return -1.
int TupleType::getFieldForScalarInit() const {
  if (Fields.empty()) return -1;
  
  int FieldWithoutDefault = -1;
  for (unsigned i = 0, e = Fields.size(); i != e; ++i) {
    // Ignore fields with a default value.
    if (Fields[i].hasInit()) continue;
    
    // If we already saw a non-vararg field missing a default value, then we
    // cannot assign a scalar to this tuple.
    if (FieldWithoutDefault != -1) {
      // Vararg fields are okay; they'll just end up being empty.
      if (Fields[i].isVararg())
        continue;
    
      return -1;
    }
    
    // Otherwise, remember this field number.
    FieldWithoutDefault = i;    
  }
  
  // If all the elements have default values, the scalar initializes the first
  // value in the tuple.
  return FieldWithoutDefault == -1 ? 0 : FieldWithoutDefault;
}


bool SubstitutableType::requiresClass() const {
  if (Superclass)
    return true;
  for (ProtocolDecl *conformed : getConformsTo())
    if (conformed->requiresClass())
      return true;
  return false;
}

ArchetypeType *ArchetypeType::getNew(const ASTContext &Ctx,
                                     ArchetypeType *Parent,
                                     Identifier Name, ArrayRef<Type> ConformsTo,
                                     Type Superclass,
                                     Optional<unsigned> Index) {
  // Gather the set of protocol declarations to which this archetype conforms.
  SmallVector<ProtocolDecl *, 4> ConformsToProtos;
  for (auto P : ConformsTo) {
    addProtocols(P, ConformsToProtos);
  }
  minimizeProtocols(ConformsToProtos);
  llvm::array_pod_sort(ConformsToProtos.begin(), ConformsToProtos.end(),
                       compareProtocols);

  auto arena = AllocationArena::Permanent;
  return new (Ctx, arena) ArchetypeType(Ctx, Parent, Name,
                                        Ctx.AllocateCopy(ConformsToProtos),
                                        Superclass, Index);
}

ArchetypeType *
ArchetypeType::getNew(const ASTContext &Ctx, ArchetypeType *Parent,
                      Identifier Name,
                      SmallVectorImpl<ProtocolDecl *> &ConformsTo,
                      Type Superclass, Optional<unsigned> Index) {
  // Gather the set of protocol declarations to which this archetype conforms.
  minimizeProtocols(ConformsTo);
  llvm::array_pod_sort(ConformsTo.begin(), ConformsTo.end(), compareProtocols);

  auto arena = AllocationArena::Permanent;
  return new (Ctx, arena) ArchetypeType(Ctx, Parent, Name,
                                        Ctx.AllocateCopy(ConformsTo),
                                        Superclass, Index);
}

namespace {
  /// \brief Function object that orders archetypes by name.
  struct OrderArchetypeByName {
    bool operator()(std::pair<Identifier, ArchetypeType *> X,
                    std::pair<Identifier, ArchetypeType *> Y) const {
      return X.first.str() < Y.second->getName().str();
    }

    bool operator()(std::pair<Identifier, ArchetypeType *> X,
                    Identifier Y) const {
      return X.first.str() < Y.str();
    }

    bool operator()(Identifier X,
                    std::pair<Identifier, ArchetypeType *> Y) const {
      return X.str() < Y.first.str();
    }

    bool operator()(Identifier X, Identifier Y) const {
      return X.str() < Y.str();
    }
  };
}

ArchetypeType *ArchetypeType::getNestedType(Identifier Name) const {
  ArrayRef<std::pair<Identifier, ArchetypeType *>>::const_iterator Pos
    = std::lower_bound(NestedTypes.begin(), NestedTypes.end(), Name,
                       OrderArchetypeByName());
  assert(Pos != NestedTypes.end() && Pos->first == Name);
  return Pos->second;
}

void
ArchetypeType::
setNestedTypes(ASTContext &Ctx,
               MutableArrayRef<std::pair<Identifier, ArchetypeType *>> Nested) {
  std::sort(Nested.begin(), Nested.end(), OrderArchetypeByName());
  NestedTypes = Ctx.AllocateCopy(Nested);
}

static void collectFullName(const ArchetypeType *Archetype,
                            SmallVectorImpl<char> &Result) {
  if (auto Parent = Archetype->getParent()) {
    collectFullName(Parent, Result);
    Result.push_back('.');
  }
  Result.append(Archetype->getName().str().begin(),
                Archetype->getName().str().end());
}

std::string ArchetypeType::getFullName() const {
  llvm::SmallString<64> Result;
  collectFullName(this, Result);
  return Result.str().str();
}

void ProtocolCompositionType::Profile(llvm::FoldingSetNodeID &ID,
                                      ArrayRef<Type> Protocols) {
  for (auto P : Protocols)
    ID.AddPointer(P.getPointer());
}

bool BoundGenericType::hasSubstitutions() {
  auto *canon = getCanonicalType()->castTo<BoundGenericType>();
  const ASTContext &ctx = canon->getASTContext();
  return (bool)ctx.getSubstitutions(canon);
}

ArrayRef<Substitution>
BoundGenericType::getSubstitutions() {
  auto *canon = getCanonicalType()->castTo<BoundGenericType>();
  const ASTContext &ctx = canon->getASTContext();
  return *ctx.getSubstitutions(canon);
}

void BoundGenericType::setSubstitutions(ArrayRef<Substitution> Subs){
  auto *canon = getCanonicalType()->castTo<BoundGenericType>();
  const ASTContext &ctx = canon->getASTContext();
  ctx.setSubstitutions(canon, Subs);
}

bool ProtocolType::requiresClass() const {
  return getDecl()->requiresClass();
}

bool ProtocolCompositionType::requiresClass() const {
  for (Type t : getProtocols()) {
    SmallVector<ProtocolDecl*, 2> protocols;
    if (t->isExistentialType(protocols))
      for (auto *proto : protocols)
        if (proto->requiresClass())
          return true;
  }
  return false;
}

Type ProtocolCompositionType::get(const ASTContext &C,
                                  ArrayRef<Type> ProtocolTypes) {
  for (Type t : ProtocolTypes) {
    if (!t->isCanonical())
      return build(C, ProtocolTypes);
  }
    
  SmallVector<ProtocolDecl *, 4> Protocols;
  for (Type t : ProtocolTypes)
    addProtocols(t, Protocols);
  
  // Minimize the set of protocols composed together.
  minimizeProtocols(Protocols);

  // If one protocol remains, its nominal type is the canonical type.
  if (Protocols.size() == 1)
    return Protocols.front()->getDeclaredType();

  // Sort the set of protocols by module + name, to give a stable
  // ordering.
  // FIXME: Consider namespaces here as well.
  llvm::array_pod_sort(Protocols.begin(), Protocols.end(), compareProtocols);

  // Form the set of canonical protocol types from the protocol
  // declarations, and use that to buid the canonical composition type.
  SmallVector<Type, 4> CanProtocolTypes;
  std::transform(Protocols.begin(), Protocols.end(),
                 std::back_inserter(CanProtocolTypes),
                 [](ProtocolDecl *Proto) {
                   return Proto->getDeclaredType();
                 });

  return build(C, CanProtocolTypes);
}

ArrayRef<GenericParam> PolymorphicFunctionType::getGenericParameters() const {
  return Params->getParams();
}

ArrayRef<ArchetypeType *> PolymorphicFunctionType::getAllArchetypes() const {
  return Params->getAllArchetypes();
}


//===----------------------------------------------------------------------===//
//  Type Printing
//===----------------------------------------------------------------------===//

void Type::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}
void Type::print(raw_ostream &OS) const {
  if (isNull())
    OS << "<null>";
  else
    Ptr->print(OS);
}

/// getString - Return the name of the type as a string, for use in
/// diagnostics only.
std::string Type::getString() const {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  print(OS);
  return OS.str();
}


/// getString - Return the name of the type as a string, for use in
/// diagnostics only.
std::string TypeBase::getString() const {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  print(OS);
  return OS.str();
}


void TypeBase::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void TypeBase::print(raw_ostream &OS) const {
  switch (getKind()) {
#define TYPE(id, parent) \
  case TypeKind::id:            return cast<id##Type>(this)->print(OS);
#include "swift/AST/TypeNodes.def"
  }
  llvm_unreachable("bad type kind!");
}

void BuiltinRawPointerType::print(raw_ostream &OS) const {
  OS << "Builtin.RawPointer";
}

void BuiltinObjectPointerType::print(raw_ostream &OS) const {
  OS << "Builtin.ObjectPointer";
}

void BuiltinObjCPointerType::print(raw_ostream &OS) const {
  OS << "Builtin.ObjCPointer";
}

void BuiltinIntegerType::print(raw_ostream &OS) const {
  OS << "Builtin.Int" << cast<BuiltinIntegerType>(this)->getBitWidth();
}

void BuiltinFloatType::print(raw_ostream &OS) const {
  switch (getFPKind()) {
  case IEEE16:  OS << "Builtin.FPIEEE16"; return;
  case IEEE32:  OS << "Builtin.FPIEEE32"; return;
  case IEEE64:  OS << "Builtin.FPIEEE64"; return;
  case IEEE80:  OS << "Builtin.FPIEEE80"; return;
  case IEEE128: OS << "Builtin.FPIEEE128"; return;
  case PPC128:  OS << "Builtin.FPPPC128"; return;
  }
}

void BuiltinVectorType::print(raw_ostream &OS) const {
  llvm::SmallString<32> underlyingStrVec;
  StringRef underlyingStr;
  {
    // FIXME: Ugly hack: remove the .Builtin from the element type.
    llvm::raw_svector_ostream underlyingOS(underlyingStrVec);
    elementType.print(OS);
    if (underlyingStrVec.startswith("Builtin."))
      underlyingStr = underlyingStrVec.substr(9);
    else
      underlyingStr = underlyingStrVec;
  }

  OS << "Builtin.Vec" << numElements << "x" << underlyingStr;
}

void ErrorType::print(raw_ostream &OS) const {
  OS << "<<error type>>";
}

void ParenType::print(raw_ostream &OS) const {
  OS << '(';
  UnderlyingType->print(OS);
  OS << ')';
}

void NameAliasType::print(raw_ostream &OS) const {
  OS << TheDecl->getName().get();
}

static void printGenericArgs(raw_ostream &OS, ArrayRef<Type> Args) {
  if (Args.empty())
    return;

  OS << '<';
  bool First = true;
  for (Type Arg : Args) {
    if (First)
      First = false;
    else
      OS << ", ";
    Arg->print(OS);
  }
  OS << '>';
}

void MetaTypeType::print(raw_ostream &OS) const {
  InstanceType->print(OS);
  OS << ".metatype";
}

void ModuleType::print(raw_ostream &OS) const {
  OS << "module<" << TheModule->Name << '>';
}


void TupleType::print(raw_ostream &OS) const {
  OS << "(";
  
  for (unsigned i = 0, e = Fields.size(); i != e; ++i) {
    if (i) OS << ", ";
    const TupleTypeElt &TD = Fields[i];
    
    if (TD.hasName())
      OS << TD.getName() << " : ";

    if (TD.isVararg())
      OS <<  TD.getVarargBaseTy() << "...";
    else
      OS << TD.getType();
  }
  OS << ')';
}

namespace {
  class AttributePrinter {
    unsigned attrCount = 0;
  public:
    raw_ostream &OS;

    explicit AttributePrinter(raw_ostream &OS) : OS(OS) {}
    
    raw_ostream &next() {
      return OS << (attrCount++ == 0 ? "[" : ", ");
    }
    
    void finish() {
      if (attrCount > 0)
        OS << "] ";
    }
  };

  static void printCC(AttributePrinter &attrs, AbstractCC cc) {
    if (cc == AbstractCC::Freestanding)
      return;
    
    attrs.next() << "cc(";
    switch (cc) {
    case AbstractCC::Freestanding:
      attrs.OS << "freestanding";
      break;
    case AbstractCC::Method:
      attrs.OS << "method";
      break;
    case AbstractCC::C:
      attrs.OS << "cdecl";
      break;
    case AbstractCC::ObjCMethod:
      attrs.OS << "objc_method";
      break;
    }
    attrs.OS << ")";
  }
}

void FunctionType::print(raw_ostream &OS) const {
  AttributePrinter attrs(OS);

  if (isAutoClosure())
    attrs.next() << "auto_closure";
  printCC(attrs, getAbstractCC());
  if (isBlock())
    attrs.next() << "objc_block";
  if (isThin())
    attrs.next() << "thin";
  if (isNoReturn())
    attrs.next() << "noreturn";

  attrs.finish();
  
  OS << getInput() << " -> " << getResult();
}

void PolymorphicFunctionType::printGenericParams(raw_ostream &OS) const {
  OS << '<';
  auto params = getGenericParameters();
  for (unsigned i = 0, e = params.size(); i != e; ++i) {
    if (i) OS << ", ";
    
    auto paramTy = params[i].getAsTypeParam();
    OS << paramTy->getName().str();
    auto inherited = paramTy->getInherited();
    if (inherited.empty()) {
      if (unsigned printSize = (paramTy->getSuperclass()? 1 : 0)
                             + paramTy->getProtocols().size()) {
        OS << " : ";
        if (printSize > 1)
          OS << "protocol<";
        bool printedFirst = false;
        if (auto superclass = paramTy->getSuperclass()) {
          printedFirst = true;
          superclass->print(OS);
        }
        for (auto proto : paramTy->getProtocols()) {
          if (printedFirst) {
            OS << ", ";
          } else {
            printedFirst = true;
          }

          proto->getDeclaredType()->print(OS);
        }
        if (printSize > 1)
          OS << ">";
      }
    } else {
      OS << " : ";
      if (inherited.size() > 1)
        OS << "protocol<";
      for (unsigned ii = 0, ie = inherited.size(); ii != ie; ++ii) {
        if (ii)
          OS << ", ";

        OS << inherited[ii].getType();
      }
      if (inherited.size() > 1)
        OS << ">";
    }
  }
  OS << '>';
}

void PolymorphicFunctionType::print(raw_ostream &OS) const {
  AttributePrinter attrs(OS);
  printCC(attrs, getAbstractCC());
  if (isThin())
    attrs.next() << "thin";
  if (isNoReturn())
    attrs.next() << "noreturn";

  attrs.finish();
    
  printGenericParams(OS);
  OS << ' ' << getInput() << " -> " << getResult();
}

void ArraySliceType::print(raw_ostream &OS) const {
  OS << getBaseType() << "[]";
}

void OptionalType::print(raw_ostream &OS) const {
  OS << getBaseType() << '?';
}

void ArrayType::print(raw_ostream &OS) const {
  OS << Base << '[' << Size << ']';
}

void ProtocolType::print(raw_ostream &OS) const {
  OS << getDecl()->getName().str();
}

void ProtocolCompositionType::print(raw_ostream &OS) const {
  OS << "protocol<";
  bool First = true;
  for (auto Proto : Protocols) {
    if (First)
      First = false;
    else
      OS << ", ";
    Proto->print(OS);
  }
  OS << ">";
}

void LValueType::print(raw_ostream &OS) const {
  OS << "[byref";

  Qual qs = getQualifiers();
  if (qs != Qual::DefaultForType) {
    bool hasQual = false;
#define APPEND_QUAL(cond, text) do { \
      if (cond) {                    \
        if (hasQual) OS << ", ";     \
        hasQual = true;              \
        OS << text;                  \
      }                              \
    } while(false)

    OS << '(';
    APPEND_QUAL(qs & Qual::Implicit, "implicit");
    APPEND_QUAL(qs & Qual::NonSettable, "nonsettable");
    OS << ')';

#undef APPEND_QUAL
  }
  OS << "] ";
  getObjectType().print(OS);
}

void UnboundGenericType::print(raw_ostream &OS) const {
  if (auto parent = getParent()) {
    parent.print(OS);
    OS << ".";
  }

  OS << getDecl()->getName().get();
}

void BoundGenericType::print(raw_ostream &OS) const {
  if (auto parent = getParent()) {
    parent.print(OS);
    OS << ".";
  }

  OS << getDecl()->getName().get();
  printGenericArgs(OS, getGenericArgs());
}

void StructType::print(raw_ostream &OS) const {
  if (auto parent = getParent()) {
    parent.print(OS);
    OS << ".";
  }

  OS << getDecl()->getName().get();
}

void ClassType::print(raw_ostream &OS) const {
  if (auto parent = getParent()) {
    parent.print(OS);
    OS << ".";
  }

  OS << getDecl()->getName().get();
}

void UnionType::print(raw_ostream &OS) const {
  if (auto parent = getParent()) {
    parent.print(OS);
    OS << ".";
  }

  OS << getDecl()->getName().get();
}

void ArchetypeType::print(raw_ostream &OS) const {
  OS << getFullName();
}

void GenericTypeParamType::print(raw_ostream &OS) const {
  auto name = getDecl()->getName();
  if (name.empty())
    OS << "<anonymous>";
  else
    OS << name.str();
}

void AssociatedTypeType::print(raw_ostream &OS) const {
  auto name = getDecl()->getName();
  if (name.empty())
    OS << "<anonymous>";
  else
    OS << name.str();
}

void SubstitutedType::print(raw_ostream &OS) const {
  getReplacementType().print(OS);
}

void DependentMemberType::print(raw_ostream &OS) const {
  getBase().print(OS);
  OS << "." << getName().str();
}

void ReferenceStorageType::print(raw_ostream &OS) const {
  switch (getOwnership()) {
  case Ownership::Strong: llvm_unreachable("strong reference storage");
  case Ownership::Unowned: OS << "[unowned] "; break;
  case Ownership::Weak: OS << "[weak] "; break;
  }
  getReferentType().print(OS);
}
