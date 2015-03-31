//===- Generics.cpp ---- Utilities for transforming generics ----*- C++ -*-===//
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

#include "swift/SILPasses/Utils/Generics.h"
#define DEBUG_TYPE "generic-specializer"

using namespace swift;

/// Create a new empty function with the correct arguments and a unique name.
SILFunction *SpecializingCloner::initCloned(SILFunction *Orig,
                                            TypeSubstitutionMap &InterfaceSubs,
                                            StringRef NewName) {
  SILModule &M = Orig->getModule();
  Module *SM = M.getSwiftModule();

  CanSILFunctionType FTy =
  SILType::substFuncType(M, SM, InterfaceSubs,
                         Orig->getLoweredFunctionType(),
                         /*dropGenerics = */ true);

  assert((Orig->isTransparent() || Orig->isBare() || Orig->getLocation())
         && "SILFunction missing location");
  assert((Orig->isTransparent() || Orig->isBare() || Orig->getDebugScope())
         && "SILFunction missing DebugScope");
  assert(!Orig->isGlobalInit() && "Global initializer cannot be cloned");

  // Create a new empty function.
  SILFunction *NewF = SILFunction::create(M,
                                          getSpecializedLinkage(Orig->getLinkage()),
                                          NewName, FTy, nullptr,
                                          Orig->getLocation(), Orig->isBare(),
                                          Orig->isTransparent(),
                                          Orig->isFragile(), Orig->isThunk(),
                                          Orig->getClassVisibility(),
                                          Orig->getInlineStrategy(),
                                          Orig->getEffectsKind(), Orig,
                                          Orig->getDebugScope(),
                                          Orig->getDeclContext());
  NewF->setSemanticsAttr(Orig->getSemanticsAttr());
  return NewF;
}

void SpecializingCloner::populateCloned() {
  SILFunction *Cloned = getCloned();
  SILModule &M = Cloned->getModule();

  // Create arguments for the entry block.
  SILBasicBlock *OrigEntryBB = Original.begin();
  SILBasicBlock *ClonedEntryBB = new (M) SILBasicBlock(Cloned);

  // Create the entry basic block with the function arguments.
  auto I = OrigEntryBB->bbarg_begin(), E = OrigEntryBB->bbarg_end();
  while (I != E) {
    SILValue MappedValue =
    new (M) SILArgument(ClonedEntryBB, remapType((*I)->getType()),
                        (*I)->getDecl());
    ValueMap.insert(std::make_pair(*I, MappedValue));
    ++I;
  }

  getBuilder().setInsertionPoint(ClonedEntryBB);
  BBMap.insert(std::make_pair(OrigEntryBB, ClonedEntryBB));
  // Recursively visit original BBs in depth-first preorder, starting with the
  // entry block, cloning all instructions other than terminators.
  visitSILBasicBlock(OrigEntryBB);

  // Now iterate over the BBs and fix up the terminators.
  for (auto BI = BBMap.begin(), BE = BBMap.end(); BI != BE; ++BI) {
    getBuilder().setInsertionPoint(BI->second);
    visit(BI->first->getTerminator());
  }
}


void GenericSpecializer::addApplyInst(ApplySite AI) {
  if (!AI || !AI.hasSubstitutions())
    return;

  SILValue CalleeVal = AI.getCallee();
  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(CalleeVal);

  if (!FRI)
    return;

  SILFunction *Callee = FRI->getReferencedFunction();
  if (Callee->isExternalDeclaration())
    if (!M->linkFunction(Callee, SILModule::LinkingMode::LinkAll))
      return;

  // Save the ApplyInst into the function/bucket that it calls.
  ApplyInstMap[Callee].push_back(AI);
}

void GenericSpecializer::collectApplyInst(SILFunction &F) {
  // Scan all of the instructions in this function in search of ApplyInsts.
  for (auto &BB : F)
    for (auto &I : BB) {
      if (ApplySite AI = ApplySite::isa(&I))
        addApplyInst(AI);
    }
}

void dumpTypeSubstitutionMap(const TypeSubstitutionMap &map) {
  llvm::errs() << "{\n";
  for (auto &kv : map) {
    llvm::errs() << "  ";
    kv.first->print(llvm::errs());
    llvm::errs() << " => ";
    kv.second->print(llvm::errs());
    llvm::errs() << "\n";
  }
  llvm::errs() << "}\n";
}

bool
GenericSpecializer::specializeApplyInstGroup(SILFunction *F, AIList &List) {
  bool Changed = false;

  assert(F->isDefinition() && "Expected definition to specialize!");

  DEBUG(llvm::dbgs() << "*** Processing: " << F->getName() << "\n");

  for (auto &AI : List) {
    DEBUG(llvm::dbgs() << "        ApplyInst: " << *AI.getInstruction());

    // Create the substitution maps.
    TypeSubstitutionMap InterfaceSubs
    = F->getLoweredFunctionType()->getGenericSignature()
    ->getSubstitutionMap(AI.getSubstitutions());

    TypeSubstitutionMap ContextSubs
    = F->getContextGenericParams()
    ->getSubstitutionMap(AI.getSubstitutions());

    // We do not support partial specialization.
    if (hasUnboundGenericTypes(InterfaceSubs)) {
      DEBUG(llvm::dbgs() << "    Can not specialize with interface subs.\n");
      continue;
    }

    llvm::SmallString<64> ClonedName;
    {
      llvm::raw_svector_ostream buffer(ClonedName);
      ArrayRef<Substitution> Subs = AI.getSubstitutions();
      Mangle::Mangler M(buffer);
      Mangle::GenericSpecializationMangler Mangler(M, F, Subs);
      Mangler.mangle();
    }

    SILFunction *NewF;
    bool createdFunction;
    // If we already have this specialization, reuse it.
    if (auto PrevF = M->lookUpFunction(ClonedName)) {
      NewF = PrevF;
      createdFunction = false;

#ifndef NDEBUG
      // Make sure that NewF's subst type matches the expected type.
      auto Subs = AI.getSubstitutions();
      auto FTy =
      F->getLoweredFunctionType()->substGenericArgs(*M,
                                                    M->getSwiftModule(),
                                                    Subs);
      assert(FTy == NewF->getLoweredFunctionType() &&
             "Previously specialized function does not match expected type.");
#endif
    } else {
      // Create a new function.
      NewF = SpecializingCloner::cloneFunction(F, InterfaceSubs, ContextSubs,
                                               ClonedName, AI);
      createdFunction = true;
    }

    // Replace all of the AI functions with the new function.
    replaceWithSpecializedFunction(AI, NewF);

    Changed = true;

    // Analyze the ApplyInsts in the new function.
    if (createdFunction) {
      collectApplyInst(*NewF);
      Worklist.push_back(NewF);
    }
  }
  
  return Changed;
}

/// Collect and specialize calls in a specific order specified by
/// \p BotUpFuncList.
bool GenericSpecializer::specialize(const std::vector<SILFunction *>
                                    &BotUpFuncList) {
  for (auto &F : *M)
    collectApplyInst(F);

  // Initialize the worklist with a call-graph bottom-up list of functions.
  // We specialize the functions in a top-down order, starting from the end
  // of the list.
  Worklist.insert(Worklist.begin(), BotUpFuncList.begin(),
                  BotUpFuncList.end());

  bool Changed = false;
  // Try to specialize generic calls.
  while (Worklist.size()) {
    SILFunction *F = Worklist.back();
    Worklist.pop_back();
    if (ApplyInstMap.count(F))
      Changed |= specializeApplyInstGroup(F, ApplyInstMap[F]);
  }
  return Changed;
}
