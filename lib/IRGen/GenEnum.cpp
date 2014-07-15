//===--- GenEnum.cpp - Swift IR Generation For 'enum' Types -------------===//
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
//  This file implements IR generation for algebraic data types (ADTs,
//  or 'enum' types) in Swift.  This includes creating the IR type as
//  well as emitting the basic access operations.
//
//  The current scheme is that all such types with are represented
//  with an initial word indicating the variant, followed by an enum
//  of all the possibilities.  This is obviously completely acceptable
//  to everyone and will not benefit from further refinement.
//
//  As a completely unimportant premature optimization, we do emit
//  types with only a single variant as simple structs wrapping that
//  variant.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "enum-layout"
#include "llvm/Support/Debug.h"

#include "GenEnum.h"

#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/Basic/Optional.h"
#include "swift/SIL/SILModule.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"

#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "NonFixedTypeInfo.h"
#include "GenMeta.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "ScalarTypeInfo.h"

using namespace swift;
using namespace irgen;

llvm::BitVector getBitVectorFromAPInt(const APInt &bits,
                                      unsigned startBit = 0) {
  llvm::BitVector result;
  result.resize(startBit, false);
  SmallVector<llvm::integerPart, 2> parts;
  
  result.resize(startBit + bits.getBitWidth(), false);
  for (unsigned i = 0, e = bits.getBitWidth(); i < e; ++i) {
    result[startBit + i] = bits[i];
  }
  
  return result;
}

void irgen::EnumImplStrategy::initializeFromParams(IRGenFunction &IGF,
                                                   Explosion &params,
                                                   Address dest,
                                                   CanType T) const {
  if (TIK >= Loadable)
    return initialize(IGF, params, dest);
  Address src = TI->getAddressForPointer(params.claimNext());
  TI->initializeWithTake(IGF, dest, src, T);
}

namespace {
  /// Implementation strategy for singleton enums, with zero or one cases.
  class SingletonEnumImplStrategy final : public EnumImplStrategy {
    const TypeInfo *getSingleton() const {
      return ElementsWithPayload.empty() ? nullptr : ElementsWithPayload[0].ti;
    }
    
    const FixedTypeInfo *getFixedSingleton() const {
      return cast_or_null<FixedTypeInfo>(getSingleton());
    }
    
    const LoadableTypeInfo *getLoadableSingleton() const {
      return cast_or_null<LoadableTypeInfo>(getSingleton());
    }

    Address getSingletonAddress(IRGenFunction &IGF, Address addr) const {
      return IGF.Builder.CreateBitCast(addr,
                             getSingleton()->getStorageType()->getPointerTo());
    }
    
    CanType getSingletonType(IRGenModule &IGM, CanType T) const {
      assert(!ElementsWithPayload.empty());
      return T->getTypeOfMember(IGM.SILMod->getSwiftModule(),
                                ElementsWithPayload[0].decl, nullptr,
                                ElementsWithPayload[0].decl->getArgumentType())
        ->getCanonicalType();
    }
    
  public:
    SingletonEnumImplStrategy(IRGenModule &IGM,
                              TypeInfoKind tik, unsigned NumElements,
                              std::vector<Element> &&WithPayload,
                              std::vector<Element> &&WithRecursivePayload,
                              std::vector<Element> &&WithNoPayload)
      : EnumImplStrategy(IGM, tik, NumElements,
                         std::move(WithPayload),
                         std::move(WithRecursivePayload),
                         std::move(WithNoPayload))
    {
      assert(NumElements <= 1);
      assert(ElementsWithPayload.size() <= 1);
    }
    
    TypeInfo *completeEnumTypeLayout(TypeConverter &TC,
                                      CanType type,
                                      EnumDecl *theEnum,
                                      llvm::StructType *enumTy) override;

    void emitSingletonSwitch(IRGenFunction &IGF,
                    ArrayRef<std::pair<EnumElementDecl*,
                                       llvm::BasicBlock*>> dests,
                    llvm::BasicBlock *defaultDest) const {
      // No dispatch necessary. Branch straight to the destination.
      assert(dests.size() <= 1 && "impossible switch table for singleton enum");
      llvm::BasicBlock *dest = dests.size() == 1
        ? dests[0].second : defaultDest;
      IGF.Builder.CreateBr(dest);
    }
    
    void emitValueSwitch(IRGenFunction &IGF,
                         Explosion &value,
                         ArrayRef<std::pair<EnumElementDecl*,
                                            llvm::BasicBlock*>> dests,
                         llvm::BasicBlock *defaultDest) const override {
      value.claim(getExplosionSize(value.getKind()));
      emitSingletonSwitch(IGF, dests, defaultDest);
    }
    
    void emitIndirectSwitch(IRGenFunction &IGF,
                            CanType T,
                            Address addr,
                            ArrayRef<std::pair<EnumElementDecl*,
                                               llvm::BasicBlock*>> dests,
                            llvm::BasicBlock *defaultDest) const override {
      emitSingletonSwitch(IGF, dests, defaultDest);
    }
    
    void emitValueProject(IRGenFunction &IGF,
                          Explosion &in,
                          EnumElementDecl *theCase,
                          Explosion &out) const override {
      // The projected value is the payload.
      if (getLoadableSingleton())
        getLoadableSingleton()->reexplode(IGF, in, out);
    }

    void emitValueInjection(IRGenFunction &IGF,
                            EnumElementDecl *elt,
                            Explosion &params,
                            Explosion &out) const override {
      // If the element carries no data, neither does the injection.
      // Otherwise, the result is identical.
      if (getLoadableSingleton())
        getLoadableSingleton()->reexplode(IGF, params, out);
    }
    
    Address projectDataForStore(IRGenFunction &IGF,
                                EnumElementDecl *elt,
                                Address enumAddr) const override {
      return getSingletonAddress(IGF, enumAddr);
    }
    
    Address destructiveProjectDataForLoad(IRGenFunction &IGF,
                                          EnumElementDecl *elt,
                                          Address enumAddr) const override {
      return getSingletonAddress(IGF, enumAddr);
    }
    
    void storeTag(IRGenFunction &IGF,
                  EnumElementDecl *elt,
                  Address enumAddr,
                  CanType T) const override {
      // No tag, nothing to do.
    }
    
    void getSchema(ExplosionSchema &schema) const {
      if (!getSingleton()) return;
      // If the payload is loadable, forward its explosion schema.
      if (TIK >= Loadable)
        return getSingleton()->getSchema(schema);
      // Otherwise, use an indirect aggregate schema with our storage
      // type.
      schema.add(ExplosionSchema::Element::forAggregate(getStorageType(),
                                      getSingleton()->getBestKnownAlignment()));
    }
    
    unsigned getExplosionSize(ResilienceExpansion kind) const {
      if (!getLoadableSingleton()) return 0;
      return getLoadableSingleton()->getExplosionSize(kind);
    }
    
    void loadAsCopy(IRGenFunction &IGF, Address addr, Explosion &e) const {
      if (!getLoadableSingleton()) return;
      getLoadableSingleton()->loadAsCopy(IGF, getSingletonAddress(IGF, addr),e);
    }

    void loadForSwitch(IRGenFunction &IGF, Address addr, Explosion &e) const {
      // Switching on a singleton does not require a value.
      return;
    }

    void loadAsTake(IRGenFunction &IGF, Address addr, Explosion &e) const {
      if (!getLoadableSingleton()) return;
      getLoadableSingleton()->loadAsTake(IGF, getSingletonAddress(IGF, addr),e);
    }
    
    void assign(IRGenFunction &IGF, Explosion &e, Address addr) const {
      if (!getLoadableSingleton()) return;
      getLoadableSingleton()->assign(IGF, e, getSingletonAddress(IGF, addr));
    }
    
    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                        CanType T) const {
      if (!getSingleton()) return;
      dest = getSingletonAddress(IGF, dest);
      src = getSingletonAddress(IGF, src);
      getSingleton()->assignWithCopy(IGF, dest, src,
                                     getSingletonType(IGF.IGM, T));
    }
    
    void assignWithTake(IRGenFunction &IGF, Address dest, Address src,
                        CanType T) const {
      if (!getSingleton()) return;
      dest = getSingletonAddress(IGF, dest);
      src = getSingletonAddress(IGF, src);
      getSingleton()->assignWithTake(IGF, dest, src,
                                     getSingletonType(IGF.IGM, T));
    }
    
    void initialize(IRGenFunction &IGF, Explosion &e, Address addr) const {
      if (!getLoadableSingleton()) return;
      getLoadableSingleton()->initialize(IGF, e, getSingletonAddress(IGF, addr));
    }
    
    void initializeWithCopy(IRGenFunction &IGF, Address dest, Address src,
                            CanType T)
    const override {
      if (!getSingleton()) return;
      dest = getSingletonAddress(IGF, dest);
      src = getSingletonAddress(IGF, src);
      getSingleton()->initializeWithCopy(IGF, dest, src,
                                         getSingletonType(IGF.IGM, T));
    }
    
    void initializeWithTake(IRGenFunction &IGF, Address dest, Address src,
                            CanType T)
    const override {
      if (!getSingleton()) return;
      dest = getSingletonAddress(IGF, dest);
      src = getSingletonAddress(IGF, src);
      getSingleton()->initializeWithTake(IGF, dest, src,
                                         getSingletonType(IGF.IGM, T));
    }
    
    void reexplode(IRGenFunction &IGF, Explosion &src, Explosion &dest)
    const override {
      if (getLoadableSingleton()) getLoadableSingleton()->reexplode(IGF, src, dest);
    }
    
    void copy(IRGenFunction &IGF, Explosion &src, Explosion &dest)
    const override {
      if (getLoadableSingleton()) getLoadableSingleton()->copy(IGF, src, dest);
    }
    
    void consume(IRGenFunction &IGF, Explosion &src) const override {
      if (getLoadableSingleton()) getLoadableSingleton()->consume(IGF, src);
    }
    
    void destroy(IRGenFunction &IGF, Address addr, CanType T) const override {
      if (getSingleton() && !getSingleton()->isPOD(ResilienceScope::Local))
        getSingleton()->destroy(IGF, getSingletonAddress(IGF, addr),
                                getSingletonType(IGF.IGM, T));
    }
    
    llvm::Value *packEnumPayload(IRGenFunction &IGF,
                                  Explosion &in,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      if (getLoadableSingleton())
        return getLoadableSingleton()->packEnumPayload(IGF, in,
                                                        bitWidth, offset);
      return PackEnumPayload::getEmpty(IGF.IGM, bitWidth);
    }
    
    void unpackEnumPayload(IRGenFunction &IGF,
                            llvm::Value *payload,
                            Explosion &dest,
                            unsigned offset) const override {
      if (!getLoadableSingleton()) return;
      getLoadableSingleton()->unpackEnumPayload(IGF, payload, dest, offset);
    }
    
