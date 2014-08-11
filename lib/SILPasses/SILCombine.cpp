//===-------------------------- SILCombine --------------------------------===//
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
// A port of LLVM's InstCombine pass to SIL. Its main purpose is for performing
// small combining operations/peepholes at the SIL level. It additionally
// performs dead code elimination when it initially adds instructions to the
// work queue in order to reduce compile time by not visiting trivially dead
// instructions.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-combine"
#include "swift/SILPasses/Passes.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILPasses/Utils/Local.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::PatternMatch;

STATISTIC(NumSimplified, "Number of instructions simplified");
STATISTIC(NumCombined, "Number of instructions combined");
STATISTIC(NumDeadInst, "Number of dead insts eliminated");

//===----------------------------------------------------------------------===//
//                             SILCombineWorklist
//===----------------------------------------------------------------------===//

namespace swift {

enum IsZeroKind {
  Zero,
  NotZero,
  Unknown
};

/// Check if the value \p Value is known to be zero, non-zero or unknown.
static IsZeroKind isZeroValue(SILValue Value) {
  // Inspect integer literals.
  if (auto *L = dyn_cast<IntegerLiteralInst>(Value.getDef())) {
    if (L->getValue().getZExtValue() == 0)
      return IsZeroKind::Zero;
    return IsZeroKind::NotZero;
  }

  // Inspect Structs.
  switch (Value.getDef()->getKind()) {
    // Bitcast of zero is zero.
    case ValueKind::UncheckedTrivialBitCastInst:
    // Extracting from a zero class returns a zero.
    case ValueKind::StructExtractInst:
      return isZeroValue(cast<SILInstruction>(Value.getDef())->getOperand(0));
    default:
      break;
  }

  // Inspect casts.
  if (auto *AI = dyn_cast<ApplyInst>(Value.getDef())) {
    auto *FR = dyn_cast<BuiltinFunctionRefInst>(AI->getCallee());
    if (!FR)
      return IsZeroKind::Unknown;
    switch (FR->getBuiltinInfo().ID) {
      case BuiltinValueKind::IntToPtr:
      case BuiltinValueKind::PtrToInt:
      case BuiltinValueKind::ZExt:
        return isZeroValue(AI->getArgument(0));
      case BuiltinValueKind::UDiv:
      case BuiltinValueKind::SDiv: {
        if (IsZeroKind::Zero == isZeroValue(AI->getArgument(0)))
          return IsZeroKind::Zero;
        return IsZeroKind::Unknown;
      }
      case BuiltinValueKind::Mul:
      case BuiltinValueKind::SMulOver:
      case BuiltinValueKind::UMulOver: {
        IsZeroKind LHS = isZeroValue(AI->getArgument(0));
        IsZeroKind RHS = isZeroValue(AI->getArgument(1));
        if (LHS == IsZeroKind::Zero || RHS == IsZeroKind::Zero)
          return IsZeroKind::Zero;

        return IsZeroKind::Unknown;
      }
      default:
        return IsZeroKind::Unknown;
    }
  }

  // Handle results of XXX_with_overflow arithmetic.
  if (auto *T = dyn_cast<TupleExtractInst>(Value.getDef())) {
    // Make sure we are extracting the number value and not
    // the overflow flag.
    if (T->getFieldNo() != 0)
      return IsZeroKind::Unknown;

    ApplyInst *CAI = dyn_cast<ApplyInst>(T->getOperand());
    if (!CAI)
      return IsZeroKind::Unknown;

    // Check that this is a builtin function.
    if (!isa<BuiltinFunctionRefInst>(CAI->getCallee()))
      return IsZeroKind::Unknown;

    return isZeroValue(T->getOperand());
  }

  //Inspect allocations and pointer literals.
  if (isa<StringLiteralInst>(Value.getDef()) ||
      isa<AllocationInst>(Value.getDef()) ||
      isa<SILGlobalAddrInst>(Value.getDef()))
    return IsZeroKind::NotZero;

  return IsZeroKind::Unknown;
}

/// This is the worklist management logic for SILCombine.
class SILCombineWorklist {
  llvm::SmallVector<SILInstruction *, 256> Worklist;
  llvm::DenseMap<SILInstruction *, unsigned> WorklistMap;
  llvm::SmallVector<SILInstruction *, 8> TrackingList;

  void operator=(const SILCombineWorklist &RHS) = delete;
  SILCombineWorklist(const SILCombineWorklist &Worklist) = delete;
public:
  SILCombineWorklist() {}

  /// Returns true if the worklist is empty.
  bool isEmpty() const { return Worklist.empty(); }

  /// Add the specified instruction to the worklist if it isn't already in it.
  void add(SILInstruction *I) {
    if (WorklistMap.insert(std::make_pair(I, Worklist.size())).second) {
      DEBUG(llvm::dbgs() << "SC: ADD: " << *I << '\n');
      Worklist.push_back(I);
    }
  }

  /// If the given ValueBase is a SILInstruction add it to the worklist.
  void addValue(ValueBase *V) {
    if (SILInstruction *I = llvm::dyn_cast<SILInstruction>(V))
      add(I);
  }

