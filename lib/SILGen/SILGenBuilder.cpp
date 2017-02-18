//===--- SILGenBuilder.cpp ------------------------------------------------===//
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

#include "SILGenBuilder.h"
#include "SILGenFunction.h"

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                              Utility Methods
//===----------------------------------------------------------------------===//

SILGenModule &SILGenBuilder::getSILGenModule() const { return gen.SGM; }

//===----------------------------------------------------------------------===//
//                                Constructors
//===----------------------------------------------------------------------===//

SILGenBuilder::SILGenBuilder(SILGenFunction &gen)
    : SILBuilder(gen.F), gen(gen) {}

SILGenBuilder::SILGenBuilder(SILGenFunction &gen, SILBasicBlock *insertBB)
    : SILBuilder(insertBB), gen(gen) {}

SILGenBuilder::SILGenBuilder(SILGenFunction &gen, SILBasicBlock *insertBB,
                             SmallVectorImpl<SILInstruction *> *insertedInsts)
    : SILBuilder(insertBB, insertedInsts), gen(gen) {}

SILGenBuilder::SILGenBuilder(SILGenFunction &gen, SILBasicBlock *insertBB,
                             SILBasicBlock::iterator insertInst)
    : SILBuilder(insertBB, insertInst), gen(gen) {}

//===----------------------------------------------------------------------===//
//                            Instruction Emission
//===----------------------------------------------------------------------===//

MetatypeInst *SILGenBuilder::createMetatype(SILLocation loc, SILType metatype) {
  auto theMetatype = metatype.castTo<MetatypeType>();
  // Getting a nontrivial metatype requires forcing any conformances necessary
  // to instantiate the type.
  switch (theMetatype->getRepresentation()) {
  case MetatypeRepresentation::Thin:
    break;
  case MetatypeRepresentation::Thick:
  case MetatypeRepresentation::ObjC: {
    // Walk the type recursively to look for substitutions we may need.
    theMetatype.getInstanceType().findIf([&](Type t) -> bool {
      if (!t->getAnyNominal())
        return false;

      auto subs =
          t->gatherAllSubstitutions(getSILGenModule().SwiftModule, nullptr);
      getSILGenModule().useConformancesFromSubstitutions(subs);
      return false;
    });

    break;
  }
  }

  return SILBuilder::createMetatype(loc, metatype);
}

ApplyInst *SILGenBuilder::createApply(SILLocation loc, SILValue fn,
                                      SILType substFnTy, SILType result,
                                      SubstitutionList subs,
                                      ArrayRef<SILValue> args) {
  getSILGenModule().useConformancesFromSubstitutions(subs);
  return SILBuilder::createApply(loc, fn, substFnTy, result, subs, args, false);
}

TryApplyInst *
SILGenBuilder::createTryApply(SILLocation loc, SILValue fn, SILType substFnTy,
                              SubstitutionList subs, ArrayRef<SILValue> args,
                              SILBasicBlock *normalBB, SILBasicBlock *errorBB) {
  getSILGenModule().useConformancesFromSubstitutions(subs);
  return SILBuilder::createTryApply(loc, fn, substFnTy, subs, args, normalBB,
                                    errorBB);
}

PartialApplyInst *
SILGenBuilder::createPartialApply(SILLocation loc, SILValue fn,
                                  SILType substFnTy, SubstitutionList subs,
                                  ArrayRef<SILValue> args, SILType closureTy) {
  getSILGenModule().useConformancesFromSubstitutions(subs);
  return SILBuilder::createPartialApply(loc, fn, substFnTy, subs, args,
                                        closureTy);
}

BuiltinInst *SILGenBuilder::createBuiltin(SILLocation loc, Identifier name,
                                          SILType resultTy,
                                          SubstitutionList subs,
                                          ArrayRef<SILValue> args) {
  getSILGenModule().useConformancesFromSubstitutions(subs);
  return SILBuilder::createBuiltin(loc, name, resultTy, subs, args);
}

InitExistentialAddrInst *SILGenBuilder::createInitExistentialAddr(
    SILLocation loc, SILValue existential, CanType formalConcreteType,
    SILType loweredConcreteType,
    ArrayRef<ProtocolConformanceRef> conformances) {
  for (auto conformance : conformances)
    getSILGenModule().useConformance(conformance);

  return SILBuilder::createInitExistentialAddr(
      loc, existential, formalConcreteType, loweredConcreteType, conformances);
}

InitExistentialMetatypeInst *SILGenBuilder::createInitExistentialMetatype(
    SILLocation loc, SILValue metatype, SILType existentialType,
    ArrayRef<ProtocolConformanceRef> conformances) {
  for (auto conformance : conformances)
    getSILGenModule().useConformance(conformance);

  return SILBuilder::createInitExistentialMetatype(
      loc, metatype, existentialType, conformances);
}

InitExistentialRefInst *SILGenBuilder::createInitExistentialRef(
    SILLocation loc, SILType existentialType, CanType formalConcreteType,
    SILValue concreteValue, ArrayRef<ProtocolConformanceRef> conformances) {
  for (auto conformance : conformances)
    getSILGenModule().useConformance(conformance);

  return SILBuilder::createInitExistentialRef(
      loc, existentialType, formalConcreteType, concreteValue, conformances);
}

AllocExistentialBoxInst *SILGenBuilder::createAllocExistentialBox(
    SILLocation loc, SILType existentialType, CanType concreteType,
    ArrayRef<ProtocolConformanceRef> conformances) {
  for (auto conformance : conformances)
    getSILGenModule().useConformance(conformance);

  return SILBuilder::createAllocExistentialBox(loc, existentialType,
                                               concreteType, conformances);
}