    void initializeMetadata(IRGenFunction &IGF,
                            llvm::Value *metadata,
                            llvm::Value *vwtable,
                            CanType T) const override {
      // Fixed-size enums don't need dynamic witness table initialization.
      if (TIK >= Fixed) return;

      assert(!ElementsWithPayload.empty() &&
             "empty singleton enum should not be dynamic!");

      // Get the value witness table for the element.
      CanType eltTy
       = ElementsWithPayload[0].decl->getArgumentType()->getCanonicalType();
      llvm::Value *eltMetadata = IGF.emitTypeMetadataRef(eltTy);
      llvm::Value *eltVWT
        = IGF.emitValueWitnessTableRefForMetadata(eltMetadata);
      
      Address vwtAddr(vwtable, IGF.IGM.getPointerAlignment());
      Address eltVWTAddr(eltVWT, IGF.IGM.getPointerAlignment());
      
      auto copyWitnessFromElt = [&](ValueWitness witness) -> llvm::Value* {
        Address dest = IGF.Builder.CreateConstArrayGEP(vwtAddr,
                                   unsigned(witness), IGF.IGM.getPointerSize());
        Address src = IGF.Builder.CreateConstArrayGEP(eltVWTAddr,
                                   unsigned(witness), IGF.IGM.getPointerSize());
        auto val = IGF.Builder.CreateLoad(src);
        IGF.Builder.CreateStore(val, dest);
        return val;
      };

      copyWitnessFromElt(ValueWitness::Size);
      auto flags = copyWitnessFromElt(ValueWitness::Flags);
      copyWitnessFromElt(ValueWitness::Stride);
      
      // If the original type had extra inhabitants, carry over its
      // extra inhabitant flags.
      auto xiBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      auto noXIBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      
      auto xiFlag = IGF.Builder.CreatePtrToInt(flags, IGF.IGM.SizeTy);
      auto xiMask
        = IGF.IGM.getSize(Size(ValueWitnessFlags::Enum_HasExtraInhabitants));
      xiFlag = IGF.Builder.CreateAnd(xiFlag, xiMask);
      auto xiBool = IGF.Builder.CreateICmpNE(xiFlag,
                                             IGF.IGM.getSize(Size(0)));
      IGF.Builder.CreateCondBr(xiBool, xiBB, noXIBB);
      
      IGF.Builder.emitBlock(xiBB);
      copyWitnessFromElt(ValueWitness::ExtraInhabitantFlags);
      IGF.Builder.CreateBr(noXIBB);
      
      IGF.Builder.emitBlock(noXIBB);
    }
    
    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      // FIXME: Hold off on registering extra inhabitants for dynamic enums
      // until initializeMetadata handles them.
      if (!getSingleton())
        return false;
      return getSingleton()->mayHaveExtraInhabitants(IGM);
    }
    
    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                         Address src, CanType T)
    const override {
      if (!getSingleton()) {
        // Any empty value is a valid value.
        return llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1);
      }

      return getSingleton()->getExtraInhabitantIndex(IGF,
                                             getSingletonAddress(IGF, src),
                                             getSingletonType(IGF.IGM, T));
    }
    
    void storeExtraInhabitant(IRGenFunction &IGF,
                              llvm::Value *index,
                              Address dest, CanType T) const override {
      if (!getSingleton()) {
        // Nothing to store for empty singletons.
        return;
      }
      getSingleton()->storeExtraInhabitant(IGF, index,
                                           getSingletonAddress(IGF, dest),
                                           getSingletonType(IGF.IGM, T));
    }
    
    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      assert(TIK >= Fixed);
      if (!getSingleton())
        return 0;
      return getFixedSingleton()->getFixedExtraInhabitantCount(IGM);
    }
    
    llvm::ConstantInt *
    getFixedExtraInhabitantValue(IRGenModule &IGM,
                                 unsigned bits,
                                 unsigned index) const override {
      assert(TIK >= Fixed);
      assert(getSingleton() && "empty singletons have no extra inhabitants");
      return getFixedSingleton()
        ->getFixedExtraInhabitantValue(IGM, bits, index);
    }
    
    llvm::BitVector getTagBitsForPayloads(IRGenModule &IGM) const override {
      // No tag bits, there's only one payload.
      llvm::BitVector result;
      if (getSingleton())
        result.resize(getFixedSingleton()->getFixedSize().getValueInBits(),
                      false);
      return result;
    }
    
    llvm::BitVector
    getBitPatternForNoPayloadElement(IRGenModule &IGM,
                                     EnumElementDecl *theCase) const override {
      // There's only a no-payload element if the type is empty.
      return {};
    }
  };
  
  /// Implementation strategy for no-payload enums, in other words, 'C-like'
  /// enums where none of the cases have data.
  class NoPayloadEnumImplStrategyBase
    : public SingleScalarTypeInfo<NoPayloadEnumImplStrategyBase,
                                  EnumImplStrategy>
  {
  protected:
    llvm::IntegerType *getDiscriminatorType() const {
      llvm::StructType *Struct = getStorageType();
      return cast<llvm::IntegerType>(Struct->getElementType(0));
    }
    
    /// Map the given element to the appropriate value in the
    /// discriminator type.
    virtual llvm::ConstantInt *getDiscriminatorIndex(EnumElementDecl *target)
      const = 0;
    
  public:
    NoPayloadEnumImplStrategyBase(IRGenModule &IGM,
                                  TypeInfoKind tik, unsigned NumElements,
                           std::vector<Element> &&WithPayload,
                           std::vector<Element> &&WithRecursivePayload,
                           std::vector<Element> &&WithNoPayload)
      : SingleScalarTypeInfo(IGM, tik, NumElements,
                             std::move(WithPayload),
                             std::move(WithRecursivePayload),
                             std::move(WithNoPayload))
    {
      assert(ElementsWithPayload.empty());
      assert(!ElementsWithNoPayload.empty());
    }
    
    void emitValueSwitch(IRGenFunction &IGF,
                         Explosion &value,
                         ArrayRef<std::pair<EnumElementDecl*,
                                            llvm::BasicBlock*>> dests,
                         llvm::BasicBlock *defaultDest) const override {
      llvm::Value *discriminator = value.claimNext();
      
      // Create an unreachable block for the default if the original SIL
      // instruction had none.
      bool unreachableDefault = false;
      if (!defaultDest) {
        unreachableDefault = true;
        defaultDest = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      }
      
      auto *i = IGF.Builder.CreateSwitch(discriminator, defaultDest,
                                         dests.size());
      for (auto &dest : dests)
        i->addCase(getDiscriminatorIndex(dest.first), dest.second);
      
      if (unreachableDefault) {
        IGF.Builder.emitBlock(defaultDest);
        IGF.Builder.CreateUnreachable();
      }
    }
    
    void emitIndirectSwitch(IRGenFunction &IGF,
                            CanType T,
                            Address addr,
                            ArrayRef<std::pair<EnumElementDecl*,
                                               llvm::BasicBlock*>> dests,
                            llvm::BasicBlock *defaultDest) const override {
      Explosion value(ResilienceExpansion::Minimal);
      loadAsTake(IGF, addr, value);
      emitValueSwitch(IGF, value, dests, defaultDest);
    }
    
    void emitValueProject(IRGenFunction &IGF,
                          Explosion &in,
                          EnumElementDecl *elt,
                          Explosion &out) const override {
      // All of the cases project an empty explosion.
      in.claim(getExplosionSize(in.getKind()));
    }
    
    void emitValueInjection(IRGenFunction &IGF,
                            EnumElementDecl *elt,
                            Explosion &params,
                            Explosion &out) const {
      out.add(getDiscriminatorIndex(elt));
    }
    
    Address projectDataForStore(IRGenFunction &IGF,
                                EnumElementDecl *elt,
                                Address enumAddr) const override {
      llvm_unreachable("cannot project data for no-payload cases");
    }
    Address destructiveProjectDataForLoad(IRGenFunction &IGF,
                                          EnumElementDecl *elt,
                                          Address enumAddr) const override {
      llvm_unreachable("cannot project data for no-payload cases");
    }
    
    void storeTag(IRGenFunction &IGF, EnumElementDecl *elt, Address enumAddr,
                  CanType T)
    const override {
      llvm::Value *discriminator = getDiscriminatorIndex(elt);
      Address discriminatorAddr
        = IGF.Builder.CreateStructGEP(enumAddr, 0, Size(0));
      IGF.Builder.CreateStore(discriminator, discriminatorAddr);
    }
    
    void initializeMetadata(IRGenFunction &IGF,
                            llvm::Value *metadata,
                            llvm::Value *vwtable,
                            CanType T) const override {
      // No-payload enums are always fixed-size so never need dynamic value
      // witness table initialization.
    }
    
    /// \group Required for SingleScalarTypeInfo
    
    llvm::Type *getScalarType() const {
      return getDiscriminatorType();
    }
    
    static Address projectScalar(IRGenFunction &IGF, Address addr) {
      return IGF.Builder.CreateStructGEP(addr, 0, Size(0));
    }

    void emitScalarRetain(IRGenFunction &IGF, llvm::Value *value) const {}
    void emitScalarRelease(IRGenFunction &IGF, llvm::Value *value) const {}
    
    void initializeWithTake(IRGenFunction &IGF, Address dest, Address src,
                            CanType T)
    const override {
      // No-payload enums are always POD, so we can always initialize by
      // primitive copy.
      llvm::Value *val = IGF.Builder.CreateLoad(src);
      IGF.Builder.CreateStore(val, dest);
    }
    
    static constexpr IsPOD_t IsScalarPOD = IsPOD;
    
    llvm::BitVector getTagBitsForPayloads(IRGenModule &IGM) const {
      // No tag bits; no-payload enums always use fixed representations.
      llvm::BitVector result;
      result.resize(cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits(),
                    false);
      return result;
    }
    
    llvm::BitVector getBitPatternForNoPayloadElement(IRGenModule &IGM,
                                               EnumElementDecl *theCase) const {
      auto bits
        = getBitVectorFromAPInt(getDiscriminatorIndex(theCase)->getValue());
      bits.resize(cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits());
      return bits;
    }
  };
  
  /// Implementation strategy for native Swift no-payload enums.
  class NoPayloadEnumImplStrategy final
    : public NoPayloadEnumImplStrategyBase
  {
  protected:
    llvm::ConstantInt *getDiscriminatorIndex(EnumElementDecl *target)
    const override {
      // The elements are assigned discriminators in declaration order.
      // FIXME: using a linear search here is fairly ridiculous.
      unsigned index = 0;
      for (auto elt : target->getParentEnum()->getAllElements()) {
        if (elt == target) break;
        index++;
      }
      return llvm::ConstantInt::get(getDiscriminatorType(), index);
    }
    
  public:
    NoPayloadEnumImplStrategy(IRGenModule &IGM,
                              TypeInfoKind tik, unsigned NumElements,
                              std::vector<Element> &&WithPayload,
                              std::vector<Element> &&WithRecursivePayload,
                              std::vector<Element> &&WithNoPayload)
      : NoPayloadEnumImplStrategyBase(IGM, tik, NumElements,
                                      std::move(WithPayload),
                                      std::move(WithRecursivePayload),
                                      std::move(WithNoPayload))
    {
      assert(ElementsWithPayload.empty());
      assert(!ElementsWithNoPayload.empty());
    }

    TypeInfo *completeEnumTypeLayout(TypeConverter &TC,
                                     CanType type,
                                     EnumDecl *theEnum,
                                     llvm::StructType *enumTy) override;
    
    /// \group Extra inhabitants for no-payload enums.

    // No-payload enums have all values above their greatest discriminator
    // value that fit inside their storage size available as extra inhabitants.
    
    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return getFixedExtraInhabitantCount(IGM) > 0;
    }
    
    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      unsigned bits = cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits();
      assert(bits < 32 && "freakishly huge no-payload enum");
      return (1U << bits) - ElementsWithNoPayload.size();
    }
    
    llvm::ConstantInt *
    getFixedExtraInhabitantValue(IRGenModule &IGM,
                                 unsigned bits,
                                 unsigned index) const override {
      unsigned value = index + ElementsWithNoPayload.size();
      return llvm::ConstantInt::get(IGM.getLLVMContext(),
                                    APInt(bits, value));
    }
    
    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                         Address src, CanType T)
    const override {
      auto &C = IGF.IGM.getLLVMContext();

      // Load the value.
      auto payloadTy = llvm::IntegerType::get(C,
                      cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits());
      src = IGF.Builder.CreateBitCast(src, payloadTy->getPointerTo());
      llvm::Value *val = IGF.Builder.CreateLoad(src);
      
      // Convert to i32.
      val = IGF.Builder.CreateZExtOrTrunc(val, IGF.IGM.Int32Ty);
      
      // Subtract the number of cases.
      val = IGF.Builder.CreateSub(val,
              llvm::ConstantInt::get(IGF.IGM.Int32Ty, ElementsWithNoPayload.size()));
      
      // If signed less than zero, we have a valid value. Otherwise, we have
      // an extra inhabitant.
      auto valid
        = IGF.Builder.CreateICmpSLT(val,
                                    llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0));
      val = IGF.Builder.CreateSelect(valid,
                        llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1), val);
      
      return val;
    }
    
    void storeExtraInhabitant(IRGenFunction &IGF,
                              llvm::Value *index,
                              Address dest, CanType T) const override {
      auto &C = IGF.IGM.getLLVMContext();
      auto payloadTy = llvm::IntegerType::get(C,
                      cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits());
      dest = IGF.Builder.CreateBitCast(dest, payloadTy->getPointerTo());

      index = IGF.Builder.CreateZExtOrTrunc(index, payloadTy);
      index = IGF.Builder.CreateAdd(index,
                llvm::ConstantInt::get(payloadTy, ElementsWithNoPayload.size()));
      IGF.Builder.CreateStore(index, dest);
    }
  };
  
  /// Implementation strategy for C-compatible enums, where none of the cases
  /// have data but they all have fixed integer associated values.
  class CCompatibleEnumImplStrategy final
    : public NoPayloadEnumImplStrategyBase
  {
  protected:
    llvm::ConstantInt *getDiscriminatorIndex(EnumElementDecl *target)
    const override {
      // The elements are assigned discriminators ABI-compatible with their
      // raw values from C.
      assert(target->hasRawValueExpr()
             && "c-compatible enum elt has no raw value?!");
      auto intExpr = cast<IntegerLiteralExpr>(target->getRawValueExpr());
      auto intType = getDiscriminatorType();
      
      APInt intValue = IntegerLiteralExpr::getValue(intExpr->getDigitsText(),
                                                    intType->getBitWidth());
      
      if (intExpr->isNegative())
        intValue = -intValue;
      
      return llvm::ConstantInt::get(intType->getContext(), intValue);
    }
    
  public:
    CCompatibleEnumImplStrategy(IRGenModule &IGM,
                                TypeInfoKind tik, unsigned NumElements,
                                std::vector<Element> &&WithPayload,
                                std::vector<Element> &&WithRecursivePayload,
                                std::vector<Element> &&WithNoPayload)
      : NoPayloadEnumImplStrategyBase(IGM, tik, NumElements,
                                      std::move(WithPayload),
                                      std::move(WithRecursivePayload),
                                      std::move(WithNoPayload))
    {
      assert(ElementsWithPayload.empty());
      assert(!ElementsWithNoPayload.empty());
    }
    
    TypeInfo *completeEnumTypeLayout(TypeConverter &TC,
                                     CanType type,
                                     EnumDecl *theEnum,
                                     llvm::StructType *enumTy) override;

    /// \group Extra inhabitants for C-compatible enums.

    // C-compatible enums have scattered inhabitants. For now, expose no
    // extra inhabitants.
    
    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return false;
    }
    
    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return 0;
    }
    
    llvm::ConstantInt *
    getFixedExtraInhabitantValue(IRGenModule &IGM,
                                 unsigned bits,
                                 unsigned index) const override {
      llvm_unreachable("no extra inhabitants");
    }
    
    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                         Address src, CanType T) const override {
      llvm_unreachable("no extra inhabitants");
    }
    
    void storeExtraInhabitant(IRGenFunction &IGF,
                              llvm::Value *index,
                              Address dest, CanType T) const override {
      llvm_unreachable("no extra inhabitants");
    }
  };
  
  /// Common base class for enums with one or more cases with data.
  class PayloadEnumImplStrategyBase : public EnumImplStrategy {
  protected:
    llvm::IntegerType *payloadTy = nullptr, *extraTagTy = nullptr;
    
    // The number of extra tag bits outside of the payload required to
    // discriminate enum cases.
    unsigned ExtraTagBitCount = ~0u;
    // The number of possible values for the extra tag bits that are used.
    // Log2(NumExtraTagValues - 1) + 1 == ExtraTagBitCount
    unsigned NumExtraTagValues = ~0u;

    void setTaggedEnumBody(IRGenModule &IGM,
                           llvm::StructType *bodyStruct,
                           unsigned payloadBits, unsigned extraTagBits) {
      // LLVM's ABI rules for I.O.U.S. (Integer Of Unusual Size) types is to
      // pad them out as if aligned to the largest native integer type, even
      // inside "packed" structs, so to accurately lay things out, we use
      // i8 arrays for the payload and extra tag bits.
      auto payloadArrayTy = llvm::ArrayType::get(IGM.Int8Ty,
                                                 (payloadBits+7U)/8U);
      
      SmallVector<llvm::Type*, 2> body;
      
      // Handle the case when the payload has no storage.
      // This may come up when a generic type with payload is instantiated on an
      // empty type.
      if (payloadBits > 0) {
        payloadTy = llvm::IntegerType::get(IGM.getLLVMContext(),
                                           payloadBits);
        body.push_back(payloadArrayTy);
      } else {
        payloadTy = nullptr;
      }
      
      if (extraTagBits > 0) {
        auto extraTagArrayTy = llvm::ArrayType::get(IGM.Int8Ty,
                                                    (extraTagBits+7U)/8U);
        body.push_back(extraTagArrayTy);
        extraTagTy = llvm::IntegerType::get(IGM.getLLVMContext(),
                                            extraTagBits);
      } else {
        extraTagTy = nullptr;
      }
      bodyStruct->setBody(body, /*isPacked*/true);
    }
    
  public:
    PayloadEnumImplStrategyBase(IRGenModule &IGM,
                                TypeInfoKind tik, unsigned NumElements,
                                std::vector<Element> &&WithPayload,
                                std::vector<Element> &&WithRecursivePayload,
                                std::vector<Element> &&WithNoPayload)
      : EnumImplStrategy(IGM, tik, NumElements,
                         std::move(WithPayload),
                         std::move(WithRecursivePayload),
                         std::move(WithNoPayload))
    {
      assert(ElementsWithPayload.size() >= 1);
    }
    
    void getSchema(ExplosionSchema &schema) const override {
      if (TIK < Loadable) {
        schema.add(ExplosionSchema::Element::forAggregate(getStorageType(),
                                                  TI->getBestKnownAlignment()));
        return;
      }
      
      if (payloadTy)
        schema.add(ExplosionSchema::Element::forScalar(payloadTy));
      if (ExtraTagBitCount > 0)
        schema.add(ExplosionSchema::Element::forScalar(extraTagTy));
    }
    
    unsigned getExplosionSize(ResilienceExpansion kind) const override {
      return unsigned(ExtraTagBitCount > 0) + unsigned(payloadTy != nullptr);
    }
    
    Address projectPayload(IRGenFunction &IGF, Address addr) const {
      assert(payloadTy && "has empty payload");

      return IGF.Builder.CreateBitCast(addr, payloadTy->getPointerTo());
    }

    Address projectExtraTagBits(IRGenFunction &IGF, Address addr) const {
      assert(ExtraTagBitCount > 0 && "does not have extra tag bits");

      if (!payloadTy) {
        return IGF.Builder.CreateBitCast(addr, extraTagTy->getPointerTo());
      }
      
      addr = IGF.Builder.CreateStructGEP(addr, 1,
                                         Size(payloadTy->getBitWidth()/8U));
      return IGF.Builder.CreateBitCast(addr, extraTagTy->getPointerTo());
    }

    void loadForSwitch(IRGenFunction &IGF, Address addr, Explosion &e)
    const {
      assert(TIK >= Fixed);
      if (payloadTy)
        e.add(IGF.Builder.CreateLoad(projectPayload(IGF, addr)));
      if (ExtraTagBitCount > 0)
        e.add(IGF.Builder.CreateLoad(projectExtraTagBits(IGF, addr)));
    }

    void loadAsTake(IRGenFunction &IGF, Address addr, Explosion &e)
    const override {
      assert(TIK >= Loadable);
      loadForSwitch(IGF, addr, e);
    }
    
    void loadAsCopy(IRGenFunction &IGF, Address addr, Explosion &e)
    const override {
      assert(TIK >= Loadable);
      Explosion tmp(e.getKind());
      loadAsTake(IGF, addr, tmp);
      copy(IGF, tmp, e);
    }
    
    void assign(IRGenFunction &IGF, Explosion &e, Address addr) const override {
      assert(TIK >= Loadable);
      Explosion old(e.getKind());
      if (!isPOD(ResilienceScope::Local))
        loadAsTake(IGF, addr, old);
      initialize(IGF, e, addr);
      if (!isPOD(ResilienceScope::Local))
        consume(IGF, old);
    }
    
    void initialize(IRGenFunction &IGF, Explosion &e, Address addr)
    const override {
      assert(TIK >= Loadable);
      if (payloadTy)
        IGF.Builder.CreateStore(e.claimNext(), projectPayload(IGF, addr));
      if (ExtraTagBitCount > 0)
        IGF.Builder.CreateStore(e.claimNext(), projectExtraTagBits(IGF, addr));
    }
    
    void reexplode(IRGenFunction &IGF, Explosion &src, Explosion &dest)
    const override {
      assert(TIK >= Loadable);
      dest.add(src.claim(getExplosionSize(ResilienceExpansion::Minimal)));
    }
    
  protected:
    /// Do a primitive copy of the enum from one address to another.
    void emitPrimitiveCopy(IRGenFunction &IGF, Address dest, Address src,
                           CanType T) const {
      // If the layout is fixed, load and store the fixed-size payload and tag.
      if (TIK >= Fixed) {
        llvm::Value *payload, *extraTag;
        std::tie(payload, extraTag)
          = emitPrimitiveLoadPayloadAndExtraTag(IGF, src);
        emitPrimitiveStorePayloadAndExtraTag(IGF, dest, payload, extraTag);
        return;
      }
      
      // Otherwise, do a memcpy of the dynamic size of the type.
      IGF.Builder.CreateMemCpy(dest.getAddress(), src.getAddress(),
                               TI->getSize(IGF, T),
                               std::min(dest.getAlignment().getValue(),
                                        src.getAlignment().getValue()));
    }
    
    void emitPrimitiveStorePayloadAndExtraTag(IRGenFunction &IGF, Address dest,
                                              llvm::Value *payload,
                                              llvm::Value *extraTag) const {
      if (payloadTy)
        IGF.Builder.CreateStore(payload, projectPayload(IGF, dest));
      if (ExtraTagBitCount > 0)
        IGF.Builder.CreateStore(extraTag, projectExtraTagBits(IGF, dest));
    }
    
    std::pair<llvm::Value*, llvm::Value*>
    getPayloadAndExtraTagFromExplosion(Explosion &src) const {
      llvm::Value *payload = src.claimNext();
      llvm::Value *extraTag = ExtraTagBitCount > 0 ? src.claimNext() : nullptr;
      return {payload, extraTag};
    }
    
    std::pair<llvm::Value*, llvm::Value*>
    emitPrimitiveLoadPayloadAndExtraTag(IRGenFunction &IGF, Address addr) const{
      llvm::Value *payload = nullptr;
      llvm::Value *extraTag = nullptr;
      if (payloadTy)
        payload = IGF.Builder.CreateLoad(projectPayload(IGF, addr));
      if (ExtraTagBitCount > 0)
        extraTag = IGF.Builder.CreateLoad(projectExtraTagBits(IGF, addr));
      return {payload, extraTag};
    }
  };

  class SinglePayloadEnumImplStrategy final
    : public PayloadEnumImplStrategyBase
  {
    EnumElementDecl *getPayloadElement() const {
      return ElementsWithPayload[0].decl;
    }
    
    CanType getPayloadType(IRGenModule &IGM, CanType T) const {
      return T->getTypeOfMember(IGM.SILMod->getSwiftModule(),
                                ElementsWithPayload[0].decl, nullptr,
                                ElementsWithPayload[0].decl->getArgumentType())
        ->getCanonicalType();
    }
    
    const TypeInfo &getPayloadTypeInfo() const {
      return *ElementsWithPayload[0].ti;
    }
    const FixedTypeInfo &getFixedPayloadTypeInfo() const {
      return cast<FixedTypeInfo>(*ElementsWithPayload[0].ti);
    }
    const LoadableTypeInfo &getLoadablePayloadTypeInfo() const {
      return cast<LoadableTypeInfo>(*ElementsWithPayload[0].ti);
    }
    
    llvm::Value *emitPayloadMetadata(IRGenFunction &IGF,
                                     CanType T) const {
      return IGF.emitTypeMetadataRef(getPayloadType(IGF.IGM, T));
    }
    
    /// More efficient value semantics implementations for certain enum layouts.
    enum CopyDestroyStrategy {
      /// No special behavior.
      Normal,
      /// The payload is POD, so copying is bitwise, and destruction is a noop.
      POD,
      /// The payload is a single Swift reference-counted value, and we have
      /// a single no-payload case which uses the null extra inhabitant, so
      /// copy and destroy can pass through to swift_retain/swift_release.
      NullableSwiftRefcounted,
      /// The payload is a single unknown-reference-counted value, and we have
      /// a single no-payload case which uses the null extra inhabitant, so
      /// copy and destroy can pass through to
      /// swift_unknownRetain/swift_unknownRelease.
      NullableUnknownRefcounted,
    };
    
    CopyDestroyStrategy CopyDestroyKind;

    unsigned NumExtraInhabitantTagValues = ~0U;
    
  public:
    SinglePayloadEnumImplStrategy(IRGenModule &IGM,
                                  TypeInfoKind tik, unsigned NumElements,
                                   std::vector<Element> &&WithPayload,
                                   std::vector<Element> &&WithRecursivePayload,
                                   std::vector<Element> &&WithNoPayload)
      : PayloadEnumImplStrategyBase(IGM, tik, NumElements,
                                     std::move(WithPayload),
                                     std::move(WithRecursivePayload),
                                     std::move(WithNoPayload)),
        CopyDestroyKind(Normal)
    {
      assert(ElementsWithPayload.size() == 1);
      
      // If the payload is POD, then we can use POD value semantics.
      auto &payloadTI = *ElementsWithPayload[0].ti;
      if (payloadTI.isPOD(ResilienceScope::Component))
        CopyDestroyKind = POD;
      // If the payload is a single refcounted pointer and we have a single
      // empty case, then the layout will be a nullable pointer, and we can
      // pass enum values directly into swift_retain/swift_release as-is.
      else if (tik >= TypeInfoKind::Loadable
          && payloadTI.isSingleUnknownRetainablePointer(
                                                     ResilienceScope::Component)
          && ElementsWithNoPayload.size() == 1
          // FIXME: All single-retainable-pointer types should eventually have
          // extra inhabitants.
          && cast<FixedTypeInfo>(payloadTI)
            .getFixedExtraInhabitantCount(IGM) > 0) {
        CopyDestroyKind = payloadTI.isSingleSwiftRetainablePointer(
                                                     ResilienceScope::Component)
          ? NullableSwiftRefcounted
          : NullableUnknownRefcounted;
      }
      
      // TODO: Same for single unknown-refcounted pointers.
    }

    /// Return the number of tag values represented with extra
    /// inhabitants in the payload.
    unsigned getNumExtraInhabitantTagValues() const {
      assert(NumExtraInhabitantTagValues != ~0U);
      return NumExtraInhabitantTagValues;
    }
    
    /// The payload for a single-payload enum is always placed in front and
    /// will never have interleaved tag bits, so we can just bitcast the enum
    /// address to the payload type for either injection or projection of the
    /// enum.
    Address projectPayloadData(IRGenFunction &IGF, Address addr) const {
      return IGF.Builder.CreateBitCast(addr,
                         getPayloadTypeInfo().getStorageType()->getPointerTo());
    }
    Address projectDataForStore(IRGenFunction &IGF, EnumElementDecl *elt,
                                Address enumAddr) const override {
      assert(elt == getPayloadElement() && "cannot project no-data case");
      return projectPayloadData(IGF, enumAddr);
    }
    Address destructiveProjectDataForLoad(IRGenFunction &IGF,
                                          EnumElementDecl *elt,
                                          Address enumAddr) const override {
      assert(elt == getPayloadElement() && "cannot project no-data case");
      return projectPayloadData(IGF, enumAddr);
    }
    
    TypeInfo *completeEnumTypeLayout(TypeConverter &TC,
                                      CanType type,
                                      EnumDecl *theEnum,
                                      llvm::StructType *enumTy) override;
  private:
    TypeInfo *completeFixedLayout(TypeConverter &TC,
                                  CanType type,
                                  EnumDecl *theEnum,
                                  llvm::StructType *enumTy);
    TypeInfo *completeDynamicLayout(TypeConverter &TC,
                                    CanType type,
                                    EnumDecl *theEnum,
                                    llvm::StructType *enumTy);
    
  public:
    llvm::Value *packEnumPayload(IRGenFunction &IGF, Explosion &src,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      PackEnumPayload pack(IGF, bitWidth);
      // Pack payload.
      pack.addAtOffset(src.claimNext(), offset);
      
      // Pack tag bits, if any.
      if (ExtraTagBitCount > 0) {
        unsigned extraTagOffset
          = getFixedPayloadTypeInfo().getFixedSize().getValueInBits() + offset;
        
        pack.addAtOffset(src.claimNext(), extraTagOffset);
      }
      
      return pack.get();
    }
    
    void unpackEnumPayload(IRGenFunction &IGF, llvm::Value *outerPayload,
                            Explosion &dest,
                            unsigned offset) const override {
      UnpackEnumPayload unpack(IGF, outerPayload);
      
      // Unpack our inner payload.
      dest.add(unpack.claimAtOffset(payloadTy, offset));
      
      // Unpack our extra tag bits, if any.
      if (ExtraTagBitCount > 0) {
        unsigned extraTagOffset
          = getFixedPayloadTypeInfo().getFixedSize().getValueInBits() + offset;
        
        dest.add(unpack.claimAtOffset(extraTagTy, extraTagOffset));
      }
    }
    
    void emitValueSwitch(IRGenFunction &IGF,
                         Explosion &value,
                         ArrayRef<std::pair<EnumElementDecl*,
                                            llvm::BasicBlock*>> dests,
                         llvm::BasicBlock *defaultDest) const override {
      auto &C = IGF.IGM.getLLVMContext();
      
      // Create a map of the destination blocks for quicker lookup.
      llvm::DenseMap<EnumElementDecl*,llvm::BasicBlock*> destMap(dests.begin(),
                                                                  dests.end());
      // Create an unreachable branch for unreachable switch defaults.
      auto *unreachableBB = llvm::BasicBlock::Create(C);
      
      // If there was no default branch in SIL, use the unreachable branch as
      // the default.
      if (!defaultDest)
        defaultDest = unreachableBB;
      
      auto blockForCase = [&](EnumElementDecl *theCase) -> llvm::BasicBlock* {
        auto found = destMap.find(theCase);
        if (found == destMap.end())
          return defaultDest;
        else
          return found->second;
      };
      
      llvm::Value *payload = nullptr;
      if (payloadTy)
        payload = value.claimNext();
      llvm::BasicBlock *payloadDest = blockForCase(getPayloadElement());
      unsigned extraInhabitantCount = getNumExtraInhabitantTagValues();
      
      // If there are extra tag bits, switch over them first.
      SmallVector<llvm::BasicBlock*, 2> tagBitBlocks;
      if (ExtraTagBitCount > 0) {
        llvm::Value *tagBits = value.claimNext();
        
        auto *swi = IGF.Builder.CreateSwitch(tagBits, unreachableBB,
                                             NumExtraTagValues);
        
        // If we have extra inhabitants, we need to check for them in the
        // zero-tag case. Otherwise, we switch directly to the payload case.
        if (extraInhabitantCount > 0) {
          auto bb = llvm::BasicBlock::Create(C);
          tagBitBlocks.push_back(bb);
          swi->addCase(llvm::ConstantInt::get(C,APInt(ExtraTagBitCount,0)), bb);
        } else {
          tagBitBlocks.push_back(payloadDest);
          swi->addCase(llvm::ConstantInt::get(C,APInt(ExtraTagBitCount,0)),
                       payloadDest);
        }
        
        for (unsigned i = 1; i < NumExtraTagValues; ++i) {
          auto bb = llvm::BasicBlock::Create(C);
          tagBitBlocks.push_back(bb);
          swi->addCase(llvm::ConstantInt::get(C,APInt(ExtraTagBitCount,i)), bb);
        }
        
        // Continue by emitting the extra inhabitant dispatch, if any.
        if (extraInhabitantCount > 0)
          IGF.Builder.emitBlock(tagBitBlocks[0]);
      }
      
      auto elements = getPayloadElement()->getParentEnum()->getAllElements();
      auto elti = elements.begin(), eltEnd = elements.end();
      if (*elti == getPayloadElement())
        ++elti;
      
      // Advance the enum element iterator, skipping the payload case.
      auto nextCase = [&]() -> EnumElementDecl* {
        assert(elti != eltEnd);
        auto result = *elti;
        ++elti;
        if (elti != eltEnd && *elti == getPayloadElement())
          ++elti;
        return result;
      };
      
      // If there are no extra tag bits, or they're set to zero, then we either
      // have a payload, or an empty case represented using an extra inhabitant.
      // Check the extra inhabitant cases if we have any.
      unsigned payloadBits
        = getFixedPayloadTypeInfo().getFixedSize().getValueInBits();
      if (extraInhabitantCount > 0) {
        assert(payload && "extra inhabitants with empty payload?!");
        auto *switchValue =
          getFixedPayloadTypeInfo().maskFixedExtraInhabitant(IGF, payload);
        auto *swi = IGF.Builder.CreateSwitch(switchValue, payloadDest);
        for (unsigned i = 0; i < extraInhabitantCount && elti != eltEnd; ++i) {
          auto v = getFixedPayloadTypeInfo().getFixedExtraInhabitantValue(
                                                       IGF.IGM, payloadBits, i);
          swi->addCase(v, blockForCase(nextCase()));
        }
      }
      
      // We should have handled the payload case either in extra inhabitant
      // or in extra tag dispatch by now.
      assert(IGF.Builder.hasPostTerminatorIP() &&
             "did not handle payload case");
      
      // If there's an empty payload, each tag value corresponds to a single
      // empty case.
      if (!payload) {
        for (unsigned i = 1, e = tagBitBlocks.size(); i < e; ++i) {
          assert(elti != eltEnd &&
                 "ran out of cases before running out of extra tags?");
          IGF.Builder.emitBlock(tagBitBlocks[i]);
          IGF.Builder.CreateBr(blockForCase(nextCase()));
        }
      } else {
        // Handle the cases covered by each tag bit value.
        unsigned casesPerTag = payloadBits >= 32 ? UINT_MAX : 1U << payloadBits;
        for (unsigned i = 1, e = tagBitBlocks.size(); i < e; ++i) {
          assert(elti != eltEnd &&
                 "ran out of cases before running out of extra tags?");
          IGF.Builder.emitBlock(tagBitBlocks[i]);
          auto swi = IGF.Builder.CreateSwitch(payload, unreachableBB);
          for (unsigned tag = 0; tag < casesPerTag && elti != eltEnd; ++tag) {
            auto v = llvm::ConstantInt::get(C, APInt(payloadBits, tag));
            swi->addCase(v, blockForCase(nextCase()));
          }
        }
      }
      
      // Delete the unreachable default block if we didn't use it, or emit it
      // if we did.
      if (unreachableBB->use_empty()) {
        delete unreachableBB;
      } else {
        IGF.Builder.emitBlock(unreachableBB);
        IGF.Builder.CreateUnreachable();
      }
    }
    
    void emitDynamicSwitch(IRGenFunction &IGF,
                           CanType T,
                           Address addr,
                           ArrayRef<std::pair<EnumElementDecl*,
                                              llvm::BasicBlock*>> dests,
                           llvm::BasicBlock *defaultDest) const {
      auto payloadMetadata = emitPayloadMetadata(IGF, T);
      auto numEmptyCases = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                                                  ElementsWithNoPayload.size());
      auto opaqueAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                                  IGF.IGM.OpaquePtrTy);
      
      // Create a map of the destination blocks for quicker lookup.
      llvm::DenseMap<EnumElementDecl*,llvm::BasicBlock*> destMap(dests.begin(),
                                                                  dests.end());

      // If there was no default branch in SIL, use an unreachable branch as
      // the default.
      llvm::BasicBlock *unreachableBB = nullptr;
      if (!defaultDest) {
        unreachableBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
        defaultDest = unreachableBB;
      }
      
      // Ask the runtime to find the case index.
      auto caseIndex = IGF.Builder.CreateCall3(
                            IGF.IGM.getGetEnumCaseSinglePayloadFn(),
                            opaqueAddr, payloadMetadata, numEmptyCases);

      // Switch on the index.
      auto *swi = IGF.Builder.CreateSwitch(caseIndex, defaultDest);

      // Add the payload case.
      auto payloadCase = destMap.find(getPayloadElement());
      if (payloadCase != destMap.end())
        swi->addCase(llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1),
                     payloadCase->second);
      
      // Add the empty cases.
      unsigned emptyCaseIndex = 0;
      for (auto &empty : ElementsWithNoPayload) {
        auto emptyCase = destMap.find(empty.decl);
        if (emptyCase != destMap.end())
          swi->addCase(llvm::ConstantInt::get(IGF.IGM.Int32Ty, emptyCaseIndex),
                       emptyCase->second);
        ++emptyCaseIndex;
      }
      
      // Emit the unreachable block, if any.
      if (unreachableBB) {
        IGF.Builder.emitBlock(unreachableBB);
        IGF.Builder.CreateUnreachable();
      }
    }
    
    void emitIndirectSwitch(IRGenFunction &IGF,
                            CanType T,
                            Address addr,
                            ArrayRef<std::pair<EnumElementDecl*,
                                               llvm::BasicBlock*>> dests,
                            llvm::BasicBlock *defaultDest) const override {
      if (TIK >= Fixed) {
        // Load the fixed-size representation and switch directly.
        Explosion value(ResilienceExpansion::Minimal);
        loadForSwitch(IGF, addr, value);
        return emitValueSwitch(IGF, value, dests, defaultDest);
      }
      
      // Use the runtime to dynamically switch.
      emitDynamicSwitch(IGF, T, addr, dests, defaultDest);
    }
    
    void emitValueProject(IRGenFunction &IGF,
                          Explosion &inEnum,
                          EnumElementDecl *theCase,
                          Explosion &out) const override {
      // Only the payload case has anything to project. The other cases are
      // empty.
      if (theCase != getPayloadElement()) {
        inEnum.claim(getExplosionSize(inEnum.getKind()));
        return;
      }
      
      if (payloadTy) {
        llvm::Value *payload = inEnum.claimNext();
        getLoadablePayloadTypeInfo().unpackEnumPayload(IGF, payload, out, 0);
      } else {
        assert(getLoadablePayloadTypeInfo()
                 .getSchema(ResilienceExpansion::Minimal)
                 .empty()
               && "empty payload with non-empty explosion schema?!");
      }
      if (ExtraTagBitCount > 0)
        inEnum.claimNext();
    }
    
  private:
    // Get the index of an enum element among the non-payload cases.
    unsigned getSimpleElementTagIndex(EnumElementDecl *elt) const {
      assert(elt != getPayloadElement() && "is payload element");
      unsigned i = 0;
      // FIXME: linear search
      for (auto *enumElt : elt->getParentEnum()->getAllElements()) {
        if (elt == enumElt)
          return i;
        if (enumElt != getPayloadElement())
          ++i;
      }
      llvm_unreachable("element was not a member of enum");
    }
    
    // Get the payload and extra tag (if any) parts of the discriminator for
    // a no-data case.
    std::pair<llvm::ConstantInt *, llvm::ConstantInt *>
    getNoPayloadCaseValue(IRGenModule &IGM, EnumElementDecl *elt) const {
      assert(elt != getPayloadElement());
      
      unsigned payloadSize
        = getFixedPayloadTypeInfo().getFixedSize().getValueInBits();
      
      // Non-payload cases use extra inhabitants, if any, or are discriminated
      // by setting the tag bits.
      unsigned tagIndex = getSimpleElementTagIndex(elt);
      unsigned numExtraInhabitants = getNumExtraInhabitantTagValues();
      llvm::ConstantInt *payload = nullptr;
      unsigned extraTagValue;
      if (tagIndex < numExtraInhabitants) {
        payload = getFixedPayloadTypeInfo().getFixedExtraInhabitantValue(
                                               IGM, payloadSize, tagIndex);
        extraTagValue = 0;
      } else {
        tagIndex -= numExtraInhabitants;
        
        // Factor the extra tag value from the payload value.
        unsigned payloadValue;
        if (payloadSize >= 32) {
          payloadValue = tagIndex;
          extraTagValue = 1U;
        } else {
          payloadValue = tagIndex & ((1U << payloadSize) - 1U);
          extraTagValue = (tagIndex >> payloadSize) + 1U;
        }
        
        if (payloadTy)
          payload = llvm::ConstantInt::get(IGM.getLLVMContext(),
                                           APInt(payloadSize, payloadValue));
      }

      llvm::ConstantInt *extraTag = nullptr;
      if (ExtraTagBitCount > 0) {
        extraTag = llvm::ConstantInt::get(IGM.getLLVMContext(),
                                        APInt(ExtraTagBitCount, extraTagValue));
      } else {
        assert(extraTagValue == 0 &&
               "non-zero extra tag value with no tag bits");
      }
      return {payload, extraTag};
    }
    
  public:
    void emitValueInjection(IRGenFunction &IGF,
                            EnumElementDecl *elt,
                            Explosion &params,
                            Explosion &out) const {
      // The payload case gets its native representation. If there are extra
      // tag bits, set them to zero.
      unsigned payloadSize
        = getFixedPayloadTypeInfo().getFixedSize().getValueInBits();
      
      if (elt == getPayloadElement()) {
        if (payloadTy) {
          auto &loadablePayloadTI = getLoadablePayloadTypeInfo();
          llvm::Value *payload
            = loadablePayloadTI.packEnumPayload(IGF, params, payloadSize, 0);
          out.add(payload);
        }

        if (ExtraTagBitCount > 0)
          out.add(getZeroExtraTagConstant(IGF.IGM));
        return;
      }
      
      // Non-payload cases use extra inhabitants, if any, or are discriminated
      // by setting the tag bits.
      llvm::Value *payload, *extraTag;
      std::tie(payload, extraTag) = getNoPayloadCaseValue(IGF.IGM, elt);
      if (payloadTy) {
        assert(payload);
        out.add(payload);
      }
      if (ExtraTagBitCount > 0) {
        assert(extraTag);
        out.add(extraTag);
      }
    }
    
  private:
    /// Emits the test(s) that determine whether the fixed-size enum contains a
    /// payload or an empty case. Emits the basic block for the "true" case and
    /// returns the unemitted basic block for the "false" case.
    llvm::BasicBlock *
    testFixedEnumContainsPayload(IRGenFunction &IGF,
                                  llvm::Value *payload,
                                  llvm::Value *extraBits) const {
      auto *falseBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      
      // We only need to apply the payload operation if the enum contains a
      // value of the payload case.
      
      // If we have extra tag bits, they will be zero if we contain a payload.
      if (ExtraTagBitCount > 0) {
        assert(extraBits);
        llvm::Value *zero = llvm::ConstantInt::get(extraBits->getType(), 0);
        llvm::Value *isZero = IGF.Builder.CreateICmp(llvm::CmpInst::ICMP_EQ,
                                                     extraBits, zero);
        
        auto *trueBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
        IGF.Builder.CreateCondBr(isZero, trueBB, falseBB);
        
        IGF.Builder.emitBlock(trueBB);
      }
      
      // If we used extra inhabitants to represent empty case discriminators,
      // weed them out.
      unsigned numExtraInhabitants = getNumExtraInhabitantTagValues();
      if (numExtraInhabitants > 0) {
        unsigned bitWidth =
          getFixedPayloadTypeInfo().getFixedSize().getValueInBits();

        auto *payloadBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
        auto *switchValue =
          getFixedPayloadTypeInfo().maskFixedExtraInhabitant(IGF, payload);
        auto *swi = IGF.Builder.CreateSwitch(switchValue, payloadBB);
        
        auto elements = getPayloadElement()->getParentEnum()->getAllElements();
        unsigned inhabitant = 0;
        for (auto i = elements.begin(), end = elements.end();
             i != end && inhabitant < numExtraInhabitants;
             ++i, ++inhabitant) {
          if (*i == getPayloadElement()) {
            ++i;
            if (i == end)
              break;
          }
          auto xi = getFixedPayloadTypeInfo().getFixedExtraInhabitantValue(
                      IGF.IGM, bitWidth, inhabitant);
          swi->addCase(xi, falseBB);
        }
        
        IGF.Builder.emitBlock(payloadBB);
      }
      
      return falseBB;
    }
    
    /// Emits the test(s) that determine whether the enum contains a payload
    /// or an empty case. For a fixed-size enum, this does a primitive load
    /// of the representation and calls down to testFixedEnumContainsPayload.
    /// For a dynamic enum, this queries the value witness table of the payload
    /// type. Emits the basic block for the "true" case and
    /// returns the unemitted basic block for the "false" case.
    llvm::BasicBlock *
    testEnumContainsPayload(IRGenFunction &IGF,
                            Address addr,
                            CanType T) const {
      auto &C = IGF.IGM.getLLVMContext();
      
      if (TIK >= Fixed) {
        llvm::Value *payload, *extraTag;
        std::tie(payload, extraTag)
          = emitPrimitiveLoadPayloadAndExtraTag(IGF, addr);
        return testFixedEnumContainsPayload(IGF, payload, extraTag);
      }

      auto *payloadBB = llvm::BasicBlock::Create(C);
      auto *noPayloadBB = llvm::BasicBlock::Create(C);

      // Look up the metadata for the payload.
      llvm::Value *metadata = emitPayloadMetadata(IGF, T);

      // Ask the runtime what case we have.
      llvm::Value *opaqueAddr = IGF.Builder.CreateBitCast(addr.getAddress(),
                                                          IGF.IGM.OpaquePtrTy);
      llvm::Value *numCases = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                                                 ElementsWithNoPayload.size());
      llvm::Value *which = IGF.Builder.CreateCall3(
                                       IGF.IGM.getGetEnumCaseSinglePayloadFn(),
                                       opaqueAddr, metadata, numCases);
      
      // If it's -1 then we have the payload.
      llvm::Value *hasPayload = IGF.Builder.CreateICmpEQ(which,
                             llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1));
      IGF.Builder.CreateCondBr(hasPayload, payloadBB, noPayloadBB);
      
      IGF.Builder.emitBlock(payloadBB);
      return noPayloadBB;
    }
    
    llvm::Type *getRefcountedPtrType(IRGenModule &IGM) const {
      switch (CopyDestroyKind) {
      case NullableSwiftRefcounted:
        return IGM.RefCountedPtrTy;
      case NullableUnknownRefcounted:
        return IGM.UnknownRefCountedPtrTy;
      case POD:
      case Normal:
        llvm_unreachable("not a refcounted payload");
      }
    }
    
    void retainRefcountedPayload(IRGenFunction &IGF,
                                 llvm::Value *ptr) const {
      switch (CopyDestroyKind) {
      case NullableSwiftRefcounted: {
        IGF.emitRetainCall(ptr);
        return;
      }
      case NullableUnknownRefcounted: {
        IGF.emitUnknownRetainCall(ptr);
        return;
      }
      case POD:
      case Normal:
        llvm_unreachable("not a refcounted payload");
      }
    }
    
    void releaseRefcountedPayload(IRGenFunction &IGF,
                                  llvm::Value *ptr) const {
      switch (CopyDestroyKind) {
      case NullableSwiftRefcounted: {
        IGF.emitRelease(ptr);
        return;
      }
      case NullableUnknownRefcounted: {
        IGF.emitUnknownRelease(ptr);
        return;
      }
      case POD:
      case Normal:
        llvm_unreachable("not a refcounted payload");
      }
    }
    
  public:
    void copy(IRGenFunction &IGF, Explosion &src, Explosion &dest)
    const override {
      assert(TIK >= Loadable);
      
      switch (CopyDestroyKind) {
      case POD:
        reexplode(IGF, src, dest);
        return;

      case Normal: {
        // Copy the payload, if we have it.
        llvm::Value *payload, *extraTag;
        std::tie(payload, extraTag) = getPayloadAndExtraTagFromExplosion(src);
        
        llvm::BasicBlock *endBB = testFixedEnumContainsPayload(IGF, payload, extraTag);
        
        if (payload) {
          Explosion payloadValue(ResilienceExpansion::Minimal);
          Explosion payloadCopy(ResilienceExpansion::Minimal);
          auto &loadableTI = getLoadablePayloadTypeInfo();
          loadableTI.unpackEnumPayload(IGF, payload, payloadValue, 0);
          loadableTI.copy(IGF, payloadValue, payloadCopy);
          payloadCopy.claimAll(); // FIXME: repack if not bit-identical
        }
        
        IGF.Builder.CreateBr(endBB);
        IGF.Builder.emitBlock(endBB);
        
        // Copy to the new explosion.
        if (payload)
          dest.add(payload);
        if (extraTag) dest.add(extraTag);
        return;
      }
      
      case NullableSwiftRefcounted:
      case NullableUnknownRefcounted: {
        // Bitcast to swift.refcounted*, and retain the pointer.
        llvm::Value *val = src.claimNext();
        llvm::Value *ptr = IGF.Builder.CreateIntToPtr(val,
                                                getRefcountedPtrType(IGF.IGM));
        retainRefcountedPayload(IGF, ptr);
        dest.add(val);
        return;
      }
      }
    }
    
    void consume(IRGenFunction &IGF, Explosion &src) const override {
      assert(TIK >= Loadable);

      switch (CopyDestroyKind) {
      case POD:
        src.claim(getExplosionSize(src.getKind()));
        return;

      case Normal: {
        // Check that we have a payload.
        llvm::Value *payload, *extraTag;
        std::tie(payload, extraTag) = getPayloadAndExtraTagFromExplosion(src);
        
        llvm::BasicBlock *endBB
          = testFixedEnumContainsPayload(IGF, payload, extraTag);
        
        // If we did, consume it.
        if (payload) {
          Explosion payloadValue(ResilienceExpansion::Minimal);
          auto &loadableTI = getLoadablePayloadTypeInfo();
          loadableTI.unpackEnumPayload(IGF, payload, payloadValue, 0);
          loadableTI.consume(IGF, payloadValue);
        }
        
        IGF.Builder.CreateBr(endBB);
        IGF.Builder.emitBlock(endBB);
        return;
      }

      case NullableSwiftRefcounted:
      case NullableUnknownRefcounted: {
        // Bitcast to swift.refcounted*, and hand to swift_release.
        llvm::Value *val = src.claimNext();
        llvm::Value *ptr = IGF.Builder.CreateIntToPtr(val,
                                                getRefcountedPtrType(IGF.IGM));
        releaseRefcountedPayload(IGF, ptr);
        return;
      }
      }
      
    }

    void destroy(IRGenFunction &IGF, Address addr, CanType T) const override {
      switch (CopyDestroyKind) {
      case POD:
        return;
          
      case Normal: {
        // Check that there is a payload at the address.
        llvm::BasicBlock *endBB = testEnumContainsPayload(IGF, addr, T);
        
        // If there is, project and destroy it.
        Address payloadAddr = projectPayloadData(IGF, addr);
        getPayloadTypeInfo().destroy(IGF, payloadAddr,
                                     getPayloadType(IGF.IGM, T));
        
        IGF.Builder.CreateBr(endBB);
        IGF.Builder.emitBlock(endBB);
        return;
      }
          
      case NullableSwiftRefcounted:
      case NullableUnknownRefcounted: {
        // Load the value as swift.refcounted, then hand to swift_release.
        addr = IGF.Builder.CreateBitCast(addr,
                                 getRefcountedPtrType(IGF.IGM)->getPointerTo());
        llvm::Value *ptr = IGF.Builder.CreateLoad(addr);
        releaseRefcountedPayload(IGF, ptr);
        return;
      }
      }
    }
    
  private:
    llvm::ConstantInt *getZeroExtraTagConstant(IRGenModule &IGM) const {
      assert(TIK >= Fixed && "not fixed layout");
      assert(ExtraTagBitCount > 0 && "no extra tag bits?!");
      return llvm::ConstantInt::get(IGM.getLLVMContext(),
                                    APInt(ExtraTagBitCount, 0));
    }
    
    /// Initialize the extra tag bits, if any, to zero to indicate a payload.
    void emitInitializeExtraTagBitsForPayload(IRGenFunction &IGF,
                                              Address dest,
                                              CanType T) const {
      if (TIK >= Fixed) {
        // We statically know whether we have extra tag bits.
        // Store zero directly to the fixed-layout extra tag field.
        if (ExtraTagBitCount > 0) {
          auto *zeroTag = getZeroExtraTagConstant(IGF.IGM);
          IGF.Builder.CreateStore(zeroTag, projectExtraTagBits(IGF, dest));
        }
        return;
      }
      
      // Ask the runtime to store the tag.
      llvm::Value *opaqueAddr = IGF.Builder.CreateBitCast(dest.getAddress(),
                                                          IGF.IGM.OpaquePtrTy);
      llvm::Value *metadata = emitPayloadMetadata(IGF, T);
      IGF.Builder.CreateCall4(IGF.IGM.getStoreEnumTagSinglePayloadFn(),
                              opaqueAddr, metadata,
                              llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1),
                              llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                                                 ElementsWithNoPayload.size()));
    }
    
    /// Emit an reassignment sequence from an enum at one address to another.
    void emitIndirectAssign(IRGenFunction &IGF,
       Address dest, Address src, CanType T,
       IsTake_t isTake)
    const {
      auto &C = IGF.IGM.getLLVMContext();
      auto PayloadT = getPayloadType(IGF.IGM, T);
      
      switch (CopyDestroyKind) {
      case POD:
        return emitPrimitiveCopy(IGF, dest, src, T);
          
      case Normal: {
        llvm::BasicBlock *endBB = llvm::BasicBlock::Create(C);

        Address destData = projectPayloadData(IGF, dest);
        Address srcData = projectPayloadData(IGF, src);
        // See whether the current value at the destination has a payload.

        llvm::BasicBlock *noDestPayloadBB
          = testEnumContainsPayload(IGF, dest, T);
        
        // Here, the destination has a payload. Now see if the source also has
        // one.
        llvm::BasicBlock *destNoSrcPayloadBB
          = testEnumContainsPayload(IGF, src, T);
        
        // Here, both source and destination have payloads. Do the reassignment
        // of the payload in-place.
        if (isTake)
          getPayloadTypeInfo().assignWithTake(IGF, destData, srcData, PayloadT);
        else
          getPayloadTypeInfo().assignWithCopy(IGF, destData, srcData, PayloadT);
        IGF.Builder.CreateBr(endBB);
        
        // If the destination has a payload but the source doesn't, we can destroy
        // the payload and primitive-store the new no-payload value.
        IGF.Builder.emitBlock(destNoSrcPayloadBB);
        getPayloadTypeInfo().destroy(IGF, destData, PayloadT);
        emitPrimitiveCopy(IGF, dest, src, T);
        IGF.Builder.CreateBr(endBB);
        
        // Now, if the destination has no payload, check if the source has one.
        IGF.Builder.emitBlock(noDestPayloadBB);
        llvm::BasicBlock *noDestNoSrcPayloadBB
          = testEnumContainsPayload(IGF, src, T);

        // Here, the source has a payload but the destination doesn't. We can
        // copy-initialize the source over the destination, then primitive-store
        // the zero extra tag (if any).
        if (isTake)
          getPayloadTypeInfo().initializeWithTake(IGF, destData, srcData, PayloadT);
        else
          getPayloadTypeInfo().initializeWithCopy(IGF, destData, srcData, PayloadT);
        emitInitializeExtraTagBitsForPayload(IGF, dest, T);
        IGF.Builder.CreateBr(endBB);
        
        // If neither destination nor source have payloads, we can just primitive-
        // store the new empty-case value.
        IGF.Builder.emitBlock(noDestNoSrcPayloadBB);
        emitPrimitiveCopy(IGF, dest, src, T);
        IGF.Builder.CreateBr(endBB);
        
        IGF.Builder.emitBlock(endBB);
        return;
      }
      
      case NullableSwiftRefcounted:
      case NullableUnknownRefcounted: {
        // Do the assignment as for a refcounted pointer.
        auto refCountedTy = getRefcountedPtrType(IGF.IGM);
        Address destAddr = IGF.Builder.CreateBitCast(dest,
                                                 refCountedTy->getPointerTo());
        Address srcAddr = IGF.Builder.CreateBitCast(src,
                                                refCountedTy->getPointerTo());
        // Load the old pointer at the destination.
        llvm::Value *oldPtr = IGF.Builder.CreateLoad(destAddr);
        // Store the new pointer.
        llvm::Value *srcPtr = IGF.Builder.CreateLoad(srcAddr);
        if (!isTake)
          retainRefcountedPayload(IGF, srcPtr);
        IGF.Builder.CreateStore(srcPtr, destAddr);
        // Release the old value.
        releaseRefcountedPayload(IGF, oldPtr);
        return;
      }
      }
      
    }
    
    /// Emit an initialization sequence, initializing an enum at one address
    /// with another at a different address.
    void emitIndirectInitialize(IRGenFunction &IGF,
                                Address dest, Address src, CanType T,
                                IsTake_t isTake)
    const {
      auto &C = IGF.IGM.getLLVMContext();
      
      switch (CopyDestroyKind) {
      case POD:
        return emitPrimitiveCopy(IGF, dest, src, T);

      case Normal: {
        llvm::BasicBlock *endBB = llvm::BasicBlock::Create(C);
        
        Address destData = projectPayloadData(IGF, dest);
        Address srcData = projectPayloadData(IGF, src);
        
        // See whether the source value has a payload.
        llvm::BasicBlock *noSrcPayloadBB
          = testEnumContainsPayload(IGF, src, T);
        
        // Here, the source value has a payload. Initialize the destination with
        // it, and set the extra tag if any to zero.
        if (isTake)
          getPayloadTypeInfo().initializeWithTake(IGF, destData, srcData,
                                                  getPayloadType(IGF.IGM, T));
        else
          getPayloadTypeInfo().initializeWithCopy(IGF, destData, srcData,
                                                  getPayloadType(IGF.IGM, T));
        emitInitializeExtraTagBitsForPayload(IGF, dest, T);
        IGF.Builder.CreateBr(endBB);
        
        // If the source value has no payload, we can primitive-store the
        // empty-case value.
        IGF.Builder.emitBlock(noSrcPayloadBB);
        emitPrimitiveCopy(IGF, dest, src, T);
        IGF.Builder.CreateBr(endBB);
        
        IGF.Builder.emitBlock(endBB);
        return;
      }
          
      case NullableSwiftRefcounted:
      case NullableUnknownRefcounted: {
        auto refCountedTy = getRefcountedPtrType(IGF.IGM);

        // Do the initialization as for a refcounted pointer.
        Address destAddr = IGF.Builder.CreateBitCast(dest,
                                                 refCountedTy->getPointerTo());
        Address srcAddr = IGF.Builder.CreateBitCast(src,
                                                 refCountedTy->getPointerTo());

        llvm::Value *srcPtr = IGF.Builder.CreateLoad(srcAddr);
        if (!isTake)
          retainRefcountedPayload(IGF, srcPtr);
        IGF.Builder.CreateStore(srcPtr, destAddr);
        return;
      }
      }
    }
    
  public:
    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                        CanType T)
    const override {
      emitIndirectAssign(IGF, dest, src, T, IsNotTake);
    }
    
    void assignWithTake(IRGenFunction &IGF, Address dest, Address src,
                        CanType T)
    const override {
      emitIndirectAssign(IGF, dest, src, T, IsTake);
    }
    
    void initializeWithCopy(IRGenFunction &IGF, Address dest, Address src,
                            CanType T)
    const override {
      emitIndirectInitialize(IGF, dest, src, T, IsNotTake);
    }
    
    void initializeWithTake(IRGenFunction &IGF, Address dest, Address src,
                            CanType T)
    const override {
      emitIndirectInitialize(IGF, dest, src, T, IsTake);
    }
    
    void storeTag(IRGenFunction &IGF,
                  EnumElementDecl *elt,
                  Address enumAddr,
                  CanType T) const override {
      if (TIK < Fixed) {
        // If the enum isn't fixed-layout, get the runtime to do this for us.
        llvm::Value *payload = emitPayloadMetadata(IGF, T);
        llvm::Value *caseIndex;
        if (elt == getPayloadElement()) {
          caseIndex = llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1);
        } else {
          auto found = std::find_if(ElementsWithNoPayload.begin(),
                                    ElementsWithNoPayload.end(),
                                    [&](Element a) { return a.decl == elt; });
          assert(found != ElementsWithNoPayload.end() &&
                 "case not in enum?!");
          unsigned caseIndexVal = found - ElementsWithNoPayload.begin();
          caseIndex = llvm::ConstantInt::get(IGF.IGM.Int32Ty, caseIndexVal);
        }
        
        llvm::Value *numEmptyCases = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                                                  ElementsWithNoPayload.size());
        
        llvm::Value *opaqueAddr
          = IGF.Builder.CreateBitCast(enumAddr.getAddress(),
                                      IGF.IGM.OpaquePtrTy);
        
        IGF.Builder.CreateCall4(IGF.IGM.getStoreEnumTagSinglePayloadFn(),
                                opaqueAddr, payload, caseIndex, numEmptyCases);
        
        return;
      }
      
      if (elt == getPayloadElement()) {
        // The data occupies the entire payload. If we have extra tag bits,
        // zero them out.
        if (ExtraTagBitCount > 0)
          IGF.Builder.CreateStore(getZeroExtraTagConstant(IGF.IGM),
                                  projectExtraTagBits(IGF, enumAddr));
        return;
      }
      
      // Store the discriminator for the no-payload case.
      llvm::Value *payload, *extraTag;
      std::tie(payload, extraTag) = getNoPayloadCaseValue(IGF.IGM, elt);
      
      if (payloadTy)
        IGF.Builder.CreateStore(payload, projectPayload(IGF, enumAddr));
      if (ExtraTagBitCount > 0)
        IGF.Builder.CreateStore(extraTag, projectExtraTagBits(IGF, enumAddr));
    }
    
    void initializeMetadata(IRGenFunction &IGF,
                            llvm::Value *metadata,
                            llvm::Value *vwtable,
                            CanType T) const override
    {
      // Fixed-size enums don't need dynamic witness table initialization.
      if (TIK >= Fixed) return;

      // Ask the runtime to do our layout using the payload metadata and number
      // of empty cases.
      auto payloadMetadata = emitPayloadMetadata(IGF, T);
      auto emptyCasesVal = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                                                  ElementsWithNoPayload.size());
      
      IGF.Builder.CreateCall3(
                    IGF.IGM.getInitEnumValueWitnessTableSinglePayloadFn(),
                    vwtable, payloadMetadata, emptyCasesVal);
    }
    
    /// \group Extra inhabitants
    
    // Extra inhabitants from the payload that we didn't use for our empty cases
    // are available to outer enums.
    // FIXME: If we spilled extra tag bits, we could offer spare bits from the
    // tag.
    
    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      if (TIK >= Fixed)
        return getFixedExtraInhabitantCount(IGM) > 0;

      return getPayloadTypeInfo().mayHaveExtraInhabitants(IGM);
    }
    
    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return getFixedPayloadTypeInfo().getFixedExtraInhabitantCount(IGM)
               - getNumExtraInhabitantTagValues();
    }
    
    llvm::ConstantInt *
    getFixedExtraInhabitantValue(IRGenModule &IGM,
                                 unsigned bits,
                                 unsigned index) const override {
      return getFixedPayloadTypeInfo()
        .getFixedExtraInhabitantValue(IGM, bits,
                                      index + getNumExtraInhabitantTagValues());
    }
    
    llvm::Value *
    getExtraInhabitantIndex(IRGenFunction &IGF,
                            Address src, CanType T) const override {
      auto payload = projectPayloadData(IGF, src);
      llvm::Value *index
        = getPayloadTypeInfo().getExtraInhabitantIndex(IGF, payload,
                                                   getPayloadType(IGF.IGM, T));
      
      // Offset the payload extra inhabitant index by the number of inhabitants
      // we used. If less than zero, it's a valid value of the enum type.
      index = IGF.Builder.CreateSub(index,
         llvm::ConstantInt::get(IGF.IGM.Int32Ty, ElementsWithNoPayload.size()));
      auto valid = IGF.Builder.CreateICmpSLT(index,
                                   llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0));
      index = IGF.Builder.CreateSelect(valid,
                              llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1),
                              index);
      return index;
    }
    
    void storeExtraInhabitant(IRGenFunction &IGF,
                              llvm::Value *index,
                              Address dest, CanType T) const override {
      // Offset the index to skip the extra inhabitants we used.
      index = IGF.Builder.CreateAdd(index,
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, ElementsWithNoPayload.size()));
      
      auto payload = projectPayloadData(IGF, dest);
      getPayloadTypeInfo().storeExtraInhabitant(IGF, index, payload,
                                                   getPayloadType(IGF.IGM, T));
    }
    
    llvm::BitVector
    getBitPatternForNoPayloadElement(IRGenModule &IGM,
                                     EnumElementDecl *theCase) const override {
      llvm::ConstantInt *payloadPart, *extraPart;
      std::tie(payloadPart, extraPart) = getNoPayloadCaseValue(IGM, theCase);
      llvm::BitVector bits;
      
      if (payloadPart)
        bits = getBitVectorFromAPInt(payloadPart->getValue());
      
      unsigned totalSize
        = cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits();
      if (extraPart) {
        llvm::BitVector extraBits = getBitVectorFromAPInt(extraPart->getValue(),
                                                          bits.size());
        bits.resize(totalSize);
        extraBits.resize(totalSize);
        bits |= extraBits;
      } else {
        assert(totalSize == bits.size());
      }
      return bits;
    }

    llvm::BitVector getTagBitsForPayloads(IRGenModule &IGM) const override {
      // We only have tag bits if we spilled extra bits.
      llvm::BitVector result;
      unsigned payloadSize
        = getFixedPayloadTypeInfo().getFixedSize().getValueInBits();
      result.resize(payloadSize, false);

      unsigned totalSize
        = cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits();
      
      if (ExtraTagBitCount) {
        result.resize(payloadSize + ExtraTagBitCount, true);
        result.resize(totalSize, false);
      } else {
        assert(payloadSize == totalSize);
      }
      return result;
    }
  };

  class MultiPayloadEnumImplStrategy final
    : public PayloadEnumImplStrategyBase
  {
    // The spare bits shared by all payloads, if any.
    // Invariant: The size of the bit vector is the size of the payload in bits,
    // rounded up to a byte boundary.
    llvm::BitVector CommonSpareBits;
    
    // The common spare bits actually used for a tag in the payload area.
    llvm::BitVector PayloadTagBits;

    // The number of tag values used for no-payload cases.
    unsigned NumEmptyElementTags = ~0u;

    /// More efficient value semantics implementations for certain enum layouts.
    enum CopyDestroyStrategy {
      /// No special behavior.
      Normal,
      /// The payloads are all POD, so copying is bitwise, and destruction is a
      /// noop.
      POD,
      /// The payloads are all Swift-reference-counted values, and there is at
      /// most one no-payload case with the tagged-zero representation. Copy
      /// and destroy can just mask out the tag bits and pass the result to
      /// swift_retain/swift_release.
      TaggedSwiftRefcounted,
      /// The payloads are all reference-counted values, and there is at
      /// most one no-payload case with the tagged-zero representation. Copy
      /// and destroy can just mask out the tag bits and pass the result to
      /// swift_unknownRetain/swift_unknownRelease.
      TaggedUnknownRefcounted,
    };
    
    CopyDestroyStrategy CopyDestroyKind;

  public:
    MultiPayloadEnumImplStrategy(IRGenModule &IGM,
                                 TypeInfoKind tik, unsigned NumElements,
                                  std::vector<Element> &&WithPayload,
                                  std::vector<Element> &&WithRecursivePayload,
                                  std::vector<Element> &&WithNoPayload)
      : PayloadEnumImplStrategyBase(IGM, tik, NumElements,
                                     std::move(WithPayload),
                                     std::move(WithRecursivePayload),
                                     std::move(WithNoPayload)),
        CopyDestroyKind(Normal)
    {
      assert(ElementsWithPayload.size() > 1);
      
      // Check the payloads to see if we can take advantage of common layout to
      // optimize our value semantics.
      bool allPOD = true;
      bool allSingleSwiftRefcount = true;
      bool allSingleUnknownRefcount = true;
      for (auto &elt : ElementsWithPayload) {
        if (!elt.ti->isPOD(ResilienceScope::Component))
          allPOD = false;
        if (!elt.ti->isSingleSwiftRetainablePointer(ResilienceScope::Component))
          allSingleSwiftRefcount = false;
        if (!elt.ti->isSingleUnknownRetainablePointer(ResilienceScope::Component))
          allSingleUnknownRefcount = false;
      }
      
      if (allPOD) {
        assert(!allSingleSwiftRefcount && !allSingleUnknownRefcount
               && "pod *and* refcounted?!");
        CopyDestroyKind = POD;
      } else if (allSingleSwiftRefcount
                 && WithNoPayload.size() <= 1) {
        CopyDestroyKind = TaggedSwiftRefcounted;
      }
      // FIXME: Memory corruption issues arise when enabling this for mixed
      // Swift/ObjC enums.
      else if (allSingleUnknownRefcount
                 && WithNoPayload.size() <= 1) {
        CopyDestroyKind = TaggedUnknownRefcounted;
      }
    }
    
    TypeInfo *completeEnumTypeLayout(TypeConverter &TC,
                                      CanType type,
                                      EnumDecl *theEnum,
                                      llvm::StructType *enumTy) override;
    
  private:
    unsigned getNumCaseBits() const {
      return CommonSpareBits.size() - CommonSpareBits.count();
    }
    
    /// The number of empty cases representable by each tag value.
    /// Equal to the size of the payload minus the spare bits used for tags.
    unsigned getNumCasesPerTag() const {
      unsigned numCaseBits = getNumCaseBits();
      return numCaseBits >= 32
        ? 0x80000000 : 1 << numCaseBits;
    }
    
    /// Extract the payload-discriminating tag from a payload and optional
    /// extra tag value.
    llvm::Value *extractPayloadTag(IRGenFunction &IGF,
                                   llvm::Value *payload,
                                   llvm::Value *extraTagBits) const {
      unsigned numSpareBits = PayloadTagBits.count();
      llvm::Value *tag = nullptr;
      unsigned numTagBits = numSpareBits + ExtraTagBitCount;
      
      // Get the tag bits from spare bits, if any.
      if (numSpareBits > 0) {
        tag = emitGatherSpareBits(IGF, PayloadTagBits, payload, 0, numTagBits);
      }
      
      // Get the extra tag bits, if any.
      if (ExtraTagBitCount > 0) {
        assert(extraTagBits);
        if (!tag) {
          return extraTagBits;
        } else {
          extraTagBits = IGF.Builder.CreateZExt(extraTagBits, tag->getType());
          extraTagBits = IGF.Builder.CreateShl(extraTagBits,
                                               numTagBits - ExtraTagBitCount);
          return IGF.Builder.CreateOr(tag, extraTagBits);
        }
      }
      assert(!extraTagBits);
      return tag;
    }
    
    llvm::Type *getRefcountedPtrType(IRGenModule &IGM) const {
      switch (CopyDestroyKind) {
      case TaggedSwiftRefcounted:
        return IGM.RefCountedPtrTy;
      case TaggedUnknownRefcounted:
        return IGM.UnknownRefCountedPtrTy;
      case POD:
      case Normal:
        llvm_unreachable("not a refcounted payload");
      }
    }
    
    void retainRefcountedPayload(IRGenFunction &IGF,
                                 llvm::Value *ptr) const {
      switch (CopyDestroyKind) {
      case TaggedSwiftRefcounted: {
        IGF.emitRetainCall(ptr);
        return;
      }
      case TaggedUnknownRefcounted: {
        IGF.emitUnknownRetainCall(ptr);
        return;
      }
      case POD:
      case Normal:
        llvm_unreachable("not a refcounted payload");
      }
    }
    
    void releaseRefcountedPayload(IRGenFunction &IGF,
                                  llvm::Value *ptr) const {
      switch (CopyDestroyKind) {
      case TaggedSwiftRefcounted: {
        IGF.emitRelease(ptr);
        return;
      }
      case TaggedUnknownRefcounted: {
        IGF.emitUnknownRelease(ptr);
        return;
      }
      case POD:
      case Normal:
        llvm_unreachable("not a refcounted payload");
      }
    }
    
    llvm::ConstantInt *getEmptyCasePayload(IRGenModule &IGM,
                                       unsigned tagIndex, unsigned idx) const {
      APInt v = interleaveSpareBits(IGM, PayloadTagBits,
                                    PayloadTagBits.size(),
                                    tagIndex, 0);
      v |= interleaveSpareBits(IGM, CommonSpareBits,
                               CommonSpareBits.size(),
                               0, idx);
      return llvm::ConstantInt::get(IGM.getLLVMContext(), v);
    }
  public:
    void emitValueSwitch(IRGenFunction &IGF,
                         Explosion &value,
                         ArrayRef<std::pair<EnumElementDecl*,
                                            llvm::BasicBlock*>> dests,
                         llvm::BasicBlock *defaultDest) const override {
      auto &C = IGF.IGM.getLLVMContext();
      
      // Create a map of the destination blocks for quicker lookup.
      llvm::DenseMap<EnumElementDecl*,llvm::BasicBlock*> destMap(dests.begin(),
                                                                  dests.end());
      
      // Create an unreachable branch for unreachable switch defaults.
      auto *unreachableBB = llvm::BasicBlock::Create(C);
      
      // If there was no default branch in SIL, use the unreachable branch as
      // the default.
      if (!defaultDest)
        defaultDest = unreachableBB;
      
      auto blockForCase = [&](EnumElementDecl *theCase) -> llvm::BasicBlock* {
        auto found = destMap.find(theCase);
        if (found == destMap.end())
          return defaultDest;
        else
          return found->second;
      };
      
      llvm::Value *payload = value.claimNext();
      llvm::Value *extraTagBits = nullptr;
      if (ExtraTagBitCount > 0)
        extraTagBits = value.claimNext();
      
      // Extract and switch on the tag bits.
      llvm::Value *tag = extractPayloadTag(IGF, payload, extraTagBits);
      unsigned numTagBits
        = cast<llvm::IntegerType>(tag->getType())->getBitWidth();
      
      auto *tagSwitch = IGF.Builder.CreateSwitch(tag, unreachableBB,
                             ElementsWithPayload.size() + NumEmptyElementTags);
      
      // Switch over the tag bits for payload cases.
      unsigned tagIndex = 0;
      for (auto &payloadCasePair : ElementsWithPayload) {
        EnumElementDecl *payloadCase = payloadCasePair.decl;
        tagSwitch->addCase(llvm::ConstantInt::get(C,APInt(numTagBits,tagIndex)),
                           blockForCase(payloadCase));
        ++tagIndex;
      }
      
      // Switch over the no-payload cases.
      unsigned casesPerTag = getNumCasesPerTag();
      
      auto elti = ElementsWithNoPayload.begin(),
           eltEnd = ElementsWithNoPayload.end();
      
      for (unsigned i = 0; i < NumEmptyElementTags; ++i) {
        assert(elti != eltEnd &&
               "ran out of cases before running out of extra tags?");
        auto *tagBB = llvm::BasicBlock::Create(C);
        tagSwitch->addCase(llvm::ConstantInt::get(C,APInt(numTagBits,tagIndex)),
                           tagBB);
        
        // Switch over the cases for this tag.
        IGF.Builder.emitBlock(tagBB);
        auto *caseSwitch = IGF.Builder.CreateSwitch(payload, unreachableBB);
        for (unsigned idx = 0; idx < casesPerTag && elti != eltEnd; ++idx) {
          APInt v = interleaveSpareBits(IGF.IGM, PayloadTagBits,
                                        PayloadTagBits.size(),
                                        tagIndex, 0);
          v |= interleaveSpareBits(IGF.IGM, CommonSpareBits,
                                   CommonSpareBits.size(),
                                   0, idx);
          
          auto val = getEmptyCasePayload(IGF.IGM, tagIndex, idx);
          caseSwitch->addCase(val, blockForCase(elti->decl));
          ++elti;
        }
        
        ++tagIndex;
      }
      
      // Delete the unreachable default block if we didn't use it, or emit it
      // if we did.
      if (unreachableBB->use_empty()) {
        delete unreachableBB;
      } else {
        IGF.Builder.emitBlock(unreachableBB);
        IGF.Builder.CreateUnreachable();
      }
    }

    void emitIndirectSwitch(IRGenFunction &IGF,
                            CanType T,
                            Address addr,
                            ArrayRef<std::pair<EnumElementDecl*,
                                               llvm::BasicBlock*>> dests,
                            llvm::BasicBlock *defaultDest) const override {
      if (TIK >= Fixed) {
        // Load the fixed-size representation and switch directly.
        Explosion value(ResilienceExpansion::Minimal);
        loadForSwitch(IGF, addr, value);
        return emitValueSwitch(IGF, value, dests, defaultDest);
      }
      
      // Use the runtime to dynamically switch.
      llvm_unreachable("dynamic switch for multi-payload enum not implemented");
    }
    
  private:
    void projectPayloadValue(IRGenFunction &IGF,
                        llvm::Value *payload,
                        unsigned payloadTag,
                        const LoadableTypeInfo &payloadTI,
                        Explosion &out) const {
      // If we have spare bits, we have to mask out any set tag bits packed
      // there.
      if (PayloadTagBits.any()) {
        unsigned spareBitCount = PayloadTagBits.count();
        if (spareBitCount < 32)
          payloadTag &= (1U << spareBitCount) - 1U;
        if (payloadTag != 0) {
          APInt mask = ~getAPIntFromBitVector(PayloadTagBits);
          auto maskVal = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                                mask);
          payload = IGF.Builder.CreateAnd(payload, maskVal);
        }
      }
      
      // Unpack the payload.
      payloadTI.unpackEnumPayload(IGF, payload, out, 0);
    }
    
  public:
    void emitValueProject(IRGenFunction &IGF,
                          Explosion &inValue,
                          EnumElementDecl *theCase,
                          Explosion &out) const override {
      auto foundPayload = std::find_if(ElementsWithPayload.begin(),
                                       ElementsWithPayload.end(),
             [&](const Element &e) { return e.decl == theCase; });
      
      // Non-payload cases project to an empty explosion.
      if (foundPayload == ElementsWithPayload.end()) {
        inValue.claim(getExplosionSize(inValue.getKind()));
        return;
      }
      
      llvm::Value *payload = inValue.claimNext();
      // We don't need the tag bits.
      if (ExtraTagBitCount > 0)
        inValue.claimNext();
      
      // Unpack the payload.
      projectPayloadValue(IGF, payload,
                          foundPayload - ElementsWithPayload.begin(),
                     cast<LoadableTypeInfo>(*foundPayload->ti), out);
    }
    
    llvm::Value *packEnumPayload(IRGenFunction &IGF, Explosion &src,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      PackEnumPayload pack(IGF, bitWidth);
      // Pack the payload.
      pack.addAtOffset(src.claimNext(), offset);
      // Pack the extra bits, if any.
      if (ExtraTagBitCount > 0) {
        pack.addAtOffset(src.claimNext(), CommonSpareBits.size() + offset);
      }
      return pack.get();
    }
    
    void unpackEnumPayload(IRGenFunction &IGF, llvm::Value *outerPayload,
                            Explosion &dest, unsigned offset) const override {
      UnpackEnumPayload unpack(IGF, outerPayload);
      // Unpack the payload.
      dest.add(unpack.claimAtOffset(payloadTy, offset));
      // Unpack the extra bits, if any.
      if (ExtraTagBitCount > 0) {
        dest.add(unpack.claimAtOffset(extraTagTy,
                                      CommonSpareBits.size() + offset));
      }
    }
    
  private:
    void emitPayloadInjection(IRGenFunction &IGF,
                              const FixedTypeInfo &payloadTI,
                              Explosion &params, Explosion &out,
                              unsigned tag) const {
      // Pack the payload.
      auto &loadablePayloadTI = cast<LoadableTypeInfo>(payloadTI); // FIXME
      llvm::Value *payload = loadablePayloadTI.packEnumPayload(IGF, params,
                                                    CommonSpareBits.size(), 0);
      
      // If we have spare bits, pack tag bits into them.
      unsigned numSpareBits = PayloadTagBits.count();
      if (numSpareBits > 0) {
        APInt tagMaskVal
          = interleaveSpareBits(IGF.IGM, PayloadTagBits,
                                PayloadTagBits.size(),
                                tag, 0);
        auto tagMask
          = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(), tagMaskVal);
        payload = IGF.Builder.CreateOr(payload, tagMask);
      }
      
      out.add(payload);
      
      // If we have extra tag bits, pack the remaining tag bits into them.
      if (ExtraTagBitCount > 0) {
        tag >>= numSpareBits;
        auto extra = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                            APInt(ExtraTagBitCount, tag));
        out.add(extra);
      }
    }
    
    std::pair<llvm::ConstantInt*, llvm::ConstantInt*>
    getNoPayloadCaseValue(IRGenModule &IGM, unsigned index) const {
      // Figure out the tag and payload for the empty case.
      unsigned numCaseBits = getNumCaseBits();
      unsigned tag, tagIndex;
      if (numCaseBits >= 32) {
        tag = ElementsWithPayload.size();
        tagIndex = index;
      } else {
        tag = (index >> numCaseBits) + ElementsWithPayload.size();
        tagIndex = index & ((1 << numCaseBits) - 1);
      }
      
      llvm::ConstantInt *payload;
      llvm::ConstantInt *extraTag = nullptr;
      unsigned numSpareBits = CommonSpareBits.count();
      if (numSpareBits > 0) {
        // If we have spare bits, pack tag bits into them.
        payload = getEmptyCasePayload(IGM, tag, tagIndex);
      } else {
        // Otherwise the payload is just the index.
        payload = llvm::ConstantInt::get(IGM.getLLVMContext(),
                                     APInt(CommonSpareBits.size(), tagIndex));
      }
      
      // If we have extra tag bits, pack the remaining tag bits into them.
      if (ExtraTagBitCount > 0) {
        tag >>= numSpareBits;
        extraTag = llvm::ConstantInt::get(IGM.getLLVMContext(),
                                          APInt(ExtraTagBitCount, tag));
      }
      return {payload, extraTag};
    }
    
    void emitNoPayloadInjection(IRGenFunction &IGF, Explosion &out,
                                unsigned index) const {
      llvm::Value *payload, *extraTag;
      std::tie(payload, extraTag) = getNoPayloadCaseValue(IGF.IGM, index);
      out.add(payload);
      if (ExtraTagBitCount > 0) {
        assert(extraTag);
        out.add(extraTag);
      }
    }
    
    void forNontrivialPayloads(IRGenFunction &IGF,
               llvm::Value *payload, llvm::Value *extraTagBits,
               std::function<void (unsigned, EnumImplStrategy::Element)> f)
    const {
      auto *endBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      
      llvm::Value *tag = extractPayloadTag(IGF, payload, extraTagBits);
      auto *swi = IGF.Builder.CreateSwitch(tag, endBB);
      auto *tagTy = cast<llvm::IntegerType>(tag->getType());
      
      // Handle nontrivial tags.
      unsigned tagIndex = 0;
      for (auto &payloadCasePair : ElementsWithPayload) {
        auto &payloadTI = *payloadCasePair.ti;
        
        // Trivial payloads don't need any work.
        if (payloadTI.isPOD(ResilienceScope::Local)) {
          ++tagIndex;
          continue;
        }
        
        // Unpack and handle nontrivial payloads.
        auto *caseBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
        swi->addCase(llvm::ConstantInt::get(tagTy, tagIndex), caseBB);
        
        IGF.Builder.emitBlock(caseBB);
        f(tagIndex, payloadCasePair);
        IGF.Builder.CreateBr(endBB);
        
        ++tagIndex;
      }
      
      IGF.Builder.emitBlock(endBB);
    }
    
    llvm::Value *maskTagBitsFromPayload(IRGenFunction &IGF,
                                        llvm::Value *payload) const {
      if (PayloadTagBits.none())
        return payload;
      
      APInt mask = ~getAPIntFromBitVector(PayloadTagBits);
      auto maskVal = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(), mask);
      return IGF.Builder.CreateAnd(payload, maskVal);
    }
    
  public:
    void emitValueInjection(IRGenFunction &IGF,
                            EnumElementDecl *elt,
                            Explosion &params,
                            Explosion &out) const {
      // See whether this is a payload or empty case we're emitting.
      auto payloadI = std::find_if(ElementsWithPayload.begin(),
                                   ElementsWithPayload.end(),
                               [&](const Element &e) { return e.decl == elt; });
      if (payloadI != ElementsWithPayload.end())
        return emitPayloadInjection(IGF, cast<FixedTypeInfo>(*payloadI->ti),
                                    params, out,
                                    payloadI - ElementsWithPayload.begin());

      auto emptyI = std::find_if(ElementsWithNoPayload.begin(),
                                 ElementsWithNoPayload.end(),
                               [&](const Element &e) { return e.decl == elt; });
      assert(emptyI != ElementsWithNoPayload.end() && "case not in enum");
      emitNoPayloadInjection(IGF, out, emptyI - ElementsWithNoPayload.begin());
    }
    
    void copy(IRGenFunction &IGF, Explosion &src, Explosion &dest)
    const override {
      assert(TIK >= Loadable);
      
      switch (CopyDestroyKind) {
      case POD:
        reexplode(IGF, src, dest);
        return;

      case Normal: {
        llvm::Value *payload = src.claimNext();
        llvm::Value *extraTagBits = ExtraTagBitCount > 0
          ? src.claimNext() : nullptr;
        
        forNontrivialPayloads(IGF, payload, extraTagBits,
          [&](unsigned tagIndex, EnumImplStrategy::Element elt) {
            auto &lti = cast<LoadableTypeInfo>(*elt.ti);
            Explosion value(ResilienceExpansion::Minimal);
            projectPayloadValue(IGF, payload, tagIndex, lti, value);

            Explosion tmp(value.getKind());
            lti.copy(IGF, value, tmp);
            tmp.claimAll(); // FIXME: repack if not bit-identical
          });
        
        dest.add(payload);
        if (extraTagBits)
          dest.add(extraTagBits);
        return;
      }
          
      case TaggedSwiftRefcounted:
      case TaggedUnknownRefcounted: {
        llvm::Value *payload = src.claimNext();
        llvm::Value *extraTagBits = ExtraTagBitCount > 0
          ? src.claimNext() : nullptr;
        
        // Mask the tag bits out of the payload, if any.
        llvm::Value *ptrVal
          = maskTagBitsFromPayload(IGF, payload);
        
        // Retain the pointer.
        auto ptr = IGF.Builder.CreateIntToPtr(ptrVal,
                                              getRefcountedPtrType(IGF.IGM));
        retainRefcountedPayload(IGF, ptr);
        
        dest.add(payload);
        if (extraTagBits)
          dest.add(extraTagBits);
        return;
      }
      }
      
    }
    
    void consume(IRGenFunction &IGF, Explosion &src) const override {
      assert(TIK >= Loadable);
      
      switch (CopyDestroyKind) {
      case POD:
        src.claim(getExplosionSize(src.getKind()));
        return;
          
      case Normal: {
        llvm::Value *payload = src.claimNext();
        llvm::Value *extraTagBits = ExtraTagBitCount > 0
          ? src.claimNext() : nullptr;
        
        forNontrivialPayloads(IGF, payload, extraTagBits,
          [&](unsigned tagIndex, EnumImplStrategy::Element elt) {
            auto &lti = cast<LoadableTypeInfo>(*elt.ti);
            Explosion value(ResilienceExpansion::Minimal);
            projectPayloadValue(IGF, payload, tagIndex, lti, value);

            lti.consume(IGF, value);
          });
        return;
      }
          
      case TaggedSwiftRefcounted:
      case TaggedUnknownRefcounted: {
        llvm::Value *payload = src.claimNext();
        if (ExtraTagBitCount > 0)
          src.claimNext();

        // Mask the tag bits out of the payload, if any.
        llvm::Value *ptrVal
          = maskTagBitsFromPayload(IGF, payload);

        // Release the pointer.
        auto ptr = IGF.Builder.CreateIntToPtr(ptrVal,
                                              getRefcountedPtrType(IGF.IGM));
        releaseRefcountedPayload(IGF, ptr);
        return;
      }
      }
    }
  
  private:
    /// Emit a reassignment sequence from an enum at one address to another.
    void emitIndirectAssign(IRGenFunction &IGF,
                            Address dest, Address src, CanType T,
                            IsTake_t isTake) const {
      auto &C = IGF.IGM.getLLVMContext();
      
      switch (CopyDestroyKind) {
      case POD:
        return emitPrimitiveCopy(IGF, dest, src, T);
      
      case TaggedSwiftRefcounted:
      case TaggedUnknownRefcounted:
      case Normal: {
        // If the enum is loadable, it's better to do this directly using values,
        // so we don't need to RMW tag bits in place.
        if (TI->isLoadable()) {
          Explosion tmpSrc(ResilienceExpansion::Minimal),
                    tmpOld(ResilienceExpansion::Minimal);
          if (isTake)
            loadAsTake(IGF, src, tmpSrc);
          else
            loadAsCopy(IGF, src, tmpSrc);
          
          loadAsTake(IGF, dest, tmpOld);
          initialize(IGF, tmpSrc, dest);
          consume(IGF, tmpOld);
          return;
        }
        
        auto *endBB = llvm::BasicBlock::Create(C);
        
        // Sanity-check whether the source and destination alias.
        llvm::Value *alias = IGF.Builder.CreateICmpEQ(dest.getAddress(),
                                                      src.getAddress());
        auto *noAliasBB = llvm::BasicBlock::Create(C);
        IGF.Builder.CreateCondBr(alias, endBB, noAliasBB);
        IGF.Builder.emitBlock(noAliasBB);
        
        // Destroy the old value.
        destroy(IGF, dest, T);
        
        // Reinitialize with the new value.
        emitIndirectInitialize(IGF, dest, src, T, isTake);
        
        IGF.Builder.CreateBr(endBB);
        IGF.Builder.emitBlock(endBB);
        return;
      }
      }
    }
    
    void emitIndirectInitialize(IRGenFunction &IGF,
                                Address dest, Address src,
                                CanType T,
                                IsTake_t isTake) const{
      auto &C = IGF.IGM.getLLVMContext();

      switch (CopyDestroyKind) {
      case POD:
        return emitPrimitiveCopy(IGF, dest, src, T);

      case TaggedSwiftRefcounted:
      case TaggedUnknownRefcounted:
      case Normal: {
        // If the enum is loadable, it's better to do this directly using values,
        // so we don't need to RMW tag bits in place.
        if (TI->isLoadable()) {
          Explosion tmpSrc(ResilienceExpansion::Minimal);
          if (isTake)
            loadAsTake(IGF, src, tmpSrc);
          else
            loadAsCopy(IGF, src, tmpSrc);
          initialize(IGF, tmpSrc, dest);
          return;
        }

        llvm::Value *payload, *extraTagBits;
        std::tie(payload, extraTagBits)
          = emitPrimitiveLoadPayloadAndExtraTag(IGF, src);

        auto *endBB = llvm::BasicBlock::Create(C);

        /// Switch out nontrivial payloads.
        auto *trivialBB = llvm::BasicBlock::Create(C);
        llvm::Value *tag = extractPayloadTag(IGF, payload, extraTagBits);
        auto *swi = IGF.Builder.CreateSwitch(tag, trivialBB);
        auto *tagTy = cast<llvm::IntegerType>(tag->getType());
        
        unsigned tagIndex = 0;
        for (auto &payloadCasePair : ElementsWithPayload) {
          CanType PayloadT = T->getTypeOfMember(IGF.IGM.SILMod->getSwiftModule(),
                                  payloadCasePair.decl, nullptr,
                                  payloadCasePair.decl->getArgumentType())
            ->getCanonicalType();
          auto &payloadTI = *payloadCasePair.ti;
          // Trivial payloads can all share the default path.
          if (payloadTI.isPOD(ResilienceScope::Local)) {
            ++tagIndex;
            continue;
          }
          
          // For nontrivial payloads, we need to copy/take the payload using its
          // value semantics.
          auto *caseBB = llvm::BasicBlock::Create(C);
          swi->addCase(llvm::ConstantInt::get(tagTy, tagIndex), caseBB);
          IGF.Builder.emitBlock(caseBB);

          // Temporarily clear the tag bits from the source so we can use the
          // data.
          preparePayloadForLoad(IGF, src, tagIndex);
          
          // Do the take/copy of the payload.
          Address srcData = IGF.Builder.CreateBitCast(src,
                                  payloadTI.getStorageType()->getPointerTo());
          Address destData = IGF.Builder.CreateBitCast(dest,
                                    payloadTI.getStorageType()->getPointerTo());

          if (isTake) {
            payloadTI.initializeWithTake(IGF, destData, srcData, PayloadT);
            // We don't need to preserve the old value.
          } else {
            payloadTI.initializeWithCopy(IGF, destData, srcData, PayloadT);
            // Replant the tag bits, if any, in the source.
            storePayloadTag(IGF, src, tagIndex);
          }
          
          // Plant spare bit tag bits, if any, into the new value.
          storePayloadTag(IGF, dest, tagIndex);
          IGF.Builder.CreateBr(endBB);
          
          ++tagIndex;
        }
        
        // For trivial payloads (including no-payload cases), we can just
        // primitive-store to the destination.
        IGF.Builder.emitBlock(trivialBB);
        emitPrimitiveStorePayloadAndExtraTag(IGF, dest, payload, extraTagBits);
        IGF.Builder.CreateBr(endBB);
        
        IGF.Builder.emitBlock(endBB);
      }
      }
    }
    
  public:
    void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                        CanType T)
    const override {
      emitIndirectAssign(IGF, dest, src, T, IsNotTake);
    }
    
    void assignWithTake(IRGenFunction &IGF, Address dest, Address src,
                        CanType T)
    const override {
      emitIndirectAssign(IGF, dest, src, T, IsTake);
    }
    
    void initializeWithCopy(IRGenFunction &IGF, Address dest, Address src,
                            CanType T)
    const override {
      emitIndirectInitialize(IGF, dest, src, T, IsNotTake);
    }
    
    void initializeWithTake(IRGenFunction &IGF, Address dest, Address src,
                            CanType T)
    const override {
      emitIndirectInitialize(IGF, dest, src, T, IsTake);
    }
    
  private:
    /// Clear any tag bits stored in the payload area of the given address.
    void preparePayloadForLoad(IRGenFunction &IGF, Address enumAddr,
                               unsigned tagIndex) const {
      // If the case has non-zero tag bits stored in spare bits, we need to
      // mask them out before the data can be read.
      unsigned numSpareBits = PayloadTagBits.count();
      if (numSpareBits > 0) {
        unsigned spareTagBits = numSpareBits >= 32
          ? tagIndex : tagIndex & ((1U << numSpareBits) - 1U);
        
        if (spareTagBits != 0) {
          assert(payloadTy && "spare bits with empty payload?!");
          Address payloadAddr = projectPayload(IGF, enumAddr);
          llvm::Value *payloadBits = IGF.Builder.CreateLoad(payloadAddr);
          auto *spareBitMask = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                      ~getAPIntFromBitVector(PayloadTagBits));
          payloadBits = IGF.Builder.CreateAnd(payloadBits, spareBitMask);
          IGF.Builder.CreateStore(payloadBits, payloadAddr);
        }
      }
    }
    
    void destroy(IRGenFunction &IGF, Address addr, CanType T)
    const override {
      switch (CopyDestroyKind) {
      case POD:
        return;
          
      case Normal:
      case TaggedSwiftRefcounted:
      case TaggedUnknownRefcounted: {
        // If loadable, it's better to do this directly to the value than
        // in place, so we don't need to RMW out the tag bits in memory.
        if (TI->isLoadable()) {
          Explosion tmp(ResilienceExpansion::Minimal);
          loadAsTake(IGF, addr, tmp);
          consume(IGF, tmp);
          return;
        }
        
        llvm::Value *payload, *extraTagBits;
        std::tie(payload, extraTagBits)
          = emitPrimitiveLoadPayloadAndExtraTag(IGF, addr);
        
        forNontrivialPayloads(IGF, payload, extraTagBits,
          [&](unsigned tagIndex, EnumImplStrategy::Element elt) {
            // Clear tag bits out of the payload area, if any.
            preparePayloadForLoad(IGF, addr, tagIndex);
            // Destroy the data.
            Address dataAddr = IGF.Builder.CreateBitCast(addr,
                                        elt.ti->getStorageType()->getPointerTo());
            CanType payloadT = T->getTypeOfMember(IGF.IGM.SILMod->getSwiftModule(),
                                                  elt.decl, nullptr,
                                                  elt.decl->getArgumentType())
              ->getCanonicalType();
            
            elt.ti->destroy(IGF, dataAddr, payloadT);
          });
        return;
      }
      }
    }
    
    Address projectDataForStore(IRGenFunction &IGF,
                                EnumElementDecl *elt,
                                Address enumAddr) const override {
      auto payloadI = std::find_if(ElementsWithPayload.begin(),
                                   ElementsWithPayload.end(),
         [&](const Element &e) { return e.decl == elt; });

      assert(payloadI != ElementsWithPayload.end() &&
             "cannot project a no-payload case");
      
      // Payloads are all placed at the beginning of the value.
      return IGF.Builder.CreateBitCast(enumAddr,
                               payloadI->ti->getStorageType()->getPointerTo());
    }

  private:
    void storePayloadTag(IRGenFunction &IGF,
                         Address enumAddr, unsigned index) const {
      // If the tag has spare bits, we need to mask them into the
      // payload area.
      unsigned numSpareBits = PayloadTagBits.count();
      if (numSpareBits > 0) {
        unsigned spareTagBits = numSpareBits >= 32
          ? index : index & ((1U << numSpareBits) - 1U);

        // Mask the spare bits into the payload area.
        assert(payloadTy && "spare bits with empty payload?!");
        Address payloadAddr = projectPayload(IGF, enumAddr);
        llvm::Value *payloadBits = IGF.Builder.CreateLoad(payloadAddr);
        auto *spareBitMask = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                      ~getAPIntFromBitVector(PayloadTagBits));
        APInt tagBitMaskVal
          = interleaveSpareBits(IGF.IGM, PayloadTagBits, PayloadTagBits.size(),
                                spareTagBits, 0);
        auto tagBitMask
          = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(), tagBitMaskVal);
        
        payloadBits = IGF.Builder.CreateAnd(payloadBits, spareBitMask);
        if (spareTagBits != 0)
          payloadBits = IGF.Builder.CreateOr(payloadBits, tagBitMask);
        IGF.Builder.CreateStore(payloadBits, payloadAddr);
      }

      // Initialize the extra tag bits, if we have them.
      if (ExtraTagBitCount > 0) {
        unsigned extraTagBits = index >> numSpareBits;
        auto *extraTagValue = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(),
                                        APInt(ExtraTagBitCount, extraTagBits));
        IGF.Builder.CreateStore(extraTagValue,
                                projectExtraTagBits(IGF, enumAddr));
      }
    }
    
    void storeNoPayloadTag(IRGenFunction &IGF, Address enumAddr,
                           unsigned index) const {
      // We can just primitive-store the representation for the empty case.
      llvm::Value *payload, *extraTag;
      std::tie(payload, extraTag) = getNoPayloadCaseValue(IGF.IGM, index);
      if (payloadTy)
        IGF.Builder.CreateStore(payload, projectPayload(IGF, enumAddr));
      if (ExtraTagBitCount > 0) {
        assert(extraTag);
        IGF.Builder.CreateStore(extraTag, projectExtraTagBits(IGF, enumAddr));
      }
    }
    
  public:
    void storeTag(IRGenFunction &IGF,
                  EnumElementDecl *elt,
                  Address enumAddr,
                  CanType T) const override {
      // See whether this is a payload or empty case we're emitting.
      auto payloadI = std::find_if(ElementsWithPayload.begin(),
                                   ElementsWithPayload.end(),
                               [&](const Element &e) { return e.decl == elt; });
      if (payloadI != ElementsWithPayload.end())
        return storePayloadTag(IGF,
                               enumAddr,
                               payloadI - ElementsWithPayload.begin());
      
      auto emptyI = std::find_if(ElementsWithNoPayload.begin(),
                                 ElementsWithNoPayload.end(),
                               [&](const Element &e) { return e.decl == elt; });
      assert(emptyI != ElementsWithNoPayload.end() && "case not in enum");
      storeNoPayloadTag(IGF, enumAddr, emptyI - ElementsWithNoPayload.begin());
    }
    
    Address destructiveProjectDataForLoad(IRGenFunction &IGF,
                                          EnumElementDecl *elt,
                                          Address enumAddr) const override {
      auto payloadI = std::find_if(ElementsWithPayload.begin(),
                               ElementsWithPayload.end(),
                               [&](const Element &e) { return e.decl == elt; });
      
      assert(payloadI != ElementsWithPayload.end() &&
             "cannot project a no-payload case");
      
      unsigned index = payloadI - ElementsWithPayload.begin();
      
      preparePayloadForLoad(IGF, enumAddr, index);
      
      // Payloads are all placed at the beginning of the value.
      return IGF.Builder.CreateBitCast(enumAddr,
                               payloadI->ti->getStorageType()->getPointerTo());
    }

    void initializeMetadata(IRGenFunction &IGF,
                            llvm::Value *metadata,
                            llvm::Value *vwtable,
                            CanType T) const override {
      // FIXME
    }
    
    /// \group Extra inhabitants
    
    // TODO
    
    bool mayHaveExtraInhabitants(IRGenModule &) const override { return false; }
    
    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                         Address src,
                                         CanType T) const override {
      llvm_unreachable("extra inhabitants for multi-payload enums not implemented");
    }
    
    void storeExtraInhabitant(IRGenFunction &IGF,
                              llvm::Value *index,
                              Address dest,
                              CanType T) const override {
      llvm_unreachable("extra inhabitants for multi-payload enums not implemented");
    }
    
    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return 0;
    }
    
    llvm::ConstantInt *
    getFixedExtraInhabitantValue(IRGenModule &IGM,
                                 unsigned bits,
                                 unsigned index) const override {
      llvm_unreachable("extra inhabitants for multi-payload enums not implemented");
    }
    
    llvm::BitVector
    getBitPatternForNoPayloadElement(IRGenModule &IGM,
                                     EnumElementDecl *theCase) const override {
      llvm::ConstantInt *payloadPart, *extraPart;
      
      auto emptyI = std::find_if(ElementsWithNoPayload.begin(),
                                 ElementsWithNoPayload.end(),
                           [&](const Element &e) { return e.decl == theCase; });
      assert(emptyI != ElementsWithNoPayload.end() && "case not in enum");

      unsigned index = emptyI - ElementsWithNoPayload.begin();
      
      std::tie(payloadPart, extraPart) = getNoPayloadCaseValue(IGM, index);
      llvm::BitVector bits = getBitVectorFromAPInt(payloadPart->getValue());
      
      unsigned totalSize
        = cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits();
      if (extraPart) {
        llvm::BitVector extraBits = getBitVectorFromAPInt(extraPart->getValue(),
                                                          bits.size());
        bits.resize(totalSize);
        extraBits.resize(totalSize);
        bits |= extraBits;
      } else {
        assert(totalSize == bits.size());
      }
      return bits;
    }

    llvm::BitVector getTagBitsForPayloads(IRGenModule &IGM) const override {
      llvm::BitVector result = PayloadTagBits;
      
      unsigned totalSize
        = cast<FixedTypeInfo>(TI)->getFixedSize().getValueInBits();
      
      if (ExtraTagBitCount) {
        result.resize(PayloadTagBits.size() + ExtraTagBitCount, true);
        result.resize(totalSize, false);
      } else {
        assert(PayloadTagBits.size() == totalSize);
      }
      return result;
    }
  };
} // end anonymous namespace