  /// Add the given list of instructions in reverse order to the worklist. This
  /// routine assumes that the worklist is empty and the given list has no
  /// duplicates.
  void addInitialGroup(ArrayRef<SILInstruction *> List) {
    assert(Worklist.empty() && "Worklist must be empty to add initial group");
    Worklist.reserve(List.size()+16);
    WorklistMap.resize(List.size());
    DEBUG(llvm::dbgs() << "SC: ADDING: " << List.size()
                       << " instrs to worklist\n");
    while (!List.empty()) {
      SILInstruction *I = List.back();
      List = List.slice(0, List.size()-1);

      WorklistMap.insert(std::make_pair(I, Worklist.size()));
      Worklist.push_back(I);
    }
  }

  // If I is in the worklist, remove it.
  void remove(SILInstruction *I) {
    auto It = WorklistMap.find(I);
    if (It == WorklistMap.end())
      return; // Not in worklist.

    // Don't bother moving everything down, just null out the slot. We will
    // check before we process any instruction if it is null.
    Worklist[It->second] = 0;

    WorklistMap.erase(It);
  }

  /// Remove the top element from the worklist.
  SILInstruction *removeOne() {
    SILInstruction *I = Worklist.pop_back_val();
    WorklistMap.erase(I);
    return I;
  }

  /// When an instruction has been simplified, add all of its users to the
  /// worklist since additional simplifications of its users may have been
  /// exposed.
  void addUsersToWorklist(ValueBase *I) {
    for (auto UI : I->getUses())
      add(UI->getUser());
  }

  /// If only one result of an instruction has been simplified, add all of the
  /// users of that result to the worklist since additional simplifications of
  /// its users may have been exposed.
  void addUsersToWorklist(ValueBase *I, unsigned Index) {
    for (auto UI : SILValue(I, Index).getUses())
      add(UI->getUser());
  }

  /// Check that the worklist is empty and nuke the backing store for the map if
  /// it is large.
  void zap() {
    assert(WorklistMap.empty() && "Worklist empty, but the map is not?");

    // Do an explicit clear, this shrinks the map if needed.
    WorklistMap.clear();
  }
};

} // end namespace swift

//===----------------------------------------------------------------------===//
//                                SILCombiner
//===----------------------------------------------------------------------===//

namespace swift {

/// This is a class which maintains the state of the combiner and simplifies
/// many operations such as removing/adding instructions and syncing them with
/// the worklist.
class SILCombiner :
    public SILInstructionVisitor<SILCombiner, SILInstruction *> {
public:
  SILCombiner(bool removeCondFails) : Worklist(), MadeChange(false),
      RemoveCondFails(removeCondFails), Iteration(0), Builder(0) { }

  bool runOnFunction(SILFunction &F) {
    clear();

    // Create a SILBuilder for F and initialize the tracking list.
    SILBuilder B(F);
    B.setTrackingList(&TrackingList);
    Builder = &B;

    bool Changed = false;
    // Perform iterations until we do not make any changes.
    while (doOneIteration(F, Iteration)) {
      Changed = true;
      Iteration++;
    }

    // Cleanup the builder and return whether or not we made any changes.
    Builder = 0;
    return Changed;
  }

  void clear() {
    Iteration = 0;
    Worklist.zap();
    MadeChange = false;
  }

  // Insert the instruction New before instruction Old in Old's parent BB. Add
  // New to the worklist.
  SILInstruction *insertNewInstBefore(SILInstruction *New,
                                      SILInstruction &Old) {
    assert(New && New->getParent() == 0 &&
           "New instruction already inserted into a basic block!");
    SILBasicBlock *BB = Old.getParent();
    BB->getInstList().insert(&Old, New);  // Insert inst
    Worklist.add(New);
    return New;
  }

  // This method is to be used when an instruction is found to be dead,
  // replacable with another preexisting expression. Here we add all uses of I
  // to the worklist, replace all uses of I with the new value, then return I,
  // so that the combiner will know that I was modified.
  SILInstruction *replaceInstUsesWith(SILInstruction &I, ValueBase *V) {
    Worklist.addUsersToWorklist(&I);   // Add all modified instrs to worklist.

    DEBUG(llvm::dbgs() << "SC: Replacing " << I << "\n"
          "    with " << *V << '\n');

    I.replaceAllUsesWith(V);

    return &I;
  }

  /// This is meant to be used when one is attempting to replace only one of the
  /// results of I with a result of V.
  SILInstruction *replaceInstUsesWith(SILInstruction &I, ValueBase *V,
                                      unsigned IIndex, unsigned VIndex=0) {
    assert(IIndex < I.getNumTypes() && "Can not have more results than "
           "types.");
    assert(VIndex < V->getNumTypes() && "Can not have more results than "
           "types.");

    // Add all modified instrs to worklist.
    Worklist.addUsersToWorklist(&I, IIndex);

    DEBUG(llvm::dbgs() << "SC: Replacing " << I << "\n"
          "    with " << *V << '\n');

    SILValue(&I, IIndex).replaceAllUsesWith(SILValue(V, VIndex));

    return &I;
  }

  // Some instructions can never be "trivially dead" due to side effects or
  // producing a void value. In those cases, since we can not rely on
  // SILCombines trivially dead instruction DCE in order to delete the
  // instruction, visit methods should use this method to delete the given
  // instruction and upon completion of their peephole return the value returned
  // by this method.
  SILInstruction *eraseInstFromFunction(SILInstruction &I) {
    DEBUG(llvm::dbgs() << "SC: ERASE " << I << '\n');

    assert(I.use_empty() && "Cannot erase instruction that is used!");
    // Make sure that we reprocess all operands now that we reduced their
    // use counts.
    if (I.getNumOperands() < 8)
      for (auto &OpI : I.getAllOperands())
        if (SILInstruction *Op = llvm::dyn_cast<SILInstruction>(&*OpI.get()))
          Worklist.add(Op);

    Worklist.remove(&I);
    I.eraseFromParent();
    MadeChange = true;
    return 0;  // Don't do anything with I
  }

  void addInitialGroup(ArrayRef<SILInstruction *> List) {
    Worklist.addInitialGroup(List);
  }

  /// Base visitor that does not do anything.
  SILInstruction *visitValueBase(ValueBase *V) { return nullptr; }

  /// Instruction visitors.
  SILInstruction *visitReleaseValueInst(ReleaseValueInst *DI);
  SILInstruction *visitRetainValueInst(RetainValueInst *CI);
  SILInstruction *visitPartialApplyInst(PartialApplyInst *AI);
  SILInstruction *visitApplyInst(ApplyInst *AI);
  SILInstruction *visitAllocArrayInst(AllocArrayInst *AAI);
  SILInstruction *visitCondFailInst(CondFailInst *CFI);
  SILInstruction *visitStrongRetainInst(StrongRetainInst *SRI);
  SILInstruction *visitRefToRawPointerInst(RefToRawPointerInst *RRPI);
  SILInstruction *visitUpcastInst(UpcastInst *UCI);
  SILInstruction *visitLoadInst(LoadInst *LI);
  SILInstruction *visitAllocStackInst(AllocStackInst *AS);
  SILInstruction *visitSwitchEnumAddrInst(SwitchEnumAddrInst *SEAI);
  SILInstruction *visitInjectEnumAddrInst(InjectEnumAddrInst *IEAI);
  SILInstruction *visitPointerToAddressInst(PointerToAddressInst *PTAI);
  SILInstruction *visitUncheckedAddrCastInst(UncheckedAddrCastInst *UADCI);
  SILInstruction *visitUncheckedRefCastInst(UncheckedRefCastInst *URCI);
  SILInstruction *visitUnconditionalCheckedCastInst(
                    UnconditionalCheckedCastInst *UCCI);
  SILInstruction *visitRawPointerToRefInst(RawPointerToRefInst *RPTR);
  SILInstruction *
  visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *TEDAI);
  SILInstruction *visitStrongReleaseInst(StrongReleaseInst *SRI);
  SILInstruction *visitCondBranchInst(CondBranchInst *CBI);
  SILInstruction *
  visitUncheckedRefBitCastInst(UncheckedRefBitCastInst *URBCI);
  SILInstruction *
  visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI);
  SILInstruction *visitEnumIsTagInst(EnumIsTagInst *EIT);

  /// Instruction visitor helpers.
  SILInstruction *optimizeBuiltinCanBeObjCClass(ApplyInst *AI);

  // Optimize the "cmp_eq_XXX" builtin. If \p NegateResult is true then negate
  // the result bit.
  SILInstruction *optimizeBuiltinCompareEq(ApplyInst *AI, bool NegateResult);

  SILInstruction *optimizeApplyOfPartialApply(ApplyInst *AI,
                                              PartialApplyInst *PAI);
  SILInstruction *optimizeApplyOfConvertFunctionInst(ApplyInst *AI,
                                                     ConvertFunctionInst *CFI);

