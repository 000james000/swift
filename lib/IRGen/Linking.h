//===--- Linking.h - Common declarations for link information ---*- C++ -*-===//
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
// This file defines structures and routines used when creating global
// entities that are placed in the LLVM module, potentially with
// external linkage.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_LINKING_H
#define SWIFT_IRGEN_LINKING_H

#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/GlobalValue.h"
#include "DebugTypeInfo.h"
#include "FunctionRef.h"
#include "IRGen.h"
#include "ValueWitness.h"

namespace llvm {
  class AttributeSet;
  class Value;
  class FunctionType;
}

namespace swift {

namespace Mangle {
  enum class ExplosionKind : unsigned;
}
  
namespace irgen {
class TypeInfo;
class IRGenModule;

/// A link entity is some sort of named declaration, combined with all
/// the information necessary to distinguish specific implementations
/// of the declaration from each other.
///
/// For example, functions may be exploded or uncurried at different
/// levels, each of which potentially creates a different top-level
/// function.
class LinkEntity {
  /// A ValueDecl*, CapturingExpr*, or a TypeBase*, depending on Kind.
  void *Pointer;

  /// A hand-rolled bitfield with the following layout:
  unsigned Data;

  enum : unsigned {
    KindShift = 0, KindMask = 0xFF,

    // These fields appear in decl kinds.
    ExplosionLevelShift = 8, ExplosionLevelMask = 0xFF00,
    UncurryLevelShift = 16, UncurryLevelMask = 0xFF0000,

    // This field appears in the Destructor kind.
    DtorKindShift = 24, DtorKindMask = 0x01000000,

    // This field appears in the Constructor kind.
    CtorKindShift = 24, CtorKindMask = 0x01000000,

    // This field appears in the ValueWitness kind.
    ValueWitnessShift = 8, ValueWitnessMask = 0xFF00,

    // These fields appear in the TypeMetadata kind.
    IsIndirectShift = 8, IsIndirectMask = 0x0100,
    IsPatternShift = 9, IsPatternMask = 0x0200,
  };
#define LINKENTITY_SET_FIELD(field, value) (value << field##Shift)
#define LINKENTITY_GET_FIELD(value, field) ((value & field##Mask) >> field##Shift)

  enum class Kind {
    /// A function.
    /// The pointer is a FuncDecl*.
    Function,
    
    /// An anonymous function.
    /// The pointer is a CapturingExpr*.
    AnonymousFunction,

    /// The getter for an entity.
    /// The pointer is a VarDecl* or SubscriptDecl*.
    Getter,

    /// The setter for an entity.
    /// The pointer is a VarDecl* or SubscriptDecl*.
    Setter,

    /// A constructor for a class.
    /// The pointer is a ConstructorDecl*.
    Constructor,

    /// The destructor for a class.
    /// The pointer is a ClassDecl*.
    Destructor,

    /// The offset to apply to a witness table or metadata object
    /// in order to find the information for a declaration.  The
    /// pointer is a ValueDecl*.
    WitnessTableOffset,

    /// A field offset.  The pointer is a VarDecl*.
    FieldOffset,

    /// An Objective-C class reference.  The pointer is a ClassDecl*.
    ObjCClass,

    /// An Objective-C metaclass reference.  The pointer is a ClassDecl*.
    ObjCMetaclass,

    /// A swift metaclass-stub reference.  The pointer is a ClassDecl*.
    SwiftMetaclassStub,

    /// Some other kind of declaration.
    /// The pointer is a Decl*.
    Other,

    /// A SIL function that has no backing Decl.  The pointer is a SILFunction
    /// which has a name already set.
    SILFunction,

    // Everything following this is a type kind.

    /// A value witness for a type.
    /// The pointer is a canonical TypeBase*.
    ValueWitness,

    /// The value witness table for a type.
    /// The pointer is a canonical TypeBase*.
    ValueWitnessTable,

    /// The metadata or metadata template for a class.
    /// The pointer is a canonical TypeBase*.
    TypeMetadata,