EnumImplStrategy *EnumImplStrategy::get(TypeConverter &TC,
                                          CanType type,
                                          EnumDecl *theEnum)
{
  unsigned numElements = 0;
  TypeInfoKind tik = Loadable;
  std::vector<Element> elementsWithPayload;
  std::vector<Element> elementsWithRecursivePayload;
  std::vector<Element> elementsWithNoPayload;
  
  for (auto elt : theEnum->getAllElements()) {
    numElements++;
    
    // Compute whether this gives us an apparent payload or dynamic layout.
    // Note that we do *not* apply substitutions from a bound generic instance
    // yet. We want all instances of a generic enum to share an implementation
    // strategy.
    Type argType = elt->getArgumentType();
    if (argType.isNull()) {
      elementsWithNoPayload.push_back({elt, nullptr});
      continue;
    }
    auto argLoweredTy = TC.IGM.SILMod->Types.getLoweredType(argType);
    auto *argTI = TC.tryGetCompleteTypeInfo(argLoweredTy.getSwiftRValueType());
    if (!argTI) {
      elementsWithRecursivePayload.push_back({elt, nullptr});
      continue;
    }

    auto loadableArgTI = dyn_cast<LoadableTypeInfo>(argTI);
    if (loadableArgTI
        && loadableArgTI->getExplosionSize(ResilienceExpansion::Minimal) == 0) {
      elementsWithNoPayload.push_back({elt, nullptr});
    } else {
      // *Now* apply the substitutions and get the type info for the instance's
      // payload type, since we know this case carries an apparent payload in
      // the generic case.
      auto *substArgTI = argTI;
      if (type->is<BoundGenericType>()) {
        Type origArgTy = elt->getArgumentType();
        Type substArgTy = type->getTypeOfMember(theEnum->getModuleContext(),
                                                elt, nullptr,
                                                origArgTy);
        substArgTI = &TC.IGM.getTypeInfoForUnlowered(AbstractionPattern(origArgTy),
                                                     substArgTy);
      }
      
      elementsWithPayload.push_back({elt, substArgTI});
      if (!substArgTI->isFixedSize())
        tik = Opaque;
      else if (!substArgTI->isLoadable() && tik > Fixed)
        tik = Fixed;
    }
  }

  // FIXME recursive enums
  if (!elementsWithRecursivePayload.empty()) {
    TC.IGM.fatal_unimplemented(theEnum->getLoc(), "recursive enum layout");
  }
  
  assert(numElements == elementsWithPayload.size()
           + elementsWithRecursivePayload.size()
           + elementsWithNoPayload.size()
         && "not all elements accounted for");
  
  // Enums from Clang use C-compatible layout.
  if (theEnum->hasClangNode()) {
    assert(elementsWithPayload.size() == 0 && "C enum with payload?!");
    return new CCompatibleEnumImplStrategy(TC.IGM, tik, numElements,
                                       std::move(elementsWithPayload),
                                       std::move(elementsWithRecursivePayload),
                                       std::move(elementsWithNoPayload));
  }
  
  if (numElements <= 1)
    return new SingletonEnumImplStrategy(TC.IGM, tik, numElements,
                                    std::move(elementsWithPayload),
                                    std::move(elementsWithRecursivePayload),
                                    std::move(elementsWithNoPayload));
  if (elementsWithPayload.size() > 1)
    return new MultiPayloadEnumImplStrategy(TC.IGM, tik, numElements,
                                    std::move(elementsWithPayload),
                                    std::move(elementsWithRecursivePayload),
                                    std::move(elementsWithNoPayload));
  if (elementsWithPayload.size() == 1)
    return new SinglePayloadEnumImplStrategy(TC.IGM, tik, numElements,
                                    std::move(elementsWithPayload),
                                    std::move(elementsWithRecursivePayload),
                                    std::move(elementsWithNoPayload));

  return new NoPayloadEnumImplStrategy(TC.IGM, tik, numElements,
                                      std::move(elementsWithPayload),
                                      std::move(elementsWithRecursivePayload),
                                      std::move(elementsWithNoPayload));
}

