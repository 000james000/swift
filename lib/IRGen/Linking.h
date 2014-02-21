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
  /// ValueDecl*, SILFunction*, ProtocolConformance*, or TypeBase*,
  /// depending on Kind.
  void *Pointer;

  /// A hand-rolled bitfield with the following layout:
  unsigned Data;

  enum : unsigned {
    KindShift = 0, KindMask = 0xFF,

    // These fields appear in decl kinds.
    ExplosionLevelShift = 8, ExplosionLevelMask = 0xFF00,
    UncurryLevelShift = 16, UncurryLevelMask = 0xFF0000,

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

    /// The nominal type descriptor for a nominal type.
    /// The pointer is a NominalTypeDecl*.
    NominalTypeDescriptor,
    
    /// The protocol descriptor for a protocol type.
    /// The pointer is a ProtocolDecl*.
    ProtocolDescriptor,

    /// Some other kind of declaration.
    /// The pointer is a Decl*.
    Other,

    /// A SIL function. The pointer is a SILFunction*.
    SILFunction,
    
    /// A SIL global variable. The pointer is a SILGlobalVariable*.
    SILGlobalVariable,

    /// A direct protocol witness table. The pointer is a ProtocolConformance*.
    DirectProtocolWitnessTable,
    
    /// A lazy protocol witness accessor function. The pointer is a
    /// ProtocolConformance*.
    LazyProtocolWitnessTableAccessor,
    
    /// A template for lazy protocol witness table initialization. The pointer
    /// is a ProtocolConformance*.
    LazyProtocolWitnessTableTemplate,
    
    /// A dependent protocol witness table instantiation function. The pointer
    /// is a ProtocolConformance*.
    DependentProtocolWitnessTableGenerator,
    
    /// A template for dependent protocol witness table instantiation. The
    /// pointer is a ProtocolConformance*.
    DependentProtocolWitnessTableTemplate,
    
    // Everything following this is a type kind.

    /// A value witness for a type.
    /// The pointer is a canonical TypeBase*.
    ValueWitness,

    /// The value witness table for a type.
    /// The pointer is a canonical TypeBase*.
    ValueWitnessTable,

    /// The metadata or metadata template for a type.
    /// The pointer is a canonical TypeBase*.
    TypeMetadata,
    
    /// A type which is being mangled just for its string.
    /// The pointer is a canonical TypeBase*.
    TypeMangling,

    /// A Swift-to-ObjC block converter function.
    /// The pointer is a canonical TypeBase*.
    BridgeToBlockConverter,
  };
  friend struct llvm::DenseMapInfo<LinkEntity>;

  static bool isFunction(ValueDecl *decl) {
    return (isa<FuncDecl>(decl) || isa<EnumElementDecl>(decl) ||
            isa<ConstructorDecl>(decl));
  }

  static bool hasGetterSetter(ValueDecl *decl) {
    return (isa<VarDecl>(decl) || isa<SubscriptDecl>(decl));
  }

  Kind getKind() const {
    return Kind(LINKENTITY_GET_FIELD(Data, Kind));
  }

  static bool isDeclKind(Kind k) {
    return !isTypeKind(k) && !isProtocolConformanceKind(k);
  }
  static bool isTypeKind(Kind k) {
    return k >= Kind::ValueWitness;
  }
  
  static bool isProtocolConformanceKind(Kind k) {
    return k >= Kind::DirectProtocolWitnessTable
      && k <= Kind::DependentProtocolWitnessTableTemplate;
  }

  void setForDecl(Kind kind, 
                  ValueDecl *decl, ResilienceExpansion explosionKind,
                  unsigned uncurryLevel) {
    assert(isDeclKind(kind));
    Pointer = decl;
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind))
         | LINKENTITY_SET_FIELD(ExplosionLevel, unsigned(explosionKind))
         | LINKENTITY_SET_FIELD(UncurryLevel, uncurryLevel);
  }

  void setForProtocolConformance(Kind kind,
                                 ProtocolConformance *c) {
    assert(isProtocolConformanceKind(kind));
    Pointer = c;
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind));
  }

  void setForType(Kind kind, CanType type) {
    assert(isTypeKind(kind));
    Pointer = type.getPointer();
    Data = LINKENTITY_SET_FIELD(Kind, unsigned(kind));
  }

  LinkEntity() = default;

