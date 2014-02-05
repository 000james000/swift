//===------------- SILSROA.cpp - Scalar Replacement of Aggregates  --------===//
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
// Change aggregate values into scalar values. Currently it takes every
// allocation and chops them up into their smallest non-captured pieces.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-sroa"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Range.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SILPasses/PassManager.h"
#include "swift/SILPasses/Passes.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Debug.h"
#include <type_traits>

using namespace swift;

STATISTIC(NumEscapingAllocas, "Number of aggregate allocas not chopped up "
          "due to uses.");
STATISTIC(NumChoppedAllocas, "Number of chopped up aggregate allocas.");
STATISTIC(NumUnhandledAllocas, "Number of non struct, tuple allocas.");

namespace {

class SROAMemoryUseAnalyzer {
  // The allocation we are analyzing.
  AllocStackInst *AI;

  // Loads from AI.
  llvm::SmallVector<LoadInst *, 4> Loads;
  // Stores to AI.
  llvm::SmallVector<StoreInst *, 4> Stores;
  // Dealloc instructions for AI.
  llvm::SmallVector<DeallocStackInst *, 4> Deallocs;
  // Instructions which extract from aggregates.
  llvm::SmallVector<SILInstruction *, 4> ExtractInsts;

  // TupleType if we are visiting a tuple.
  TupleType *TT = nullptr;
  // StructDecl if we are visiting a struct.
  StructDecl *SD = nullptr;
public:
  SROAMemoryUseAnalyzer(AllocStackInst *AI) : AI(AI) {
    assert(AI && "AI should never be null here.");
  }

  bool analyze();
  void chopUpAlloca(std::vector<AllocStackInst *> &Worklist);

private:
  SILInstruction *createAgg(SILBuilder &B, SILLocation Loc, SILType Ty,
                                  ArrayRef<SILValue> Elements);
  SILInstruction *createAggProjection(SILBuilder &B, SILLocation Loc,
                                      SILValue Operand, unsigned EltNo);
  unsigned getEltNoForProjection(SILInstruction *Inst);
  void createAllocas(llvm::SmallVector<AllocStackInst *, 4> &NewAllocations);
};

} // end anonymous namespace

SILInstruction *
SROAMemoryUseAnalyzer::createAgg(SILBuilder &B, SILLocation Loc,
                                       SILType Ty,
                                       ArrayRef<SILValue> Elements) {
  if (TT)
    return B.createTuple(Loc, Ty, Elements);

  assert(SD && "SD must not be null here since it or TT must be set to call"
         " this method.");
  return B.createStruct(Loc, Ty, Elements);
}

SILInstruction *
SROAMemoryUseAnalyzer::createAggProjection(SILBuilder &B, SILLocation Loc,
                                           SILValue Operand,
                                           unsigned EltNo) {
  if (TT)
    return B.createTupleExtract(Loc, Operand, EltNo);

  assert(SD && "SD should not be null since either it or TT must be set at "
         "this point.");

  auto Properties = SD->getStoredProperties();
  unsigned Counter = 0;
  for (auto *D : Properties)
    if (Counter++ == EltNo)
      return B.createStructExtract(Loc, Operand, D);
  llvm_unreachable("Unknown field.");
}

unsigned SROAMemoryUseAnalyzer::getEltNoForProjection(SILInstruction *Inst) {
  if (TT)
    return cast<TupleElementAddrInst>(Inst)->getFieldNo();

  assert(SD && "SD should not be null since either it or TT must be set at "
         "this point.");
  StructElementAddrInst *SEA = cast<StructElementAddrInst>(Inst);
  VarDecl *Field = SEA->getField();
  unsigned EltNo = 0;
  for (auto *D : SD->getStoredProperties()) {
    if (D == Field)
      return EltNo;
    ++EltNo;
  }
  llvm_unreachable("Unknown field.");
}

bool SROAMemoryUseAnalyzer::analyze() {
  // We only know how to split structs and tuples... So if we have a scalar or a
  // different sort of aggregate, bail.
  SILType Type = SILValue(AI, 1).getType();
  if (!(TT = Type.getAs<TupleType>()) &&
      !(SD = Type.getStructOrBoundGenericStruct())) {
    ++NumUnhandledAllocas;
    return false;
  }

  // Go through uses of the memory allocation of AI...
  for (auto *Operand : SILValue(AI, 1).getUses()) {
    SILInstruction *User = Operand->getUser();
    DEBUG(llvm::dbgs() << "    Visiting use: " << *User);

    // If we store the alloca pointer, we can not analyze its uses so bail...
    // It is ok if we store into the alloca pointer though.
    if (auto *SI = dyn_cast<StoreInst>(User)) {
      if (SI->getDest().getDef() == AI) {
        DEBUG(llvm::dbgs() << "        Found a store into the "
              "projection.\n");
        Stores.push_back(SI);
        continue;
      } else {
        DEBUG(llvm::dbgs() << "        Found a store of the "
              "projection pointer. Escapes!.\n");
        ++NumEscapingAllocas;
        return false;
      }
    }

    // If the use is a load, keep track of it for splitting later...
    if (auto *LI = dyn_cast<LoadInst>(User)) {
      DEBUG(llvm::dbgs() << "        Found a load of the projection.\n");
      Loads.push_back(LI);
      continue;
    }

    // If the use is a struct_element_addr, add it to the worklist so we check
    // if it or one of its descendents escape.
    if (auto *ASI = dyn_cast<StructElementAddrInst>(User)) {
      DEBUG(llvm::dbgs() << "        Found a struct subprojection!\n");
      ExtractInsts.push_back(ASI);
      continue;
    }

    // If the use is a tuple_element_addr, add it to the worklist so we check
    // if it or one of its descendents escape.
    if (auto *TSI = dyn_cast<TupleElementAddrInst>(User)) {
      DEBUG(llvm::dbgs() << "        Found a tuple subprojection!\n");
      ExtractInsts.push_back(TSI);
      continue;
    }

    // Otherwise we do not understand this instruction, so bail.
    DEBUG(llvm::dbgs() << "        Found unknown user, pointer escapes!\n");
    ++NumEscapingAllocas;
    return false;
  }

  // Analysis was successful. We can break up this allocation!
  ++NumChoppedAllocas;
  return true;
}

