//===-- Generics.h - Utilities for transforming generics --------*- C++ -*-===//
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
// This containts utilities for transforming generics.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_GENERICS_H
#define SWIFT_SIL_GENERICS_H

#include "swift/AST/Mangle.h"
#include "swift/SIL/Mangle.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

class SpecializingCloner : public TypeSubstCloner<SpecializingCloner> {
public:
  SpecializingCloner(SILFunction *F,
                     TypeSubstitutionMap &InterfaceSubs,
                     TypeSubstitutionMap &ContextSubs,
                     StringRef NewName,
                     ArrayRef<Substitution> ApplySubs)
  : TypeSubstCloner(*initCloned(F, InterfaceSubs, NewName), *F, ContextSubs,
                    ApplySubs) {}
  /// Clone and remap the types in \p F according to the substitution
  /// list in \p Subs.
  static SILFunction *cloneFunction(SILFunction *F,
                                    TypeSubstitutionMap &InterfaceSubs,
                                    TypeSubstitutionMap &ContextSubs,
                                    StringRef NewName, ApplyInst *Caller) {
    // Clone and specialize the function.
    SpecializingCloner SC(F, InterfaceSubs, ContextSubs, NewName,
                          Caller->getSubstitutions());
    SC.populateCloned();
    return SC.getCloned();
  }

private:
  static SILFunction *initCloned(SILFunction *Orig,
                                 TypeSubstitutionMap &InterfaceSubs,
                                 StringRef NewName);
  /// Clone the body of the function into the empty function that was created
  /// by initCloned.
  void populateCloned();
  SILFunction *getCloned() { return &getBuilder().getFunction(); }
};

struct GenericSpecializer {
  /// A list of ApplyInst instructions.
  typedef SmallVector<ApplyInst *, 16> AIList;

  /// The SIL Module.
  SILModule *M;

  /// Maps a function to all of the ApplyInst that call it.
  llvm::MapVector<SILFunction *, AIList> ApplyInstMap;

  /// A worklist of functions to specialize.
  std::vector<SILFunction*> Worklist;

  GenericSpecializer(SILModule *Mod) : M(Mod) {}

  bool specializeApplyInstGroup(SILFunction *F, AIList &List);

  /// Scan the function and collect all of the ApplyInst with generic
  /// substitutions into buckets according to the called function.
  void collectApplyInst(SILFunction &F);

  /// Add the call \p AI into the list of calls to inspect.
  void addApplyInst(ApplyInst *AI);

  /// The driver for the generic specialization pass.
  bool specialize(const std::vector<SILFunction *> &BotUpFuncList) {
    bool Changed = false;
    for (auto &F : *M)
      collectApplyInst(F);

    // Initialize the worklist with a call-graph bottom-up list of functions.
    // We specialize the functions in a top-down order, starting from the end
    // of the list.
    Worklist.insert(Worklist.begin(),
                    BotUpFuncList.begin(), BotUpFuncList.end());

    while (Worklist.size()) {
      SILFunction *F = Worklist.back();
      Worklist.pop_back();
      if (ApplyInstMap.count(F))
        Changed |= specializeApplyInstGroup(F, ApplyInstMap[F]);
    }
    return Changed;
  }
};

#endif