namespace {
  /// Common base template for enum type infos.
  template<typename BaseTypeInfo>
  class EnumTypeInfoBase : public BaseTypeInfo {
  public:
    EnumImplStrategy &Strategy;
    
    template<typename...AA>
    EnumTypeInfoBase(EnumImplStrategy &strategy, AA &&...args)
      : BaseTypeInfo(std::forward<AA>(args)...), Strategy(strategy) {}
    
    llvm::StructType *getStorageType() const {
      return cast<llvm::StructType>(TypeInfo::getStorageType());
    }
    
    /// \group Methods delegated to the EnumImplStrategy
    
    void getSchema(ExplosionSchema &s) const override {
      return Strategy.getSchema(s);
    }
    void destroy(IRGenFunction &IGF, Address addr, CanType T) const override {
      return Strategy.destroy(IGF, addr, T);
    }
    bool isIndirectArgument(ResilienceExpansion kind) const override {
      return Strategy.isIndirectArgument(kind);
    }
    void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                              Address dest, CanType T) const override {
      return Strategy.initializeFromParams(IGF, params, dest, T);
    }
    void initializeWithCopy(IRGenFunction &IGF, Address dest,
                            Address src, CanType T) const override {
      return Strategy.initializeWithCopy(IGF, dest, src, T);
    }
    void initializeWithTake(IRGenFunction &IGF, Address dest,
                            Address src, CanType T) const override {
      return Strategy.initializeWithTake(IGF, dest, src, T);
    }
    void assignWithCopy(IRGenFunction &IGF, Address dest,
                        Address src, CanType T) const override {
      return Strategy.assignWithCopy(IGF, dest, src, T);
    }
    void assignWithTake(IRGenFunction &IGF, Address dest,
                        Address src, CanType T) const override {
      return Strategy.assignWithTake(IGF, dest, src, T);
    }
    void initializeMetadata(IRGenFunction &IGF,
                            llvm::Value *metadata,
                            llvm::Value *vwtable,
                            CanType T) const override {
      return Strategy.initializeMetadata(IGF, metadata, vwtable, T);
    }
    bool mayHaveExtraInhabitants(IRGenModule &IGM) const override {
      return Strategy.mayHaveExtraInhabitants(IGM);
    }
    llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                         Address src,
                                         CanType T) const override {
      return Strategy.getExtraInhabitantIndex(IGF, src, T);
    }
    void storeExtraInhabitant(IRGenFunction &IGF,
                              llvm::Value *index,
                              Address dest,
                              CanType T) const override {
      return Strategy.storeExtraInhabitant(IGF, index, dest, T);
    }
  };
  
  /// TypeInfo for fixed-layout, address-only enum types.
  class FixedEnumTypeInfo : public EnumTypeInfoBase<FixedTypeInfo> {
  public:
    FixedEnumTypeInfo(EnumImplStrategy &strategy,
                       llvm::StructType *T, Size S, llvm::BitVector SB,
                       Alignment A, IsPOD_t isPOD, IsBitwiseTakable_t isBT)
      : EnumTypeInfoBase(strategy, T, S, std::move(SB), A, isPOD, isBT) {}
    
    /// \group Methods delegated to the EnumImplStrategy

    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return Strategy.getFixedExtraInhabitantCount(IGM);
    }

    llvm::ConstantInt *getFixedExtraInhabitantValue(IRGenModule &IGM,
                                                    unsigned bits,
                                                    unsigned index)
    const override {
      return Strategy.getFixedExtraInhabitantValue(IGM, bits, index);
    }
  };

  /// TypeInfo for loadable enum types.
  class LoadableEnumTypeInfo : public EnumTypeInfoBase<LoadableTypeInfo> {
  public:
    // FIXME: Derive spare bits from element layout.
    LoadableEnumTypeInfo(EnumImplStrategy &strategy,
                          llvm::StructType *T, Size S, llvm::BitVector SB,
                          Alignment A, IsPOD_t isPOD)
      : EnumTypeInfoBase(strategy, T, S, std::move(SB), A, isPOD) {}

    unsigned getExplosionSize(ResilienceExpansion kind) const override {
      return Strategy.getExplosionSize(kind);
    }
    void loadAsCopy(IRGenFunction &IGF, Address addr,
                    Explosion &e) const override {
      return Strategy.loadAsCopy(IGF, addr, e);
    }
    void loadAsTake(IRGenFunction &IGF, Address addr,
                    Explosion &e) const override {
      return Strategy.loadAsTake(IGF, addr, e);
    }
    void assign(IRGenFunction &IGF, Explosion &e,
                Address addr) const override {
      return Strategy.assign(IGF, e, addr);
    }
    void initialize(IRGenFunction &IGF, Explosion &e,
                    Address addr) const override {
      return Strategy.initialize(IGF, e, addr);
    }
    void reexplode(IRGenFunction &IGF, Explosion &src,
                   Explosion &dest) const override {
      return Strategy.reexplode(IGF, src, dest);
    }
    void copy(IRGenFunction &IGF, Explosion &src,
              Explosion &dest) const override {
      return Strategy.copy(IGF, src, dest);
    }
    void consume(IRGenFunction &IGF, Explosion &src) const override {
      return Strategy.consume(IGF, src);
    }
    llvm::Value *packEnumPayload(IRGenFunction &IGF,
                                  Explosion &in,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      return Strategy.packEnumPayload(IGF, in, bitWidth, offset);
    }
    void unpackEnumPayload(IRGenFunction &IGF,
                            llvm::Value *payload,
                            Explosion &dest,
                            unsigned offset) const override {
      return Strategy.unpackEnumPayload(IGF, payload, dest, offset);
    }
    unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const override {
      return Strategy.getFixedExtraInhabitantCount(IGM);
    }
    
    llvm::ConstantInt *getFixedExtraInhabitantValue(IRGenModule &IGM,
                                                    unsigned bits,
                                                    unsigned index)
    const override {
      return Strategy.getFixedExtraInhabitantValue(IGM, bits, index);
    }
  };
  
  /// TypeInfo for dynamically-sized enum types.
  class NonFixedEnumTypeInfo
    : public EnumTypeInfoBase<WitnessSizedTypeInfo<NonFixedEnumTypeInfo>>
  {
  public:
    NonFixedEnumTypeInfo(EnumImplStrategy &strategy,
                         llvm::Type *irTy,
                         Alignment align,
                         IsPOD_t pod,
                         IsBitwiseTakable_t bt)
      : EnumTypeInfoBase(strategy, irTy, align, pod, bt) {}
  };
} // end anonymous namespace

