//===---- SILCombinerVisitors.cpp - SILCombiner Visitor Impl -*- C++ -*----===//
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

#define DEBUG_TYPE "sil-combine"
#include "SILCombiner.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILAnalysis/AliasAnalysis.h"
#include "swift/SILAnalysis/ValueTracking.h"
#include "swift/SILPasses/Utils/Local.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

using namespace swift;
using namespace swift::PatternMatch;

SILInstruction *SILCombiner::visitStructExtractInst(StructExtractInst *SEI) {
  // If our operand has archetypes or our field is not trivial, do not do
  // anything.
  SILValue Op = SEI->getOperand();
  SILType OpType = Op.getType();
  if (OpType.hasArchetype() || OpType.isTrivial(SEI->getModule()))
    return nullptr;

  // (struct_extract (unchecked_ref_bit_cast X->Y x) #z)
  //    ->
  // (unchecked_ref_bit_cast X->Z x)
  //
  // Where #z is a Z typed field of single field struct Y.
  auto *URBCI = dyn_cast<UncheckedRefBitCastInst>(Op);
  if (!URBCI)
    return nullptr;

  // If we only have one stored property, then we are layout compatible with
  // that property and can perform the operation.
  StructDecl *S = SEI->getStructDecl();
  auto R = S->getStoredProperties();
  auto B = R.begin();
  if (B == R.end())
    return nullptr;
  ++B;
  if (B != R.end())
    return nullptr;

  return new (SEI->getModule()) UncheckedRefBitCastInst(SEI->getLoc(),
                                                        URBCI->getOperand(),
                                                        SEI->getType());
}

static bool isFirstPayloadedCase(EnumDecl *E, EnumElementDecl *Elt) {
  for (EnumElementDecl *Iter : E->getAllElements())
    if (Iter->hasArgumentType())
      return Iter == Elt;
  return false;
}

SILInstruction *
SILCombiner::
visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI) {
  // First to be safe, do not perform this optimization on unchecked_enum_data
  // on bounded generic nominal types.
  SILValue Op = UEDI->getOperand();
  SILType OpType = Op.getType();
  if (OpType.hasArchetype() || OpType.isTrivial(UEDI->getModule()))
    return nullptr;

  // (unchecked_enum_data (unchecked_ref_bit_cast X->Y x) #z)
  //    ->
  // (unchecked_ref_bit_cast X->Z x)
  //
  // Where #z is the payload of type Z of the first payloaded case of the enum
  // Y.
  auto *URBCI = dyn_cast<UncheckedRefBitCastInst>(Op);
  if (!URBCI)
    return nullptr;

  // A UEDI performs a layout compatible operation if it is extracting the first
  // argument case of the enum.
  EnumDecl *E = OpType.getEnumOrBoundGenericEnum();
  if (!isFirstPayloadedCase(E, UEDI->getElement()))
    return nullptr;

  return new (UEDI->getModule()) UncheckedRefBitCastInst(UEDI->getLoc(),
                                                         URBCI->getOperand(),
                                                         UEDI->getType());
}

SILInstruction *SILCombiner::visitSwitchEnumAddrInst(SwitchEnumAddrInst *SEAI) {
  // Promote switch_enum_addr to switch_enum if the enum is loadable.
  //   switch_enum_addr %ptr : $*Optional<SomeClass>, case ...
  //     ->
  //   %value = load %ptr
  //   switch_enum %value
  SILType Ty = SEAI->getOperand().getType();
  if (!Ty.isLoadable(SEAI->getModule()))
    return nullptr;

  SmallVector<std::pair<EnumElementDecl*, SILBasicBlock*>, 8> Cases;
  for (int i = 0, e = SEAI->getNumCases(); i < e; ++i)
    Cases.push_back(SEAI->getCase(i));


  SILBasicBlock *Default = SEAI->hasDefault() ? SEAI->getDefaultBB() : 0;
  LoadInst *EnumVal = Builder->createLoad(SEAI->getLoc(), SEAI->getOperand());
  EnumVal->setDebugScope(SEAI->getDebugScope());
  Builder->createSwitchEnum(SEAI->getLoc(), EnumVal, Default, Cases)
    ->setDebugScope(SEAI->getDebugScope());
  return eraseInstFromFunction(*SEAI);
}

SILInstruction *SILCombiner::visitSelectEnumAddrInst(SelectEnumAddrInst *SEAI) {
  // Promote select_enum_addr to select_enum if the enum is loadable.
  //   = select_enum_addr %ptr : $*Optional<SomeClass>, case ...
  //     ->
  //   %value = load %ptr
  //   = select_enum %value
  SILType Ty = SEAI->getEnumOperand().getType();
  if (!Ty.isLoadable(SEAI->getModule()))
    return nullptr;

  SmallVector<std::pair<EnumElementDecl*, SILValue>, 8> Cases;
  for (int i = 0, e = SEAI->getNumCases(); i < e; ++i)
    Cases.push_back(SEAI->getCase(i));

  SILValue Default = SEAI->hasDefault() ? SEAI->getDefaultResult() : SILValue();
  LoadInst *EnumVal = Builder->createLoad(SEAI->getLoc(),
                                          SEAI->getEnumOperand());
  EnumVal->setDebugScope(SEAI->getDebugScope());
  auto *I = SelectEnumInst::create(SEAI->getLoc(), EnumVal, SEAI->getType(),
                                   Default, Cases,
                                   *SEAI->getFunction());
  I->setDebugScope(SEAI->getDebugScope());
  return I;
}

SILInstruction *SILCombiner::visitSelectValueInst(SelectValueInst *SVI) {
  return nullptr;
}