public:
  static LinkEntity forFunction(CodeRef fn) {
    LinkEntity entity;
    entity.setForDecl(Kind::Function, fn.getDecl(),
                      fn.getExplosionLevel(), fn.getUncurryLevel());
    return entity;
  }
  
  static LinkEntity forNonFunction(ValueDecl *decl) {
    assert(!isFunction(decl));

    LinkEntity entity;
    entity.setForDecl(Kind::Other, decl, ResilienceExpansion(0), 0);
    return entity;
  }

  static LinkEntity forWitnessTableOffset(ValueDecl *decl,
                                          ResilienceExpansion explosionKind,
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

  static LinkEntity forObjCClass(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCClass, decl, ResilienceExpansion::Minimal, 0);
    return entity;
  }

  static LinkEntity forObjCMetaclass(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ObjCMetaclass, decl, ResilienceExpansion::Minimal, 0);
    return entity;
  }

  static LinkEntity forSwiftMetaclassStub(ClassDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::SwiftMetaclassStub,
                      decl, ResilienceExpansion::Minimal, 0);
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
  
  static LinkEntity forNominalTypeDescriptor(NominalTypeDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::NominalTypeDescriptor,
                      decl, ResilienceExpansion::Minimal, 0);
    return entity;
  }
  
  static LinkEntity forProtocolDescriptor(ProtocolDecl *decl) {
    LinkEntity entity;
    entity.setForDecl(Kind::ProtocolDescriptor,
                      decl, ResilienceExpansion::Minimal, 0);
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

  static LinkEntity forBridgeToBlockConverter(SILType type) {
    LinkEntity entity;
    entity.setForType(Kind::BridgeToBlockConverter, type.getSwiftRValueType());
    return entity;
  }

  static LinkEntity forSILFunction(SILFunction *F)
  {
    LinkEntity entity;
    entity.Pointer = F;
    entity.Data = LINKENTITY_SET_FIELD(Kind, unsigned(Kind::SILFunction));
    return entity;
  }
  
  static LinkEntity forSILGlobalVariable(SILGlobalVariable *G) {
    LinkEntity entity;
    entity.Pointer = G;
    entity.Data = LINKENTITY_SET_FIELD(Kind, unsigned(Kind::SILGlobalVariable));
    return entity;
  }
  
  static LinkEntity forDirectProtocolWitnessTable(const ProtocolConformance *C){
    LinkEntity entity;
    entity.Pointer = const_cast<ProtocolConformance*>(C);
    entity.Data
      = LINKENTITY_SET_FIELD(Kind, unsigned(Kind::DirectProtocolWitnessTable));
    return entity;
  }

  void mangle(llvm::raw_ostream &out) const;
  void mangle(SmallVectorImpl<char> &buffer) const;
  SILLinkage getLinkage(ForDefinition_t isDefinition) const;

  ValueDecl *getDecl() const {
    assert(isDeclKind(getKind()));
    return reinterpret_cast<ValueDecl*>(Pointer);
  }
  
  SILFunction *getSILFunction() const {
    assert(getKind() == Kind::SILFunction);
    return reinterpret_cast<SILFunction*>(Pointer);
  }
  
  SILGlobalVariable *getSILGlobalVariable() const {
    assert(getKind() == Kind::SILGlobalVariable);
    return reinterpret_cast<SILGlobalVariable*>(Pointer);
  }
  
  ProtocolConformance *getProtocolConformance() const {
    assert(isProtocolConformanceKind(getKind()));
    return reinterpret_cast<ProtocolConformance*>(Pointer);
  }
  
  ResilienceExpansion getResilienceExpansion() const {
    assert(isDeclKind(getKind()));
    return ResilienceExpansion(LINKENTITY_GET_FIELD(Data, ExplosionLevel));
  }
  unsigned getUncurryLevel() const {
    return LINKENTITY_GET_FIELD(Data, UncurryLevel);
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
  static LinkInfo get(IRGenModule &IGM, const LinkEntity &entity,
                      ForDefinition_t forDefinition);

  StringRef getName() const {
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
                                 const llvm::AttributeSet &attrs,
                                 llvm::Function *insertBefore = nullptr);


  llvm::GlobalVariable *createVariable(IRGenModule &IGM,
                                  llvm::Type *objectType,
                                  DebugTypeInfo DebugType=DebugTypeInfo(),
                                  Optional<SILLocation> DebugLoc = Nothing,
                                  StringRef DebugName = StringRef());
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