void
SROAMemoryUseAnalyzer::
createAllocas(llvm::SmallVector<AllocStackInst *, 4> &NewAllocations) {
  SILBuilder B(AI);
  SILType Type = AI->getType(1).getObjectType();

  if (TT) {
    for (unsigned EltNo : indices(TT->getElementTypes())) {
      SILType EltTy = Type.getTupleElementType(EltNo);
      NewAllocations.push_back(B.createAllocStack(AI->getLoc(), EltTy));
    }
  } else {
    assert(SD && "SD should not be null since either it or TT must be set at "
           "this point.");
    SILModule &M = AI->getModule();
    for (auto *D : SD->getStoredProperties())
      NewAllocations.push_back(B.createAllocStack(AI->getLoc(),
                                                  Type.getFieldType(D, M)));
  }
}

void SROAMemoryUseAnalyzer::chopUpAlloca(std::vector<AllocStackInst *> &Worklist) {
  // Create allocations for this instruction.
  llvm::SmallVector<AllocStackInst *, 4> NewAllocations;
  createAllocas(NewAllocations);
  // Add the new allocations to the worklist for recursive processing.
  //
  // TODO: Change this into an assert. For some reason I am running into compile
  // issues when I try it now.
  for (auto *AI : NewAllocations)
    Worklist.push_back(AI);

  // Change any aggregate loads into field loads + aggregate structure.
  for (auto *LI : Loads) {
    SILBuilder B(LI);
    llvm::SmallVector<SILValue, 4> Elements;
    for (auto *NewAI : NewAllocations)
      Elements.push_back(B.createLoad(LI->getLoc(), SILValue(NewAI, 1)));
    auto *Agg = createAgg(B, LI->getLoc(), LI->getType().getObjectType(),
                          Elements);
    SILValue(LI).replaceAllUsesWith(Agg);
    LI->eraseFromParent();
  }

  // Change any aggregate stores into extracts + field stores.
  for (auto *SI : Stores) {
    SILBuilder B(SI);
    for (unsigned EltNo : indices(NewAllocations))
      B.createStore(SI->getLoc(),
                    createAggProjection(B, SI->getLoc(), SI->getSrc(), EltNo),
                    SILValue(NewAllocations[EltNo], 1));
    SI->eraseFromParent();
  }

  // Forward any field extracts to the new allocation.
  for (auto *Ext : ExtractInsts) {
    SILValue NewValue = SILValue(NewAllocations[getEltNoForProjection(Ext)], 1);
    SILValue(Ext).replaceAllUsesWith(NewValue);
    Ext->eraseFromParent();
  }

  // Find all dealloc instruction that touch the local storage handle for AI
  // and then chop them up.
  for (auto *Operand : SILValue(AI, 0).getUses()) {
    SILInstruction *User = Operand->getUser();
    SILBuilder B(User);

    // If the use is a DSI, add it to our memory analysis so that if we can chop
    // up allocas, we also chop up the relevant dealloc stack insts.
    if (auto *DSI = dyn_cast<DeallocStackInst>(User)) {
      DEBUG(llvm::dbgs() << "        Found DeallocStackInst!\n");
      // Create the allocations in reverse order.
      for (auto *NewAI : swift::reversed(NewAllocations))
        B.createDeallocStack(DSI->getLoc(), SILValue(NewAI));
      DSI->eraseFromParent();
    }
  }

  AI->eraseFromParent();
}

static void runSROAOnFunction(SILFunction &Fn) {
  std::vector<AllocStackInst *> Worklist;

  // For each basic block BB in Fn...
  for (auto &BB : Fn)
    // For each instruction in BB...
    for (auto &I : BB)
      // If the instruction is an alloc stack inst, add it to the worklist.
      if (auto *AI = dyn_cast<AllocStackInst>(&I))
        Worklist.push_back(AI);

  while (!Worklist.empty()) {
    AllocStackInst *AI = Worklist.back();
    Worklist.pop_back();

    SROAMemoryUseAnalyzer Analyzer(AI);

    if (!Analyzer.analyze())
      continue;

    Analyzer.chopUpAlloca(Worklist);
  }
}

void swift::performSILSROA(SILModule *M) {
  DEBUG(llvm::dbgs() << "*** SIL SROA ***\n");

  // For each function Fn in M...
  for (auto &Fn : *M) {

    // If Fn has no basic blocks skip it...
    if (Fn.empty())
      continue;


    // Otherwise perform SROA on Fn.
    runSROAOnFunction(Fn);
  }
}

class SILSROA : public SILFunctionTrans {
  virtual ~SILSROA() {}

  /// The entry point to the transformation.
  virtual void runOnFunction(SILFunction &F, SILPassManager *PM) {
    DEBUG(llvm::dbgs() << "***** SROA on function: " << F.getName() <<
          " *****\n");

    runSROAOnFunction(F);
    PM->invalidateAllAnalisys(&F, SILAnalysis::IK_Instructions);
  }
};

SILTransform *swift::createSROA() {
  return new SILSROA();
}