const EnumImplStrategy &
irgen::getEnumImplStrategy(IRGenModule &IGM, CanType ty) {
  assert(ty->getEnumOrBoundGenericEnum() && "not an enum");
  auto *ti = &IGM.getTypeInfoForLowered(ty);
  if (auto *loadableTI = dyn_cast<LoadableTypeInfo>(ti))
    return loadableTI->as<LoadableEnumTypeInfo>().Strategy;
  if (auto *fti = dyn_cast<FixedTypeInfo>(ti))
    return fti->as<FixedEnumTypeInfo>().Strategy;
  return ti->as<NonFixedEnumTypeInfo>().Strategy;
}

const EnumImplStrategy &
irgen::getEnumImplStrategy(IRGenModule &IGM, SILType ty) {
  return getEnumImplStrategy(IGM, ty.getSwiftRValueType());
}

TypeInfo *
EnumImplStrategy::getFixedEnumTypeInfo(llvm::StructType *T, Size S,
                                    llvm::BitVector SB,
                                    Alignment A, IsPOD_t isPOD,
                                    IsBitwiseTakable_t isBT) {
  TypeInfo *mutableTI;
  switch (TIK) {
  case Opaque:
    llvm_unreachable("not valid");
  case Fixed:
    mutableTI = new FixedEnumTypeInfo(*this, T, S, SB, A, isPOD, isBT);
    break;
  case Loadable:
    assert(isBT && "loadable enum not bitwise takable?!");
    mutableTI = new LoadableEnumTypeInfo(*this, T, S, SB, A, isPOD);
    break;
  }
  TI = mutableTI;
  return mutableTI;
}