private:
  /// Perform one SILCombine iteration.
  bool doOneIteration(SILFunction &F, unsigned Iteration);

  /// Worklist containing all of the instructions primed for simplification.
  SILCombineWorklist Worklist;
  /// Variable to track if the SILCombiner made any changes.
  bool MadeChange;
  /// If set to true then the optimizer is free to erase cond_fail instructions.
  bool RemoveCondFails;
  /// The current iteration of the SILCombine.
  unsigned Iteration;
  /// Builder used to insert instructions.
  SILBuilder *Builder;
  /// A list that the builder inserts newly created instructions into. Its
  /// contents are added to the worklist after every iteration and then the list
  /// is cleared.
  llvm::SmallVector<SILInstruction *, 64> TrackingList;
};

} // end namespace swift

//===----------------------------------------------------------------------===//
//                         SILCombine Implementation
//===----------------------------------------------------------------------===//

/// addReachableCodeToWorklist - Walk the function in depth-first order, adding
/// all reachable code to the worklist.
///
/// This has a couple of tricks to make the code faster and more powerful.  In
/// particular, we DCE instructions as we go, to avoid adding them to the
/// worklist (this significantly speeds up SILCombine on code where many
/// instructions are dead or constant).
static void addReachableCodeToWorklist(SILBasicBlock *BB, SILCombiner &SC) {
  llvm::SmallVector<SILBasicBlock*, 256> Worklist;
  llvm::SmallVector<SILInstruction*, 128> InstrsForSILCombineWorklist;
  llvm::SmallPtrSet<SILBasicBlock*, 64> Visited;

  Worklist.push_back(BB);
  do {
    BB = Worklist.pop_back_val();

    // We have now visited this block!  If we've already been here, ignore it.
    if (!Visited.insert(BB)) continue;

    for (SILBasicBlock::iterator BBI = BB->begin(), E = BB->end(); BBI != E; ) {
      SILInstruction *Inst = BBI++;

      // DCE instruction if trivially dead.
      if (isInstructionTriviallyDead(Inst)) {
        ++NumDeadInst;
        DEBUG(llvm::dbgs() << "SC: DCE: " << *Inst << '\n');
        Inst->eraseFromParent();
        continue;
      }

      InstrsForSILCombineWorklist.push_back(Inst);
    }

    // Recursively visit successors.
    for (auto SI = BB->succ_begin(), SE = BB->succ_end(); SI != SE; ++SI)
      Worklist.push_back(*SI);
  } while (!Worklist.empty());

  // Once we've found all of the instructions to add to the worklist, add them
  // in reverse order. This way SILCombine will visit from the top of the
  // function down. This jives well with the way that it adds all uses of
  // instructions to the worklist after doing a transformation, thus avoiding
  // some N^2 behavior in pathological cases.
  SC.addInitialGroup(InstrsForSILCombineWorklist);
}

