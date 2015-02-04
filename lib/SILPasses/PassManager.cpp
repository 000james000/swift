//===----- PassManager.cpp - Swift Pass Manager ---------------------------===//
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

#define DEBUG_TYPE "sil-passmanager"

#include "swift/SILPasses/PassManager.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILFunction.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/TimeValue.h"

using namespace swift;

STATISTIC(NumOptzIterations, "Number of optimization iterations");

llvm::cl::opt<std::string>
    SILPrintOnlyFun("sil-print-only-function", llvm::cl::init(""),
                    llvm::cl::desc("Only print out the sil for this function"));

llvm::cl::opt<std::string>
    SILPrintOnlyFuns("sil-print-only-functions", llvm::cl::init(""),
                    llvm::cl::desc("Only print out the sil for the functions whose name contains this substring"));

llvm::cl::list<std::string>
    SILPrintBefore("sil-print-before",
                   llvm::cl::desc("Print out the sil before passes which "
                                  "contain a string from this list."));

llvm::cl::list<std::string>
    SILPrintAfter("sil-print-after",
                  llvm::cl::desc("Print out the sil after passes which contain "
                                 "a string from this list."));

llvm::cl::list<std::string>
    SILPrintAround("sil-print-around",
                   llvm::cl::desc("Print out the sil before and after passes "
                                  "which contain a string from this list"));

static bool doPrintBefore(SILTransform *T, SILFunction *F) {
  if (!SILPrintOnlyFun.empty() && F && F->getName() != SILPrintOnlyFun)
    return false;

  if (!SILPrintOnlyFuns.empty() && F &&
      F->getName().find(SILPrintOnlyFuns, 0) != StringRef::npos)
    return true;

  auto MatchFun = [&](const std::string &Str) -> bool {
    return T->getName().find(Str) != StringRef::npos;
  };

  if (SILPrintBefore.end() !=
      std::find_if(SILPrintBefore.begin(), SILPrintBefore.end(), MatchFun))
    return true;

  if (SILPrintAround.end() !=
      std::find_if(SILPrintAround.begin(), SILPrintAround.end(), MatchFun))
    return true;

  return false;
}

static bool doPrintAfter(SILTransform *T, SILFunction *F, bool Default) {
  if (!SILPrintOnlyFun.empty() && F && F->getName() != SILPrintOnlyFun)
    return false;

  if (!SILPrintOnlyFuns.empty() && F &&
      F->getName().find(SILPrintOnlyFuns, 0) != StringRef::npos)
    return true;

  auto MatchFun = [&](const std::string &Str) -> bool {
    return T->getName().find(Str) != StringRef::npos;
  };

  if (SILPrintAfter.end() !=
      std::find_if(SILPrintAfter.begin(), SILPrintAfter.end(), MatchFun))
    return true;

  if (SILPrintAround.end() !=
      std::find_if(SILPrintAround.begin(), SILPrintAround.end(), MatchFun))
    return true;

  return Default;
}

static void printModule(SILModule *Mod) {
  if (SILPrintOnlyFun.empty() && SILPrintOnlyFuns.empty()) {
    Mod->dump();
    return;
  }
  for (auto &F : *Mod) {
    if (!SILPrintOnlyFun.empty() && F.getName().str() == SILPrintOnlyFun)
      F.dump();

    if (!SILPrintOnlyFuns.empty() &&
        F.getName().find(SILPrintOnlyFuns, 0) != StringRef::npos)
      F.dump();
  }
}

bool SILPassManager::
runFunctionPasses(llvm::ArrayRef<SILFunctionTransform*> FuncTransforms) {
  CompleteFunctions *CompleteFuncs = getAnalysis<CompleteFunctions>();
  const SILOptions &Options = getOptions();

  for (auto &F : *Mod) {
    if (F.empty() || CompleteFuncs->isComplete(&F))
      continue;

    for (auto SFT : FuncTransforms) {
      CompleteFuncs->resetChanged();
      SFT->injectPassManager(this);
      SFT->injectFunction(&F);

      if (Options.PrintPassName)
        llvm::dbgs() << "#" << NumPassesRun << " Stage: " << StageName
                     << " Pass: " << SFT->getName()
                     << ", Function: " << F.getName() << "\n";

      if (doPrintBefore(SFT, &F)) {
        llvm::dbgs() << "*** SIL function before " << StageName << " "
                     << SFT->getName() << " (" << NumOptimizationIterations
                     << ") ***\n";
        F.dump();
      }
      
      llvm::sys::TimeValue StartTime = llvm::sys::TimeValue::now();
      SFT->run();

      if (Options.TimeTransforms) {
        auto Delta = llvm::sys::TimeValue::now().nanoseconds() -
          StartTime.nanoseconds();
        llvm::dbgs() << Delta << " (" << SFT->getName() << "," <<
        F.getName() << ")\n";
      }

      // If this pass invalidated anything, print and verify.
      if (doPrintAfter(SFT, &F,
                       CompleteFuncs->hasChanged() && Options.PrintAll)) {
        llvm::dbgs() << "*** SIL function after " << StageName << " "
                     << SFT->getName() << " (" << NumOptimizationIterations
                     << ") ***\n";
        F.dump();
      }
      if (CompleteFuncs->hasChanged() && Options.VerifyAll) {
        F.verify();
      }

      ++NumPassesRun;
      if (Mod->getStage() == SILStage::Canonical
          && NumPassesRun >= Options.NumOptPassesToRun)
        return false;
    }
  }

  return true;
}