namespace {
  TypeInfo *
  SingletonEnumImplStrategy::completeEnumTypeLayout(TypeConverter &TC,
                                                   CanType type,
                                                   EnumDecl *theEnum,
                                                   llvm::StructType *enumTy) {
    if (ElementsWithPayload.empty()) {
      enumTy->setBody(ArrayRef<llvm::Type*>{}, /*isPacked*/ true);
      return registerEnumTypeInfo(new LoadableEnumTypeInfo(*this, enumTy,
                                                             Size(0), {},
                                                             Alignment(1),
                                                             IsPOD));
    } else {
      const TypeInfo &eltTI = *getSingleton();

      // Use the singleton element's storage type if fixed-size.
      if (eltTI.isFixedSize()) {
        llvm::Type *body[] = { eltTI.StorageType };
        enumTy->setBody(body, /*isPacked*/ true);
      } else {
        enumTy->setBody(ArrayRef<llvm::Type*>{}, /*isPacked*/ true);
      }
      
      if (TIK <= Opaque) {
        return registerEnumTypeInfo(new NonFixedEnumTypeInfo(*this, enumTy,
                               eltTI.getBestKnownAlignment(),
                               eltTI.isPOD(ResilienceScope::Local),
                               eltTI.isBitwiseTakable(ResilienceScope::Local)));
      } else {
        auto &fixedEltTI = cast<FixedTypeInfo>(eltTI);
        return getFixedEnumTypeInfo(enumTy,
                          fixedEltTI.getFixedSize(),
                          fixedEltTI.getSpareBits(),
                          fixedEltTI.getFixedAlignment(),
                          fixedEltTI.isPOD(ResilienceScope::Local),
                          fixedEltTI.isBitwiseTakable(ResilienceScope::Local));
      }
    }
  }
  