bool SILCombiner::doOneIteration(SILFunction &F, unsigned Iteration) {
  MadeChange = false;

  DEBUG(llvm::dbgs() << "\n\nSILCOMBINE ITERATION #" << Iteration << " on "
                     << F.getName() << "\n");

  // Add reachable instructions to our worklist.
  addReachableCodeToWorklist(F.begin(), *this);

  // Process until we run out of items in our worklist.
  while (!Worklist.isEmpty()) {
    SILInstruction *I = Worklist.removeOne();

    // When we erase an instruction, we use the map in the worklist to check if
    // the instruction is in the worklist. If it is, we replace it with null
    // instead of shifting all members of the worklist towards the front. This
    // check makes sure that if we run into any such residual null pointers, we
    // skip them.
    if (I == 0)
      continue;

    // Check to see if we can DCE the instruction.
    if (isInstructionTriviallyDead(I)) {
      DEBUG(llvm::dbgs() << "SC: DCE: " << *I << '\n');
      eraseInstFromFunction(*I);
      ++NumDeadInst;
      MadeChange = true;
      continue;
    }

    // Check to see if we can instsimplify the instruction.
    if (SILValue Result = simplifyInstruction(I)) {
      ++NumSimplified;

      DEBUG(llvm::dbgs() << "SC: Simplify Old = " << *I << '\n'
                         << "    New = " << *Result.getDef() << '\n');

      // Everything uses the new instruction now.
      replaceInstUsesWith(*I, Result.getDef(), 0, Result.getResultNumber());

      // Push the new instruction and any users onto the worklist.
      Worklist.addUsersToWorklist(Result.getDef());

      eraseInstFromFunction(*I);
      MadeChange = true;
      continue;
    }

    // If we have reached this point, all attempts to do simple simplifications
    // have failed. Prepare to SILCombine.
    Builder->setInsertionPoint(I->getParent(), I);

#ifndef NDEBUG
    std::string OrigI;
#endif
    DEBUG(llvm::raw_string_ostream SS(OrigI); I->print(SS); OrigI = SS.str(););
    DEBUG(llvm::dbgs() << "SC: Visiting: " << OrigI << '\n');

    if (SILInstruction *Result = visit(I)) {
      ++NumCombined;
      // Should we replace the old instruction with a new one?
      if (Result != I) {
        // Insert the new instruction into the basic block.
        I->getParent()->getInstList().insert(I, Result);

        DEBUG(llvm::dbgs() << "SC: Old = " << *I << '\n'
                           << "    New = " << *Result << '\n');

        // Everything uses the new instruction now.
        replaceInstUsesWith(*I, Result);

        // Push the new instruction and any users onto the worklist.
        Worklist.add(Result);
        Worklist.addUsersToWorklist(Result);


        eraseInstFromFunction(*I);
      } else {
        DEBUG(llvm::dbgs() << "SC: Mod = " << OrigI << '\n'
                     << "    New = " << *I << '\n');

        // If the instruction was modified, it's possible that it is now dead.
        // if so, remove it.
        if (isInstructionTriviallyDead(I)) {
          eraseInstFromFunction(*I);
        } else {
          Worklist.add(I);
          Worklist.addUsersToWorklist(I);
        }
      }
      MadeChange = true;
    }

    // Our tracking list has been accumulating instructions created by the
    // SILBuilder during this iteration. Go through the tracking list and add
    // its contents to the worklist and then clear said list in preparation for
    // the next iteration.
    for (SILInstruction *I : TrackingList)
      Worklist.add(I);
    TrackingList.clear();
  }

  Worklist.zap();
  return MadeChange;
}

//===----------------------------------------------------------------------===//
//                                  Visitors
//===----------------------------------------------------------------------===//

