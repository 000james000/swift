//===--- GenTypes.cpp - Swift IR Generation For Types ---------------------===//
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
//  This file implements IR generation for types in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/Decl.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILModule.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"

#include "LoadableTypeInfo.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "Address.h"
#include "Explosion.h"
#include "GenOpaque.h"
#include "HeapTypeInfo.h"
#include "Linking.h"
#include "ProtocolInfo.h"
#include "ReferenceTypeInfo.h"
#include "ScalarTypeInfo.h"
#include "UnownedTypeInfo.h"
#include "WeakTypeInfo.h"

using namespace swift;
using namespace irgen;

llvm::DenseMap<TypeBase*, TypeCacheEntry> &
TypeConverter::Types_t::getCacheFor(TypeBase *t) {
  return t->isDependentType() ? DependentCache : IndependentCache;
}

bool TypeInfo::isSingleSwiftRetainablePointer(ResilienceScope scope) const {
  return false;
}

bool TypeInfo::isSingleUnknownRetainablePointer(ResilienceScope scope) const {
  return isSingleSwiftRetainablePointer(scope);
}

FixedPacking TypeInfo::getFixedPacking(IRGenModule &IGM) const {
  auto fixedTI = dyn_cast<FixedTypeInfo>(this);
  
  // If the type is fixed, we have to do something dynamic.
  // FIXME: some types are provably too big (or aligned) to be
  // allocated inline.
  if (!fixedTI)
    return FixedPacking::Dynamic;
  
  Size bufferSize = getFixedBufferSize(IGM);
  Size requiredSize = fixedTI->getFixedSize();
  
  // Flat out, if we need more space than the buffer provides,
  // we always have to allocate.
  // FIXME: there might be some interesting cases where this
  // is suboptimal for enums.
  if (requiredSize > bufferSize)
    return FixedPacking::Allocate;
  
  Alignment bufferAlign = getFixedBufferAlignment(IGM);
  Alignment requiredAlign = fixedTI->getFixedAlignment();
  
  // If the buffer alignment is good enough for the type, great.
  if (bufferAlign >= requiredAlign)
    return FixedPacking::OffsetZero;
  
  // TODO: consider using a slower mode that dynamically checks
  // whether the buffer size is small enough.
  
  // Otherwise we're stuck and have to separately allocate.
  return FixedPacking::Allocate;
}

ExplosionSchema TypeInfo::getSchema(ResilienceExpansion kind) const {
  ExplosionSchema schema(kind);
  getSchema(schema);
  return schema;
}

Address TypeInfo::getAddressForPointer(llvm::Value *ptr) const {
  assert(ptr->getType()->getPointerElementType() == StorageType);
  return Address(ptr, StorageAlignment);
}

Address TypeInfo::getUndefAddress() const {
  return Address(llvm::UndefValue::get(getStorageType()->getPointerTo(0)),
                 StorageAlignment);
}

/// Whether this type is known to be empty.
bool TypeInfo::isKnownEmpty() const {
  if (auto fixed = dyn_cast<FixedTypeInfo>(this))
    return fixed->isKnownEmpty();
  return false;
}

/// Copy a value from one object to a new object, directly taking
/// responsibility for anything it might have.  This is like C++
/// move-initialization, except the old object will not be destroyed.
void FixedTypeInfo::initializeWithTake(IRGenFunction &IGF,
                                       Address destAddr,
                                       Address srcAddr,
                                       CanType T) const {
  // Prefer loads and stores if we won't make a million of them.
  // Maybe this should also require the scalars to have a fixed offset.
  ExplosionSchema schema = getSchema(ResilienceExpansion::Maximal);
  if (!schema.containsAggregate() && schema.size() <= 2) {
    auto &loadableTI = cast<LoadableTypeInfo>(*this);
    Explosion copy(ResilienceExpansion::Maximal);
    loadableTI.loadAsTake(IGF, srcAddr, copy);
    loadableTI.initialize(IGF, copy, destAddr);
    return;
  }

  // Otherwise, use a memcpy.
  IGF.emitMemCpy(destAddr, srcAddr, getFixedSize());
}

/// Copy a value from one object to a new object.  This is just the
/// default implementation.
void LoadableTypeInfo::initializeWithCopy(IRGenFunction &IGF,
                                          Address destAddr,
                                          Address srcAddr,
                                          CanType T) const {
  // Use memcpy if that's legal.
  if (isPOD(ResilienceScope::Local)) {
    return initializeWithTake(IGF, destAddr, srcAddr, T);
  }

  // Otherwise explode and re-implode.
  Explosion copy(ResilienceExpansion::Maximal);
  loadAsCopy(IGF, srcAddr, copy);
  initialize(IGF, copy, destAddr);
}

static llvm::Constant *asSizeConstant(IRGenModule &IGM, Size size) {
  return llvm::ConstantInt::get(IGM.SizeTy, size.getValue());
}

/// Return the size and alignment of this type.
std::pair<llvm::Value*,llvm::Value*>
FixedTypeInfo::getSizeAndAlignmentMask(IRGenFunction &IGF,
                                       CanType T) const {
  return {FixedTypeInfo::getSize(IGF, T),
          FixedTypeInfo::getAlignmentMask(IGF, T)};
}
std::tuple<llvm::Value*,llvm::Value*,llvm::Value*>
FixedTypeInfo::getSizeAndAlignmentMaskAndStride(IRGenFunction &IGF,
                                                CanType T) const {
  return std::make_tuple(FixedTypeInfo::getSize(IGF, T),
                         FixedTypeInfo::getAlignmentMask(IGF, T),
                         FixedTypeInfo::getStride(IGF, T));
}

llvm::Value *FixedTypeInfo::getSize(IRGenFunction &IGF, CanType T) const {
  return FixedTypeInfo::getStaticSize(IGF.IGM);
}
llvm::Constant *FixedTypeInfo::getStaticSize(IRGenModule &IGM) const {
  return asSizeConstant(IGM, getFixedSize());
}

llvm::Value *FixedTypeInfo::getAlignmentMask(IRGenFunction &IGF,
                                             CanType T) const {
  return FixedTypeInfo::getStaticAlignmentMask(IGF.IGM);
}
llvm::Constant *FixedTypeInfo::getStaticAlignmentMask(IRGenModule &IGM) const {
  return asSizeConstant(IGM, Size(getFixedAlignment().getValue() - 1));
}

llvm::Value *FixedTypeInfo::getStride(IRGenFunction &IGF, CanType T) const {
  return FixedTypeInfo::getStaticStride(IGF.IGM);
}
llvm::Constant *FixedTypeInfo::getStaticStride(IRGenModule &IGM) const {
  return asSizeConstant(IGM, getFixedStride());
}

unsigned FixedTypeInfo::getSpareBitExtraInhabitantCount() const {
  if (SpareBits.none())
    return 0;
  // The runtime supports a max of 0x7FFFFFFF extra inhabitants, which ought
  // to be enough for anybody.
  if (StorageSize.getValue() >= 4)
    return 0x7FFFFFFF;
  unsigned spareBitCount = SpareBits.count();
  assert(spareBitCount <= StorageSize.getValueInBits()
         && "more spare bits than storage bits?!");
  unsigned inhabitedBitCount = StorageSize.getValueInBits() - spareBitCount;
  return ((1U << spareBitCount) - 1U) << inhabitedBitCount;
}