SILInstruction *SILCombiner::visitSwitchValueInst(SwitchValueInst *SVI) {
  return nullptr;
}
SILInstruction *SILCombiner::visitAllocStackInst(AllocStackInst *AS) {
  // init_existential instructions behave like memory allocation within
  // the allocated object. We can promote the init_existential allocation
  // into a dedicated allocation.

  // Detect this pattern
  // %0 = alloc_stack $LogicValue
  // %1 = init_existential %0#1 : $*LogicValue, $*Bool
  // ...
  // use of %1
  // ...
  // destroy_addr %0#1 : $*LogicValue
  // dealloc_stack %0#0 : $*@local_storage LogicValue
  bool LegalUsers = true;
  InitExistentialInst *IEI = nullptr;
  // Scan all of the uses of the AllocStack and check if it is not used for
  // anything other than the init_existential container.
  for (Operand *Op: AS->getUses()) {
    // Destroy and dealloc are both fine.
    if (isa<DestroyAddrInst>(Op->getUser()) ||
        isa<DeallocStackInst>(Op->getUser()))
      continue;

    // Make sure there is exactly one init_existential.
    if (auto *I = dyn_cast<InitExistentialInst>(Op->getUser())) {
      if (IEI) {
        LegalUsers = false;
        break;
      }
      IEI = I;
      continue;
    }

    // All other instructions are illegal.
    LegalUsers = false;
    break;
  }

  // Save the original insertion point.
  auto OrigInsertionPoint = Builder->getInsertionPoint();

  // If the only users of the alloc_stack are alloc, destroy and
  // init_existential then we can promote the allocation of the init
  // existential.
  if (LegalUsers && IEI) {
    auto *ConcAlloc = Builder->createAllocStack(AS->getLoc(),
                                                IEI->getLoweredConcreteType());
    ConcAlloc->setDebugScope(AS->getDebugScope());
    SILValue(IEI, 0).replaceAllUsesWith(ConcAlloc->getAddressResult());
    eraseInstFromFunction(*IEI);


    for (Operand *Op: AS->getUses()) {
      if (auto *DA = dyn_cast<DestroyAddrInst>(Op->getUser())) {
        Builder->setInsertionPoint(DA);
        Builder->createDestroyAddr(DA->getLoc(), SILValue(ConcAlloc, 1))
          ->setDebugScope(DA->getDebugScope());
        eraseInstFromFunction(*DA);

      }
      if (auto *DS = dyn_cast<DeallocStackInst>(Op->getUser())) {
        Builder->setInsertionPoint(DS);
        Builder->createDeallocStack(DS->getLoc(), SILValue(ConcAlloc, 0))
          ->setDebugScope(DS->getDebugScope());
        eraseInstFromFunction(*DS);
      }
    }

    eraseInstFromFunction(*AS);
    // Restore the insertion point.
    Builder->setInsertionPoint(OrigInsertionPoint);
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitLoadInst(LoadInst *LI) {
  // (load (upcast-ptr %x)) -> (upcast-ref (load %x))
  if (auto *UI = dyn_cast<UpcastInst>(LI->getOperand())) {
    auto NewLI = Builder->createLoad(LI->getLoc(), UI->getOperand());
    NewLI->setDebugScope(LI->getDebugScope());
    return new (UI->getModule()) UpcastInst(LI->getLoc(), NewLI,
                                            LI->getType());
  }

  // Given a load with multiple struct_extracts/tuple_extracts and no other
  // uses, canonicalize the load into several (struct_element_addr (load))
  // pairs.
  using ProjInstPairTy = std::pair<Projection, SILInstruction *>;

  // Go through the loads uses and add any users that are projections to the
  // projection list.
  llvm::SmallVector<ProjInstPairTy, 8> Projections;
  for (auto *UI : LI->getUses()) {
    auto *User = UI->getUser();

    // If we have any non SEI, TEI instruction, don't do anything here.
    if (!isa<StructExtractInst>(User) && !isa<TupleExtractInst>(User))
      return nullptr;

    auto P = Projection::valueProjectionForInstruction(User);
    Projections.push_back({P.getValue(), User});
  }

  // The reason why we sort the list is so that we will process projections with
  // the same value decl and tuples with the same indices together. This makes
  // it easy to reuse the load from the first such projection for all subsequent
  // projections on the same value decl or index.
  std::sort(Projections.begin(), Projections.end());

  // Go through our sorted list creating new GEPs only when we need to.
  Projection *LastProj = nullptr;
  LoadInst *LastNewLoad = nullptr;
  for (auto &Pair : Projections) {
    auto &Proj = Pair.first;
    auto *Inst = Pair.second;

    // If this projection is the same as the last projection we processed, just
    // replace all uses of the projection with the load we created previously.
    if (LastProj && Proj == *LastProj) {
      replaceInstUsesWith(*Inst, LastNewLoad, 0);
      eraseInstFromFunction(*Inst);
      continue;
    }

    // Ok, we have started to visit the range of instructions associated with
    // a new projection. Create the new address projection.
    auto I = Proj.createAddrProjection(*Builder, LI->getLoc(), LI->getOperand());
    LastProj = &Proj;
    I.get()->setDebugScope(LI->getDebugScope());
    LastNewLoad = Builder->createLoad(LI->getLoc(), I.get());
    LastNewLoad->setDebugScope(LI->getDebugScope());
    replaceInstUsesWith(*Inst, LastNewLoad, 0);
    eraseInstFromFunction(*Inst);
  }

  // Erase the old load.
  return eraseInstFromFunction(*LI);
}

SILInstruction *SILCombiner::visitReleaseValueInst(ReleaseValueInst *RVI) {
  SILValue Operand = RVI->getOperand();
  SILType OperandTy = Operand.getType();

  // Destroy value of an enum with a trivial payload or no-payload is a no-op.
  if (auto *EI = dyn_cast<EnumInst>(Operand)) {
    if (!EI->hasOperand() ||
        EI->getOperand().getType().isTrivial(EI->getModule()))
      return eraseInstFromFunction(*RVI);

    // retain_value of an enum_inst where we know that it has a payload can be
    // reduced to a retain_value on the payload.
    if (EI->hasOperand()) {
      return new (RVI->getModule()) ReleaseValueInst(RVI->getLoc(),
                                                     EI->getOperand());
    }
  }

  // ReleaseValueInst of a reference type is a strong_release.
  if (OperandTy.hasReferenceSemantics())
    return new (RVI->getModule()) StrongReleaseInst(RVI->getLoc(), Operand);

  // ReleaseValueInst of a trivial type is a no-op.
  if (OperandTy.isTrivial(RVI->getModule()))
    return eraseInstFromFunction(*RVI);

  // Do nothing for non-trivial non-reference types.
  return nullptr;
}

SILInstruction *SILCombiner::visitRetainValueInst(RetainValueInst *RVI) {
  SILValue Operand = RVI->getOperand();
  SILType OperandTy = Operand.getType();

  // retain_value of an enum with a trivial payload or no-payload is a no-op +
  // RAUW.
  if (auto *EI = dyn_cast<EnumInst>(Operand)) {
    if (!EI->hasOperand() ||
        EI->getOperand().getType().isTrivial(RVI->getModule())) {
      return eraseInstFromFunction(*RVI);
    }

    // retain_value of an enum_inst where we know that it has a payload can be
    // reduced to a retain_value on the payload.
    if (EI->hasOperand()) {
      return new (RVI->getModule()) RetainValueInst(RVI->getLoc(),
                                                    EI->getOperand());
    }
  }

  // RetainValueInst of a reference type is a strong_release.
  if (OperandTy.hasReferenceSemantics()) {
    return new (RVI->getModule()) StrongRetainInst(RVI->getLoc(), Operand);
  }

  // RetainValueInst of a trivial type is a no-op + use propogation.
  if (OperandTy.isTrivial(RVI->getModule())) {
    return eraseInstFromFunction(*RVI);
  }

  // Sometimes in the stdlib due to hand offs, we will see code like:
  //
  // release_value %0
  // retain_value %0
  //
  // with the matching retain_value to the release_value in a predecessor basic
  // block and the matching release_value for the retain_value_retain in a
  // successor basic block.
  //
  // Due to the matching pairs being in different basic blocks, the ARC
  // Optimizer (which is currently local to one basic block does not handle
  // it). But that does not mean that we can not eliminate this pair with a
  // peephole.

  // If we are not the first instruction in this basic block...
  if (RVI != &*RVI->getParent()->begin()) {
    SILBasicBlock::iterator Pred = RVI;
    --Pred;

    // ...and the predecessor instruction is a release_value on the same value
    // as our retain_value...
    if (ReleaseValueInst *Release = dyn_cast<ReleaseValueInst>(&*Pred))
      // Remove them...
      if (Release->getOperand() == RVI->getOperand()) {
        eraseInstFromFunction(*Release);
        return eraseInstFromFunction(*RVI);
      }
  }

  return nullptr;
}

/// Returns the post-dominating release of a series of cancelling
/// retain/releases on the partial apply if there are no other users than the
/// retain/release.
/// Currently, this only handles the case where all retain/releases are in the
/// same basic block.
static StrongReleaseInst *
hasOnlyRetainReleaseUsers(PartialApplyInst *PAI,
                          SmallVectorImpl<RefCountingInst *> &RCsToDelete) {
  SILBasicBlock *BB = nullptr;
  SmallPtrSet<RefCountingInst *, 16> RCs;

  // Collect all reference counting users.
  for (auto *Opd : PAI->getUses()) {

    // Reference counting instruction.
    if (auto *RCounting = dyn_cast<RefCountingInst>(Opd->getUser())) {
      if (!isa<StrongRetainInst>(RCounting) &&
          !isa<StrongReleaseInst>(RCounting))
        return nullptr;

      RCs.insert(RCounting);
      // Check that we are in the same BB (we don't handle any multi BB case).
      if (!BB)
        BB = RCounting->getParent();
      else if (BB != RCounting->getParent())
        return nullptr;
    } else
      return nullptr;
  }

  // Need to have a least one release.
  if (!BB)
    return nullptr;

  // Find the postdominating release. For now we only handle the single BB case.
  RefCountingInst *PostDom = nullptr;
  unsigned RetainCount = 0;
  unsigned ReleaseCount = 0;
  for (auto &Inst : *BB) {
    auto *RCounting = dyn_cast<RefCountingInst>(&Inst);
    if (!RCounting)
      continue;
    // One of the retain/releases on the partial apply.
    if (RCs.count(RCounting)) {
      PostDom = RCounting;
      RetainCount += isa<StrongRetainInst>(&Inst);
      ReleaseCount += isa<StrongReleaseInst>(&Inst);
    }
  }

  // The retain release count better match up.
  assert(RetainCount == (ReleaseCount - 1) && "Retain release mismatch!?");
  if (RetainCount != (ReleaseCount - 1))
    return nullptr;

  RCsToDelete.append(RCs.begin(), RCs.end());

  assert(isa<StrongReleaseInst>(PostDom) && "Post dominating retain?!");
  return dyn_cast<StrongReleaseInst>(PostDom);
}

SILInstruction *SILCombiner::visitPartialApplyInst(PartialApplyInst *PAI) {
  // partial_apply without any substitutions or arguments is just a
  // thin_to_thick_function.
  if (!PAI->hasSubstitutions() && (PAI->getNumArguments() == 0))
    return new (PAI->getModule()) ThinToThickFunctionInst(PAI->getLoc(),
                                                          PAI->getCallee(),
                                                          PAI->getType());

  // Delete dead closures of this form:
  //
  // %X = partial_apply %x(...)
  // BB:
  // strong_retain %X
  // strong_release %X
  // strong_release %X // <-- Post dominating release.

  SmallVector<RefCountingInst *, 16> RCToDelete;
  auto *PostDomRelease = hasOnlyRetainReleaseUsers(PAI, RCToDelete);
  if (!PostDomRelease)
    return nullptr;

  SILLocation Loc = PAI->getLoc();

  SILFunctionType *ClosureTy =
    dyn_cast<SILFunctionType>(PAI->getCallee().getType().getSwiftType());
  if (!ClosureTy)
    return nullptr;

  // Emit a destroy value for each captured closure argument.
  auto Params = ClosureTy->getParameters();
  auto Args = PAI->getArguments();
  unsigned Delta = Params.size() - Args.size();
  assert(Delta <= Params.size() && "Error, more Args to partial apply than "
                                   "params in its interface.");

  // Set the insertion point of the release_value to be that of the post
  // dominating release, which is the end of the lifetime of the partial_apply.
  auto OrigInsertPoint = Builder->getInsertionPoint();
  Builder->setInsertionPoint(PostDomRelease);

  for (unsigned AI = 0, AE = Args.size(); AI != AE; ++AI) {
    SILValue Arg = Args[AI];
    auto Param = Params[AI + Delta];

    if (!Param.isIndirect() && Param.isConsumed())
      if (!Arg.getType().isAddress())
        Builder->createReleaseValue(Loc, Arg)
          ->setDebugScope(PAI->getDebugScope());
  }

  // Reset the insert point.
  Builder->setInsertionPoint(OrigInsertPoint);

  // Delete the strong_release/retains.
  for (auto *RC : RCToDelete)
    eraseInstFromFunction(*RC);

  // Delete the partial_apply.
  return eraseInstFromFunction(*PAI);
}

SILInstruction *
SILCombiner::optimizeApplyOfPartialApply(ApplyInst *AI, PartialApplyInst *PAI) {
  // Don't handle generic applys.
  if (AI->hasSubstitutions())
    return nullptr;

  // Make sure that the substitution list of the PAI does not contain any
  // archetypes.
  ArrayRef<Substitution> Subs = PAI->getSubstitutions();
  for (Substitution S : Subs)
    if (S.getReplacement()->getCanonicalType()->hasArchetype())
      return nullptr;

  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(PAI->getCallee());
  if (!FRI)
    return nullptr;

  // Prepare the args.
  SmallVector<SILValue, 8> Args;
  // First the ApplyInst args.
  for (auto Op : AI->getArguments())
    Args.push_back(Op);
  // Next, the partial apply args.
  for (auto Op : PAI->getArguments())
    Args.push_back(Op);

  // The thunk that implements the partial apply calls the closure function
  // that expects all arguments to be consumed by the function. However, the
  // captured arguments are not arguments of *this* apply, so they are not
  // pre-incremented. When we combine the partial_apply and this apply into
  // a new apply we need to retain all of the closure non-address type
  // arguments.
  for (auto Arg : PAI->getArguments())
    if (!Arg.getType().isAddress())
      Builder->emitRetainValueOperation(PAI->getLoc(), Arg);

  SILFunction *F = FRI->getReferencedFunction();
  SILType FnType = F->getLoweredType();
  SILType ResultTy = F->getLoweredFunctionType()->getSILResult();
  if (!Subs.empty()) {
    FnType = FnType.substGenericArgs(PAI->getModule(), Subs);
    ResultTy = FnType.getAs<SILFunctionType>()->getSILResult();
  }

  ApplyInst *NAI = Builder->createApply(AI->getLoc(), FRI, FnType, ResultTy,
                                        Subs, Args,
                                 FRI->getReferencedFunction()->isTransparent());
  NAI->setDebugScope(AI->getDebugScope());

  // We also need to release the partial_apply instruction itself because it
  // is consumed by the apply_instruction.
  Builder->createStrongRelease(AI->getLoc(), PAI)
    ->setDebugScope(AI->getDebugScope());

  replaceInstUsesWith(*AI, NAI);
  return eraseInstFromFunction(*AI);
}

SILInstruction *SILCombiner::optimizeBuiltinCanBeObjCClass(BuiltinInst *BI) {
  assert(BI->hasSubstitutions() && "Expected substitutions for canBeClass");

  auto const &Subs = BI->getSubstitutions();
  assert((Subs.size() == 1) &&
         "Expected one substitution in call to canBeClass");

  auto Ty = Subs[0].getReplacement()->getCanonicalType();
  switch (Ty->canBeClass()) {
  case TypeTraitResult::IsNot:
    return IntegerLiteralInst::create(BI->getLoc(), BI->getType(),
                                      APInt(8, 0), *BI->getFunction());
  case TypeTraitResult::Is:
    return IntegerLiteralInst::create(BI->getLoc(), BI->getType(),
                                      APInt(8, 1), *BI->getFunction());
  case TypeTraitResult::CanBe:
    return nullptr;
  }
}

SILInstruction *SILCombiner::optimizeBuiltinCompareEq(BuiltinInst *BI,
                                                      bool NegateResult) {
  IsZeroKind LHS = isZeroValue(BI->getArguments()[0]);
  IsZeroKind RHS = isZeroValue(BI->getArguments()[1]);

  // Can't handle unknown values.
  if (LHS == IsZeroKind::Unknown || RHS == IsZeroKind::Unknown)
    return nullptr;

  // Can't handle non-zero ptr values.
  if (LHS == IsZeroKind::NotZero && RHS == IsZeroKind::NotZero)
    return nullptr;

  // Set to true if both sides are zero. Set to false if only one side is zero.
  bool Val = (LHS == RHS) ^ NegateResult;

  return IntegerLiteralInst::create(BI->getLoc(), BI->getType(), APInt(1, Val),
                                    *BI->getFunction());
}

SILInstruction *
SILCombiner::optimizeApplyOfConvertFunctionInst(ApplyInst *AI,
                                                ConvertFunctionInst *CFI) {
  // We only handle simplification of static function references. If we don't
  // have one, bail.
  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(CFI->getOperand());
  if (!FRI)
    return nullptr;

  // Grab our relevant callee types...
  CanSILFunctionType SubstCalleeTy = AI->getSubstCalleeType();
  auto ConvertCalleeTy =
      CFI->getOperand().getType().castTo<SILFunctionType>();

  // ... and make sure they have no unsubstituted generics. If they do, bail.
  if (SubstCalleeTy->hasArchetype() || ConvertCalleeTy->hasArchetype())
    return nullptr;

  // Ok, we can now perform our transformation. Grab AI's operands and the
  // relevant types from the ConvertFunction function type and AI.
  OperandValueArrayRef Ops = AI->getArgumentsWithoutIndirectResult();
  auto OldOpTypes = SubstCalleeTy->getParameterSILTypes();
  auto NewOpTypes = ConvertCalleeTy->getParameterSILTypes();

  assert(Ops.size() == OldOpTypes.size() &&
         "Ops and op types must have same size.");
  assert(Ops.size() == NewOpTypes.size() &&
         "Ops and op types must have same size.");

  llvm::SmallVector<SILValue, 8> Args;
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    SILValue Op = Ops[i];
    SILType OldOpType = OldOpTypes[i];
    SILType NewOpType = NewOpTypes[i];

    // Convert function takes refs to refs, address to addresses, and leaves
    // other types alone.
    if (OldOpType.isAddress()) {
      assert(NewOpType.isAddress() && "Addresses should map to addresses.");
      auto UAC = Builder->createUncheckedAddrCast(AI->getLoc(), Op, NewOpType);
      UAC->setDebugScope(AI->getDebugScope());
      Args.push_back(UAC);
    } else if (OldOpType.isHeapObjectReferenceType()) {
      assert(NewOpType.isHeapObjectReferenceType() &&
             "refs should map to refs.");
      auto URC = Builder->createUncheckedRefCast(AI->getLoc(), Op, NewOpType);
      URC->setDebugScope(AI->getDebugScope());
      Args.push_back(URC);
    } else {
      Args.push_back(Op);
    }
  }

  SILType CCSILTy = SILType::getPrimitiveObjectType(ConvertCalleeTy);
  // Create the new apply inst.
  auto NAI = ApplyInst::create(AI->getLoc(), FRI, CCSILTy,
                               ConvertCalleeTy->getSILResult(),
                               ArrayRef<Substitution>(), Args, false,
                               *FRI->getReferencedFunction());
  NAI->setDebugScope(AI->getDebugScope());
  return NAI;
}

typedef SmallVector<SILInstruction*, 4> UserListTy;
/// \brief Returns a list of instructions that project or perform reference
/// counting operations on the instruction or its uses in argument \p Inst.
/// The function returns False if there are non-ARC instructions.
static bool recursivelyCollectARCUsers(UserListTy &Uses, SILInstruction *Inst) {
  Uses.push_back(Inst);
  for (auto Inst : Inst->getUses()) {
    if (isa<RefCountingInst>(Inst->getUser()) ||
        isa<DebugValueInst>(Inst->getUser())) {
      Uses.push_back(Inst->getUser());
      continue;
    }
    if (auto SI = dyn_cast<StructExtractInst>(Inst->getUser()))
      if (recursivelyCollectARCUsers(Uses, SI))
        continue;

    return false;
  }

  return true;
}



SILInstruction *
SILCombiner::optimizeConcatenationOfStringLiterals(ApplyInst *AI) {
  // String literals concatenation optimizer.
  StringConcatenationOptimizer SLConcatenationOptimizer(AI, Builder);
  return SLConcatenationOptimizer.optimize();
}

/// \brief Returns a list of instructions that only write into the uninitialized
/// array \p Inst.
static bool recursivelyCollectArrayWritesInstr(UserListTy &Uses,
                                               SILInstruction *Inst) {
  Uses.push_back(Inst);
  for (auto Op : Inst->getUses()) {
    if (isa<RefCountingInst>(Op->getUser()) ||
        // The store must not store the array but only to the array.
        (isa<StoreInst>(Op->getUser()) &&
         dyn_cast<StoreInst>(Op->getUser())->getSrc().getDef() != Inst) ||
        isa<DebugValueInst>(Op->getUser())) {
      Uses.push_back(Op->getUser());
      continue;
    }

    SILInstruction *Proj;
    if ((Proj = dyn_cast<TupleExtractInst>(Op->getUser())) ||
        (Proj = dyn_cast<StructExtractInst>(Op->getUser())) ||
        (Proj = dyn_cast<IndexAddrInst>(Op->getUser())) ||
        (Proj = dyn_cast<PointerToAddressInst>(Op->getUser())))
      if (recursivelyCollectArrayWritesInstr(Uses, Proj))
        continue;

    return false;
  }

  return true;
}

/// Optimize builtins which receive the same value in their first and second
/// operand.
static SILInstruction *optimizeBuiltinWithSameOperands(BuiltinInst *I,
                                                       SILCombiner *C) {
  SILFunction &F = *I->getFunction();

  // Handle all builtins which can be optimized.
  // We have to take special care about floating point operations because of
  // potential NaN values. E.g. ordered equal FCMP_OEQ(Nan, Nan) is not true.
  switch (I->getBuiltinInfo().ID) {
      
  // Replace the uses with one of the (identical) operands.
  case BuiltinValueKind::And:
  case BuiltinValueKind::Or: {
    // We cannot just _return_ the operand because it is not necessarily an
    // instruction. It can be an argument.
    SILValue Op = I->getOperand(0);
    C->replaceInstUsesWith(*I, Op.getDef(), 0, Op.getResultNumber());
    break;
  }

  // Return 0 or false.
  case BuiltinValueKind::Sub:
  case BuiltinValueKind::SRem:
  case BuiltinValueKind::URem:
  case BuiltinValueKind::Xor:
  case BuiltinValueKind::ICMP_NE:
  case BuiltinValueKind::ICMP_SLT:
  case BuiltinValueKind::ICMP_SGT:
  case BuiltinValueKind::ICMP_ULT:
  case BuiltinValueKind::ICMP_UGT:
  case BuiltinValueKind::FCMP_ONE:
    if (auto Ty = I->getType().getAs<BuiltinIntegerType>()) {
      return IntegerLiteralInst::create(I->getLoc(), I->getType(),
                                        APInt(Ty->getGreatestWidth(), 0), F);
    }
    break;
      
  // Return 1 or true.
  case BuiltinValueKind::ICMP_EQ:
  case BuiltinValueKind::ICMP_SLE:
  case BuiltinValueKind::ICMP_SGE:
  case BuiltinValueKind::ICMP_ULE:
  case BuiltinValueKind::ICMP_UGE:
  case BuiltinValueKind::FCMP_UEQ:
  case BuiltinValueKind::FCMP_UGE:
  case BuiltinValueKind::FCMP_ULE:
  case BuiltinValueKind::SDiv:
  case BuiltinValueKind::UDiv:
    if (auto Ty = I->getType().getAs<BuiltinIntegerType>()) {
      return IntegerLiteralInst::create(I->getLoc(), I->getType(),
                                        APInt(Ty->getGreatestWidth(), 1), F);
    }
    break;

  // Return 0 in a tuple.
  case BuiltinValueKind::SSubOver:
  case BuiltinValueKind::USubOver: {
    SILType Ty = I->getType();
    SILType IntTy = Ty.getTupleElementType(0);
    SILType BoolTy = Ty.getTupleElementType(1);
    SILBuilderWithScope<4> B(I);
    SILValue Elements[] = {
      B.createIntegerLiteral(I->getLoc(), IntTy, /* Result */ 0),
      B.createIntegerLiteral(I->getLoc(), BoolTy, /* Overflow */ 0)
    };
    return TupleInst::create(I->getLoc(), Ty, Elements, F);
  }
      
  default:
    break;
  }
  return nullptr;
}

SILInstruction *SILCombiner::visitBuiltinInst(BuiltinInst *I) {
  if (I->getBuiltinInfo().ID == BuiltinValueKind::CanBeObjCClass)
    return optimizeBuiltinCanBeObjCClass(I);

  if (I->getNumOperands() >= 2 && I->getOperand(0) == I->getOperand(1)) {
    // It's a builtin which has the same value in its first and second operand.
    SILInstruction *Replacement = optimizeBuiltinWithSameOperands(I, this);
    if (Replacement)
      return Replacement;
  }
  
  if (I->getBuiltinInfo().ID == BuiltinValueKind::ICMP_EQ)
    return optimizeBuiltinCompareEq(I, /*Negate Eq result*/ false);

  if (I->getBuiltinInfo().ID == BuiltinValueKind::ICMP_NE)
    return optimizeBuiltinCompareEq(I, /*Negate Eq result*/ true);

  // Optimize sub(ptrtoint(index_raw_pointer(v, x)), ptrtoint(v)) -> x.
  BuiltinInst *Bytes2;
  IndexRawPointerInst *Indexraw;
  if (I->getNumOperands() == 2 &&
      match(I, m_BuiltinInst(BuiltinValueKind::Sub,
                             m_BuiltinInst(BuiltinValueKind::PtrToInt,
                                           m_IndexRawPointerInst(Indexraw)),
                             m_BuiltinInst(Bytes2)))) {
    if (match(Bytes2,
              m_BuiltinInst(BuiltinValueKind::PtrToInt, m_ValueBase()))) {
      if (Indexraw->getOperand(0) == Bytes2->getOperand(0) &&
          Indexraw->getOperand(1).getType() == I->getType()) {
        replaceInstUsesWith(*I, Indexraw->getOperand(1).getDef());
        return eraseInstFromFunction(*I);
      }
    }
  }

  // Canonicalize multiplication by a stride to be such that the stride is
  // always the second argument.
  if (I->getNumOperands() != 3)
    return nullptr;

  if (match(I, m_ApplyInst(BuiltinValueKind::SMulOver,
                            m_ApplyInst(BuiltinValueKind::Strideof),
                            m_ValueBase(), m_IntegerLiteralInst())) ||
      match(I, m_ApplyInst(BuiltinValueKind::SMulOver,
                            m_ApplyInst(BuiltinValueKind::StrideofNonZero),
                            m_ValueBase(), m_IntegerLiteralInst()))) {
    I->swapOperands(0, 1);
    return I;
  }

  return nullptr;
}

/// Propagate information about a concrete type from init_existential
/// or init_existential_ref into witness_method conformances and into
/// apply instructions.
/// This helps the devirtualizer to replace witness_method by
/// class_method instructions and then devirtualize.
SILInstruction *
SILCombiner::propagateConcreteTypeOfInitExistential(ApplyInst *AI,
                                                    WitnessMethodInst *WMI,
                                                    SILValue InitExistential,
                                                    SILType InstanceType) {
  // Replace this witness_method by a more concrete one
  ArrayRef<ProtocolConformance*> Conformances;
  CanType ConcreteType;
  SILValue LastArg;

  if (auto IE = dyn_cast<InitExistentialInst>(InitExistential)) {
    Conformances = IE->getConformances();
    ConcreteType = IE->getFormalConcreteType();
    LastArg = IE;
  } else if (auto IER = dyn_cast<InitExistentialRefInst>(InitExistential)) {
    Conformances = IER->getConformances();
    ConcreteType = IER->getFormalConcreteType();
    LastArg = IER->getOperand();
  }

  auto ConcreteTypeSubsts = ConcreteType->gatherAllSubstitutions(
      AI->getModule().getSwiftModule(), nullptr);
  if (!ConcreteTypeSubsts.empty()) {
    // Bail if any generic types parameters of the concrete type are unbound.
    if (hasUnboundGenericTypes(ConcreteTypeSubsts))
      return nullptr;
    // At this point we know that all replacements use concrete types
    // and therefore the whole Lookup type is concrete. So, we can
    // propagate it, because we know how to devirtualize it.
  }


  if (Conformances.empty())
    return nullptr;

  // Find the conformance related to witness_method
  for (auto Conformance : Conformances) {
    if (Conformance->getProtocol() == WMI->getLookupProtocol()) {
      SmallVector<SILValue, 8> Args;
      for (auto Arg : AI->getArgumentsWithoutSelf()) {
        Args.push_back(Arg);
      }

      Args.push_back(LastArg);

      SILValue OptionalExistential =
          WMI->hasOperand() ? WMI->getOperand() : SILValue();
      auto *NewWMI = Builder->createWitnessMethod(
          WMI->getLoc(), ConcreteType, Conformance, WMI->getMember(),
          WMI->getType(), OptionalExistential, WMI->isVolatile());

      replaceInstUsesWith(*WMI, NewWMI, 0);
      eraseInstFromFunction(*WMI);

      SmallVector<Substitution, 8> Substitutions;
      for (auto Subst : AI->getSubstitutions()) {
        if (Subst.getArchetype()->isSelfDerived()) {
          Substitution NewSubst(Subst.getArchetype(), ConcreteType,
                                Subst.getConformances());
          Substitutions.push_back(NewSubst);
        } else
          Substitutions.push_back(Subst);
      }

      SILType SubstCalleeType = AI->getSubstCalleeSILType();

      SILType NewSubstCalleeType;

      auto FnTy = AI->getCallee().getType().getAs<SILFunctionType>();
      if (FnTy && FnTy->isPolymorphic())
        // Handle polymorphic functions by properly substituting
        // their parameter types.
        NewSubstCalleeType =
            SILType::getPrimitiveObjectType(FnTy->substGenericArgs(
                AI->getModule(), AI->getModule().getSwiftModule(),
                Substitutions));
      else {
        TypeSubstitutionMap TypeSubstitutions;
        TypeSubstitutions[InstanceType.getSwiftType().getPointer()] = ConcreteType;
        NewSubstCalleeType = SubstCalleeType.subst(
            AI->getModule(), AI->getModule().getSwiftModule(),
            TypeSubstitutions);
      }

      auto NewAI = Builder->createApply(
          AI->getLoc(), AI->getCallee(), NewSubstCalleeType,
          AI->getType(), Substitutions, Args, AI->isTransparent());

      replaceInstUsesWith(*AI, NewAI, 0);
      eraseInstFromFunction(*AI);

      return nullptr;
    }
  }

  return nullptr;
}

/// Optimize thin_func_to_ptr->ptr_to_thin_func casts into a type substituted
/// apply.
/// This kind of code arises in generic materializeForSet code that was
/// specialized for a concrete type.
///
/// Note: this is not as general as it should be. The general solution is the
/// introduction of a partial_apply_thin_recoverable (an instruction that
/// partially applies a type and returns a thin_function) as suggested in
/// SILGenBuiltin.cpp.
///
/// %208 = thin_function_to_pointer %207 :
///  $@thin <τ_0_0> (Builtin.RawPointer, @inout Builtin.UnsafeValueBuffer,
///                  @inout UnsafeMutableBufferPointer<τ_0_0>,
///                  @thick UnsafeMutableBufferPointer<τ_0_0>.Type) -> ()
///                  to $Builtin.RawPointer
/// %209 = pointer_to_thin_function %217 : $Builtin.RawPointer to
///  $@thin (Builtin.RawPointer, @inout Builtin.UnsafeValueBuffer,
///          @inout UnsafeMutableBufferPointer<Int>,
///          @thick UnsafeMutableBufferPointer<Int>.Type) -> ()
/// apply %209(%227, %200#1, %0, %224) : $@thin (Builtin.RawPointer,
///  @inout Builtin.UnsafeValueBuffer, @inout UnsafeMutableBufferPointer<Int>,
///  @thick UnsafeMutableBufferPointer<Int>.Type) -> ()
///
///  => apply %207<Int>(%227, ...)
static ApplyInst *optimizeCastThroughThinFuntionPointer(
    SILBuilder *Builder, ApplyInst *AI, FunctionRefInst *OrigThinFun,
    PointerToThinFunctionInst *CastedThinFun) {

  // The original function type needs to be polymorphic.
  auto ConvertCalleeTy = OrigThinFun->getType().castTo<SILFunctionType>();
  assert(ConvertCalleeTy);
  if (!ConvertCalleeTy->isPolymorphic())
    return nullptr;

  // Need to have four parameters.
  auto OrigParams = ConvertCalleeTy->getParameters();
  if (OrigParams.size() != 4)
    return nullptr;

  // There must only be one parameter to substitute.
  auto *ReferencedFunction = OrigThinFun->getReferencedFunction();
  assert(ReferencedFunction);
  if (ReferencedFunction->isExternalDeclaration())
    return nullptr;
  auto Params = ReferencedFunction->getContextGenericParams()->getParams();
  if (Params.size() != 1)
    return nullptr;

  // Get the concrete type from the casted to function.
  auto CastedFunTy = CastedThinFun->getType().castTo<SILFunctionType>();
  auto CastedParams = CastedFunTy->getParameters();
  if (CastedParams.size() != 4)
    return nullptr;

  // The fourth parameter is a metatype of a bound generic type. Use it to
  // obtain  the type substitutions to apply.
  auto MetaTy = dyn_cast<MetatypeType>(CastedParams[3].getType());
  if (!MetaTy)
    return nullptr;

  // Get the bound generic type from the metatype.
  auto BoundGenericInstTy = dyn_cast_or_null<BoundGenericType>(
      MetaTy->getInstanceType().getCanonicalTypeOrNull());
  if (!BoundGenericInstTy)
    return nullptr;

  // The bound generic type will carry the substitutions to apply.
  auto Subs = BoundGenericInstTy->getSubstitutions(
      AI->getModule().getSwiftModule(), nullptr);
  assert(Subs.size() == 1);

  SmallVector<SILValue, 16> Args;
  for (auto Arg : AI->getArguments())
    Args.push_back(Arg);

  auto NewSubstCalleeType =
      SILType::getPrimitiveObjectType(ConvertCalleeTy->substGenericArgs(
          AI->getModule(), AI->getModule().getSwiftModule(), Subs));

  ApplyInst *NewApply = Builder->createApply(
      AI->getLoc(), OrigThinFun, NewSubstCalleeType, AI->getType(), Subs, Args,
      OrigThinFun->getReferencedFunction()->isTransparent());
  NewApply->setDebugScope(AI->getDebugScope());

  return NewApply;
}

SILInstruction *SILCombiner::visitApplyInst(ApplyInst *AI) {
  // Optimize apply{partial_apply(x,y)}(z) -> apply(z,x,y).
  if (auto *PAI = dyn_cast<PartialApplyInst>(AI->getCallee()))
    return optimizeApplyOfPartialApply(AI, PAI);

  if (auto *CFI = dyn_cast<ConvertFunctionInst>(AI->getCallee()))
    return optimizeApplyOfConvertFunctionInst(AI, CFI);

  if (auto *CastedThinFun =
          dyn_cast<PointerToThinFunctionInst>(AI->getCallee()))
    if (auto *Ptr =
            dyn_cast<ThinFunctionToPointerInst>(CastedThinFun->getOperand()))
      if (auto *OrigThinFun = dyn_cast<FunctionRefInst>(Ptr->getOperand()))
        if (auto *NewAI = optimizeCastThroughThinFuntionPointer(
                Builder, AI, OrigThinFun, CastedThinFun)) {
          replaceInstUsesWith(*AI, NewAI, 0);
          eraseInstFromFunction(*AI);
          return nullptr;
        }

  // Optimize readonly functions with no meaningful users.
  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(AI->getCallee());
  if (FRI &&
      FRI->getReferencedFunction()->getEffectsKind() < EffectsKind::ReadWrite) {
    UserListTy Users;
    if (recursivelyCollectARCUsers(Users, AI)) {
      // When deleting Apply instructions make sure to release any owned
      // arguments.
      auto FT = FRI->getFunctionType();
      for (int i = 0, e = AI->getNumArguments(); i < e; ++i) {
        SILParameterInfo PI = FT->getParameters()[i];
        auto Arg = AI->getArgument(i);
        if (PI.isConsumed() && !Arg.getType().isAddress())
          Builder->emitReleaseValueOperation(AI->getLoc(), Arg);
      }

      // Erase all of the reference counting instructions and the Apply itself.
      for (auto rit = Users.rbegin(), re = Users.rend(); rit != re; ++rit)
        eraseInstFromFunction(**rit);

      return nullptr;
    }
    // We found a user that we can't handle.
  }

  if (FRI) {
    auto *SF = FRI->getReferencedFunction();
    if (SF->getEffectsKind() < EffectsKind::ReadWrite) {
      // Try to optimize string concatenation.
      if (auto I = optimizeConcatenationOfStringLiterals(AI)) {
        return I;
      }
    }
    if (SF->hasSemanticsString("array.uninitialized")) {
      UserListTy Users;
      // If the uninitialized array is only written into then it can be removed.
      if (recursivelyCollectArrayWritesInstr(Users, AI)) {
        // Erase all of the reference counting instructions and the array
        // allocation instruction.
        for (auto rit = Users.rbegin(), re = Users.rend(); rit != re; ++rit)
          eraseInstFromFunction(**rit);
      }
    }
  }


  // (apply (thin_to_thick_function f)) to (apply f)
  if (auto *TTTFI = dyn_cast<ThinToThickFunctionInst>(AI->getCallee())) {
    // TODO: Handle substitutions and indirect results
    if (AI->hasSubstitutions() || AI->hasIndirectResult())
      return nullptr;
    SmallVector<SILValue, 4> Arguments;
    for (auto &Op : AI->getArgumentOperands()) {
      Arguments.push_back(Op.get());
    }
    // The type of the substition is the source type of the thin to thick
    // instruction.
    SILType substTy = TTTFI->getOperand().getType();
    return ApplyInst::create(AI->getLoc(), TTTFI->getOperand(),
                             substTy, AI->getType(),
                             AI->getSubstitutions(), Arguments,
                             AI->isTransparent(),
                             *AI->getFunction());
  }

  // (apply (witness_method)) -> propagate information about
  // a concrete type from init_existential or init_existential_ref.
  if (auto *WMI = dyn_cast<WitnessMethodInst>(AI->getCallee())) {
    if (WMI->getConformance())
      return nullptr;
    auto LastArg = AI->getArguments().back();
    // Try to derive conformances from the apply_inst
    if (auto *Instance = dyn_cast<OpenExistentialInst>(LastArg)) {
      auto Op = Instance->getOperand();
      for (auto Use : Op.getUses()) {
        if (auto *IE = dyn_cast<InitExistentialInst>(Use->getUser())) {
          // IE should dominate Instance.
          // Without a DomTree we want to be very defensive
          // and only allow this optimization when it is used
          // inside the same BB.
          if (IE->getParent() != AI->getParent())
            continue;
          return propagateConcreteTypeOfInitExistential(AI, WMI, IE,
                                                        Instance->getType());
        }
      }
    }

    if (auto *Instance = dyn_cast<OpenExistentialRefInst>(LastArg)) {
      if (auto *IE = dyn_cast<InitExistentialRefInst>(Instance->getOperand())) {
        // IE should dominate Instance.
        // Without a DomTree we want to be very defensive
        // and only allow this optimization when it is used
        // inside the same BB.
        if (IE->getParent() == AI->getParent())
          return propagateConcreteTypeOfInitExistential(AI, WMI, IE,
                                                        Instance->getType());
      }
    }
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitCondFailInst(CondFailInst *CFI) {
  // Remove runtime asserts such as overflow checks and bounds checks.
  if (RemoveCondFails)
    return eraseInstFromFunction(*CFI);

  // Erase. (cond_fail 0)
  if (auto *I = dyn_cast<IntegerLiteralInst>(CFI->getOperand()))
    if (!I->getValue().getBoolValue())
      return eraseInstFromFunction(*CFI);

  return nullptr;
}

SILInstruction *SILCombiner::visitStrongRetainInst(StrongRetainInst *SRI) {
  // Retain of ThinToThickFunction is a no-op.
  if (isa<ThinToThickFunctionInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  if (isa<ObjCExistentialMetatypeToObjectInst>(SRI->getOperand()) ||
      isa<ObjCMetatypeToObjectInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  // Sometimes in the stdlib due to hand offs, we will see code like:
  //
  // strong_release %0
  // strong_retain %0
  //
  // with the matching strong_retain to the strong_release in a predecessor
  // basic block and the matching strong_release for the strong_retain in a
  // successor basic block.
  //
  // Due to the matching pairs being in different basic blocks, the ARC
  // Optimizer (which is currently local to one basic block does not handle
  // it). But that does not mean that we can not eliminate this pair with a
  // peephole.

  // If we are not the first instruction in this basic block...
  if (SRI != &*SRI->getParent()->begin()) {
    SILBasicBlock::iterator Pred = SRI;
    --Pred;

    // ...and the predecessor instruction is a strong_release on the same value
    // as our strong_retain...
    if (StrongReleaseInst *Release = dyn_cast<StrongReleaseInst>(&*Pred))
      // Remove them...
      if (Release->getOperand() == SRI->getOperand()) {
        eraseInstFromFunction(*Release);
        return eraseInstFromFunction(*SRI);
      }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitRefToRawPointerInst(RefToRawPointerInst *RRPI) {
  // Ref to raw pointer consumption of other ref casts.
  //
  // (ref_to_raw_pointer (unchecked_ref_cast x))
  //    -> (ref_to_raw_pointer x)
  if (auto *ROPI = dyn_cast<UncheckedRefCastInst>(RRPI->getOperand())) {
    RRPI->setOperand(ROPI->getOperand());
    return ROPI->use_empty() ? eraseInstFromFunction(*ROPI) : nullptr;
  }

  // (ref_to_raw_pointer (open_existential_ref (init_existential_ref x))) ->
  // (ref_to_raw_pointer x)
  if (auto *OER = dyn_cast<OpenExistentialRefInst>(RRPI->getOperand()))
    if (auto *IER = dyn_cast<InitExistentialRefInst>(OER->getOperand()))
      return new (RRPI->getModule()) RefToRawPointerInst(
          RRPI->getLoc(), IER->getOperand(), RRPI->getType());

  // (ref_to_raw_pointer (unchecked_ref_bit_cast x))
  //    -> (unchecked_trivial_bit_cast x)
  if (auto *URBCI = dyn_cast<UncheckedRefBitCastInst>(RRPI->getOperand())) {
    return new (RRPI->getModule()) UncheckedTrivialBitCastInst(
        RRPI->getLoc(), URBCI->getOperand(), RRPI->getType());
  }

  return nullptr;
}

/// Simplify the following two frontend patterns:
///
///   %payload_addr = init_enum_data_addr %payload_allocation
///   store %payload to %payload_addr
///   inject_enum_addr %payload_allocation, $EnumType.case
///
///   inject_enum_add %nopayload_allocation, $EnumType.case
///
/// for a concrete enum type $EnumType.case to:
///
///   %1 = enum $EnumType, $EnumType.case, %payload
///   store %1 to %payload_addr
///
///   %1 = enum $EnumType, $EnumType.case
///   store %1 to %nopayload_addr
///
/// We leave the cleaning up to mem2reg.
SILInstruction *
SILCombiner::visitInjectEnumAddrInst(InjectEnumAddrInst *IEAI) {
  // Given an inject_enum_addr of a concrete type without payload, promote it to
  // a store of an enum. Mem2reg/load forwarding will clean things up for us. We
  // can't handle the payload case here due to the flow problems caused by the
  // dependency in between the enum and its data.

  assert(IEAI->getOperand().getType().isAddress() && "Must be an address");

  if (IEAI->getOperand().getType().isAddressOnly(IEAI->getModule())) {
    // Check for the following pattern inside the current basic block:
    // inject_enum_addr %payload_allocation, $EnumType.case1
    // ... no insns storing anything into %payload_allocation
    // select_enum_addr  %payload_allocation,
    //                   case $EnumType.case1: %Result1,
    //                   case case $EnumType.case2: %bResult2
    //                   ...
    //
    // Replace the select_enum_addr by %Result1

    auto *Term = IEAI->getParent()->getTerminator();
    auto *CBI = dyn_cast<CondBranchInst>(Term);
    if (!CBI)
      return nullptr;
    auto BeforeTerm = prev(prev(IEAI->getParent()->end()));
    auto *SEAI = dyn_cast<SelectEnumAddrInst>(BeforeTerm);
    if (!SEAI)
      return nullptr;

    SILBasicBlock::iterator II = IEAI;
    StoreInst *SI = nullptr;
    for (;;) {
      SILInstruction *CI = II;
      if (CI == SEAI)
        break;
      ++II;
      SI = dyn_cast<StoreInst>(CI);
      if (SI) {
        if (SI->getDest() == IEAI->getOperand())
          return nullptr;
      }
      // Allow all instructions inbetween, which don't have any dependency to
      // the store.
      if (AA->mayWriteToMemory(II, IEAI->getOperand()))
        return nullptr;
    }

    auto *InjectedEnumElement = IEAI->getElement();
    auto Result = SEAI->getCaseResult(InjectedEnumElement);

    // Replace select_enum_addr by the result
    replaceInstUsesWith(*SEAI, Result.getDef());

    return nullptr;
  }

  // If the enum does not have a payload create the enum/store since we don't
  // need to worry about payloads.
  if (!IEAI->getElement()->hasArgumentType()) {
    EnumInst *E =
      Builder->createEnum(IEAI->getLoc(), SILValue(), IEAI->getElement(),
                          IEAI->getOperand().getType().getObjectType());
    E->setDebugScope(IEAI->getDebugScope());
    Builder->createStore(IEAI->getLoc(), E, IEAI->getOperand())
      ->setDebugScope(IEAI->getDebugScope());
    return eraseInstFromFunction(*IEAI);
  }

  // Ok, we have a payload enum, make sure that we have a store previous to
  // us...
  SILBasicBlock::iterator II = IEAI;
  StoreInst *SI = nullptr;
  InitEnumDataAddrInst *DataAddrInst = nullptr;
  for (;;) {
    if (II == IEAI->getParent()->begin())
      return nullptr;
    --II;
    SI = dyn_cast<StoreInst>(&*II);
    if (SI) {
      // Find a Store whose destination is taken from an init_enum_data_addr
      // whose address is same allocation as our inject_enum_addr.
      DataAddrInst = dyn_cast<InitEnumDataAddrInst>(SI->getDest().getDef());
      if (DataAddrInst && DataAddrInst->getOperand() == IEAI->getOperand())
        break;
    }
    // Allow all instructions inbetween, which don't have any dependency to
    // the store.
    if (AA->mayWriteToMemory(II, IEAI->getOperand()))
      return nullptr;
  }
  // Found the store to this enum payload. Check if the store is the only use.
  if (!DataAddrInst->hasOneUse())
    return nullptr;

  // In that case, create the payload enum/store.
  EnumInst *E =
      Builder->createEnum(DataAddrInst->getLoc(), SI->getSrc(),
                          DataAddrInst->getElement(),
                          DataAddrInst->getOperand().getType().getObjectType());
  E->setDebugScope(DataAddrInst->getDebugScope());
  Builder->createStore(DataAddrInst->getLoc(), E, DataAddrInst->getOperand())
    ->setDebugScope(DataAddrInst->getDebugScope());
  // Cleanup.
  eraseInstFromFunction(*SI);
  eraseInstFromFunction(*DataAddrInst);
  return eraseInstFromFunction(*IEAI);
}

SILInstruction *SILCombiner::visitUpcastInst(UpcastInst *UCI) {
  // Ref to raw pointer consumption of other ref casts.
  //
  // (upcast (upcast x)) -> (upcast x)
  if (auto *Op = dyn_cast<UpcastInst>(UCI->getOperand())) {
    UCI->setOperand(Op->getOperand());
    return Op->use_empty() ? eraseInstFromFunction(*Op) : nullptr;
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitPointerToAddressInst(PointerToAddressInst *PTAI) {
  // If we reach this point, we know that the types must be different since
  // otherwise simplifyInstruction would have handled the identity case. This is
  // always legal to do since address-to-pointer pointer-to-address implies
  // layout compatibility.
  //
  // (pointer-to-address (address-to-pointer %x)) -> unchecked_
  if (auto *ATPI = dyn_cast<AddressToPointerInst>(PTAI->getOperand())) {
    return new (PTAI->getModule()) UncheckedAddrCastInst(PTAI->getLoc(),
                                                         ATPI->getOperand(),
                                                         PTAI->getType());
  }

  // Turn this also into a index_addr. We generate this pattern after switching
  // the Word type to an explicit Int32 or Int64 in the stdlib.
  //
  // %101 = builtin "strideof_nonzero"<Int>(%84 : $@thick Int.Type) :
  //         $Builtin.Word
  // %102 = builtin "zextOrBitCast_Word_Int64"(%101 : $Builtin.Word) :
  //         $Builtin.Int64
  // %111 = builtin "smul_with_overflow_Int64"(%108 : $Builtin.Int64,
  //                               %102 : $Builtin.Int64, %20 : $Builtin.Int1) :
  //         $(Builtin.Int64, Builtin.Int1)
  // %112 = tuple_extract %111 : $(Builtin.Int64, Builtin.Int1), 0
  // %113 = builtin "truncOrBitCast_Int64_Word"(%112 : $Builtin.Int64) :
  //         $Builtin.Word
  // %114 = index_raw_pointer %100 : $Builtin.RawPointer, %113 : $Builtin.Word
  // %115 = pointer_to_address %114 : $Builtin.RawPointer to $*Int
  SILValue Distance;
  SILValue TruncOrBitCast;
  MetatypeInst *Metatype;
  IndexRawPointerInst *IndexRawPtr;
  BuiltinInst *StrideMul;
  if (match(
          PTAI->getOperand(),
          m_IndexRawPointerInst(IndexRawPtr))) {
    SILValue Ptr = IndexRawPtr->getOperand(0);
    SILValue TruncOrBitCast = IndexRawPtr->getOperand(1);
    if (match(TruncOrBitCast,
              m_ApplyInst(BuiltinValueKind::TruncOrBitCast,
                          m_TupleExtractInst(m_BuiltinInst(StrideMul), 0)))) {
      if (match(StrideMul,
                m_ApplyInst(
                    BuiltinValueKind::SMulOver, m_SILValue(Distance),
                    m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                                m_ApplyInst(BuiltinValueKind::StrideofNonZero,
                                            m_MetatypeInst(Metatype))))) ||
          match(StrideMul,
                m_ApplyInst(
                    BuiltinValueKind::SMulOver,
                    m_ApplyInst(BuiltinValueKind::ZExtOrBitCast,
                                m_ApplyInst(BuiltinValueKind::StrideofNonZero,
                                            m_MetatypeInst(Metatype))),
                    m_SILValue(Distance)))) {
        SILType InstanceType =
            Metatype->getType().getMetatypeInstanceType(PTAI->getModule());
        auto *Trunc = cast<BuiltinInst>(TruncOrBitCast);

        // Make sure that the type of the metatype matches the type that we are
        // casting to so we stride by the correct amount.
        if (InstanceType.getAddressType() != PTAI->getType()) {
          return nullptr;
        }

        auto *NewPTAI = Builder->createPointerToAddress(PTAI->getLoc(), Ptr,
                                                        PTAI->getType());
        auto DistanceAsWord = Builder->createBuiltin(
            PTAI->getLoc(), Trunc->getName(), Trunc->getType(), {}, Distance);

        NewPTAI->setDebugScope(PTAI->getDebugScope());
        return new (PTAI->getModule())
            IndexAddrInst(PTAI->getLoc(), NewPTAI, DistanceAsWord);
      }
    }
  }
  // Turn:
  //
  //   %stride = Builtin.strideof(T) * %distance
  //   %ptr' = index_raw_pointer %ptr, %stride
  //   %result = pointer_to_address %ptr, $T'
  //
  // To:
  //
  //   %addr = pointer_to_address %ptr, $T
  //   %result = index_addr %addr, %distance
  //
  BuiltinInst *Bytes;
  if (match(PTAI->getOperand(),
            m_IndexRawPointerInst(m_ValueBase(),
                                  m_TupleExtractInst(m_BuiltinInst(Bytes),
                                                     0)))) {
    if (match(Bytes, m_ApplyInst(BuiltinValueKind::SMulOver, m_ValueBase(),
                                 m_ApplyInst(BuiltinValueKind::Strideof,
                                             m_MetatypeInst(Metatype)),
                                 m_ValueBase())) ||
        match(Bytes, m_ApplyInst(BuiltinValueKind::SMulOver, m_ValueBase(),
                                 m_ApplyInst(BuiltinValueKind::StrideofNonZero,
                                             m_MetatypeInst(Metatype)),
                                 m_ValueBase()))) {
      SILType InstanceType =
        Metatype->getType().getMetatypeInstanceType(PTAI->getModule());

      // Make sure that the type of the metatype matches the type that we are
      // casting to so we stride by the correct amount.
      if (InstanceType.getAddressType() != PTAI->getType())
        return nullptr;

      auto IRPI = cast<IndexRawPointerInst>(PTAI->getOperand().getDef());
      SILValue Ptr = IRPI->getOperand(0);
      SILValue Distance = Bytes->getArguments()[0];
      auto *NewPTAI =
          Builder->createPointerToAddress(PTAI->getLoc(), Ptr, PTAI->getType());
      NewPTAI->setDebugScope(PTAI->getDebugScope());
      return new (PTAI->getModule())
          IndexAddrInst(PTAI->getLoc(), NewPTAI, Distance);
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitUncheckedAddrCastInst(UncheckedAddrCastInst *UADCI) {
  SILModule &Mod = UADCI->getModule();

  // (unchecked-addr-cast (unchecked-addr-cast x X->Y) Y->Z)
  //   ->
  // (unchecked-addr-cast x X->Z)
  if (auto *OtherUADCI = dyn_cast<UncheckedAddrCastInst>(UADCI->getOperand()))
    return new (Mod) UncheckedAddrCastInst(
        UADCI->getLoc(), OtherUADCI->getOperand(), UADCI->getType());

  // (unchecked-addr-cast cls->superclass) -> (upcast cls->superclass)
  if (UADCI->getType() != UADCI->getOperand().getType() &&
      UADCI->getType().isSuperclassOf(UADCI->getOperand().getType()))
    return new (Mod) UpcastInst(UADCI->getLoc(), UADCI->getOperand(),
                                UADCI->getType());

  // See if we have all loads from this unchecked_addr_cast. If we do, load the
  // original type and create the appropriate bitcast.

  // First if our UADCI has not users, bail. This will be eliminated by DCE.
  if (UADCI->use_empty())
    return nullptr;

  SILType InputTy = UADCI->getOperand().getType();
  SILType OutputTy = UADCI->getType();

  // If either type is address only, do not do anything here.
  if (InputTy.isAddressOnly(Mod) || OutputTy.isAddressOnly(Mod))
    return nullptr;

  bool InputIsTrivial = InputTy.isTrivial(Mod);
  bool OutputIsTrivial = OutputTy.isTrivial(Mod);

  // If our input is trivial and our output type is not, do not do
  // anything. This is to ensure that we do not change any types reference
  // semantics from trivial -> reference counted.
  if (InputIsTrivial && !OutputIsTrivial)
    return nullptr;

  // The structs could have different size. We have code in the stdlib that
  // casts pointers to differently sized integer types. This code prevents that
  // we bitcast the values.
  if (InputTy.getStructOrBoundGenericStruct() &&
      OutputTy.getStructOrBoundGenericStruct())
    return nullptr;

  // For each user U of the unchecked_addr_cast...
  for (auto U : UADCI->getUses())
    // Check if it is load. If it is not a load, bail...
    if (!isa<LoadInst>(U->getUser()))
      return nullptr;

  SILValue Op = UADCI->getOperand();
  SILLocation Loc = UADCI->getLoc();
  SILDebugScope *Scope = UADCI->getDebugScope();

  // Ok, we have all loads. Lets simplify this. Go back through the loads a
  // second time, rewriting them into a load + bitcast from our source.
  for (auto U : UADCI->getUses()) {
    // Grab the original load.
    LoadInst *L = cast<LoadInst>(U->getUser());

    // Insert a new load from our source and bitcast that as appropriate.
    LoadInst *NewLoad = Builder->createLoad(Loc, Op);
    NewLoad->setDebugScope(Scope);
    SILInstruction *BitCast = nullptr;
    if (OutputIsTrivial)
      BitCast = Builder->createUncheckedTrivialBitCast(Loc, NewLoad,
                                                       OutputTy.getObjectType());
    else
      BitCast = Builder->createUncheckedRefBitCast(Loc, NewLoad,
                                                   OutputTy.getObjectType());
    BitCast->setDebugScope(Scope);

    // Replace all uses of the old load with the new bitcasted result and erase
    // the old load.
    replaceInstUsesWith(*L, BitCast, 0);
    eraseInstFromFunction(*L);
  }

  // Delete the old cast.
  return eraseInstFromFunction(*UADCI);
}

SILInstruction *
SILCombiner::visitUncheckedRefCastInst(UncheckedRefCastInst *URCI) {
  // (unchecked-ref-cast (unchecked-ref-cast x X->Y) Y->Z)
  //   ->
  // (unchecked-ref-cast x X->Z)
  if (auto *OtherURCI = dyn_cast<UncheckedRefCastInst>(URCI->getOperand()))
    return new (URCI->getModule()) UncheckedRefCastInst(
        URCI->getLoc(), OtherURCI->getOperand(), URCI->getType());

  // (unchecked_ref_cast (upcast x X->Y) Y->Z) -> (unchecked_ref_cast x X->Z)
  if (auto *UI = dyn_cast<UpcastInst>(URCI->getOperand()))
    return new (URCI->getModule())
        UncheckedRefCastInst(URCI->getLoc(), UI->getOperand(), URCI->getType());

  if (URCI->getType() != URCI->getOperand().getType() &&
      URCI->getType().isSuperclassOf(URCI->getOperand().getType()))
    return new (URCI->getModule())
        UpcastInst(URCI->getLoc(), URCI->getOperand(), URCI->getType());

  // (unchecked_ref_cast (open_existential_ref (init_existential_ref X))) ->
  // (unchecked_ref_cast X)
  if (auto *OER = dyn_cast<OpenExistentialRefInst>(URCI->getOperand()))
    if (auto *IER = dyn_cast<InitExistentialRefInst>(OER->getOperand()))
      return new (URCI->getModule()) UncheckedRefCastInst(
          URCI->getLoc(), IER->getOperand(), URCI->getType());

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI) {
  // FIXME: rename from RemoveCondFails to RemoveRuntimeAsserts.
  if (RemoveCondFails) {
    SILModule &Mod = UCCI->getModule();
    SILValue Op = UCCI->getOperand();
    SILLocation Loc = UCCI->getLoc();

    if (Op.getType().isAddress()) {
      // unconditional_checked_cast -> unchecked_addr_cast
      return new (Mod) UncheckedAddrCastInst(Loc, Op, UCCI->getType());
    } else if (Op.getType().isHeapObjectReferenceType()) {
      // unconditional_checked_cast -> unchecked_ref_cast
      return new (Mod) UncheckedRefCastInst(Loc, Op, UCCI->getType());
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitRawPointerToRefInst(RawPointerToRefInst *RawToRef) {
  // (raw_pointer_to_ref (ref_to_raw_pointer x X->Y) Y->Z)
  //   ->
  // (unchecked_ref_cast X->Z)
  if (auto *RefToRaw = dyn_cast<RefToRawPointerInst>(RawToRef->getOperand())) {
    return new (RawToRef->getModule()) UncheckedRefCastInst(
        RawToRef->getLoc(), RefToRaw->getOperand(), RawToRef->getType());
  }

  return nullptr;
}

/// We really want to eliminate unchecked_take_enum_data_addr. Thus if we find
/// one go through all of its uses and see if they are all loads and address
/// projections (in many common situations this is true). If so, perform:
///
/// (load (unchecked_take_enum_data_addr x)) -> (unchecked_enum_data (load x))
///
/// FIXME: Implement this for address projections.
SILInstruction *
SILCombiner::
visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *TEDAI) {
  // If our TEDAI has no users, there is nothing to do.
  if (TEDAI->use_empty())
    return nullptr;

  // If our enum type is address only, we can not do anything here. The key
  // thing to remember is that an enum is address only if any of its cases are
  // address only. So we *could* have a loadable payload resulting from the
  // TEDAI without the TEDAI being loadable itself.
  if (TEDAI->getOperand().getType().isAddressOnly(TEDAI->getModule()))
    return nullptr;

  // For each user U of the take_enum_data_addr...
  for (auto U : TEDAI->getUses())
    // Check if it is load. If it is not a load, bail...
    if (!isa<LoadInst>(U->getUser()))
      return nullptr;

  // Grab the EnumAddr.
  SILLocation Loc = TEDAI->getLoc();
  SILDebugScope *Scope = TEDAI->getDebugScope();
  SILValue EnumAddr = TEDAI->getOperand();
  EnumElementDecl *EnumElt = TEDAI->getElement();
  SILType PayloadType = TEDAI->getType().getObjectType();

  // Go back through a second time now that we know all of our users are
  // loads. Perform the transformation on each load.
  for (auto U : TEDAI->getUses()) {
    // Grab the load.
    LoadInst *L = cast<LoadInst>(U->getUser());

    // Insert a new Load of the enum and extract the data from that.
    auto *Load = Builder->createLoad(Loc, EnumAddr);
    Load->setDebugScope(Scope);
    auto *D = Builder->createUncheckedEnumData(
        Loc, Load, EnumElt, PayloadType);
    D->setDebugScope(Scope);

    // Replace all uses of the old load with the data and erase the old load.
    replaceInstUsesWith(*L, D, 0);
    eraseInstFromFunction(*L);
  }

  return eraseInstFromFunction(*TEDAI);
}

SILInstruction *SILCombiner::visitStrongReleaseInst(StrongReleaseInst *SRI) {
  // Release of ThinToThickFunction is a no-op.
  if (isa<ThinToThickFunctionInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  if (isa<ObjCExistentialMetatypeToObjectInst>(SRI->getOperand()) ||
      isa<ObjCMetatypeToObjectInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  return nullptr;
}

SILInstruction *SILCombiner::visitCondBranchInst(CondBranchInst *CBI) {
  // cond_br(xor(x, 1)), t_label, f_label -> cond_br x, f_label, t_label
  SILValue X;
  if (match(CBI->getCondition(), m_ApplyInst(BuiltinValueKind::Xor,
                                             m_SILValue(X), m_One()))) {
    SmallVector<SILValue, 4> OrigTrueArgs, OrigFalseArgs;
    for (const auto &Op : CBI->getTrueArgs())
      OrigTrueArgs.push_back(Op);
    for (const auto &Op : CBI->getFalseArgs())
      OrigFalseArgs.push_back(Op);
    return CondBranchInst::create(CBI->getLoc(), X,
                                  CBI->getFalseBB(), OrigFalseArgs,
                                  CBI->getTrueBB(), OrigTrueArgs,
                                  *CBI->getFunction());
  }

  // cond_br (select_enum) -> switch_enum
  // This pattern often occurs as a result of using optionals.
  if (auto *SEI = dyn_cast<SelectEnumInst>(CBI->getCondition())) {
    // No bb args should be passed
    if (!CBI->getTrueArgs().empty() || !CBI->getFalseArgs().empty())
      return nullptr;
    auto EnumOperandTy = SEI->getEnumOperand().getType();
    // Type should be loadable
    if (!EnumOperandTy.isLoadable(SEI->getModule()))
      return nullptr;

    // Result of the selec_enum should be a boolean.
    if (SEI->getType() != CBI->getCondition().getType())
      return nullptr;

    // If any of cond_br edges are critical edges, do not perform
    // the transformation, as SIL in canonical form may
    // only have critical edges that are originating from cond_br
    // instructions.
    if (!CBI->getTrueBB()->getSinglePredecessor())
      return nullptr;

    if (!CBI->getFalseBB()->getSinglePredecessor())
      return nullptr;

    SILBasicBlock *Default = nullptr;

    match_integer<0> Zero;

    if (SEI->hasDefault()) {
      bool isFalse = match(SEI->getDefaultResult(), Zero);
      Default = isFalse ? CBI->getFalseBB() : Default = CBI->getTrueBB();
    }

    // We can now convert cond_br(select_enum) into switch_enum
    SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 8> Cases;
    for (int i = 0, e = SEI->getNumCases(); i < e; ++i) {
      auto Pair = SEI->getCase(i);
      if (isa<IntegerLiteralInst>(Pair.second)) {
        bool isFalse = match(Pair.second, Zero);
        if (!isFalse && Default != CBI->getTrueBB()) {
          Cases.push_back(std::make_pair(Pair.first, CBI->getTrueBB()));
        }
        if (isFalse && Default != CBI->getFalseBB()) {
          Cases.push_back(std::make_pair(Pair.first, CBI->getFalseBB()));
        }
        continue;
      }

      return nullptr;
    }

    return SwitchEnumInst::create(SEI->getLoc(), SEI->getEnumOperand(), Default,
                                  Cases, *SEI->getFunction());
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUncheckedRefBitCastInst(UncheckedRefBitCastInst *URBCI) {
  // (unchecked_ref_bit_cast Y->Z (unchecked_ref_bit_cast X->Y x))
  //   ->
  // (unchecked_ref_bit_cast X->Z x)
  if (auto *Op = dyn_cast<UncheckedRefBitCastInst>(URBCI->getOperand())) {
    return new (URBCI->getModule()) UncheckedRefBitCastInst(URBCI->getLoc(),
                                                            Op->getOperand(),
                                                            URBCI->getType());
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI) {
  // (unchecked_trivial_bit_cast Y->Z
  //                                 (unchecked_trivial_bit_cast X->Y x))
  //   ->
  // (unchecked_trivial_bit_cast X->Z x)
  SILValue Op = UTBCI->getOperand();
  if (auto *OtherUTBCI = dyn_cast<UncheckedTrivialBitCastInst>(Op)) {
    SILModule &Mod = UTBCI->getModule();
    return new (Mod) UncheckedTrivialBitCastInst(UTBCI->getLoc(),
                                                 OtherUTBCI->getOperand(),
                                                 UTBCI->getType());
  }

  // (unchecked_trivial_bit_cast Y->Z
  //                                 (unchecked_ref_bit_cast X->Y x))
  //   ->
  // (unchecked_trivial_bit_cast X->Z x)
  if (auto *URBCI = dyn_cast<UncheckedRefBitCastInst>(Op)) {
    SILModule &Mod = UTBCI->getModule();
    return new (Mod) UncheckedTrivialBitCastInst(UTBCI->getLoc(),
                                                 URBCI->getOperand(),
                                                 UTBCI->getType());
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitSelectEnumInst(SelectEnumInst *EIT) {
  // TODO: We should be able to flat-out replace the select_enum instruction
  // with the selected value in another pass. For parity with the enum_is_tag
  // combiner pass, handle integer literals for now.
  auto *EI = dyn_cast<EnumInst>(EIT->getEnumOperand());
  if (!EI)
    return nullptr;

  SILValue selected;
  for (unsigned i = 0, e = EIT->getNumCases(); i < e; ++i) {
    auto casePair = EIT->getCase(i);
    if (casePair.first == EI->getElement()) {
      selected = casePair.second;
      break;
    }
  }
  if (!selected)
    selected = EIT->getDefaultResult();

  if (auto inst = dyn_cast<IntegerLiteralInst>(selected)) {
    return IntegerLiteralInst::create(inst->getLoc(), inst->getType(),
                                      inst->getValue(), *EIT->getFunction());
  }

  return nullptr;
}

/// Helper function for simplifying convertions between
/// thick and objc metatypes.
static SILInstruction *
visitMetatypeConversionInst(ConversionInst *MCI,
                            MetatypeRepresentation Representation) {
  SILValue Op = MCI->getOperand(0);
  SILModule &Mod = MCI->getModule();
  // Instruction has a proper target type already.
  SILType Ty = MCI->getType();
  auto MetatypeTy = Op.getType().getAs<AnyMetatypeType>();

  if (MetatypeTy->getRepresentation() != Representation)
    return nullptr;

  if (dyn_cast<MetatypeInst>(Op)) {
    return new (Mod) MetatypeInst(MCI->getLoc(), Ty);
  } else if (auto *VMI = dyn_cast<ValueMetatypeInst>(Op)) {
    return new (Mod) ValueMetatypeInst(MCI->getLoc(),
                                       Ty,
                                       VMI->getOperand());
  } else if (auto *EMI = dyn_cast<ExistentialMetatypeInst>(Op)) {
    return new (Mod) ExistentialMetatypeInst(MCI->getLoc(),
                                             Ty,
                                             EMI->getOperand());
  }
  return nullptr;
}

SILInstruction *
SILCombiner::visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *TTOCMI) {
  // Perform the following transformations:
  // (thick_to_objc_metatype (metatype @thick)) ->
  // (metatype @objc_metatype)
  //
  // (thick_to_objc_metatype (value_metatype @thick)) ->
  // (value_metatype @objc_metatype)
  //
  // (thick_to_objc_metatype (existential_metatype @thick)) ->
  // (existential_metatype @objc_metatype)
  return visitMetatypeConversionInst(TTOCMI, MetatypeRepresentation::Thick);
}

SILInstruction *
SILCombiner::visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *OCTTMI) {
  // Perform the following transformations:
  // (objc_to_thick_metatype (metatype @objc_metatype)) ->
  // (metatype @thick)
  //
  // (objc_to_thick_metatype (value_metatype @objc_metatype)) ->
  // (value_metatype @thick)
  //
  // (objc_to_thick_metatype (existential_metatype @objc_metatype)) ->
  // (existential_metatype @thick)
  return visitMetatypeConversionInst(OCTTMI, MetatypeRepresentation::ObjC);
}

SILInstruction *SILCombiner::visitTupleExtractInst(TupleExtractInst *TEI) {
  // tuple_extract(apply([add|sub|...]overflow(x, 0)), 1) -> 0
  // if it can be proven that no overflow can happen.
  if (TEI->getFieldNo() != 1)
    return nullptr;

  if (auto *BI = dyn_cast<BuiltinInst>(TEI->getOperand()))
    if (!canOverflow(BI))
      return IntegerLiteralInst::create(TEI->getLoc(), TEI->getType(),
                                        APInt(1, 0), *TEI->getFunction());
  return nullptr;
}

SILInstruction *SILCombiner::visitFixLifetimeInst(FixLifetimeInst *FLI) {
  // fix_lifetime(alloc_stack) -> fix_lifetime(load(alloc_stack))
  if (auto *AI = dyn_cast<AllocStackInst>(FLI->getOperand())) {
    if (FLI->getOperand().getType().isLoadable(FLI->getModule())) {
      auto Load = Builder->createLoad(FLI->getLoc(), SILValue(AI, 1));
      Load->setDebugScope(FLI->getDebugScope());
      return new (FLI->getModule())
          FixLifetimeInst(FLI->getLoc(), SILValue(Load, 0));
    }
  }
  return nullptr;
}

SILInstruction *
SILCombiner::
visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *CCABI) {
  // Try to determine the outcome of the cast from a known type
  // to a protocol type at compile-time.
  if (!CCABI->getTargetType().isAnyExistentialType())
    return nullptr;
  auto SILSourceTy = SILType::getPrimitiveObjectType(CCABI->getSourceType());
  auto SILTargetTy = SILType::getPrimitiveObjectType(CCABI->getTargetType());
  // Check if we can statically figure out the outcome of this cast.
  auto *SourceNominalTy = CCABI->getSourceType().getAnyNominal();
  if (SILSourceTy.isExistentialType() || !SourceNominalTy)
    return nullptr;

  if (SILTargetTy.isExistentialType()) {
    auto *TargetProtocol = SILTargetTy.getSwiftRValueType().getAnyNominal();
    auto SourceProtocols = SourceNominalTy->getProtocols();
    auto SourceExtensions = SourceNominalTy->getExtensions();

    // Check all protocols implemented by the type.
    for (auto *Protocol : SourceProtocols) {
      if (Protocol == TargetProtocol) {
        auto *UCCA = Builder->createUnconditionalCheckedCastAddr(
            CCABI->getLoc(), CCABI->getConsumptionKind(), CCABI->getSrc(),
            CCABI->getSourceType(), CCABI->getDest(), CCABI->getTargetType());
        (void)UCCA;
        Builder->createBranch(CCABI->getLoc(), CCABI->getSuccessBB());
        eraseInstFromFunction(*CCABI);
        return nullptr;
      }
    }

    // Check all protocols implemented by the type extensions.
    for(auto *Extension: SourceExtensions) {
      SourceProtocols = Extension->getProtocols();
      for (auto *Protocol: SourceProtocols) {
        if (Protocol == TargetProtocol) {
          auto *UCCA = Builder->createUnconditionalCheckedCastAddr(
              CCABI->getLoc(), CCABI->getConsumptionKind(), CCABI->getSrc(),
              CCABI->getSourceType(), CCABI->getDest(), CCABI->getTargetType());
          (void)UCCA;
          Builder->createBranch(CCABI->getLoc(), CCABI->getSuccessBB());
          eraseInstFromFunction(*CCABI);
          return nullptr;
        }
      }
    }

    // If type is private or internal, its conformances cannot be changed
    // at run-time. Therefore it is safe to make a negative decision
    // at compile-time.
    if (SourceNominalTy->getAccessibility() < Accessibility::Public) {
      // This cast is always fasle. Replace it with a branch to the
      // failure block.
      Builder->createBranch(CCABI->getLoc(), CCABI->getFailureBB());
      eraseInstFromFunction(*CCABI);
    }
  }
  return nullptr;
}