SILInstruction *SILCombiner::visitSwitchEnumAddrInst(SwitchEnumAddrInst *SEAI) {
  // Promote switch_enum_addr to switch_enum. Detect the pattern:
  //store %X to %Y#1 : $*Optional<SomeClass>
  //switch_enum_addr %Y#1 : $*Optional<SomeClass>, case ...

  SILBasicBlock::iterator it = SEAI;

  // Retains are moved as far down the block as possible, so we should skip over
  // them when we search backwards for a store.
  do {
    if (it == SEAI->getParent()->begin())
      return nullptr;
    --it;
  } while (isa<RetainValueInst>(it));

  if (StoreInst *SI = dyn_cast<StoreInst>(it)) {
    SILValue EnumVal = SI->getSrc();

    // Make sure that the store destination and the switch address is the same
    // address.
    if (SI->getDest() != SEAI->getOperand())
      return nullptr;

    SmallVector<std::pair<EnumElementDecl*, SILBasicBlock*>, 8> Cases;
    for (int i = 0, e = SEAI->getNumCases(); i < e; ++i)
      Cases.push_back(SEAI->getCase(i));

    SILBasicBlock *Default = SEAI->hasDefault() ? SEAI->getDefaultBB() : 0;
    Builder->createSwitchEnum(SEAI->getLoc(), EnumVal, Default, Cases);
    eraseInstFromFunction(*SEAI);
    return nullptr;
  }

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
                                                IEI->getConcreteType());
    SILValue(IEI, 0).replaceAllUsesWith(ConcAlloc->getAddressResult());
    eraseInstFromFunction(*IEI);


    for (Operand *Op: AS->getUses()) {
      if (auto *DA = dyn_cast<DestroyAddrInst>(Op->getUser())) {
        Builder->setInsertionPoint(DA);
        Builder->createDestroyAddr(DA->getLoc(), SILValue(ConcAlloc, 1));
        eraseInstFromFunction(*DA);

      }
      if (auto *DS = dyn_cast<DeallocStackInst>(Op->getUser())) {
        Builder->setInsertionPoint(DS);
        Builder->createDeallocStack(DS->getLoc(), SILValue(ConcAlloc, 0));
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
    SILValue NewLI = Builder->createLoad(LI->getLoc(), UI->getOperand());
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
    if (auto *SEI = dyn_cast<StructExtractInst>(UI->getUser())) {
      Projections.push_back({Projection(SEI), SEI});
      continue;
    }

    if (auto *TEI = dyn_cast<TupleExtractInst>(UI->getUser())) {
      Projections.push_back({Projection(TEI), TEI});
      continue;
    }

    // If we have any non SEI, TEI instruction, don't do anything here.
    return nullptr;
  }

  // Sort the list.
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
    // a new projection. If we have a VarDecl, create a struct_element_addr +
    // load. Make sure to update LastProj, LastNewLoad.
    if (ValueDecl *V = Proj.getDecl()) {
      assert(isa<StructExtractInst>(Inst) && "A projection with a VarDecl "
             "should be associated with a struct_extract.");

      LastProj = &Proj;
      auto *SEA =
        Builder->createStructElementAddr(LI->getLoc(), LI->getOperand(),
                                         cast<VarDecl>(V),
                                         Inst->getType(0).getAddressType());
      LastNewLoad = Builder->createLoad(LI->getLoc(), SEA);
      replaceInstUsesWith(*Inst, LastNewLoad, 0);
      eraseInstFromFunction(*Inst);
      continue;
    }

    // If we have an index, then create a new tuple_element_addr + load.
    assert(isa<TupleExtractInst>(Inst) && "A projection with an integer "
           "should be associated with a tuple_extract.");

    LastProj = &Proj;
    auto *TEA =
      Builder->createTupleElementAddr(LI->getLoc(), LI->getOperand(),
                                      Proj.getIndex(),
                                      Inst->getType(0).getAddressType());
    LastNewLoad = Builder->createLoad(LI->getLoc(), TEA);
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

SILInstruction *SILCombiner::visitPartialApplyInst(PartialApplyInst *PAI) {
  // partial_apply without any substitutions or arguments is just a
  // thin_to_thick_function.
  if (!PAI->hasSubstitutions() && (PAI->getNumArguments() == 0))
    return new (PAI->getModule()) ThinToThickFunctionInst(PAI->getLoc(),
                                                          PAI->getCallee(),
                                                          PAI->getType());

  // Delete dead closures of this form:
  //
  // %X = partial_apply %x(...)    // has 1 use.
  // strong_release %X;

  // Only handle PartialApplyInst with one use.
  if (!PAI->hasOneUse())
    return nullptr;

  SILLocation Loc = PAI->getLoc();

  // The single user must be the StrongReleaseInst.
  if (auto *SRI = dyn_cast<StrongReleaseInst>(PAI->use_begin()->getUser())) {
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

    // Set the insertion point of the release_value to be that of the release,
    // which is the end of the lifetime of the partial_apply.
    auto OrigInsertPoint = Builder->getInsertionPoint();
    SILInstruction *SingleUser = PAI->use_begin()->getUser();
    Builder->setInsertionPoint(SingleUser);

    for (unsigned AI = 0, AE = Args.size(); AI != AE; ++AI) {
      SILValue Arg = Args[AI];
      auto Param = Params[AI + Delta];

      if (!Param.isIndirect() && Param.isConsumed())
        if (!Arg.getType().isAddress())
          Builder->createReleaseValue(Loc, Arg);
    }

    Builder->setInsertionPoint(OrigInsertPoint);

    // Delete the strong_release.
    eraseInstFromFunction(*SRI);
    // Delete the partial_apply.
    return eraseInstFromFunction(*PAI);
  }
  return nullptr;
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
                                        Subs, Args, AI->isTransparent());

  // We also need to release the partial_apply instruction itself because it
  // is consumed by the apply_instruction.
  Builder->createStrongRelease(AI->getLoc(), PAI);

  replaceInstUsesWith(*AI, NAI);
  return eraseInstFromFunction(*AI);
}