void FixedTypeInfo::applyFixedSpareBitsMask(llvm::BitVector &bits) const {
  auto numBits = StorageSize.getValueInBits();
  
  // Grow the mask with one bits if needed.
  if (bits.size() < numBits) {
    bits.resize(numBits, true);
  }
  
  // If there are no SpareBits, mask out the range.
  if (SpareBits.empty()) {
    bits.reset(0, numBits);
    return;
  }
  
  // Apply the mask.
  if (SpareBits.size() < bits.size()) {
    // Pad mask with one bits so we don't disturb bits unused by the type.
    auto paddedSpareBits = SpareBits;
    paddedSpareBits.resize(bits.size(), true);
    bits &= paddedSpareBits;
  } else {
    bits &= SpareBits;
  }
}

llvm::ConstantInt *
FixedTypeInfo::getSpareBitFixedExtraInhabitantValue(IRGenModule &IGM,
                                                    unsigned bits,
                                                    unsigned index) const {
  // Factor the index into the part that goes in the occupied bits and the
  // part that goes in the spare bits.
  unsigned occupiedIndex, spareIndex = 0;
  
  unsigned spareBitCount = SpareBits.count();
  unsigned occupiedBitCount
    = getFixedSize().getValueInBits() - spareBitCount;
  
  if (occupiedBitCount >= 31) {
    occupiedIndex = index;
    // The spare bit value is biased by one because all zero spare bits
    // represents a valid value of the type.
    spareIndex = 1;
  } else {
    occupiedIndex = index & ((1 << occupiedBitCount) - 1);
    // The spare bit value is biased by one because all zero spare bits
    // represents a valid value of the type.
    spareIndex = (index >> occupiedBitCount) + 1;
  }

  APInt val
    = interleaveSpareBits(IGM, SpareBits, bits, spareIndex, occupiedIndex);
  return llvm::ConstantInt::get(IGM.getLLVMContext(), val);
}

llvm::Value *
FixedTypeInfo::getSpareBitExtraInhabitantIndex(IRGenFunction &IGF,
                                               Address src) const {
  assert(!SpareBits.empty() && "no spare bits");
  
  auto &C = IGF.IGM.getLLVMContext();
  
  // Load the value.
  auto payloadTy = llvm::IntegerType::get(C, StorageSize.getValueInBits());
  src = IGF.Builder.CreateBitCast(src, payloadTy->getPointerTo());
  auto val = IGF.Builder.CreateLoad(src);
  
  // If the spare bits are all zero, then we have a valid value and not an
  // extra inhabitant.
  auto spareBitsMask
    = llvm::ConstantInt::get(C, getAPIntFromBitVector(SpareBits));
  auto valSpareBits = IGF.Builder.CreateAnd(val, spareBitsMask);
  auto isValid = IGF.Builder.CreateICmpEQ(valSpareBits,
                                          llvm::ConstantInt::get(payloadTy, 0));
  
  auto *origBB = IGF.Builder.GetInsertBlock();
  auto *endBB = llvm::BasicBlock::Create(C);
  auto *spareBB = llvm::BasicBlock::Create(C);
  IGF.Builder.CreateCondBr(isValid, endBB, spareBB);

  IGF.Builder.emitBlock(spareBB);
  
  // Gather the occupied bits.
  auto OccupiedBits = SpareBits;
  OccupiedBits.flip();
  llvm::Value *idx = emitGatherSpareBits(IGF, OccupiedBits, val, 0, 31);
  
  // See if spare bits fit into the 31 bits of the index.
  unsigned numSpareBits = SpareBits.count();
  unsigned numOccupiedBits = StorageSize.getValueInBits() - numSpareBits;
  if (numOccupiedBits < 31) {
    // Gather the spare bits.
    llvm::Value *spareIdx
      = emitGatherSpareBits(IGF, SpareBits, val, numOccupiedBits, 31);
    // Unbias by subtracting one.
    spareIdx = IGF.Builder.CreateSub(spareIdx,
            llvm::ConstantInt::get(spareIdx->getType(), 1 << numOccupiedBits));
    idx = IGF.Builder.CreateOr(idx, spareIdx);
  }
  idx = IGF.Builder.CreateZExt(idx, IGF.IGM.Int32Ty);
  
  IGF.Builder.CreateBr(endBB);
  IGF.Builder.emitBlock(endBB);
  
  // If we had a valid value, return -1. Otherwise, return the index.
  auto phi = IGF.Builder.CreatePHI(IGF.IGM.Int32Ty, 2);
  phi->addIncoming(llvm::ConstantInt::get(IGF.IGM.Int32Ty, -1), origBB);
  phi->addIncoming(idx, spareBB);;
  
  return phi;
}

void
FixedTypeInfo::storeSpareBitExtraInhabitant(IRGenFunction &IGF,
                                            llvm::Value *index,
                                            Address dest) const {
  assert(!SpareBits.empty() && "no spare bits");
  
  auto &C = IGF.IGM.getLLVMContext();

  auto payloadTy = llvm::IntegerType::get(C, StorageSize.getValueInBits());

  unsigned numSpareBits = SpareBits.count();
  unsigned numOccupiedBits = StorageSize.getValueInBits() - numSpareBits;
  llvm::Value *spareBitValue;
  llvm::Value *occupiedBitValue;
  
  // The spare bit value is biased by one because all zero spare bits
  // represents a valid value of the type.
  auto spareBitBias = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                                             1U << numOccupiedBits);
  
  // Factor the spare and occupied bit values from the index.
  if (numOccupiedBits >= 31) {
    occupiedBitValue = index;
    spareBitValue = spareBitBias;
  } else {
    auto occupiedBitMask = APInt::getAllOnesValue(numOccupiedBits);
    occupiedBitMask = occupiedBitMask.zext(32);
    auto occupiedBitMaskValue = llvm::ConstantInt::get(C, occupiedBitMask);
    occupiedBitValue = IGF.Builder.CreateAnd(index, occupiedBitMaskValue);
    
    auto spareBitMask = ~occupiedBitMask;
    auto spareBitMaskValue = llvm::ConstantInt::get(C, spareBitMask);
    spareBitValue = IGF.Builder.CreateAnd(index, spareBitMaskValue);
    // The spare bit value is biased by one because all zero spare bits
    // represents a valid value of the type.
    spareBitValue = IGF.Builder.CreateAdd(spareBitValue, spareBitBias);
  }
  
  // Scatter the occupied bits.
  auto OccupiedBits = SpareBits;
  OccupiedBits.flip();
  llvm::Value *occupied = emitScatterSpareBits(IGF, OccupiedBits,
                                               occupiedBitValue, 0);
  
  // Scatter the spare bits.
  llvm::Value *spare = emitScatterSpareBits(IGF, SpareBits, spareBitValue,
                                            numOccupiedBits);
  
  // Combine the values and store to the destination.
  llvm::Value *inhabitant = IGF.Builder.CreateOr(occupied, spare);
  
  dest = IGF.Builder.CreateBitCast(dest, payloadTy->getPointerTo());
  IGF.Builder.CreateStore(inhabitant, dest);
}

static unsigned getNumLowObjCReservedBits(IRGenModule &IGM) {
  llvm::BitVector ObjCMask = IGM.TargetInfo.ObjCPointerReservedBits;
  ObjCMask.flip();
  return ObjCMask.find_first();
}