ManagedValue SILGenBuilder::createStructExtract(SILLocation loc,
                                                ManagedValue base,
                                                VarDecl *decl) {
  ManagedValue borrowedBase = gen.emitManagedBeginBorrow(loc, base.getValue());
  SILValue extract =
      SILBuilder::createStructExtract(loc, borrowedBase.getValue(), decl);
  return ManagedValue::forUnmanaged(extract);
}

ManagedValue SILGenBuilder::createCopyValue(SILLocation loc,
                                            ManagedValue originalValue) {
  SILValue result = SILBuilder::createCopyValue(loc, originalValue.getValue());
  return gen.emitManagedRValueWithCleanup(result);
}

ManagedValue SILGenBuilder::createCopyUnownedValue(SILLocation loc,
                                                   ManagedValue originalValue) {
  auto unownedType = originalValue.getType().castTo<UnownedStorageType>();
  assert(unownedType->isLoadable(ResilienceExpansion::Maximal));
  (void)unownedType;

  SILValue result =
      SILBuilder::createCopyUnownedValue(loc, originalValue.getValue());
  return gen.emitManagedRValueWithCleanup(result);
}

ManagedValue
SILGenBuilder::createUnsafeCopyUnownedValue(SILLocation loc,
                                            ManagedValue originalValue) {
  auto unmanagedType = originalValue.getType().getAs<UnmanagedStorageType>();
  SILValue result = SILBuilder::createUnmanagedToRef(
      loc, originalValue.getValue(),
      SILType::getPrimitiveObjectType(unmanagedType.getReferentType()));
  SILBuilder::createUnmanagedRetainValue(loc, result);
  return gen.emitManagedRValueWithCleanup(result);
}

ManagedValue SILGenBuilder::createOwnedPHIArgument(SILType type) {
  SILPHIArgument *arg =
      getInsertionBB()->createPHIArgument(type, ValueOwnershipKind::Owned);
  return gen.emitManagedRValueWithCleanup(arg);
}

ManagedValue SILGenBuilder::createAllocRef(
    SILLocation loc, SILType refType, bool objc, bool canAllocOnStack,
    ArrayRef<SILType> inputElementTypes,
    ArrayRef<ManagedValue> inputElementCountOperands) {
  llvm::SmallVector<SILType, 8> elementTypes(inputElementTypes.begin(),
                                             inputElementTypes.end());
  llvm::SmallVector<SILValue, 8> elementCountOperands;
  std::transform(std::begin(inputElementCountOperands),
                 std::end(inputElementCountOperands),
                 std::back_inserter(elementCountOperands),
                 [](ManagedValue mv) -> SILValue { return mv.getValue(); });

  AllocRefInst *i = SILBuilder::createAllocRef(
      loc, refType, objc, canAllocOnStack, elementTypes, elementCountOperands);
  return gen.emitManagedRValueWithCleanup(i);
}

ManagedValue SILGenBuilder::createAllocRefDynamic(
    SILLocation loc, ManagedValue operand, SILType refType, bool objc,
    ArrayRef<SILType> inputElementTypes,
    ArrayRef<ManagedValue> inputElementCountOperands) {
  llvm::SmallVector<SILType, 8> elementTypes(inputElementTypes.begin(),
                                             inputElementTypes.end());
  llvm::SmallVector<SILValue, 8> elementCountOperands;
  std::transform(std::begin(inputElementCountOperands),
                 std::end(inputElementCountOperands),
                 std::back_inserter(elementCountOperands),
                 [](ManagedValue mv) -> SILValue { return mv.getValue(); });

  AllocRefDynamicInst *i =
      SILBuilder::createAllocRefDynamic(loc, operand.getValue(), refType, objc,
                                        elementTypes, elementCountOperands);
  return gen.emitManagedRValueWithCleanup(i);
}

ManagedValue SILGenBuilder::createTupleExtract(SILLocation loc,
                                               ManagedValue base,
                                               unsigned index, SILType type) {
  ManagedValue borrowedBase = gen.emitManagedBeginBorrow(loc, base.getValue());
  SILValue extract =
      SILBuilder::createTupleExtract(loc, borrowedBase.getValue(), index, type);
  return ManagedValue::forUnmanaged(extract);
}

ManagedValue SILGenBuilder::createTupleExtract(SILLocation loc,
                                               ManagedValue value,
                                               unsigned index) {
  SILType type = value.getType().getTupleElementType(index);
  return createTupleExtract(loc, value, index, type);
}

ManagedValue SILGenBuilder::createLoadBorrow(SILLocation loc,
                                             ManagedValue base) {
  if (getFunction().getTypeLowering(base.getType()).isTrivial()) {
    auto *i = SILBuilder::createLoad(loc, base.getValue(),
                                     LoadOwnershipQualifier::Trivial);
    return ManagedValue::forUnmanaged(i);
  }

  auto *i = SILBuilder::createLoadBorrow(loc, base.getValue());
  return gen.emitManagedBorrowedRValueWithCleanup(base.getValue(), i);
}

ManagedValue SILGenBuilder::createFormalAccessLoadBorrow(SILLocation loc,
                                                         ManagedValue base) {
  if (getFunction().getTypeLowering(base.getType()).isTrivial()) {
    auto *i = SILBuilder::createLoad(loc, base.getValue(),
                                     LoadOwnershipQualifier::Trivial);
    return ManagedValue::forUnmanaged(i);
  }

  SILValue baseValue = base.getValue();
  auto *i = SILBuilder::createLoadBorrow(loc, baseValue);
  return gen.emitFormalEvaluationManagedBorrowedRValueWithCleanup(loc,
                                                                  baseValue, i);
}
