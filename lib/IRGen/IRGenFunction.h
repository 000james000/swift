//===--- IRGenFunction.h - IR Generation for Swift Functions ---*- C++ -*-===//
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
// This file defines the structure used to generate the IR body of a
// function.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_IRGENFUNCTION_H
#define SWIFT_IRGEN_IRGENFUNCTION_H

#include "swift/Basic/LLVM.h"
#include "swift/AST/Type.h"
#include "swift/SIL/SILLocation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/CallingConv.h"
#include "IRBuilder.h"


namespace llvm {
  class AllocaInst;
  class CallSite;
  class Constant;
  class Function;
}

namespace swift {
  class ArchetypeType;
  class ClassDecl;
  class ConstructorDecl;
  class Decl;
  class ExtensionDecl;
  class FuncDecl;
  class EnumElementDecl;
  class EnumType;
  template<typename T> class Optional;
  class Pattern;
  class PatternBindingDecl;
  class SILDebugScope;
  class SILType;
  class SourceLoc;
  class StructType;
  class Substitution;
  class ValueDecl;
  class VarDecl;
  
namespace irgen {
  enum class CheckedCastMode : unsigned char;
  class Explosion;
  class FunctionRef;
  class HeapLayout;
  class IRGenModule;
  class LinkEntity;
  class Scope;
  class TypeInfo;

/// LocalTypeData - A nonce value for storing some sort of
/// locally-known information about a type.
/// 
/// The enumerated values are all in the "negative" range and so do
/// not collide with reasonable index values.
enum class LocalTypeData : unsigned {
  /// A reference to a metatype.
  Metatype = ~0U
};
  
/// Discriminator for checked cast modes.
enum class CheckedCastMode : unsigned char {
  Unconditional,
  Conditional,
};

/// IRGenFunction - Primary class for emitting LLVM instructions for a
/// specific function.
class IRGenFunction {
public:
  IRGenModule &IGM;
  IRBuilder Builder;

  llvm::Function *CurFn;
  llvm::Value *ContextPtr;

  IRGenFunction(IRGenModule &IGM,
                llvm::Function *fn,
                SILDebugScope* DbgScope = nullptr,
                Optional<SILLocation> DbgLoc = Nothing);
  ~IRGenFunction();

  void unimplemented(SourceLoc Loc, StringRef Message);

  friend class Scope;

//--- Function prologue and epilogue -------------------------------------------
public:
  Explosion collectParameters(ResilienceExpansion explosionLevel);
  void emitScalarReturn(SILType resultTy, Explosion &scalars);
  
  void emitBBForReturn();
  bool emitBranchToReturnBB();
  
private:
  void emitPrologue();
  void emitEpilogue();

  Address ReturnSlot;
  llvm::BasicBlock *ReturnBB;

//--- Helper methods -----------------------------------------------------------
public:
  Address createAlloca(llvm::Type *ty, Alignment align,
                       const llvm::Twine &name);

  llvm::BasicBlock *createBasicBlock(const llvm::Twine &Name);
  const TypeInfo &getTypeInfoForUnlowered(Type subst);
  const TypeInfo &getTypeInfoForUnlowered(AbstractionPattern orig, Type subst);
  const TypeInfo &getTypeInfoForUnlowered(AbstractionPattern orig,
                                          CanType subst);
  const TypeInfo &getTypeInfoForLowered(CanType T);
  const TypeInfo &getTypeInfo(SILType T);
  void emitMemCpy(llvm::Value *dest, llvm::Value *src,
                  Size size, Alignment align);
  void emitMemCpy(llvm::Value *dest, llvm::Value *src,
                  llvm::Value *size, Alignment align);
  void emitMemCpy(Address dest, Address src, Size size);
  void emitMemCpy(Address dest, Address src, llvm::Value *size);

  llvm::Value *emitByteOffsetGEP(llvm::Value *base, llvm::Value *offset,
                                 llvm::Type *objectType,
                                 const llvm::Twine &name = "");
  Address emitByteOffsetGEP(llvm::Value *base, llvm::Value *offset,
                            const TypeInfo &type,
                            const llvm::Twine &name = "");

  llvm::Value *emitAllocObjectCall(llvm::Value *metadata, llvm::Value *size,
                                   llvm::Value *alignMask,
                                   const llvm::Twine &name = "");
  llvm::Value *emitAllocRawCall(llvm::Value *size, llvm::Value *alignMask,
                                const llvm::Twine &name ="");
  void emitDeallocRawCall(llvm::Value *pointer, llvm::Value *size);
  
  void emitAllocBoxCall(llvm::Value *typeMetadata,
                        llvm::Value *&box,
                        llvm::Value *&valueAddress);

  llvm::Value *emitTypeMetadataRef(CanType type);
  llvm::Value *emitTypeMetadataRef(SILType type);
  llvm::Value *emitValueWitnessTableRefForMetadata(llvm::Value *metadata);

  /// Emit a load of a reference to the given Objective-C selector.
  llvm::Value *emitObjCSelectorRefLoad(StringRef selector);