    /// A type which is being mangled just for its string.
    /// The pointer is a canonical TypeBase*.
    TypeMangling,

    /// A type which is being mangled for the DWARF debug info.
    /// The pointer is a canonical TypeBase*.
    DebuggerTypeMangling,
    
    /// A Swift-to-ObjC block converter function.
    /// The pointer is a canonical TypeBase*.
    BridgeToBlockConverter,
  };
  friend struct llvm::DenseMapInfo<LinkEntity>;

  static bool isFunction(ValueDecl *decl) {
    return (isa<FuncDecl>(decl) || isa<OneOfElementDecl>(decl) ||
            isa<ConstructorDecl>(decl));
  }

  static bool hasGetterSetter(ValueDecl *decl) {
    return (isa<VarDecl>(decl) || isa<SubscriptDecl>(decl));
  }

  Kind getKind() const {
    return Kind(LINKENTITY_GET_FIELD(Data, Kind));
  }

  static bool isDeclKind(Kind k) {
    return !isTypeKind(k) && !isCapturingExprKind(k);
  }
  static bool isCapturingExprKind(Kind k) {
    return k == Kind::AnonymousFunction;
  }
  static bool isTypeKind(Kind k) {
    return k >= Kind::ValueWitness;
  }

  void setForDecl(Kind kind, 
                  ValueDecl *decl, Mangle::ExplosionKind explosionKind,
                  unsigned uncurryLevel) {
    assert(isDeclKind(kind));
    Pointer = decl;
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind))
         | LINKENTITY_SET_FIELD(ExplosionLevel, unsigned(explosionKind))
         | LINKENTITY_SET_FIELD(UncurryLevel, uncurryLevel);
  }

  void setForCapturingExpr(Kind kind,
                  CapturingExpr *expr, Mangle::ExplosionKind explosionKind,
                  unsigned uncurryLevel) {
    assert(isCapturingExprKind(kind));
    Pointer = expr;
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind))
         | LINKENTITY_SET_FIELD(ExplosionLevel, unsigned(explosionKind))
         | LINKENTITY_SET_FIELD(UncurryLevel, uncurryLevel);
  }

  void setForType(Kind kind, CanType type) {
    assert(isTypeKind(kind));
    Pointer = type.getPointer();
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind));
  }

  static Kind getKindForFunction(FunctionRef::Kind fn) {
    switch (fn) {
    case FunctionRef::Kind::Function: return Kind::Function;
    case FunctionRef::Kind::Getter: return Kind::Getter;
    case FunctionRef::Kind::Setter: return Kind::Setter;
    }
    llvm_unreachable("bad FunctionRef kind");
  }

  LinkEntity() = default;

