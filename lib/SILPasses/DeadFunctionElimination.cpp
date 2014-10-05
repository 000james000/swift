//===--- DeadFunctionElimination.cpp - Eliminate dead functions -----------===//
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

#define DEBUG_TYPE "sil-dead-function-elimination"
#include "swift/SILAnalysis/CallGraphAnalysis.h"
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

STATISTIC(NumDeadFunc, "Number of dead functions eliminated");

namespace {
struct FinalEliminator {
  llvm::SmallSetVector<SILFunction*, 16> Worklist;

  // Update module information before actually removing a SILFunction.
  void updateBeforeRemoveFunction(SILFunction *F) {
    // Collect all FRIs and insert the callees to the work list.
    for (auto &BB : *F)
      for (auto &I : BB)
        if (auto FRI = dyn_cast<FunctionRefInst>(&I))
          Worklist.insert(FRI->getReferencedFunction());
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                             Utility Functions
//===----------------------------------------------------------------------===//

bool tryToRemoveFunction(SILFunction *F, FinalEliminator *FE = nullptr) {
  
  SILModule &M = F->getModule();
  
  // Remove internal functions that are not referenced by anything.
  // TODO: main is currently marked as internal so we explicitly check
  // for functions with this name and keep them around.
  if (isPossiblyUsedExternally(F->getLinkage(), M.isWholeModule()) ||
      F->getRefCount() || F->getName() == SWIFT_ENTRY_POINT_FUNCTION)
    return false;

  if (F->getLoweredFunctionType()->getAbstractCC() == AbstractCC::ObjCMethod) {
    // ObjC functions are called through the runtime and are therefore alive
    // even if not referenced inside SIL.
    return false;
  }
  
  DEBUG(llvm::dbgs() << "DEAD FUNCTION ELIMINATION: Erasing:" << F->getName()
                     << "\n");
  if (FE) {
    FE->updateBeforeRemoveFunction(F);
  }

  M.eraseFunction(F);
  NumDeadFunc++;
  return true;
}

//===----------------------------------------------------------------------===//
//                      Pass Definition and Entry Points
//===----------------------------------------------------------------------===//

namespace {

class SILDeadFuncElimination : public SILModuleTransform {

  void run() override {
    CallGraphAnalysis *CGA = PM->getAnalysis<CallGraphAnalysis>();
    SILModule *M = getModule();
    bool Changed = false;

    // Erase trivially dead functions that may not be a part of the call graph.
    for (auto FI = M->begin(), EI = M->end(); FI != EI;) {
      SILFunction *F = FI++;
      Changed |= tryToRemoveFunction(F);
    }

    if (Changed)
      CGA->invalidate(SILAnalysis::InvalidationKind::CallGraph);

    // If we are debugging serialization, don't eliminate any dead functions.
    if (getOptions().DebugSerialization)
      return;

    auto &CG = CGA->getCallGraph();
    // A bottom-up list of functions, leafs first.
    const std::vector<SILFunction *> &Order = CG.getBottomUpFunctionOrder();

    // Scan the call graph top-down (caller first) because eliminating functions
    // can generate more opportunities.
    for (auto I = Order.rbegin(), E = Order.rend(); I != E; ++I)
      Changed |= tryToRemoveFunction(*I);

    // Invalidate the call graph.
    if (Changed)
      invalidateAnalysis(SILAnalysis::InvalidationKind::CallGraph);
  }

  StringRef getName() override { return "Dead Function Elimination"; }
};

} // end anonymous namespace

SILTransform *swift::createDeadFunctionElimination() {
  return new SILDeadFuncElimination();
}

bool swift::performSILElimination(SILModule *M) {
  bool Changed = false;
  llvm::SmallSet<SILFunction *, 16> removedFuncs;

  FinalEliminator FE;

  for (auto FI = M->begin(), EI = M->end(); FI != EI;) {
    SILFunction *F = FI++;
    if (tryToRemoveFunction(F, &FE)) {
      Changed = true;
      removedFuncs.insert(F);
    }
  }

  while (!FE.Worklist.empty()) {
    llvm::SmallSetVector<SILFunction *, 16> CurrWorklist = FE.Worklist;
    FE.Worklist.clear();
    for (auto entry : CurrWorklist)
      if (!removedFuncs.count(entry))
        if (tryToRemoveFunction(entry, &FE)) {
          Changed = true;
          removedFuncs.insert(entry);
        }
  }
  return Changed;
}