  /// Return the SILDebugScope for this function.
  SILDebugScope* getDebugScope() const { return DbgScope; }
  llvm::Value *coerceValue(llvm::Value *value, llvm::Type *fromTy,
                           llvm::Type *toTy);

private:
  llvm::Instruction *AllocaIP;
  SILDebugScope* DbgScope;

//--- Reference-counting methods -----------------------------------------------
public:
  llvm::Value *emitUnmanagedAlloc(const HeapLayout &layout,
                                  const llvm::Twine &name);
  void emitLoadAndRetain(Address addr, Explosion &explosion);
  void emitAssignRetained(llvm::Value *value, Address addr);
  void emitInitializeRetained(llvm::Value *value, Address addr);
  void emitRetain(llvm::Value *value, Explosion &explosion);
  void emitRetainCall(llvm::Value *value);
  void emitRelease(llvm::Value *value);
  void emitRetainUnowned(llvm::Value *value);
  void emitUnownedRetain(llvm::Value *value);
  void emitUnownedRelease(llvm::Value *value);
  void emitWeakInit(llvm::Value *value, Address dest);
  void emitWeakAssign(llvm::Value *value, Address dest);
  llvm::Value *emitWeakLoadStrong(Address src, llvm::Type *type);
  llvm::Value *emitWeakTakeStrong(Address src, llvm::Type *type);
  void emitWeakDestroy(Address addr);
  void emitWeakCopyInit(Address destAddr, Address srcAddr);
  void emitWeakTakeInit(Address destAddr, Address srcAddr);
  void emitWeakCopyAssign(Address destAddr, Address srcAddr);
  void emitWeakTakeAssign(Address destAddr, Address srcAddr);
  void emitObjCRetain(llvm::Value *value, Explosion &explosion);
  llvm::Value *emitObjCRetainCall(llvm::Value *value);
  void emitObjCRelease(llvm::Value *value);
  
  /// Emit a retain of a class instance with unknown retain semantics.
  void emitUnknownRetain(llvm::Value *value, Explosion &explosion);
  /// Emit a retain of a class instance with unknown retain semantics, and
  /// return the retained value.
  llvm::Value *emitUnknownRetainCall(llvm::Value *value);
  /// Emit a release of a class instance with unknown retain semantics.
  void emitUnknownRelease(llvm::Value *value);
  void emitUnknownUnownedRetain(llvm::Value *value);
  void emitUnknownUnownedRelease(llvm::Value *value);
  void emitUnknownRetainUnowned(llvm::Value *value);
  void emitUnknownWeakDestroy(Address addr);
  void emitUnknownWeakCopyInit(Address destAddr, Address srcAddr);
  void emitUnknownWeakTakeInit(Address destAddr, Address srcAddr);
  void emitUnknownWeakCopyAssign(Address destAddr, Address srcAddr);
  void emitUnknownWeakTakeAssign(Address destAddr, Address srcAddr);
  void emitUnknownWeakInit(llvm::Value *value, Address dest);
  void emitUnknownWeakAssign(llvm::Value *value, Address dest);
  llvm::Value *emitUnknownWeakLoadStrong(Address src, llvm::Type *type);
  llvm::Value *emitUnknownWeakTakeStrong(Address src, llvm::Type *type);

//--- Expression emission ------------------------------------------------------
public:
  void emitFakeExplosion(const TypeInfo &type, Explosion &explosion);

  /// \brief Convert the given explosion to the given destination archetype,
  /// using a runtime-checked cast.
  llvm::Value *emitSuperToClassArchetypeConversion(llvm::Value *super,
                                                   SILType destType,
                                                   CheckedCastMode mode);
  
  /// \brief Convert the given value to the given destination type, using a
  /// runtime-checked cast.
  llvm::Value *emitDowncast(llvm::Value *from,
                            SILType toType,
                            CheckedCastMode mode);
  

//--- Declaration emission -----------------------------------------------------
public:

  void bindArchetype(ArchetypeType *type,
                     llvm::Value *metadata,
                     ArrayRef<llvm::Value*> wtables);

//--- Type emission ------------------------------------------------------------
public:
  /// Look for a mapping for a local type-metadata reference.
  llvm::Value *tryGetLocalTypeData(CanType type, LocalTypeData index) {
    auto key = getLocalTypeDataKey(type, index);
    auto it = LocalTypeDataMap.find(key);
    if (it == LocalTypeDataMap.end())
      return nullptr;
    return it->second;
  }

  /// Retrieve a local type-metadata reference which is known to exist.
  llvm::Value *getLocalTypeData(CanType type, LocalTypeData index) {
    auto key = getLocalTypeDataKey(type, index);
    assert(LocalTypeDataMap.count(key) && "no mapping for local type data");
    return LocalTypeDataMap.find(key)->second;
  }

  /// Add a local type-metadata reference at a point which dominates
  /// the entire function.
  void setUnscopedLocalTypeData(CanType type, LocalTypeData index,
                                llvm::Value *data) {
    assert(data && "setting a null value for type data!");

    auto key = getLocalTypeDataKey(type, index);
    assert(!LocalTypeDataMap.count(key) &&
           "existing mapping for local type data");
    LocalTypeDataMap.insert(std::make_pair(key, data));
  }

private:
  typedef unsigned LocalTypeDataDepth;
  typedef std::pair<TypeBase*,unsigned> LocalTypeDataPair;
  LocalTypeDataPair getLocalTypeDataKey(CanType type, LocalTypeData index) {
    return LocalTypeDataPair(type.getPointer(), unsigned(index));
  }

  llvm::DenseMap<LocalTypeDataPair, llvm::Value*> LocalTypeDataMap;
};

} // end namespace irgen
} // end namespace swift

#endif