SILInstruction *SILCombiner::optimizeBuiltinCanBeObjCClass(ApplyInst *AI) {
  assert(AI->hasSubstitutions() && "Expected substitutions for canBeClass");

  auto const &Subs = AI->getSubstitutions();
  assert((Subs.size() == 1) &&
         "Expected one substitution in call to canBeClass");

  auto Ty = Subs[0].getReplacement()->getCanonicalType();
  switch (Ty->canBeClass()) {
  case TypeTraitResult::IsNot:
    return IntegerLiteralInst::create(AI->getLoc(), AI->getType(),
                                      APInt(1, 0), *AI->getFunction());
  case TypeTraitResult::Is:
    return IntegerLiteralInst::create(AI->getLoc(), AI->getType(),
                                      APInt(1, 1), *AI->getFunction());
  case TypeTraitResult::CanBe:
    return nullptr;
  }
}

SILInstruction *SILCombiner::optimizeBuiltinCompareEq(ApplyInst *AI,
                                                       bool NegateResult) {
  IsZeroKind LHS = isZeroValue(AI->getArgument(0));
  IsZeroKind RHS = isZeroValue(AI->getArgument(1));

  // Can't handle unknown values.
  if (LHS == IsZeroKind::Unknown || RHS == IsZeroKind::Unknown)
    return nullptr;

  // Can't handle non-zero ptr values.
  if (LHS == IsZeroKind::NotZero && RHS == IsZeroKind::NotZero)
    return nullptr;

  // Set to true if both sides are zero. Set to false if only one side is zero.
  bool Val = (LHS == RHS) ^ NegateResult;

  return IntegerLiteralInst::create(AI->getLoc(), AI->getType(), APInt(1, Val),
                                    *AI->getFunction());
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
      Args.push_back(Builder->createUncheckedAddrCast(AI->getLoc(),
                                                      Op, NewOpType));
    } else if (OldOpType.isHeapObjectReferenceType()) {
      assert(NewOpType.isHeapObjectReferenceType() &&
             "refs should map to refs.");
      Args.push_back(Builder->createUncheckedRefCast(AI->getLoc(),
                                                     Op, NewOpType));
    } else {
      Args.push_back(Op);
    }
  }  

  SILType CCSILTy = SILType::getPrimitiveObjectType(ConvertCalleeTy);
  // Create the new apply inst.
  return ApplyInst::create(AI->getLoc(), FRI, CCSILTy,
                           ConvertCalleeTy->getSILResult(),
                           ArrayRef<Substitution>(), Args, false,
                           *FRI->getReferencedFunction());
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

/// \brief Returns a list of instructions that only write into
// / the the array \p Inst.
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
    if (auto SI = dyn_cast<IndexAddrInst>(Op->getUser()))
      if (recursivelyCollectArrayWritesInstr(Uses, SI))
        continue;

    return false;
  }

  return true;
}