public:
  static LinkEntity forFunction(CodeRef fn) {
    LinkEntity entity;
    entity.setForDecl(getKindForFunction(fn.getKind()), fn.getDecl(),
                      fn.getExplosionLevel(), fn.getUncurryLevel());
    return entity;
  }
  
  static LinkEntity forAnonymousFunction(CapturingExpr *expr,
                                         Mangle::ExplosionKind explosionLevel,
                                         unsigned uncurryLevel) {
    LinkEntity entity;
    entity.setForCapturingExpr(Kind::AnonymousFunction,
                               expr, explosionLevel, uncurryLevel);
    return entity;
  }

  static LinkEntity forNonFunction(ValueDecl *decl) {
    assert(!isFunction(decl));

    LinkEntity entity;
    entity.setForDecl(Kind::Other, decl, Mangle::ExplosionKind(0), 0);
    return entity;
  }

  static LinkEntity forWitnessTableOffset(ValueDecl *decl,
                                          Mangle::ExplosionKind explosionKind,
                                          unsigned uncurryLevel) {
    LinkEntity entity;
    entity.setForDecl(Kind::WitnessTableOffset, decl,
                      explosionKind, uncurryLevel);
    return entity;
  }

  static LinkEntity forFieldOffset(VarDecl *decl, bool isIndirect) {
    LinkEntity entity;
    entity.Pointer = decl;
    entity.Data = LINKENTITY_SET_FIELD(Kind, unsigned(Kind::FieldOffset))
                | LINKENTITY_SET_FIELD(IsIndirect, unsigned(isIndirect));
    return entity;
  }

  static LinkEntity forConstructor(CodeRef fn, ConstructorKind kind) {
    LinkEntity entity;
    entity.setForDecl(Kind::Constructor, fn.getDecl(),
                      fn.getExplosionLevel(), fn.getUncurryLevel());
    entity.Data |= LINKENTITY_SET_FIELD(CtorKind, unsigned(kind));
    return entity;
  }

  static LinkEntity forDestructor(ClassDecl *decl, DestructorKind kind) {
    LinkEntity entity;
    entity.setForDecl(Kind::Destructor, decl, Mangle::ExplosionKind::Minimal, 0);
    entity.Data |= LINKENTITY_SET_FIELD(DtorKind, unsigned(kind));
    return entity;
  }

  static LinkEntity forObjCClass(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCClass, decl, Mangle::ExplosionKind::Minimal, 0);
    return entity;
  }

  static LinkEntity forObjCMetaclass(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCMetaclass, decl, Mangle::ExplosionKind::Minimal, 0);
    return entity;
  }

  static LinkEntity forSwiftMetaclassStub(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::SwiftMetaclassStub,
                      decl, Mangle::ExplosionKind::Minimal, 0);
    return entity;
  }

  static LinkEntity forTypeMetadata(CanType concreteType, bool isIndirect,
                                    bool isPattern) {
    LinkEntity entity;
    entity.Pointer = concreteType.getPointer();
    entity.Data = LINKENTITY_SET_FIELD(Kind, unsigned(Kind::TypeMetadata))
                | LINKENTITY_SET_FIELD(IsIndirect, unsigned(isIndirect))
                | LINKENTITY_SET_FIELD(IsPattern, unsigned(isPattern));
    return entity;
  }

  static LinkEntity forValueWitness(CanType concreteType, ValueWitness witness) {
    LinkEntity entity;
    entity.Pointer = concreteType.getPointer();
    entity.Data = LINKENTITY_SET_FIELD(Kind, unsigned(Kind::ValueWitness))
                | LINKENTITY_SET_FIELD(ValueWitness, unsigned(witness));
    return entity;
  }

  static LinkEntity forValueWitnessTable(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::ValueWitnessTable, type);
    return entity;
  }

  static LinkEntity forTypeMangling(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::TypeMangling, type);
    return entity;
  }

  static LinkEntity forDebuggerTypeMangling(CanType type) {
    LinkEntity entity;
    entity.setForType(Kind::DebuggerTypeMangling, type);
    return entity;
  }
  
  static LinkEntity forBridgeToBlockConverter(SILType type) {
    LinkEntity entity;
    entity.setForType(Kind::BridgeToBlockConverter, type.getSwiftRValueType());
    return entity;
  }

  static LinkEntity forSILFunction(SILFunction *F, Mangle::ExplosionKind level)
  {
    LinkEntity entity;
    entity.Pointer = F;
    entity.Data = LINKENTITY_SET_FIELD(Kind, unsigned(Kind::SILFunction))
                | LINKENTITY_SET_FIELD(ExplosionLevel, unsigned(level));
    return entity;
  }

  void mangle(llvm::raw_ostream &out) const;
  void mangle(llvm::SmallVectorImpl<char> &buffer) const;
  bool isLocalLinkage() const;
  bool isClangThunk() const;

  ValueDecl *getDecl() const {
    assert(isDeclKind(getKind()));
    return reinterpret_cast<ValueDecl*>(Pointer);
  }
  
  CapturingExpr *getCapturingExpr() const {
    assert(isCapturingExprKind(getKind()));
    return reinterpret_cast<CapturingExpr*>(Pointer);
  }

  SILFunction *getSILFunction() const {
    assert(getKind() == Kind::SILFunction);
    return reinterpret_cast<SILFunction*>(Pointer);
  }
  
  Mangle::ExplosionKind getExplosionKind() const {
    assert(isDeclKind(getKind()) || isCapturingExprKind(getKind()));
    return Mangle::ExplosionKind(LINKENTITY_GET_FIELD(Data, ExplosionLevel));
  }
  unsigned getUncurryLevel() const {
    return LINKENTITY_GET_FIELD(Data, UncurryLevel);
  }

  DestructorKind getDestructorKind() const {
    assert(getKind() == Kind::Destructor);
    return DestructorKind(LINKENTITY_GET_FIELD(Data, DtorKind));
  }

  ConstructorKind getConstructorKind() const {
    assert(getKind() == Kind::Constructor);
    return ConstructorKind(LINKENTITY_GET_FIELD(Data, CtorKind));
  }

  bool isValueWitness() const { return getKind() == Kind::ValueWitness; }
  CanType getType() const {
    assert(isTypeKind(getKind()));
    return CanType(reinterpret_cast<TypeBase*>(Pointer));
  }
  ValueWitness getValueWitness() const {
    assert(getKind() == Kind::ValueWitness);
    return ValueWitness(LINKENTITY_GET_FIELD(Data, ValueWitness));
  }
  bool isMetadataIndirect() const {
    assert(getKind() == Kind::TypeMetadata);
    return LINKENTITY_GET_FIELD(Data, IsIndirect);
  }
  bool isMetadataPattern() const {
    assert(getKind() == Kind::TypeMetadata);
    return LINKENTITY_GET_FIELD(Data, IsPattern);
  }

  bool isOffsetIndirect() const {
    assert(getKind() == Kind::FieldOffset);
    return LINKENTITY_GET_FIELD(Data, IsIndirect);
  }