  TypeInfo *
  NoPayloadEnumImplStrategy::completeEnumTypeLayout(TypeConverter &TC,
                                                    CanType type,
                                                    EnumDecl *theEnum,
                                                    llvm::StructType *enumTy) {
    // Since there are no payloads, we need just enough bits to hold a
    // discriminator.
    unsigned tagBits = llvm::Log2_32(ElementsWithNoPayload.size() - 1) + 1;
    auto tagTy = llvm::IntegerType::get(TC.IGM.getLLVMContext(), tagBits);
    // Round the physical size up to the next power of two.
    unsigned tagBytes = (tagBits + 7U)/8U;
    if (!llvm::isPowerOf2_32(tagBytes))
      tagBytes = llvm::NextPowerOf2(tagBytes);
    Size tagSize(tagBytes);
    
    llvm::Type *body[] = { tagTy };
    enumTy->setBody(body, /*isPacked*/true);
    
    // Unused tag bits in the physical size can be used as spare bits.
    // TODO: We can use all values greater than the largest discriminator as
    // extra inhabitants, not just those made available by spare bits.
    llvm::BitVector spareBits(tagBits, false);
    spareBits.resize(tagSize.getValueInBits(), true);
    
    return registerEnumTypeInfo(new LoadableEnumTypeInfo(*this,
                                     enumTy, tagSize, std::move(spareBits),
                                     Alignment(tagBytes), IsPOD));
  }
  
  TypeInfo *
  CCompatibleEnumImplStrategy::completeEnumTypeLayout(TypeConverter &TC,
                                                      CanType type,
                                                      EnumDecl *theEnum,
                                                      llvm::StructType *enumTy){
    // The type should have come from Clang and should have a raw type.
    assert(theEnum->hasClangNode()
           && "c-compatible enum didn't come from clang!");
    assert(theEnum->hasRawType()
           && "c-compatible enum doesn't have raw type!");
    assert(!theEnum->getDeclaredTypeInContext()->is<BoundGenericType>()
           && "c-compatible enum is generic!");
    
    // The raw type should be a C integer type, which should have a single
    // scalar representation as a Swift struct. We'll use that same
    // representation type for the enum so that it's ABI-compatible.
    auto &rawTI = TC.getCompleteTypeInfo(
                                     theEnum->getRawType()->getCanonicalType());
    auto &rawFixedTI = cast<FixedTypeInfo>(rawTI);
    assert(rawFixedTI.isPOD(ResilienceScope::Component)
           && "c-compatible raw type isn't POD?!");
    ExplosionSchema rawSchema = rawTI.getSchema(ResilienceExpansion::Minimal);
    assert(rawSchema.size() == 1
           && "c-compatible raw type has non-single-scalar representation?!");
    assert(rawSchema.begin()[0].isScalar()
           && "c-compatible raw type has non-single-scalar representation?!");
    llvm::Type *tagTy = rawSchema.begin()[0].getScalarType();
    
    llvm::Type *body[] = { tagTy };
    enumTy->setBody(body, /*isPacked*/ false);
    
    return registerEnumTypeInfo(new LoadableEnumTypeInfo(*this, enumTy,
                                                 rawFixedTI.getFixedSize(),
                                                 rawFixedTI.getSpareBits(),
                                                 rawFixedTI.getFixedAlignment(),
                                                 IsPOD));
  }
  
  TypeInfo *SinglePayloadEnumImplStrategy::completeFixedLayout(
                                      TypeConverter &TC,
                                      CanType type,
                                      EnumDecl *theEnum,
                                      llvm::StructType *enumTy) {
    // See whether the payload case's type has extra inhabitants.
    unsigned fixedExtraInhabitants = 0;
    unsigned numTags = ElementsWithNoPayload.size();
    
    auto &payloadTI = getFixedPayloadTypeInfo(); // FIXME non-fixed payload
    fixedExtraInhabitants = payloadTI.getFixedExtraInhabitantCount(TC.IGM);
    
    // Determine how many tag bits we need. Given N extra inhabitants, we
    // represent the first N tags using those inhabitants. For additional tags,
    // we use discriminator bit(s) to inhabit the full bit size of the payload.
    NumExtraInhabitantTagValues = std::min(numTags, fixedExtraInhabitants);
    
    unsigned tagsWithoutInhabitants = numTags - NumExtraInhabitantTagValues;    
    if (tagsWithoutInhabitants == 0) {
      ExtraTagBitCount = 0;
      NumExtraTagValues = 0;
    // If the payload size is greater than 32 bits, the calculation would
    // overflow, but one tag bit should suffice. if you have more than 2^32
    // enum discriminators you have other problems.
    } else if (payloadTI.getFixedSize().getValue() >= 4) {
      ExtraTagBitCount = 1;
      NumExtraTagValues = 2;
    } else {
      unsigned tagsPerTagBitValue =
        1 << payloadTI.getFixedSize().getValueInBits();
      NumExtraTagValues
        = (tagsWithoutInhabitants+(tagsPerTagBitValue-1))/tagsPerTagBitValue+1;
      ExtraTagBitCount = llvm::Log2_32(NumExtraTagValues-1) + 1;
    }

    // Create the body type.
    setTaggedEnumBody(TC.IGM, enumTy,
                       payloadTI.getFixedSize().getValueInBits(),
                       ExtraTagBitCount);
    
    // The enum has the alignment of the payload. The size includes the added
    // tag bits.
    auto sizeWithTag = payloadTI.getFixedSize().getValue();
    unsigned extraTagByteCount = (ExtraTagBitCount+7U)/8U;
    sizeWithTag += extraTagByteCount;
    
    // FIXME: We don't have enough semantic understanding of extra inhabitant
    // sets to be able to reason about how many spare bits from the payload type
    // we can forward. If we spilled tag bits, however, we can offer the unused
    // bits we have in that byte.
    llvm::BitVector spareBits;
    if (ExtraTagBitCount > 0 && ExtraTagBitCount < extraTagByteCount * 8) {
      spareBits.resize(payloadTI.getFixedSize().getValueInBits()
                         + ExtraTagBitCount,
                       false);
      spareBits.resize(payloadTI.getFixedSize().getValueInBits()
                         + extraTagByteCount * 8,
                       true);
    }
    return getFixedEnumTypeInfo(enumTy, Size(sizeWithTag), std::move(spareBits),
                        payloadTI.getFixedAlignment(),
                        payloadTI.isPOD(ResilienceScope::Component),
                        payloadTI.isBitwiseTakable(ResilienceScope::Component));
  }
  
  TypeInfo *SinglePayloadEnumImplStrategy::completeDynamicLayout(
                                                  TypeConverter &TC,
                                                  CanType type,
                                                  EnumDecl *theEnum,
                                                  llvm::StructType *enumTy) {
    // The body is runtime-dependent, so we can't put anything useful here
    // statically.
    enumTy->setBody(ArrayRef<llvm::Type*>{}, /*isPacked*/true);

    // Layout has to be done when the value witness table is instantiated,
    // during initializeMetadata.
    return registerEnumTypeInfo(new NonFixedEnumTypeInfo(*this, enumTy,
           getPayloadTypeInfo().getBestKnownAlignment(),
           getPayloadTypeInfo().isPOD(ResilienceScope::Component),
           getPayloadTypeInfo().isBitwiseTakable(ResilienceScope::Component)));
  }
  
  TypeInfo *
  SinglePayloadEnumImplStrategy::completeEnumTypeLayout(TypeConverter &TC,
                                                  CanType type,
                                                  EnumDecl *theEnum,
                                                  llvm::StructType *enumTy) {
    if (TIK >= Fixed)
      return completeFixedLayout(TC, type, theEnum, enumTy);
    return completeDynamicLayout(TC, type, theEnum, enumTy);
  }
  