SILInstruction *SILCombiner::visitApplyInst(ApplyInst *AI) {
  // Optimize apply{partial_apply(x,y)}(z) -> apply(z,x,y).
  if (auto *PAI = dyn_cast<PartialApplyInst>(AI->getCallee()))
    return optimizeApplyOfPartialApply(AI, PAI);

  if (auto *BFRI = dyn_cast<BuiltinFunctionRefInst>(AI->getCallee())) {
    if (BFRI->getBuiltinInfo().ID == BuiltinValueKind::CanBeObjCClass)
      return optimizeBuiltinCanBeObjCClass(AI);

    if (BFRI->getBuiltinInfo().ID == BuiltinValueKind::ICMP_EQ)
      return optimizeBuiltinCompareEq(AI, /*Negate Eq result*/ false);

    if (BFRI->getBuiltinInfo().ID == BuiltinValueKind::ICMP_NE)
      return optimizeBuiltinCompareEq(AI, /*Negate Eq result*/ true);
  }

  if (auto *CFI = dyn_cast<ConvertFunctionInst>(AI->getCallee()))
    return optimizeApplyOfConvertFunctionInst(AI, CFI);

  // Optimize readonly functions with no meaningful users.
  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(AI->getCallee());
  if (FRI && FRI->getReferencedFunction()->getEffectsInfo() <
      EffectsKind::ReadWrite){
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
    }

    // We found a user that we can't handle.
    return nullptr;
  }

  // Optimize sub(x - x) -> 0.
  if (AI->getNumOperands() == 3 &&
      match(AI, m_ApplyInst(BuiltinValueKind::Sub, m_ValueBase())) &&
      AI->getOperand(1) == AI->getOperand(2))
    if (auto DestTy = AI->getType().getAs<BuiltinIntegerType>())
      return IntegerLiteralInst::create(AI->getLoc(), AI->getType(),
                                        APInt(DestTy->getGreatestWidth(), 0),
                                        *AI->getFunction());

  // Optimize sub(ptrtoint(index_raw_pointer(v, x)), ptrtoint(v)) -> x.
  ApplyInst *Bytes2;
  IndexRawPointerInst *Indexraw;
  if (AI->getNumOperands() == 3 &&
      match(AI, m_ApplyInst(BuiltinValueKind::Sub,
                            m_ApplyInst(BuiltinValueKind::PtrToInt,
                                        m_IndexRawPointerInst(Indexraw)),
                            m_ApplyInst(Bytes2)))) {
    if (match(Bytes2, m_ApplyInst(BuiltinValueKind::PtrToInt, m_ValueBase()))) {
      if (Indexraw->getOperand(0) == Bytes2->getOperand(1) &&
          Indexraw->getOperand(1).getType() == AI->getType()) {
        replaceInstUsesWith(*AI, Indexraw->getOperand(1).getDef());
        return eraseInstFromFunction(*AI);
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

  // Canonicalize multiplication by a stride to be such that the stride is
  // always the second argument.
  if (AI->getNumOperands() != 4)
    return nullptr;

  if (match(AI, m_ApplyInst(BuiltinValueKind::SMulOver,
                            m_ApplyInst(BuiltinValueKind::Strideof),
                            m_ValueBase(), m_IntegerLiteralInst())) ||
      match(AI, m_ApplyInst(BuiltinValueKind::SMulOver,
                            m_ApplyInst(BuiltinValueKind::StrideofNonZero),
                            m_ValueBase(), m_IntegerLiteralInst()))) {
    AI->swapOperands(1, 2);
    return AI;
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitAllocArrayInst(AllocArrayInst *AAI) {
  UserListTy Users;
  // If the array alloc is only written into then it can be removed.
  if (recursivelyCollectArrayWritesInstr(Users, AAI)) {
    // Erase all of the reference counting instructions and the array
    // allocation instruction.
    for (auto rit = Users.rbegin(), re = Users.rend(); rit != re; ++rit)
      eraseInstFromFunction(**rit);
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
  if (IEAI->getOperand().getType().isAddressOnly(IEAI->getModule()))
    return nullptr;

  // If the enum does not have a payload create the enum/store since we don't
  // need to worry about payloads.
  if (!IEAI->getElement()->hasArgumentType()) {
    EnumInst *E =
      Builder->createEnum(IEAI->getLoc(), SILValue(), IEAI->getElement(),
                          IEAI->getOperand().getType().getObjectType());
    Builder->createStore(IEAI->getLoc(), E, IEAI->getOperand());
    return eraseInstFromFunction(*IEAI);
  }

  // Ok, we have a payload enum, make sure that we have a store previous to
  // us...
  SILBasicBlock::iterator II = IEAI;
  if (II == IEAI->getParent()->begin())
    return nullptr;
  --II;
  auto *SI = dyn_cast<StoreInst>(&*II);
  if (!SI)
    return nullptr;

  // ... whose destination is taken from an init_enum_data_addr whose only user
  // is the store that points to the same allocation as our inject_enum_addr. We
  // enforce such a strong condition as being directly previously since we want
  // to avoid any flow issues.
  auto *IEDAI = dyn_cast<InitEnumDataAddrInst>(SI->getDest().getDef());
  if (!IEDAI || IEDAI->getOperand() != IEAI->getOperand() ||
      !IEDAI->hasOneUse())
    return nullptr;

  // In that case, create the payload enum/store.
  EnumInst *E =
      Builder->createEnum(IEDAI->getLoc(), SI->getSrc(), IEDAI->getElement(),
                          IEDAI->getOperand().getType().getObjectType());
  Builder->createStore(IEDAI->getLoc(), E, IEDAI->getOperand());

  // Cleanup.
  eraseInstFromFunction(*SI);
  eraseInstFromFunction(*IEDAI);
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
  ApplyInst *Bytes;
  MetatypeInst *Metatype;
  if (match(PTAI->getOperand(),
            m_IndexRawPointerInst(m_ValueBase(),
                                  m_TupleExtractInst(m_ApplyInst(Bytes), 0)))) {
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
      SILValue Distance = Bytes->getArgument(0);
      auto *NewPTAI =
          Builder->createPointerToAddress(PTAI->getLoc(), Ptr, PTAI->getType());
      return new (PTAI->getModule())
          IndexAddrInst(PTAI->getLoc(), NewPTAI, Distance);
    }
  }

  return nullptr;
}

/// Prove that Ty1 is layout compatible with Ty2. This is separate from the
/// implementation in SILType since we are only interested in rewritting
/// unchecked_addr_cast from structs, enums into respectively fields, payloads.
///
/// TODO: Refactor this into SILType?
static bool areLayoutCompatibleTypes(SILType Ty1, SILType Ty2, SILModule &Mod,
                                     llvm::SmallVectorImpl<Projection> &Projs) {
  // If Ty1 == Ty2, they must be layout compatible.
  if (Ty1 == Ty2)
    return true;

  // We do not know the final types of generics implying we can not know if they
  // are layout compatible.
  if (Ty1.hasArchetype() || Ty2.hasArchetype())
    return false;

  SILType TyIter = Ty2;

  while (true) {
    // If this type is the type we are searching for (Ty1), we have succeeded,
    // return.
    if (TyIter == Ty1)
      return true;

    // Then if we have an enum...
    if (EnumDecl *E = TyIter.getEnumOrBoundGenericEnum()) {
      // Add the first payloaded field to the list. If we have no payloads,
      // bail.
      bool FoundResult = false;
      for (EnumElementDecl *Elt : E->getAllElements()) {
        if (Elt->hasArgumentType()) {
          TyIter = TyIter.getEnumElementType(Elt, Mod);
          Projs.push_back({TyIter, Elt});
          FoundResult = true;
          break;
        }
      }

      if (FoundResult)
        continue;
      return false;
    }

    // Then if we have a struct address...
    if (StructDecl *S = TyIter.getStructOrBoundGenericStruct()) {
      // Look through its stored properties.
      auto Range = S->getStoredProperties();

      // If it has no stored properties, there is nothing we can do, bail.
      if (Range.begin() == Range.end())
        return false;

      // Grab the first field.
      auto Iter = Range.begin();
      VarDecl *FirstVar = *Iter;
      ++Iter;

      // If we have more than one stored field, the struct is not able to have
      // layout compatible relationships with any of its fields.
      if (Iter != Range.end())
        return false;

      // Otherwise we can search into the structs fields.
      TyIter = TyIter.getFieldType(FirstVar, Mod);
      Projs.push_back({TyIter, FirstVar, Projection::NominalType::Struct});
      continue;
    }

    // If we reached this point, then this type has no subrecords we are
    // interested in. Thus we have failed. Return false.
    return false;
  }
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

  // *NOTE* InstSimplify already handles the identity case so we don't need to
  // worry about that problem here and can assume that the cast types are
  // different.
  llvm::SmallVector<Projection, 4> Projs;

  // Given (unchecked_addr_cast x X->Y), we prove that Y is layout compatible
  // with X as an aggregate. If we can do that, then we can rewrite the cast as
  // a typed GEP.
  if (areLayoutCompatibleTypes(UADCI->getType(),
                               UADCI->getOperand().getType(),
                               Mod,
                               Projs)) {
    SILBuilder Builder(UADCI);
    SILValue V = UADCI->getOperand();

    for (auto &P : Projs) {
      if (P.getNominalType() == Projection::NominalType::Struct) {
        V = Builder.createStructElementAddr(UADCI->getLoc(),
                                            V, cast<VarDecl>(P.getDecl()),
                                            P.getType());
      } else {
        assert(P.getNominalType() == Projection::NominalType::Enum &&
               "We only support rewriting of reinterpretCasts into enums and "
               "structs");
        V = Builder.createUncheckedTakeEnumDataAddr(
            UADCI->getLoc(), V, cast<EnumElementDecl>(P.getDecl()),
            P.getType());
      }
    }

    return replaceInstUsesWith(*UADCI, V.getDef(), 0);
  }

  // Ok, we can't rewrite this one. See if we have all loads from this
  // instruction. If we do, load the original type and create a bitcast.

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

  // For each user U of the unchecked_addr_cast...
  for (auto U : UADCI->getUses())
    // Check if it is load. If it is not a load, bail...
    if (!isa<LoadInst>(U->getUser()))
      return nullptr;

  SILValue Op = UADCI->getOperand();
  SILLocation Loc = UADCI->getLoc();

  // Ok, we have all loads. Lets simplify this. Go back through the loads a
  // second time, rewriting them into a load + bitcast from our source.
  for (auto U : UADCI->getUses()) {
    // Grab the original load.
    LoadInst *L = cast<LoadInst>(U->getUser());

    // Insert a new load from our source and bitcast that as appropriate.
    LoadInst *NewLoad = Builder->createLoad(Loc, Op);
    SILInstruction *BitCast = nullptr;
    if (OutputIsTrivial)
      BitCast = Builder->createUncheckedTrivialBitCast(Loc, NewLoad,
                                                       OutputTy.getObjectType());
    else
      BitCast = Builder->createUncheckedRefBitCast(Loc, NewLoad,
                                                   OutputTy.getObjectType());

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

  return nullptr;
}

SILInstruction *SILCombiner::visitUnconditionalCheckedCastInst(
                               UnconditionalCheckedCastInst *UCCI) {
  // FIXME: rename from RemoveCondFails to RemoveRuntimeAsserts.
  if (RemoveCondFails) {
    if (UCCI->getOperand().getType().isAddress()) {
      // unconditional_checked_cast -> unchecked_addr_cast
      return new (UCCI->getModule()) UncheckedAddrCastInst(
        UCCI->getLoc(), UCCI->getOperand(), UCCI->getType());
    } else if (UCCI->getOperand().getType().isHeapObjectReferenceType()) {
      // unconditional_checked_cast -> unchecked_ref_cast
      return new (UCCI->getModule()) UncheckedRefCastInst(
        UCCI->getLoc(), UCCI->getOperand(), UCCI->getType());
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
  SILValue EnumAddr = TEDAI->getOperand();
  EnumElementDecl *EnumElt = TEDAI->getElement();
  SILType PayloadType = TEDAI->getType().getObjectType();

  // Go back through a second time now that we know all of our users are
  // loads. Perform the transformation on each load.
  for (auto U : TEDAI->getUses()) {
    // Grab the load.
    LoadInst *L = cast<LoadInst>(U->getUser());

    // Insert a new Load of the enum and extract the data from that.
    auto *D = Builder->createUncheckedEnumData(
        Loc, Builder->createLoad(Loc, EnumAddr), EnumElt, PayloadType);

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

SILInstruction *SILCombiner::visitEnumIsTagInst(EnumIsTagInst *EIT) {
  auto *EI = dyn_cast<EnumInst>(EIT->getOperand());
  if (!EI)
    return nullptr;

  bool SameTag = EI->getElement() == EIT->getElement();
  return IntegerLiteralInst::create(EIT->getLoc(), EIT->getType(),
                                    APInt(1, SameTag), *EIT->getFunction());
}

//===----------------------------------------------------------------------===//
//                                Entry Points
//===----------------------------------------------------------------------===//

namespace {

class SILCombine : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() override {
    SILCombiner Combiner(getOptions().RemoveRuntimeAsserts);
    bool Changed = Combiner.runOnFunction(*getFunction());
    if (Changed)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }

  StringRef getName() override { return "SIL Combine"; }
};

} // end anonymous namespace

SILTransform *swift::createSILCombine() {
  return new SILCombine();
}