#undef LINKENTITY_GET_FIELD
#undef LINKENTITY_SET_FIELD
};

/// Encapsulated information about the linkage of an entity.
class LinkInfo {
  LinkInfo() = default;

  llvm::SmallString<32> Name;
  llvm::GlobalValue::LinkageTypes Linkage;
  llvm::GlobalValue::VisibilityTypes Visibility;

public:
  /// Compute linkage information for the given 
  static LinkInfo get(IRGenModule &IGM, const LinkEntity &entity);

  llvm::StringRef getName() const {
    return Name.str();
  }
  llvm::GlobalValue::LinkageTypes getLinkage() const {
    return Linkage;
  }
  llvm::GlobalValue::VisibilityTypes getVisibility() const {
    return Visibility;
  }

  llvm::Function *createFunction(IRGenModule &IGM,
                                 llvm::FunctionType *fnType,
                                 llvm::CallingConv::ID cc,
                                 const llvm::AttributeSet &attrs);


  llvm::GlobalVariable *createVariable(IRGenModule &IGM,
                                       llvm::Type *objectType,
                                       DebugTypeInfo DebugType=DebugTypeInfo(),
                                       SILLocation DebugLoc = SILLocation());
};

} // end namespace irgen
} // end namespace swift

/// Allow LinkEntity to be used as a key for a DenseMap.
template <> struct llvm::DenseMapInfo<swift::irgen::LinkEntity> {
  typedef swift::irgen::LinkEntity LinkEntity;
  static LinkEntity getEmptyKey() {
    LinkEntity entity;
    entity.Pointer = nullptr;
    entity.Data = 0;
    return entity;
  }
  static LinkEntity getTombstoneKey() {
    LinkEntity entity;
    entity.Pointer = nullptr;
    entity.Data = 1;
    return entity;
  }
  static unsigned getHashValue(const LinkEntity &entity) {
    return DenseMapInfo<void*>::getHashValue(entity.Pointer)
         ^ entity.Data;
  }
  static bool isEqual(const LinkEntity &LHS, const LinkEntity &RHS) {
    return LHS.Pointer == RHS.Pointer && LHS.Data == RHS.Data;
  }
};

#endif