  TypeInfo *
  MultiPayloadEnumImplStrategy::completeEnumTypeLayout(TypeConverter &TC,
                                                  CanType type,
                                                  EnumDecl *theEnum,
                                                  llvm::StructType *enumTy) {
    // TODO Dynamic layout for multi-payload enums.
    if (!TC.IGM.Opts.EnableDynamicValueTypeLayout && TIK < Fixed) {
      TC.IGM.fatal_unimplemented(theEnum->getLoc(),
                                 "non-fixed multi-payload enum layout");
    }

    // We need tags for each of the payload types, which we may be able to form
    // using spare bits, plus a minimal number of tags with which we can
    // represent the empty cases.
    unsigned numPayloadTags = ElementsWithPayload.size();
    unsigned numEmptyElements = ElementsWithNoPayload.size();
    
    // See if the payload types have any spare bits in common.
    // At the end of the loop CommonSpareBits.size() will be the size (in bits)
    // of the largest payload.
    CommonSpareBits = {};
    Alignment worstAlignment(1);
    IsPOD_t isPOD = IsPOD;
    IsBitwiseTakable_t isBT = IsBitwiseTakable;
    for (auto &elt : ElementsWithPayload) {
      auto &fixedPayloadTI = cast<FixedTypeInfo>(*elt.ti); // FIXME
      if (fixedPayloadTI.getFixedAlignment() > worstAlignment)
        worstAlignment = fixedPayloadTI.getFixedAlignment();
      if (!fixedPayloadTI.isPOD(ResilienceScope::Component))
        isPOD = IsNotPOD;
      if (!fixedPayloadTI.isBitwiseTakable(ResilienceScope::Component))
        isBT = IsNotBitwiseTakable;

      // As a hack, if the payload type is generic, don't use any spare bits
      // from it, even if our concrete instance has them. We can't support
      // runtime-dependent spare bits yet. There's a corresponding hack in
      // TypeConverter::convertArchetypeType to give class archetypes no
      // spare bits.
      if (elt.decl->getInterfaceType()->isDependentType())
        CommonSpareBits.reset(0, fixedPayloadTI.getFixedSize().getValueInBits());
      else
        fixedPayloadTI.applyFixedSpareBitsMask(CommonSpareBits);
    }
    
    unsigned commonSpareBitCount = CommonSpareBits.count();
    unsigned usedBitCount = CommonSpareBits.size() - commonSpareBitCount;
    
    // Determine how many tags we need to accommodate the empty cases, if any.
    if (ElementsWithNoPayload.empty()) {
      NumEmptyElementTags = 0;
    } else {
      // We can store tags for the empty elements using the inhabited bits with
      // their own tag(s).
      if (usedBitCount >= 32) {
        NumEmptyElementTags = 1;
      } else {
        unsigned emptyElementsPerTag = 1 << usedBitCount;
        NumEmptyElementTags
          = (numEmptyElements + (emptyElementsPerTag-1))/emptyElementsPerTag;
      }
    }
    
    unsigned numTags = numPayloadTags + NumEmptyElementTags;
    unsigned numTagBits = llvm::Log2_32(numTags-1) + 1;
    ExtraTagBitCount = numTagBits <= commonSpareBitCount
      ? 0 : numTagBits - commonSpareBitCount;
    NumExtraTagValues = numTags >> commonSpareBitCount;
    
    // Create the type. We need enough bits to store the largest payload plus
    // extra tag bits we need.
    setTaggedEnumBody(TC.IGM, enumTy,
                       CommonSpareBits.size(),
                       ExtraTagBitCount);
    
    // The enum has the worst alignment of its payloads. The size includes the
    // added tag bits.
    auto sizeWithTag = (CommonSpareBits.size() + 7U)/8U;
    unsigned extraTagByteCount = (ExtraTagBitCount+7U)/8U;
    sizeWithTag += extraTagByteCount;
    
    // Determine tag bits.
    llvm::BitVector spareBits;
    // We may have bits left over that we didn't use in the payload.
    if (numTagBits < commonSpareBitCount) {
      assert(ExtraTagBitCount == 0
             && "spilled extra tag bits with spare bits available?!");
      spareBits = CommonSpareBits;
      PayloadTagBits.resize(CommonSpareBits.size());
      // Mark the bits we'll use as occupied. Take bits starting from the most
      // significant.
      for (unsigned i = 0, bit = CommonSpareBits.size() - 1;
           i < numTagBits; ++i) {
        while (!CommonSpareBits[bit]) {
          assert(bit > 0 && "ran out of spare bits?!");
          --bit;
        }
        spareBits[bit] = false;
        PayloadTagBits[bit] = true;
        --bit;
      }
      assert(PayloadTagBits.count() == numTagBits);
    // If we spilled into extra tag bits, there may be spare bits in that
    // byte.
    } else {
      PayloadTagBits = CommonSpareBits;
      if (ExtraTagBitCount < extraTagByteCount*8) {
        spareBits.resize(CommonSpareBits.size() + ExtraTagBitCount, false);
        spareBits.resize(CommonSpareBits.size() + extraTagByteCount*8, true);
      }
    }
    return getFixedEnumTypeInfo(enumTy, Size(sizeWithTag), std::move(spareBits),
                                worstAlignment, isPOD, isBT);
  }
}

const TypeInfo *TypeConverter::convertEnumType(TypeBase *key, CanType type,
                                               EnumDecl *theEnum) {
  llvm::StructType *convertedStruct = IGM.createNominalType(theEnum);

  // Create a forward declaration for that type.
  addForwardDecl(key, convertedStruct);

  // Determine the implementation strategy.
  EnumImplStrategy *strategy = EnumImplStrategy::get(*this, type, theEnum);

  // Create the TI.
  auto *ti = strategy->completeEnumTypeLayout(*this, type,
                                              theEnum, convertedStruct);
  // Assert that the layout query functions for fixed-layout enums work, for
  // LLDB's sake.
#ifndef NDEBUG
  if (auto fixedTI = dyn_cast<FixedTypeInfo>(ti)) {
    DEBUG(llvm::dbgs() << "Layout for enum ";
          type->print(llvm::dbgs());
          llvm::dbgs() << ":\n";);

    llvm::BitVector spareBits;
    fixedTI->applyFixedSpareBitsMask(spareBits);
    
    for (auto &elt : strategy->getElementsWithNoPayload()) {
      auto bitPattern = strategy->getBitPatternForNoPayloadElement(IGM, elt.decl);
      assert(bitPattern.size() == fixedTI->getFixedSize().getValueInBits());
      DEBUG(llvm::dbgs() << "  no-payload case " << elt.decl->getName().str()
                         << ":\t";
            for (unsigned i = bitPattern.size(); i-- > 0;) {
              llvm::dbgs() << (bitPattern[i] ? '1' : '0');
            }
            llvm::dbgs() << '\n');
      
      bitPattern &= spareBits;
      assert(bitPattern.none() && "no-payload case occupies spare bits?!");
    }
    auto tagBits = strategy->getTagBitsForPayloads(IGM);
    assert(tagBits.count() >= 32
            || (1U << tagBits.count())
               >= strategy->getElementsWithPayload().size());
    DEBUG(llvm::dbgs() << "  payload tag bits:\t";
          for (unsigned i = tagBits.size(); i-- > 0;) {
            llvm::dbgs() << (tagBits[i] ? '1' : '0');
          }
          llvm::dbgs() << '\n');
    
    tagBits &= spareBits;
    assert(tagBits.none() && "tag bits overlap spare bits?!");
  }
#endif
  return ti;
}

/// emitEnumDecl - Emit all the declarations associated with this enum type.
void IRGenModule::emitEnumDecl(EnumDecl *theEnum) {
  emitEnumMetadata(*this, theEnum);
  
  // FIXME: This is mostly copy-paste from emitExtension;
  // figure out how to refactor! 
  for (Decl *member : theEnum->getMembers()) {
    switch (member->getKind()) {
    case DeclKind::Import:
    case DeclKind::TopLevelCode:
    case DeclKind::Protocol:
    case DeclKind::Extension:
    case DeclKind::Destructor:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
    case DeclKind::Param:
      llvm_unreachable("decl not allowed in enum!");

    // We can't have meaningful initializers for variables; these just show
    // up as part of parsing properties.
    case DeclKind::PatternBinding:
      continue;

    // Active members of the IfConfig block are handled separately.
    case DeclKind::IfConfig:
       continue;

    case DeclKind::Subscript:
      // Getter/setter will be handled separately.
      continue;
    case DeclKind::TypeAlias:
    case DeclKind::AssociatedType:
    case DeclKind::GenericTypeParam:
      continue;
    case DeclKind::Enum:
      emitEnumDecl(cast<EnumDecl>(member));
      continue;
    case DeclKind::Struct:
      emitStructDecl(cast<StructDecl>(member));
      continue;
    case DeclKind::Class:
      emitClassDecl(cast<ClassDecl>(member));
      continue;
    case DeclKind::Var:
      if (!cast<VarDecl>(member)->hasStorage())
        // Getter/setter will be handled separately.
        continue;
      continue;
    case DeclKind::Func:
      emitLocalDecls(cast<FuncDecl>(member));
      continue;
    case DeclKind::Constructor:
      emitLocalDecls(cast<ConstructorDecl>(member));
      continue;
        
    case DeclKind::EnumCase:
    case DeclKind::EnumElement:
      // Lowered in SIL.
      continue;
    }
    llvm_unreachable("bad extension member kind");
  }
}

// FIXME: PackEnumPayload and UnpackEnumPayload need to be endian-aware.

PackEnumPayload::PackEnumPayload(IRGenFunction &IGF, unsigned bitSize)
  : IGF(IGF), bitSize(bitSize)
{}

void PackEnumPayload::add(llvm::Value *v) {
  // First, bitcast to an integer type.
  if (isa<llvm::PointerType>(v->getType())) {
    v = IGF.Builder.CreatePtrToInt(v, IGF.IGM.SizeTy);
  } else if (!isa<llvm::IntegerType>(v->getType())) {
    unsigned bitSize = IGF.IGM.DataLayout.getTypeSizeInBits(v->getType());
    auto intTy = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), bitSize);
    v = IGF.Builder.CreateBitCast(v, intTy);
  }
  auto fromTy = cast<llvm::IntegerType>(v->getType());
  
  // If this was the first added value, use it to start our packed value.
  if (!packedValue) {
    // Zero-extend the integer value out to the value size.
    // FIXME: On big-endian, shift out to the value size.
    if (fromTy->getBitWidth() < bitSize) {
      auto toTy = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), bitSize);
      v = IGF.Builder.CreateZExt(v, toTy);
    }
    if (packedBits != 0)
      v = IGF.Builder.CreateShl(v, packedBits);
    packedBits += fromTy->getBitWidth();
    packedValue = v;
    return;
  }
  
  // Otherwise, shift and bitor the value into the existing value.
  v = IGF.Builder.CreateZExt(v, packedValue->getType());
  v = IGF.Builder.CreateShl(v, packedBits);
  packedBits += fromTy->getBitWidth();
  packedValue = IGF.Builder.CreateOr(packedValue, v);
}

void PackEnumPayload::combine(llvm::Value *v) {
  if (!packedValue)
    packedValue = v;
  else
    packedValue = IGF.Builder.CreateOr(packedValue, v);
}

llvm::Value *PackEnumPayload::get() {
  if (!packedValue)
    packedValue = getEmpty(IGF.IGM, bitSize);
  return packedValue;
}

llvm::Value *PackEnumPayload::getEmpty(IRGenModule &IGM, unsigned bitSize) {
  return llvm::ConstantInt::get(IGM.getLLVMContext(), APInt(bitSize, 0));
}

UnpackEnumPayload::UnpackEnumPayload(IRGenFunction &IGF,
                                       llvm::Value *packedValue)
  : IGF(IGF), packedValue(packedValue)
{}

llvm::Value *UnpackEnumPayload::claim(llvm::Type *ty) {
  // Mask out the bits for the value.
  unsigned bitSize = IGF.IGM.DataLayout.getTypeSizeInBits(ty);
  auto bitTy = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), bitSize);
  llvm::Value *unpacked = unpackedBits == 0
    ? packedValue
    : IGF.Builder.CreateLShr(packedValue, unpackedBits);
  if (bitSize < cast<llvm::IntegerType>(packedValue->getType())->getBitWidth())
    unpacked = IGF.Builder.CreateTrunc(unpacked, bitTy);

  unpackedBits += bitSize;

  // Bitcast to the destination type.
  if (isa<llvm::PointerType>(ty))
    return IGF.Builder.CreateIntToPtr(unpacked, ty);
  return IGF.Builder.CreateBitCast(unpacked, ty);
}

void irgen::emitSwitchLoadableEnumDispatch(IRGenFunction &IGF,
                                  SILType enumTy,
                                  Explosion &enumValue,
                                  ArrayRef<std::pair<EnumElementDecl *,
                                                     llvm::BasicBlock *>> dests,
                                  llvm::BasicBlock *defaultDest) {
  getEnumImplStrategy(IGF.IGM, enumTy)
    .emitValueSwitch(IGF, enumValue, dests, defaultDest);
}

void irgen::emitSwitchAddressOnlyEnumDispatch(IRGenFunction &IGF,
                                  SILType enumTy,
                                  Address enumAddr,
                                  ArrayRef<std::pair<EnumElementDecl *,
                                                     llvm::BasicBlock *>> dests,
                                  llvm::BasicBlock *defaultDest) {
  auto &strategy = getEnumImplStrategy(IGF.IGM, enumTy);
  strategy.emitIndirectSwitch(IGF, enumTy.getSwiftRValueType(),
                              enumAddr, dests, defaultDest);
}

void irgen::emitInjectLoadableEnum(IRGenFunction &IGF, SILType enumTy,
                                    EnumElementDecl *theCase,
                                    Explosion &data,
                                    Explosion &out) {
  getEnumImplStrategy(IGF.IGM, enumTy)
    .emitValueInjection(IGF, theCase, data, out);
}

void irgen::emitProjectLoadableEnum(IRGenFunction &IGF, SILType enumTy,
                                     Explosion &inEnumValue,
                                     EnumElementDecl *theCase,
                                     Explosion &out) {
  getEnumImplStrategy(IGF.IGM, enumTy)
    .emitValueProject(IGF, inEnumValue, theCase, out);
}

Address irgen::emitProjectEnumAddressForStore(IRGenFunction &IGF,
                                               SILType enumTy,
                                               Address enumAddr,
                                               EnumElementDecl *theCase) {
  return getEnumImplStrategy(IGF.IGM, enumTy)
    .projectDataForStore(IGF, theCase, enumAddr);
}

Address irgen::emitDestructiveProjectEnumAddressForLoad(IRGenFunction &IGF,
                                                   SILType enumTy,
                                                   Address enumAddr,
                                                   EnumElementDecl *theCase) {
  return getEnumImplStrategy(IGF.IGM, enumTy)
    .destructiveProjectDataForLoad(IGF, theCase, enumAddr);
}

void irgen::emitStoreEnumTagToAddress(IRGenFunction &IGF,
                                       SILType enumTy,
                                       Address enumAddr,
                                       EnumElementDecl *theCase) {
  getEnumImplStrategy(IGF.IGM, enumTy)
    .storeTag(IGF, theCase, enumAddr, enumTy.getSwiftRValueType());
}

APInt irgen::getAPIntFromBitVector(const llvm::BitVector &bits) {
  SmallVector<llvm::integerPart, 2> parts;
  
  for (unsigned i = 0; i < bits.size();) {
    llvm::integerPart part = 0UL;
    for (llvm::integerPart bit = 1; bit != 0 && i < bits.size();
         ++i, bit <<= 1) {
      if (bits[i])
        part |= bit;
    }
    parts.push_back(part);
  }
  
  return APInt(bits.size(), parts);
}

/// Gather spare bits into the low bits of a smaller integer value.
llvm::Value *irgen::emitGatherSpareBits(IRGenFunction &IGF,
                                        const llvm::BitVector &spareBitMask,
                                        llvm::Value *spareBits,
                                        unsigned resultLowBit,
                                        unsigned resultBitWidth) {
  auto destTy
    = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), resultBitWidth);
  unsigned usedBits = resultLowBit;
  llvm::Value *result = nullptr;
  
  for (int i = spareBitMask.find_first(); i != -1 && usedBits < resultBitWidth;
       i = spareBitMask.find_next(i)) {
    assert(i >= 0);
    unsigned u = i;
    assert(u >= (usedBits - resultLowBit) &&
           "used more bits than we've processed?!");
    
    // Shift the bits into place.
    llvm::Value *newBits;
    if (u > usedBits)
      newBits = IGF.Builder.CreateLShr(spareBits, u - usedBits);
    else if (u < usedBits)
      newBits = IGF.Builder.CreateShl(spareBits, usedBits - u);
    else
      newBits = spareBits;
    newBits = IGF.Builder.CreateZExtOrTrunc(newBits, destTy);
    
    // See how many consecutive bits we have.
    unsigned numBits = 1;
    ++u;
    // We don't need more bits than the size of the result.
    unsigned maxBits = resultBitWidth - usedBits;
    for (unsigned e = spareBitMask.size();
         u < e && numBits < maxBits && spareBitMask[u];
         ++u)
      ++numBits;
    
    // Mask out the selected bits.
    auto val = APInt::getAllOnesValue(numBits);
    if (numBits < resultBitWidth)
      val = val.zext(resultBitWidth);
    val = val.shl(usedBits);
    auto *mask = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(), val);
    newBits = IGF.Builder.CreateAnd(newBits, mask);
    
    // Accumulate the result.
    if (result)
      result = IGF.Builder.CreateOr(result, newBits);
    else
      result = newBits;
    
    usedBits += numBits;
    i = u;
  }
  
  return result;
}

/// Scatter spare bits from the low bits of an integer value.
llvm::Value *irgen::emitScatterSpareBits(IRGenFunction &IGF,
                                         const llvm::BitVector &spareBitMask,
                                         llvm::Value *packedBits,
                                         unsigned packedLowBit) {
  auto destTy
    = llvm::IntegerType::get(IGF.IGM.getLLVMContext(), spareBitMask.size());
  llvm::Value *result = nullptr;
  unsigned usedBits = packedLowBit;
  
  // Expand the packed bits to the destination type.
  packedBits = IGF.Builder.CreateZExtOrTrunc(packedBits, destTy);
  
  for (int i = spareBitMask.find_first(); i != -1;
       i = spareBitMask.find_next(i)) {
    assert(i >= 0);
    unsigned u = i, startBit = u;
    assert(u >= usedBits - packedLowBit
           && "used more bits than we've processed?!");
    
    // Shift the selected bits into place.
    llvm::Value *newBits;
    if (u > usedBits)
      newBits = IGF.Builder.CreateShl(packedBits, u - usedBits);
    else if (u < usedBits)
      newBits = IGF.Builder.CreateLShr(packedBits, usedBits - u);
    else
      newBits = packedBits;
    
    // See how many consecutive bits we have.
    unsigned numBits = 1;
    ++u;
    for (unsigned e = spareBitMask.size(); u < e && spareBitMask[u]; ++u)
      ++numBits;
    
    // Mask out the selected bits.
    auto val = APInt::getAllOnesValue(numBits);
    if (numBits < spareBitMask.size())
      val = val.zext(spareBitMask.size());
    val = val.shl(startBit);
    auto mask = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(), val);
    newBits = IGF.Builder.CreateAnd(newBits, mask);
    
    // Accumulate the result.
    if (result)
      result = IGF.Builder.CreateOr(result, newBits);
    else
      result = newBits;
    
    usedBits += numBits;
    i = u;
  }
  
  return result;
}

/// Interleave the occupiedValue and spareValue bits, taking a bit from one
/// or the other at each position based on the spareBits mask.
APInt
irgen::interleaveSpareBits(IRGenModule &IGM, const llvm::BitVector &spareBits,
                           unsigned bits,
                           unsigned spareValue, unsigned occupiedValue) {
  // FIXME: endianness.
  SmallVector<llvm::integerPart, 2> valueParts;
  valueParts.push_back(0);
  
  llvm::integerPart valueBit = 1;
  auto advanceValueBit = [&]{
    valueBit <<= 1;
    if (valueBit == 0) {
      valueParts.push_back(0);
      valueBit = 1;
    }
  };
  
  for (unsigned i = 0, e = spareBits.size();
       (occupiedValue || spareValue) && i < e;
       ++i, advanceValueBit()) {
    if (spareBits[i]) {
      if (spareValue & 1)
        valueParts.back() |= valueBit;
      spareValue >>= 1;
    } else {
      if (occupiedValue & 1)
        valueParts.back() |= valueBit;
      occupiedValue >>= 1;
    }
  }
  
  // Create the value.
  return llvm::APInt(bits, valueParts);
}

static void setAlignmentBits(llvm::BitVector &v, Alignment align) {
  switch (align.getValue()) {
  case 16:
    v[3] = true;
    SWIFT_FALLTHROUGH;
  case 8:
    v[2] = true;
    SWIFT_FALLTHROUGH;
  case 4:
    v[1] = true;
    SWIFT_FALLTHROUGH;
  case 2:
    v[0] = true;
    SWIFT_FALLTHROUGH;
  case 1:
  case 0:
    break;
  default:
    llvm_unreachable("unexpected heap object alignment");
  }
}

const llvm::BitVector &
IRGenModule::getHeapObjectSpareBits() const {
  return HeapPointerSpareBits.cache([&]{
    // Start with the spare bit mask for all pointers.
    llvm::BitVector r = TargetInfo.PointerSpareBits;

    // Low bits are made available by heap object alignment.
    setAlignmentBits(r, TargetInfo.HeapObjectAlignment);
    
    return r;
  });
}