unsigned irgen::getHeapObjectExtraInhabitantCount(IRGenModule &IGM) {
  // This must be consistent with the extra inhabitant count produced
  // by the runtime's getHeapObjectExtraInhabitantCount function in
  // KnownMetadata.cpp.
  
  // FIXME: We could also make extra inhabitants using spare bits, but we
  // probably don't need to.
  uint64_t rawCount = IGM.TargetInfo.LeastValidPointerValue
    >> getNumLowObjCReservedBits(IGM);
  
  // The runtime limits the count to INT_MAX.
  return std::min((uint64_t)INT_MAX, rawCount);
}

llvm::ConstantInt *irgen::getHeapObjectFixedExtraInhabitantValue(
                                                       IRGenModule &IGM,
                                                       unsigned bits,
                                                       unsigned index,
                                                       unsigned offset) {
  // This must be consistent with the extra inhabitant calculation implemented
  // in the runtime's storeHeapObjectExtraInhabitant and
  // getHeapObjectExtraInhabitantIndex functions in KnownMetadata.cpp.
  
  assert(index < getHeapObjectExtraInhabitantCount(IGM) &&
         "heap object extra inhabitant out of bounds");
  uint64_t value = (uint64_t)index << getNumLowObjCReservedBits(IGM);
  APInt apValue(bits, value);
  if (offset > 0)
    apValue = apValue.shl(offset);
  
  return llvm::ConstantInt::get(IGM.getLLVMContext(), apValue);
}

llvm::Value *irgen::getHeapObjectExtraInhabitantIndex(IRGenFunction &IGF,
                                                      Address src) {
  // This must be consistent with the extra inhabitant calculation implemented
  // in the runtime's getHeapObjectExtraInhabitantIndex function in
  // KnownMetadata.cpp.

  llvm::BasicBlock *contBB = IGF.createBasicBlock("validpointer");
  llvm::BasicBlock *invalidBB = IGF.createBasicBlock("invalidpointer");
  llvm::BasicBlock *invalidObjCBB = IGF.createBasicBlock("invalidobjc");
  llvm::BasicBlock *origBB = IGF.Builder.GetInsertBlock();
  
  src = IGF.Builder.CreateBitCast(src, IGF.IGM.SizeTy->getPointerTo());
  
  // Check if the inhabitant is below the least valid pointer value.
  llvm::Value *val = IGF.Builder.CreateLoad(src);
  llvm::Value *leastValid = llvm::ConstantInt::get(IGF.IGM.SizeTy,
                                     IGF.IGM.TargetInfo.LeastValidPointerValue);
  llvm::Value *isValid = IGF.Builder.CreateICmpUGE(val, leastValid);
  
  IGF.Builder.CreateCondBr(isValid, contBB, invalidBB);
  
  IGF.Builder.emitBlock(invalidBB);
  // Check if the inhabitant has any ObjC-reserved bits set.
  // FIXME: This check is unneeded if the type is known to be pure Swift.
  APInt objcMaskInt = getAPIntFromBitVector(
                                    IGF.IGM.TargetInfo.ObjCPointerReservedBits);
  auto objcMask = llvm::ConstantInt::get(IGF.IGM.getLLVMContext(), objcMaskInt);
  llvm::Value *masked = IGF.Builder.CreateAnd(val, objcMask);
  llvm::Value *maskedZero = IGF.Builder.CreateICmpEQ(masked,
                                     llvm::ConstantInt::get(IGF.IGM.SizeTy, 0));
  IGF.Builder.CreateCondBr(maskedZero, invalidObjCBB, contBB);
  
  IGF.Builder.emitBlock(invalidObjCBB);
  // The inhabitant is an invalid pointer. Derive its extra inhabitant index.
  llvm::Value *index = IGF.Builder.CreateLShr(val,
                    llvm::ConstantInt::get(IGF.IGM.SizeTy,
                                           getNumLowObjCReservedBits(IGF.IGM)));
  if (index->getType() != IGF.IGM.Int32Ty)
    index = IGF.Builder.CreateTrunc(index, IGF.IGM.Int32Ty);
  IGF.Builder.CreateBr(contBB);
  
  IGF.Builder.emitBlock(contBB);
  auto phi = IGF.Builder.CreatePHI(IGF.IGM.Int32Ty, 2);
  phi->addIncoming(llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1), origBB);
  phi->addIncoming(llvm::ConstantInt::getSigned(IGF.IGM.Int32Ty, -1), invalidBB);
  phi->addIncoming(index, invalidObjCBB);
  
  return phi;
}

void irgen::storeHeapObjectExtraInhabitant(IRGenFunction &IGF,
                                           llvm::Value *index,
                                           Address dest) {
  // This must be consistent with the extra inhabitant calculation implemented
  // in the runtime's storeHeapObjectExtraInhabitant function in
  // KnownMetadata.cpp.

  if (index->getType() != IGF.IGM.SizeTy)
    index = IGF.Builder.CreateZExt(index, IGF.IGM.SizeTy);
  
  index = IGF.Builder.CreateShl(index, llvm::ConstantInt::get(IGF.IGM.SizeTy,
                                           getNumLowObjCReservedBits(IGF.IGM)));
  dest = IGF.Builder.CreateBitCast(dest, IGF.IGM.SizeTy->getPointerTo());
  IGF.Builder.CreateStore(index, dest);
}

namespace {
  /// A TypeInfo implementation for empty types.
  struct EmptyTypeInfo : ScalarTypeInfo<EmptyTypeInfo, LoadableTypeInfo> {
    EmptyTypeInfo(llvm::Type *ty)
      : ScalarTypeInfo(ty, Size(0), llvm::BitVector{}, Alignment(1), IsPOD) {}
    unsigned getExplosionSize(ResilienceExpansion kind) const { return 0; }
    void getSchema(ExplosionSchema &schema) const {}
    void loadAsCopy(IRGenFunction &IGF, Address addr, Explosion &e) const {}
    void loadAsTake(IRGenFunction &IGF, Address addr, Explosion &e) const {}
    void assign(IRGenFunction &IGF, Explosion &e, Address addr) const {}
    void initialize(IRGenFunction &IGF, Explosion &e, Address addr) const {}
    void copy(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {}
    void consume(IRGenFunction &IGF, Explosion &src) const {}
    void destroy(IRGenFunction &IGF, Address addr, CanType T) const {}
    llvm::Value *packEnumPayload(IRGenFunction &IGF, Explosion &src,
                                  unsigned bitWidth,
                                  unsigned offset) const override {
      return PackEnumPayload::getEmpty(IGF.IGM, bitWidth);
    }
    void unpackEnumPayload(IRGenFunction &IGF, llvm::Value *payload,
                            Explosion &dest,
                            unsigned offset) const override {}
  };