void SILPassManager::runOneIteration() {
  const SILOptions &Options = getOptions();

  DEBUG(llvm::dbgs() << "*** Optimizing the module (" << StageName
        << ") *** \n");
  if (Options.PrintAll && NumOptimizationIterations == 0) {
    llvm::dbgs() << "*** SIL module before "  << StageName
                 << " transformation (" << NumOptimizationIterations
                 << ") ***\n";
    printModule(Mod);
  }
  NumOptzIterations++;
  NumOptimizationIterations++;
  CompleteFunctions *CompleteFuncs = getAnalysis<CompleteFunctions>();
  SmallVector<SILFunctionTransform*, 16> PendingFuncTransforms;

  // For each transformation:
  for (SILTransform *ST : Transformations) {
    // Bail out if we've hit the optimization pass limit.
    if (Mod->getStage() == SILStage::Canonical
        && NumPassesRun >= Options.NumOptPassesToRun)
      return;

    // Run module transformations on the module.
    if (SILModuleTransform *SMT = llvm::dyn_cast<SILModuleTransform>(ST)) {
      // Run all function passes that we've seen since the last module pass. If
      // one of the passes asked us to stop the pass pipeline, return false.
      if (!runFunctionPasses(PendingFuncTransforms))
        return;

      PendingFuncTransforms.clear();

      CompleteFuncs->resetChanged();
      SMT->injectPassManager(this);
      SMT->injectModule(Mod);

      if (Options.PrintPassName)
        llvm::dbgs() << "#" << NumPassesRun << " Stage: " << StageName
                     << " Pass: " << SMT->getName() << " (module pass)\n";

      if (doPrintBefore(SMT, nullptr)) {
        llvm::dbgs() << "*** SIL module before " << StageName << " "
                     << SMT->getName() << " (" << NumOptimizationIterations
                     << ") ***\n";
        printModule(Mod);
      }
    
      llvm::sys::TimeValue StartTime = llvm::sys::TimeValue::now();
      SMT->run();

      if (Options.TimeTransforms) {
        auto Delta = llvm::sys::TimeValue::now().nanoseconds() -
          StartTime.nanoseconds();
        llvm::dbgs() << Delta << " (" << SMT->getName() << ",Module)\n";
      }

      // If this pass invalidated anything, print and verify.
      if (doPrintAfter(SMT, nullptr,
                       CompleteFuncs->hasChanged() && Options.PrintAll)) {
        llvm::dbgs() << "*** SIL module after " << StageName << " "
                     << SMT->getName() << " (" << NumOptimizationIterations
                     << ") ***\n";
        printModule(Mod);
      }

      if (CompleteFuncs->hasChanged() && Options.VerifyAll) {
        Mod->verify();
      }

      ++NumPassesRun;
      if (Mod->getStage() == SILStage::Canonical
          && NumPassesRun >= Options.NumOptPassesToRun) {
        return;
      }
      
      continue;
    }

    // Run function transformation on all functions.
    if (SILFunctionTransform *SFT = llvm::dyn_cast<SILFunctionTransform>(ST)) {
      PendingFuncTransforms.push_back(SFT);      
      continue;
    }

    llvm_unreachable("Unknown pass kind.");
  }

  runFunctionPasses(PendingFuncTransforms);
  CompleteFuncs->setComplete();
}

void SILPassManager::run() {
  const SILOptions &Options = getOptions();
  if (Options.PrintAll) {
    if (SILPrintOnlyFun.empty() && SILPrintOnlyFuns.empty()) {
      llvm::dbgs() << "*** SIL module before transformation ("
                   << NumOptimizationIterations << ") ***\n";
      Mod->dump();
    } else {
      for (auto &F : *Mod) {
        if (!SILPrintOnlyFun.empty() && F.getName().str() == SILPrintOnlyFun) {
          llvm::dbgs() << "*** SIL function before transformation ("
                       << NumOptimizationIterations << ") ***\n";
          F.dump();
        }
        if (!SILPrintOnlyFuns.empty() &&
            F.getName().find(SILPrintOnlyFuns, 0) != StringRef::npos)
          llvm::dbgs() << "*** SIL function before transformation ("
                       << NumOptimizationIterations << ") ***\n";
          F.dump();
      }
    }
  }
  // Keep optimizing the module until no pass requested another iteration
  // of the pass or we reach the maximum.
  const unsigned IterationLimit = 20;
  do {
    anotherIteration = false;
    runOneIteration();
  } while (anotherIteration && NumOptimizationIterations < IterationLimit);
}

/// D'tor.
SILPassManager::~SILPassManager() {
  // Free all transformations.
  for (auto T : Transformations)
    delete T;

  // delete the analyis.
  for (auto A : Analysis)
    delete A;
}

/// \brief Reset the state of the pass manager and remove all transformation
/// owned by the pass manager. Anaysis passes will be kept.
void SILPassManager::resetAndRemoveTransformations(StringRef NextStage) {
  for (auto T : Transformations)
    delete T;

  Transformations.clear();
  NumOptimizationIterations = 0;
  anotherIteration = false;
  CompleteFunctions *CompleteFuncs = getAnalysis<CompleteFunctions>();
  CompleteFuncs->reset();
  StageName = NextStage;
}

const SILOptions &SILPassManager::getOptions() const {
  return Mod->getOptions();
}