  /// A TypeInfo implementation for types represented as a single
  /// scalar type.
  class PrimitiveTypeInfo :
    public PODSingleScalarTypeInfo<PrimitiveTypeInfo, LoadableTypeInfo> {
  public:
    PrimitiveTypeInfo(llvm::Type *storage, Size size, llvm::BitVector spareBits,
                      Alignment align)
      : PODSingleScalarTypeInfo(storage, size, spareBits, align) {}
  };
}

/// Constructs a type info which performs simple loads and stores of
/// the given IR type.
const TypeInfo *TypeConverter::createPrimitive(llvm::Type *type,
                                               Size size, Alignment align) {
  return new PrimitiveTypeInfo(type, size, IGM.getSpareBitsForType(type),
                               align);
}

static TypeInfo *invalidTypeInfo() { return (TypeInfo*) 1; }
static ProtocolInfo *invalidProtocolInfo() { return (ProtocolInfo*) 1; }

TypeConverter::TypeConverter(IRGenModule &IGM)
  : IGM(IGM),
    FirstType(invalidTypeInfo()),
    FirstProtocol(invalidProtocolInfo()) {}

TypeConverter::~TypeConverter() {
  // Delete all the converted type infos.
  for (const TypeInfo *I = FirstType; I != invalidTypeInfo(); ) {
    const TypeInfo *Cur = I;
    I = Cur->NextConverted;
    delete Cur;
  }
  
  for (const ProtocolInfo *I = FirstProtocol; I != invalidProtocolInfo(); ) {
    const ProtocolInfo *Cur = I;
    I = Cur->NextConverted;
    delete Cur;
  }
}

void TypeConverter::pushGenericContext(GenericSignature *signature) {
  if (!signature)
    return;
  
  // Push the generic context down to the SIL TypeConverter, so we can share
  // archetypes with SIL.
  IGM.SILMod->Types.pushGenericContext(signature);
}

void TypeConverter::popGenericContext(GenericSignature *signature) {
  if (!signature)
    return;

  // Pop the SIL TypeConverter's generic context too.
  IGM.SILMod->Types.popGenericContext(signature);
  
  Types.DependentCache.clear();
}

ArchetypeBuilder &TypeConverter::getArchetypes() {
  return IGM.SILMod->Types.getArchetypes();
}

ArchetypeBuilder &IRGenModule::getContextArchetypes() {
  return Types.getArchetypes();
}

/// Add a temporary forward declaration for a type.  This will live
/// only until a proper mapping is added.
void TypeConverter::addForwardDecl(TypeBase *key, llvm::Type *type) {
  assert(key->isCanonical());
  assert(!key->isDependentType());
  assert(!Types.IndependentCache.count(key) && "entry already exists for type!");
  Types.IndependentCache.insert(std::make_pair(key, type));
}

const TypeInfo &IRGenModule::getWitnessTablePtrTypeInfo() {
  return Types.getWitnessTablePtrTypeInfo();
}

const TypeInfo &TypeConverter::getWitnessTablePtrTypeInfo() {
  if (WitnessTablePtrTI) return *WitnessTablePtrTI;
  WitnessTablePtrTI = createPrimitive(IGM.WitnessTablePtrTy,
                                      IGM.getPointerSize(),
                                      IGM.getPointerAlignment());
  WitnessTablePtrTI->NextConverted = FirstType;
  FirstType = WitnessTablePtrTI;
  return *WitnessTablePtrTI;
}

const TypeInfo &IRGenModule::getTypeMetadataPtrTypeInfo() {
  return Types.getTypeMetadataPtrTypeInfo();
}

const TypeInfo &TypeConverter::getTypeMetadataPtrTypeInfo() {
  if (TypeMetadataPtrTI) return *TypeMetadataPtrTI;
  TypeMetadataPtrTI = createPrimitive(IGM.TypeMetadataPtrTy,
                                      IGM.getPointerSize(),
                                      IGM.getPointerAlignment());
  TypeMetadataPtrTI->NextConverted = FirstType;
  FirstType = TypeMetadataPtrTI;
  return *TypeMetadataPtrTI;
}

/// Get the fragile type information for the given type, which may not
/// have yet undergone SIL type lowering.  The type can serve as its own
/// abstraction pattern.
const TypeInfo &IRGenFunction::getTypeInfoForUnlowered(Type subst) {
  return IGM.getTypeInfoForUnlowered(subst);
}

/// Get the fragile type information for the given type, which may not
/// have yet undergone SIL type lowering.
const TypeInfo &
IRGenFunction::getTypeInfoForUnlowered(AbstractionPattern orig, Type subst) {
  return IGM.getTypeInfoForUnlowered(orig, subst);
}

/// Get the fragile type information for the given type, which may not
/// have yet undergone SIL type lowering.
const TypeInfo &
IRGenFunction::getTypeInfoForUnlowered(AbstractionPattern orig, CanType subst) {
  return IGM.getTypeInfoForUnlowered(orig, subst);
}

/// Get the fragile type information for the given type, which is known
/// to have undergone SIL type lowering (or be one of the types for
/// which that lowering is the identity function).
const TypeInfo &IRGenFunction::getTypeInfoForLowered(CanType T) {
  return IGM.getTypeInfoForLowered(T);
}

/// Get the fragile type information for the given type.
const TypeInfo &IRGenFunction::getTypeInfo(SILType T) {
  return IGM.getTypeInfo(T);
}

/// Return the SIL-lowering of the given type.
SILType IRGenModule::getLoweredType(AbstractionPattern orig, Type subst) {
  return SILMod->Types.getLoweredType(orig, subst);
}

/// Get a pointer to the storage type for the given type.  Note that,
/// unlike fetching the type info and asking it for the storage type,
/// this operation will succeed for forward-declarations.
llvm::PointerType *IRGenModule::getStoragePointerType(SILType T) {
  return getStoragePointerTypeForLowered(T.getSwiftRValueType());
}
llvm::PointerType *IRGenModule::getStoragePointerTypeForUnlowered(Type T) {
  return getStorageTypeForUnlowered(T)->getPointerTo();
}
llvm::PointerType *IRGenModule::getStoragePointerTypeForLowered(CanType T) {
  return getStorageTypeForLowered(T)->getPointerTo();
}

llvm::Type *IRGenModule::getStorageTypeForUnlowered(Type subst) {
  return getStorageType(SILMod->Types.getLoweredType(subst));
}

llvm::Type *IRGenModule::getStorageType(SILType T) {
  return getStorageTypeForLowered(T.getSwiftRValueType());
}

/// Get the storage type for the given type.  Note that, unlike
/// fetching the type info and asking it for the storage type, this
/// operation will succeed for forward-declarations.
llvm::Type *IRGenModule::getStorageTypeForLowered(CanType T) {
  // TODO: we can avoid creating entries for some obvious cases here.
  auto entry = Types.getTypeEntry(T);
  if (auto ti = entry.dyn_cast<const TypeInfo*>()) {
    return ti->getStorageType();
  } else {
    return entry.get<llvm::Type*>();
  }
}

/// Get the type information for the given type, which may not have
/// yet undergone SIL type lowering.  The type can serve as its own
/// abstraction pattern.
const TypeInfo &IRGenModule::getTypeInfoForUnlowered(Type subst) {
  return getTypeInfoForUnlowered(AbstractionPattern(subst), subst);
}

/// Get the type information for the given type, which may not
/// have yet undergone SIL type lowering.
const TypeInfo &
IRGenModule::getTypeInfoForUnlowered(AbstractionPattern orig, Type subst) {
  return getTypeInfoForUnlowered(orig, subst->getCanonicalType());
}

/// Get the type information for the given type, which may not
/// have yet undergone SIL type lowering.
const TypeInfo &
IRGenModule::getTypeInfoForUnlowered(AbstractionPattern orig, CanType subst) {
  return getTypeInfo(SILMod->Types.getLoweredType(orig, subst));
}

/// Get the fragile type information for the given type, which is known
/// to have undergone SIL type lowering (or be one of the types for
/// which that lowering is the identity function).
const TypeInfo &IRGenModule::getTypeInfo(SILType T) {
  return getTypeInfoForLowered(T.getSwiftRValueType());
}

/// Get the fragile type information for the given type.
const TypeInfo &IRGenModule::getTypeInfoForLowered(CanType T) {
  return Types.getCompleteTypeInfo(T);
}

/// 
const TypeInfo &TypeConverter::getCompleteTypeInfo(CanType T) {
  auto entry = getTypeEntry(T);
  assert(entry.is<const TypeInfo*>() && "getting TypeInfo recursively!");
  auto &ti = *entry.get<const TypeInfo*>();
  assert(ti.isComplete());
  return ti;
}

const TypeInfo *TypeConverter::tryGetCompleteTypeInfo(CanType T) {
  auto entry = getTypeEntry(T);
  if (!entry.is<const TypeInfo*>()) return nullptr;
  auto &ti = *entry.get<const TypeInfo*>();
  if (!ti.isComplete()) return nullptr;
  return &ti;
}

/// Profile the archetype constraints that may affect type layout into a
/// folding set node ID.
static void profileArchetypeConstraints(ArchetypeType *arch,
                                        llvm::FoldingSetNodeID &ID,
                                        unsigned depth = 0) {
  // Is the archetype class-constrained?
  ID.AddBoolean(arch->requiresClass());
  
  // The archetype's superclass constraint.
  auto superclass = arch->getSuperclass();
  auto superclassPtr = superclass ? superclass->getCanonicalType().getPointer()
                                  : nullptr;
  ID.AddPointer(superclassPtr);

  // The archetype's protocol constraints.
  for (auto proto : arch->getConformsTo()) {
    ID.AddPointer(proto);
  }
  
  // Recursively profile nested archetypes.
  for (auto nested : arch->getNestedTypes()) {
    profileArchetypeConstraints(nested.second, ID, depth + 1);
  }
}

void ExemplarArchetype::Profile(llvm::FoldingSetNodeID &ID) const {
  profileArchetypeConstraints(Archetype, ID);
}

ArchetypeType *TypeConverter::getExemplarArchetype(ArchetypeType *t) {
  // Check the folding set to see whether we already have an exemplar matching
  // this archetype.
  llvm::FoldingSetNodeID ID;
  profileArchetypeConstraints(t, ID);
  void *insertPos;
  ExemplarArchetype *existing
    = Types.ExemplarArchetypes.FindNodeOrInsertPos(ID, insertPos);
  if (existing) {
    return existing->Archetype;
  }
  
  // Otherwise, use this archetype as the exemplar for future similar
  // archetypes.
  Types.ExemplarArchetypeStorage.push_back({t});
  Types.ExemplarArchetypes.InsertNode(&Types.ExemplarArchetypeStorage.back(),
                                      insertPos);
  return t;
}

/// Fold archetypes to unique exemplars. Any archetype with the same
/// constraints is equivalent for type lowering purposes.
CanType TypeConverter::getExemplarType(CanType contextTy) {
  // FIXME: A generic SILFunctionType should not contain any nondependent
  // archetypes.
  if (isa<SILFunctionType>(contextTy)
      && cast<SILFunctionType>(contextTy)->isPolymorphic())
    return contextTy;
  else
    return CanType(contextTy.transform([&](Type t) -> Type {
      if (auto arch = dyn_cast<ArchetypeType>(t.getPointer()))
        return getExemplarArchetype(arch);
      return t;
    }));
}

TypeCacheEntry TypeConverter::getTypeEntry(CanType canonicalTy) {
  // Cache this entry in the dependent or independent cache appropriate to it.
  auto &Cache = Types.getCacheFor(canonicalTy.getPointer());

  {
    auto it = Cache.find(canonicalTy.getPointer());
    if (it != Cache.end()) {
      return it->second;
    }
  }
  
  // If the type is dependent, substitute it into our current context.
  auto contextTy = canonicalTy;
  if (contextTy->isDependentType())
    contextTy = getArchetypes().substDependentType(contextTy)
                              ->getCanonicalType();
  
  // Fold archetypes to unique exemplars. Any archetype with the same
  // constraints is equivalent for type lowering purposes.
  CanType exemplarTy = getExemplarType(contextTy);
  assert(!exemplarTy->isDependentType());
  
  // See whether we lowered a type equivalent to this one.
  if (exemplarTy != canonicalTy) {
    auto it = Types.IndependentCache.find(exemplarTy.getPointer());
    if (it != Types.IndependentCache.end()) {
      // Record the object under the original type.
      auto result = it->second;
      Cache[canonicalTy.getPointer()] = result;
      return result;
    }
  }

  // Convert the type.
  TypeCacheEntry convertedEntry = convertType(exemplarTy);
  auto convertedTI = convertedEntry.dyn_cast<const TypeInfo*>();

  // If that gives us a forward declaration (which can happen with
  // bound generic types), don't propagate that into the cache here,
  // because we won't know how to clear it later.
  if (!convertedTI) {
    return convertedEntry;
  }

  // Cache the entry under the original type and the exemplar type, so that
  // we can avoid relowering equivalent types.
  auto insertEntry = [&](TypeCacheEntry &entry) {
    assert(entry == TypeCacheEntry() ||
           (entry.is<llvm::Type*>() &&
            entry.get<llvm::Type*>() == convertedTI->getStorageType()));
    entry = convertedTI;
  };
  insertEntry(Cache[canonicalTy.getPointer()]);
  if (canonicalTy != exemplarTy)
    insertEntry(Types.IndependentCache[exemplarTy.getPointer()]);
  
  // If the type info hasn't been added to the list of types, do so.
  if (!convertedTI->NextConverted) {
    convertedTI->NextConverted = FirstType;
    FirstType = convertedTI;
  }

  return convertedTI;
}

/// A convenience for grabbing the TypeInfo for a class declaration.
const TypeInfo &TypeConverter::getTypeInfo(ClassDecl *theClass) {
  // This type doesn't really matter except for serving as a key.
  CanType theType
    = getExemplarType(theClass->getDeclaredType()->getCanonicalType());

  // If we have generic parameters, use the bound-generics conversion
  // routine.  This does an extra level of caching based on the common
  // class decl.
  TypeCacheEntry entry;
  if (theClass->getGenericParams()) {
    entry = convertAnyNominalType(theType, theClass);

  // Otherwise, just look up the declared type.
  } else {
    assert(isa<ClassType>(theType));
    entry = getTypeEntry(theType);
  }

  // This will always yield a TypeInfo because forward-declarations
  // are unnecessary when converting class types.
  return *entry.get<const TypeInfo*>();
}

/// \brief Convert a primitive builtin type to its LLVM type, size, and
/// alignment.
static std::tuple<llvm::Type *, Size, Alignment>
convertPrimitiveBuiltin(IRGenModule &IGM, CanType canTy) {
  using RetTy = std::tuple<llvm::Type *, Size, Alignment>;
  llvm::LLVMContext &ctx = IGM.getLLVMContext();
  TypeBase *ty = canTy.getPointer();
  switch (ty->getKind()) {
  case TypeKind::BuiltinRawPointer:
    return RetTy{ IGM.Int8PtrTy, IGM.getPointerSize(),
                  IGM.getPointerAlignment() };
  case TypeKind::BuiltinFloat:
    switch (cast<BuiltinFloatType>(ty)->getFPKind()) {
    case BuiltinFloatType::IEEE16:
      return RetTy{ llvm::Type::getHalfTy(ctx), Size(2), Alignment(2) };
    case BuiltinFloatType::IEEE32:
      return RetTy{ llvm::Type::getFloatTy(ctx), Size(4), Alignment(4) };
    case BuiltinFloatType::IEEE64:
      return RetTy{ llvm::Type::getDoubleTy(ctx), Size(8), Alignment(8) };
    case BuiltinFloatType::IEEE80:
      return RetTy{ llvm::Type::getX86_FP80Ty(ctx), Size(16), Alignment(16) };
    case BuiltinFloatType::IEEE128:
      return RetTy{ llvm::Type::getFP128Ty(ctx), Size(16), Alignment(16) };
    case BuiltinFloatType::PPC128:
      return RetTy{ llvm::Type::getPPC_FP128Ty(ctx),Size(16), Alignment(16) };
    }
    llvm_unreachable("bad builtin floating-point type kind");
  case TypeKind::BuiltinInteger: {
    unsigned BitWidth = IGM.getBuiltinIntegerWidth(cast<BuiltinIntegerType>(ty));
    unsigned ByteSize = (BitWidth+7U)/8U;
    // Round up the memory size and alignment to a power of 2.
    if (!llvm::isPowerOf2_32(ByteSize))
      ByteSize = llvm::NextPowerOf2(ByteSize);

    return RetTy{ llvm::IntegerType::get(ctx, BitWidth), Size(ByteSize),
             Alignment(ByteSize) };
  }
  case TypeKind::BuiltinVector: {
    auto vecTy = ty->castTo<BuiltinVectorType>();
    llvm::Type *elementTy;
    Size size;
    Alignment align;
    std::tie(elementTy, size, align)
      = convertPrimitiveBuiltin(IGM,
                                vecTy->getElementType()->getCanonicalType());

    auto llvmVecTy = llvm::VectorType::get(elementTy, vecTy->getNumElements());
    unsigned bitSize = size.getValue() * vecTy->getNumElements() * 8;
    if (!llvm::isPowerOf2_32(bitSize))
      bitSize = llvm::NextPowerOf2(bitSize);

    return RetTy{ llvmVecTy, Size(bitSize / 8), align };
  }
  default:
    llvm_unreachable("Not a primitive builtin type");
  }
}

TypeCacheEntry TypeConverter::convertType(CanType ty) {
  PrettyStackTraceType stackTrace(IGM.Context, "converting", ty);

  switch (ty->getKind()) {
#define UNCHECKED_TYPE(id, parent) \
  case TypeKind::id: \
    llvm_unreachable("found a " #id "Type in IR-gen");
#define SUGARED_TYPE(id, parent) \
  case TypeKind::id: \
    llvm_unreachable("converting a " #id "Type after canonicalization");
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::LValue: assert(0 && "@lvalue type made it to irgen");
  case TypeKind::Metatype:
    return convertMetatypeType(cast<MetatypeType>(ty));
  case TypeKind::Module:
    return convertModuleType(cast<ModuleType>(ty));
  case TypeKind::DynamicSelf: {
    // DynamicSelf has the same representation as its superclass type.
    auto dynamicSelf = cast<DynamicSelfType>(ty);
    auto nominal = dynamicSelf->getSelfType()->getAnyNominal();
    return convertAnyNominalType(ty, nominal);
  }
  case TypeKind::BuiltinObjectPointer:
    return convertBuiltinObjectPointer();
  case TypeKind::BuiltinObjCPointer:
    return convertBuiltinObjCPointer();
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinVector: {
    llvm::Type *llvmTy;
    Size size;
    Alignment align;
    std::tie(llvmTy, size, align) = convertPrimitiveBuiltin(IGM, ty);
    return createPrimitive(llvmTy, size, align);
  }

  case TypeKind::Archetype:
    return convertArchetypeType(cast<ArchetypeType>(ty));
  case TypeKind::Class:
  case TypeKind::Enum:
  case TypeKind::Struct:
    return convertAnyNominalType(ty, cast<NominalType>(ty)->getDecl());
  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct:
    return convertAnyNominalType(ty, cast<BoundGenericType>(ty)->getDecl());
  case TypeKind::InOut:
    return convertInOutType(cast<InOutType>(ty));
  case TypeKind::Tuple:
    return convertTupleType(cast<TupleType>(ty));
  case TypeKind::Function:
  case TypeKind::PolymorphicFunction:
  case TypeKind::GenericFunction:
    llvm_unreachable("AST FunctionTypes should be lowered by SILGen");
  case TypeKind::SILFunction:
    return convertFunctionType(cast<SILFunctionType>(ty));
  case TypeKind::Array:
    llvm_unreachable("array types should be lowered by SILGen");
  case TypeKind::Protocol:
    return convertProtocolType(cast<ProtocolType>(ty));
  case TypeKind::ProtocolComposition:
    return convertProtocolCompositionType(cast<ProtocolCompositionType>(ty));
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    llvm_unreachable("can't convert dependent type");
  case TypeKind::UnownedStorage:
    return convertUnownedStorageType(cast<UnownedStorageType>(ty));
  case TypeKind::WeakStorage:
    return convertWeakStorageType(cast<WeakStorageType>(ty));
  }
  llvm_unreachable("bad type kind");
}

/// Convert an inout type.  This is always just a bare pointer.
const TypeInfo *TypeConverter::convertInOutType(InOutType *T) {
  auto referenceType =
    IGM.getStoragePointerTypeForUnlowered(CanType(T->getObjectType()));
  
  // Just use the reference type as a primitive pointer.
  return createPrimitive(referenceType, IGM.getPointerSize(),
                         IGM.getPointerAlignment());
}

/// Convert an [unowned] storage type.  The implementation here
/// depends on the underlying reference type.
const TypeInfo *
TypeConverter::convertUnownedStorageType(UnownedStorageType *refType) {
  CanType referent = CanType(refType->getReferentType());
  assert(referent->allowsOwnership());
  auto &referentTI = cast<ReferenceTypeInfo>(getCompleteTypeInfo(referent));
  return referentTI.createUnownedStorageType(*this);
}

/// Convert a [weak] storage type.  The implementation here
/// depends on the underlying reference type.
const TypeInfo *
TypeConverter::convertWeakStorageType(WeakStorageType *refType) {
  CanType referent = CanType(refType->getReferentType());
  assert(referent->allowsOwnership());
  auto &referentTI = cast<ReferenceTypeInfo>(getCompleteTypeInfo(referent));
  return referentTI.createWeakStorageType(*this);
}

static void overwriteForwardDecl(llvm::DenseMap<TypeBase*, TypeCacheEntry> &cache,
                                 TypeBase *key, const TypeInfo *result) {
  assert(cache.count(key) && "no forward declaration?");
  assert(cache[key].is<llvm::Type*>() && "overwriting real entry!");
  cache[key] = result;
}

TypeCacheEntry TypeConverter::convertAnyNominalType(CanType type,
                                                    NominalTypeDecl *decl) {
  // By "any", we don't mean existentials.
  assert(!isa<ProtocolDecl>(decl));

  // We want to try to re-use implementations between generic
  // specializations.  However, don't bother with this secondary hash
  // if the type isn't generic or if its type is obviously fixed.
  //
  // (But if it's generic and even *resilient*, we might need the
  // implementation to store a real type in order to grab the value
  // witnesses successfully.)
  if (!decl->getGenericParams() ||
      (!isa<ClassDecl>(decl) && // fast path obvious case
       IGM.classifyTypeSize(SILType::getPrimitiveObjectType(
                        decl->getDeclaredTypeInContext()->getCanonicalType()),
                            ResilienceScope::Local)
         != ObjectSize::Fixed)) {
    switch (decl->getKind()) {
#define NOMINAL_TYPE_DECL(ID, PARENT)
#define DECL(ID, PARENT) \
    case DeclKind::ID:
#include "swift/AST/DeclNodes.def"
      llvm_unreachable("not a nominal type declaration");
    case DeclKind::Protocol:
      llvm_unreachable("protocol types shouldn't be handled here");

    case DeclKind::Class:
      return convertClassType(cast<ClassDecl>(decl));
    case DeclKind::Enum:
      return convertEnumType(type.getPointer(), type, cast<EnumDecl>(decl));
    case DeclKind::Struct:
      return convertStructType(type.getPointer(), type, cast<StructDecl>(decl));
    }
    llvm_unreachable("bad declaration kind");
  }

  assert(decl->getGenericParams());

  // Look to see if we've already emitted this type under a different
  // set of arguments.  We cache under the unbound type, which should
  // never collide with anything.
  //
  // FIXME: this isn't really inherently good; we might want to use
  // different type implementations for different applications.
  assert(decl->getDeclaredType()->isCanonical());
  assert(decl->getDeclaredType()->is<UnboundGenericType>());
  TypeBase *key = decl->getDeclaredType().getPointer();
  auto &Cache = Types.IndependentCache;
  auto entry = Cache.find(key);
  if (entry != Cache.end())
    return entry->second;

  switch (decl->getKind()) {
#define NOMINAL_TYPE_DECL(ID, PARENT)
#define DECL(ID, PARENT) \
  case DeclKind::ID:
#include "swift/AST/DeclNodes.def"
    llvm_unreachable("not a nominal type declaration");

  case DeclKind::Protocol:
    llvm_unreachable("protocol types don't take generic parameters");

  case DeclKind::Class: {
    auto result = convertClassType(cast<ClassDecl>(decl));
    assert(!Cache.count(key));
    Cache.insert(std::make_pair(key, result));
    return result;
  }

  case DeclKind::Enum: {
    auto type = CanType(decl->getDeclaredTypeInContext());
    auto result = convertEnumType(key, type, cast<EnumDecl>(decl));
    overwriteForwardDecl(Cache, key, result);
    return result;
  }

  case DeclKind::Struct: {
    auto type = CanType(decl->getDeclaredTypeInContext());
    auto result = convertStructType(key, type, cast<StructDecl>(decl));
    overwriteForwardDecl(Cache, key, result);
    return result;
  }
  }
  llvm_unreachable("bad declaration kind");
}

const TypeInfo *TypeConverter::convertModuleType(ModuleType *T) {
  return new EmptyTypeInfo(IGM.Int8Ty);
}

const TypeInfo *TypeConverter::convertMetatypeType(MetatypeType *T) {
  assert(T->hasRepresentation() &&
         "metatype should have been assigned a representation by SIL");
  
  switch (T->getRepresentation()) {
  case MetatypeRepresentation::Thin:
    // Thin metatypes are empty.
    return new EmptyTypeInfo(IGM.Int8Ty);

  case MetatypeRepresentation::Thick:
    // Thick metatypes are represented with a metadata pointer.
    return &getTypeMetadataPtrTypeInfo();

  case MetatypeRepresentation::ObjC:
    // ObjC metatypes are represented with an objc_class pointer.
    return &getObjCClassPtrTypeInfo();
  }
}

/// createNominalType - Create a new nominal type.
llvm::StructType *IRGenModule::createNominalType(TypeDecl *decl) {
  llvm::SmallString<32> typeName;
  if (decl->getDeclContext()->isLocalContext()) {
    typeName = decl->getName().str();
    typeName.append(".local");
  } else {
    auto type = decl->getDeclaredType()->getCanonicalType();
    LinkEntity::forTypeMangling(type).mangle(typeName);
  }
  return llvm::StructType::create(getLLVMContext(), typeName.str());
}

/// createNominalType - Create a new nominal LLVM type for the given
/// protocol composition type.  Protocol composition types are
/// structural in the swift type system, but LLVM's type system
/// doesn't really care about this distinction, and it's nice to
/// distinguish different cases.
llvm::StructType *
IRGenModule::createNominalType(ProtocolCompositionType *type) {
  llvm::SmallString<32> typeName;

  SmallVector<ProtocolDecl *, 4> protocols;
  type->isExistentialType(protocols);

  typeName.append("protocol<");
  for (unsigned i = 0, e = protocols.size(); i != e; ++i) {
    if (i) typeName.push_back(',');
    LinkEntity::forNonFunction(protocols[i]).mangle(typeName);
  }
  typeName.push_back('>');
  return llvm::StructType::create(getLLVMContext(), typeName.str());
}

/// Compute the explosion schema for the given type.
ExplosionSchema IRGenModule::getSchema(SILType type, ResilienceExpansion kind) {
  ExplosionSchema schema(kind);
  getSchema(type, schema);
  return schema;
}

/// Compute the explosion schema for the given type.
void IRGenModule::getSchema(SILType type, ExplosionSchema &schema) {
  // As an optimization, avoid actually building a TypeInfo for any
  // obvious TupleTypes.  This assumes that a TupleType's explosion
  // schema is always the concatenation of its component's schemas.
  if (CanTupleType tuple = type.getAs<TupleType>()) {
    for (auto index : indices(tuple.getElementTypes()))
      getSchema(type.getTupleElementType(index), schema);
    return;
  }

  // Okay, that didn't work;  just do the general thing.
  getTypeInfo(type).getSchema(schema);
}

/// Compute the explosion schema for the given type.
unsigned IRGenModule::getExplosionSize(SILType type, ResilienceExpansion kind) {
  // As an optimization, avoid actually building a TypeInfo for any
  // obvious TupleTypes.  This assumes that a TupleType's explosion
  // schema is always the concatenation of its component's schemas.
  if (auto tuple = type.getAs<TupleType>()) {
    unsigned count = 0;
    for (auto index : indices(tuple.getElementTypes()))
      count += getExplosionSize(type.getTupleElementType(index), kind);
    return count;
  }

  // If the type isn't loadable, the explosion size is always 1.
  auto *loadableTI = dyn_cast<LoadableTypeInfo>(&getTypeInfo(type));
  if (!loadableTI) return 1;

  // Okay, that didn't work;  just do the general thing.
  return loadableTI->getExplosionSize(kind);
}

/// Determine whether this type is a single value that is passed
/// indirectly at the given level.
llvm::PointerType *IRGenModule::isSingleIndirectValue(SILType type,
                                                      ResilienceExpansion kind) {
  if (auto archetype = type.getAs<ArchetypeType>()) {
    if (!archetype->requiresClass())
      return OpaquePtrTy;
  }

  ExplosionSchema schema(kind);
  getSchema(type, schema);
  if (schema.size() == 1 && schema.begin()->isAggregate())
    return schema.begin()->getAggregateType()->getPointerTo(0);
  return nullptr;
}

/// Determine whether this type requires an indirect result.
llvm::PointerType *IRGenModule::requiresIndirectResult(SILType type,
                                                       ResilienceExpansion kind) {
  auto &ti = getTypeInfo(type);
  ExplosionSchema schema = ti.getSchema(kind);
  if (schema.requiresIndirectResult(*this))
    return ti.getStorageType()->getPointerTo();
  return nullptr;
}

/// Determine whether this type is known to be POD.
bool IRGenModule::isPOD(SILType type, ResilienceScope scope) {
  if (type.is<ArchetypeType>()) return false;
  if (type.is<ClassType>()) return false;
  if (type.is<BoundGenericClassType>()) return false;
  if (auto tuple = type.getAs<TupleType>()) {
    for (auto index : indices(tuple.getElementTypes()))
      if (!isPOD(type.getTupleElementType(index), scope))
        return false;
    return true;
  }
  return getTypeInfo(type).isPOD(scope);
}


namespace {
  struct ClassifyTypeSize : CanTypeVisitor<ClassifyTypeSize, ObjectSize> {
    IRGenModule &IGM;
    ResilienceScope Scope;
    ClassifyTypeSize(IRGenModule &IGM, ResilienceScope scope)
      : IGM(IGM), Scope(scope) {}

#define ALWAYS(KIND, RESULT) \
    ObjectSize visit##KIND##Type(KIND##Type *t) { return ObjectSize::RESULT; }

    ALWAYS(Builtin, Fixed)
    ALWAYS(SILFunction, Fixed)
    ALWAYS(Class, Fixed)
    ALWAYS(BoundGenericClass, Fixed)
    ALWAYS(Protocol, Fixed)
    ALWAYS(ProtocolComposition, Fixed)
    ALWAYS(LValue, Dependent)
#undef ALWAYS
    
    ObjectSize visitArchetypeType(CanArchetypeType archetype) {
      if (archetype->requiresClass())
        return ObjectSize::Fixed;
      return ObjectSize::Dependent;
    }
    
    ObjectSize visitTupleType(CanTupleType tuple) {
      ObjectSize result = ObjectSize::Fixed;
      for (auto eltType : tuple.getElementTypes()) {
        result = std::max(result, visit(eltType));
      }
      return result;
    }

    ObjectSize visitArrayType(CanArrayType array) {
      return visit(array.getBaseType());
    }

    ObjectSize visitStructType(CanStructType type) {
      if (type->getDecl()->getGenericParamsOfContext())
        return visitGenericStructType(type, type->getDecl());
      if (IGM.isResilient(type->getDecl(), Scope))
        return ObjectSize::Resilient;
      return ObjectSize::Fixed;
    }

    ObjectSize visitBoundGenericStructType(CanBoundGenericStructType type) {
      return visitGenericStructType(type, type->getDecl());
    }

    ObjectSize visitGenericStructType(CanType type, StructDecl *D) {
      assert(D->getGenericParamsOfContext());

      // If a generic struct is resilient, we have to assume that any
      // unknown fields might be dependently-sized.
      if (IGM.isResilient(D, Scope))
        return ObjectSize::Dependent;

      auto structType = SILType::getPrimitiveAddressType(type);

      ObjectSize result = ObjectSize::Fixed;
      for (auto field : D->getStoredProperties()) {
        auto fieldType = structType.getFieldType(field, *IGM.SILMod);
        result = std::max(result, visitSILType(fieldType));
      }
      return result;
    }

    ObjectSize visitEnumType(CanEnumType type) {
      if (type->getDecl()->getGenericParamsOfContext())
        return visitGenericEnumType(type, type->getDecl());
      if (IGM.isResilient(type->getDecl(), Scope))
        return ObjectSize::Resilient;
      return ObjectSize::Fixed;
    }

    ObjectSize visitBoundGenericEnumType(CanBoundGenericEnumType type) {
      return visitGenericEnumType(type, type->getDecl());
    }

    ObjectSize visitGenericEnumType(CanType type, EnumDecl *D) {
      assert(D->getGenericParamsOfContext());

      // If a generic enum is resilient, we have to assume that any
      // unknown elements might be dependently-sized.
      if (IGM.isResilient(D, Scope))
        return ObjectSize::Dependent;

      auto enumType = SILType::getPrimitiveAddressType(type);

      ObjectSize result = ObjectSize::Fixed;
      for (auto elt : D->getAllElements()) {
        if (!elt->hasArgumentType()) continue;
        auto eltType = enumType.getEnumElementType(elt, *IGM.SILMod);
        result = std::max(result, visitSILType(eltType));
      }
      return result;
    }

    ObjectSize visitType(CanType type) {
      return ObjectSize::Fixed;
    }

    ObjectSize visitSILType(SILType type) {
      return visit(type.getSwiftRValueType());
    }
  };
}

ObjectSize IRGenModule::classifyTypeSize(SILType type, ResilienceScope scope) {
  return ClassifyTypeSize(*this, scope).visitSILType(type);
}

llvm::BitVector IRGenModule::getSpareBitsForType(llvm::Type *scalarTy) {
  if (SpareBitsForTypes.count(scalarTy))
    return SpareBitsForTypes[scalarTy];
  
  {
    // FIXME: Currently we only implement spare bits for single-element
    // primitive integer types.
    while (auto structTy = dyn_cast<llvm::StructType>(scalarTy)) {
      if (structTy->getNumElements() != 1)
        goto no_spare_bits;
      scalarTy = structTy->getElementType(0);
    }

    auto *intTy = dyn_cast<llvm::IntegerType>(scalarTy);
    if (!intTy)
      goto no_spare_bits;

    // Round Integer-Of-Unusual-Size types up to their allocation size according
    // to the target data layout.
    unsigned allocBits = DataLayout.getTypeAllocSizeInBits(intTy);
    assert(allocBits >= intTy->getBitWidth());
    // Integer types get rounded up to the next power-of-two size in our layout,
    // so non-power-of-two integer types get spare bits up to that power of two.
    if (allocBits == intTy->getBitWidth())
      goto no_spare_bits;
        
    // FIXME: Endianness.
    llvm::BitVector &result = SpareBitsForTypes[scalarTy];
    result.resize(intTy->getBitWidth(), false);
    result.resize(allocBits, true);
    return result;
  }
  
no_spare_bits:
  SpareBitsForTypes[scalarTy] = {};
  return {};
}

unsigned IRGenModule::getBuiltinIntegerWidth(BuiltinIntegerType *t) {
  return getBuiltinIntegerWidth(t->getWidth());
}

unsigned IRGenModule::getBuiltinIntegerWidth(BuiltinIntegerWidth w) {
  if (w.isFixedWidth())
    return w.getFixedWidth();
  if (w.isPointerWidth())
    return getPointerSize().getValueInBits();
  llvm_unreachable("impossible width value");
}

#ifndef NDEBUG
CanType TypeConverter::getTypeThatLoweredTo(llvm::Type *t) const {
  for (auto &mapping : Types.IndependentCache) {
    if (auto fwd = mapping.second.dyn_cast<llvm::Type*>())
      if (fwd == t)
        return CanType(mapping.first);
    if (auto *ti = mapping.second.dyn_cast<const TypeInfo *>())
      if (ti->getStorageType() == t)
        return CanType(mapping.first);
  }
  return CanType();
}

bool TypeConverter::isExemplarArchetype(ArchetypeType *arch) const {
  for (auto &ea : Types.ExemplarArchetypeStorage)
    if (ea.Archetype == arch) return true;
  return false;
}

#endif
