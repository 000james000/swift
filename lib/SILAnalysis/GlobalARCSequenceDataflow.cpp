//===--- GlobalARCSequenceDataflow.cpp - ARC Sequence Dataflow Analysis ---===//
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

#define DEBUG_TYPE "sil-global-arc-opts"
#include "GlobalARCSequenceDataflow.h"
#include "swift/SILAnalysis/ARCAnalysis.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILSuccessor.h"
#include "swift/SIL/CFG.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::arc;

//===----------------------------------------------------------------------===//
//                                 Utilities
//===----------------------------------------------------------------------===//

static bool isAutoreleasePoolCall(SILInstruction &I) {
  ApplyInst *AI = dyn_cast<ApplyInst>(&I);
  if (!AI)
    return false;

  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(AI->getCallee());
  if (!FRI)
    return false;

  return llvm::StringSwitch<bool>(FRI->getReferencedFunction()->getName())
      .Case("objc_autoreleasePoolPush", true)
      .Case("objc_autoreleasePoolPop", true)
      .Default(false);
}

namespace llvm {
raw_ostream &operator<<(raw_ostream &OS,
                        BottomUpRefCountState::LatticeState S) {
  using LatticeState = BottomUpRefCountState::LatticeState;
  switch (S) {
  case LatticeState::None:
    return OS << "None";
  case LatticeState::Decremented:
    return OS << "Decremented";
  case LatticeState::MightBeUsed:
    return OS << "MightBeUsed";
  case LatticeState::MightBeDecremented:
    return OS << "MightBeDecremented";
  }
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              TopDownRefCountState::LatticeState S) {
  using LatticeState = TopDownRefCountState::LatticeState;
  switch (S) {
  case LatticeState::None:
    return OS << "None";
  case LatticeState::Incremented:
    return OS << "Incremented";
  case LatticeState::MightBeUsed:
    return OS << "MightBeUsed";
  case LatticeState::MightBeDecremented:
    return OS << "MightBeDecremented";
  }
}
} // end namespace llvm

/// Wrapper around SILValue::stripCasts to handle the UncheckedRefBitCastInst.
static SILValue stripCasts(SILValue V) {
  while (true) {
    V = V.stripCasts();
    auto *BCI = dyn_cast<UncheckedRefBitCastInst>(V);
    if (!BCI)
      return V;
    V = BCI->getOperand();
  }
}

//===----------------------------------------------------------------------===//
//                           Lattice State Merging
//===----------------------------------------------------------------------===//

static inline BottomUpRefCountState::LatticeState
MergeBottomUpLatticeStates(BottomUpRefCountState::LatticeState L1,
                           BottomUpRefCountState::LatticeState L2) {
  using LatticeState = BottomUpRefCountState::LatticeState;
  // If both states equal, return the first.
  if (L1 == L2)
    return L1;

  // If either are none, return None.
  if (L1 == LatticeState::None || L2 == LatticeState::None)
    return LatticeState::None;

  // Canonicalize.
  if (unsigned(L1) > unsigned(L2))
    std::swap(L1, L2);

  // Choose the side further along in the sequence.
  if ((L1 == LatticeState::Decremented || L1 == LatticeState::MightBeUsed) ||
      (L2 == LatticeState::MightBeUsed ||
       L2 == LatticeState::MightBeDecremented))
    return L2;

  // Otherwise, we don't know what happened, be conservative and return none.
  return LatticeState::None;
}

static inline TopDownRefCountState::LatticeState
MergeTopDownLatticeStates(TopDownRefCountState::LatticeState L1,
                          TopDownRefCountState::LatticeState L2) {
  using LatticeState = TopDownRefCountState::LatticeState;
  // If both states equal, return the first.
  if (L1 == L2)
    return L1;

  // If either are none, return None.
  if (L1 == LatticeState::None || L2 == LatticeState::None)
    return LatticeState::None;

  // Canonicalize.
  if (unsigned(L1) > unsigned(L2))
    std::swap(L1, L2);

  // Choose the side further along in the sequence.
  if ((L1 == LatticeState::Incremented ||
       L1 == LatticeState::MightBeDecremented) ||
      (L2 == LatticeState::MightBeDecremented ||
       L2 == LatticeState::MightBeUsed))
    return L2;

  // Otherwise, we don't know what happened, return none.
  return LatticeState::None;
}

//===----------------------------------------------------------------------===//
//                         ARCBBState Implementation
//===----------------------------------------------------------------------===//

/// Merge in the state of the successor basic block. This is currently a stub.
void ARCBBState::mergeSuccBottomUp(ARCBBState &SuccBB) {
  // For each entry in the other set, if our set has an entry with the same key,
  // merge the entires. Otherwise, copy the entry and merge it with an empty
  // entry.
  for (auto MI : SuccBB.getBottomupStates()) {
    auto Pair = PtrToBottomUpState.insert(MI);
    // If we fail to merge, bail.
    if (!Pair.first->second.merge(Pair.second ? BottomUpRefCountState()
                                              : MI.second)) {
      clear();
      return;
    }
  }

  for (auto Pair : getBottomupStates()) {
    if (SuccBB.PtrToBottomUpState.find(Pair.first) ==
        SuccBB.PtrToBottomUpState.end())
      // If we fail to merge, bail.
      if (!Pair.second.merge(BottomUpRefCountState())) {
        clear();
        return;
      }
  }
}

/// Initialize this BB with the state of the successor basic block. This is
/// called on a basic block's state and then any other successors states are
/// merged in. This is currently a stub.
void ARCBBState::initSuccBottomUp(ARCBBState &SuccBB) {
  PtrToBottomUpState = SuccBB.PtrToBottomUpState;
}

/// Merge in the state of the predecessor basic block. This is currently a stub.
void ARCBBState::mergePredTopDown(ARCBBState &PredBB) {
  // For each entry in the other set, if our set has an entry with the same key,
  // merge the entires. Otherwise, copy the entry and merge it with an empty
  // entry.
  for (auto MI : PredBB.getTopDownStates()) {
    auto Pair = PtrToTopDownState.insert(MI);
    // If we fail to merge, bail.
    if (!Pair.first->second.merge(Pair.second ? TopDownRefCountState()
                                              : MI.second)) {
      clear();
      return;
    }
  }

  for (auto Pair : getTopDownStates()) {
    if (PredBB.PtrToTopDownState.find(Pair.first) ==
        PredBB.PtrToTopDownState.end())
      // If we fail to merge, bail.
      if (!Pair.second.merge(TopDownRefCountState())) {
        clear();
        return;
      }
  }
}

/// Initialize the state for this BB with the state of its predecessor
/// BB. Used to create an initial state before we merge in other
/// predecessors. This is currently a stub.
void ARCBBState::initPredTopDown(ARCBBState &PredBB) {
  PtrToTopDownState = PredBB.PtrToTopDownState;
}

//===----------------------------------------------------------------------===//
//                    Reference Count State Implementation
//===----------------------------------------------------------------------===//

bool TopDownRefCountState::merge(const TopDownRefCountState &Other) {
  auto NewState = MergeTopDownLatticeStates(LatState, Other.LatState);
  DEBUG(llvm::dbgs() << "            Performing TopDown Merge.\n");
  DEBUG(llvm::dbgs() << "                Left: " << LatState << "; Right: "
                     << Other.LatState << "; Result: " << NewState << "\n");
  DEBUG(llvm::dbgs() << "                V: ";
        if (getValue())
          getValue()->dump();
        else
          llvm::dbgs() << "\n";
        llvm::dbgs() << "                OtherV: ";
        if (Other.getValue())
          Other.getValue()->dump();
        else
          llvm::dbgs() << "\n");

  LatState = NewState;
  KnownSafe &= Other.KnownSafe;

  // If we're doing a merge on a path that's previously seen a partial merge,
  // conservatively drop the sequence, to avoid doing partial RR
  // elimination. If the branch predicates for the two merge differ, mixing
  // them is unsafe since they are not control dependent.
  if (LatState == TopDownRefCountState::LatticeState::None) {
    RefCountState<TopDownRefCountState>::clear();
    DEBUG(llvm::dbgs() << "            Found LatticeState::None. "
                          "Clearing State!\n");
    return false;
  }

  // We should never have an argument path merge with a non-argument path.
  if (Argument.isNull() != Other.Argument.isNull()) {
      RefCountState<TopDownRefCountState>::clear();
      DEBUG(llvm::dbgs() << "            Can not merge Argument with "
            "Non-Argument path... Bailing!\n");
      return false;
  }

  Increments.insert(Other.Increments.begin(), Other.Increments.end());

  Partial |= InsertPts.size() != Other.InsertPts.size();
  for (auto *SI : Other.InsertPts)
    Partial |= InsertPts.insert(SI);

  return true;
}

bool BottomUpRefCountState::merge(const BottomUpRefCountState &Other) {

  auto NewState = MergeBottomUpLatticeStates(LatState, Other.LatState);
  DEBUG(llvm::dbgs() << "            Performing BottomUp Merge.\n");
  DEBUG(llvm::dbgs() << "                Left: " << LatState << "; Right: "
                     << Other.LatState << "; Result: " << NewState << "\n");
  DEBUG(llvm::dbgs() << "                V: ";
        if (getValue())
          getValue()->dump();
        else
          llvm::dbgs() << "\n";
        llvm::dbgs() << "                OtherV: ";
        if (Other.getValue())
          Other.getValue()->dump();
        else
          llvm::dbgs() << "\n");

  LatState = NewState;
  KnownSafe &= Other.KnownSafe;

  // If we're doing a merge on a path that's previously seen a partial merge,
  // conservatively drop the sequence, to avoid doing partial RR
  // elimination. If the branch predicates for the two merge differ, mixing
  // them is unsafe since they are not control dependent.
  if (LatState == BottomUpRefCountState::LatticeState::None) {
    DEBUG(llvm::dbgs() << "            Found LatticeState::None. "
                          "Clearing State!\n");
    RefCountState<BottomUpRefCountState>::clear();
    return false;
  }

  Decrements.insert(Other.Decrements.begin(), Other.Decrements.end());

  Partial |= InsertPts.size() != Other.InsertPts.size();
  for (auto *SI : Other.InsertPts)
    Partial |= InsertPts.insert(SI);

  return true;
}

//===----------------------------------------------------------------------===//
//                             Top Down Dataflow
//===----------------------------------------------------------------------===//

/// Analyze a single BB for refcount inc/dec instructions.
///
/// If anything was found it will be added to DecToIncStateMap.
///
/// NestingDetected will be set to indicate that the block needs to be
/// reanalyzed if code motion occurs.
static bool processBBTopDown(
    ARCBBState &BBState,
    BlotMapVector<SILInstruction *, TopDownRefCountState> &DecToIncStateMap,
    AliasAnalysis *AA) {
  DEBUG(llvm::dbgs() << ">>>> Top Down!\n");

  SILBasicBlock &BB = BBState.getBB();

  bool NestingDetected = false;

  // If the current BB is the entry BB, initialize a state corresponding to each
  // of its owned parameters.
  //
  // TODO: Handle gauranteed parameters.
  if (&BB == &*BB.getParent()->begin()) {
    auto Args = BB.getBBArgs();
    auto SignatureParams =
        BB.getParent()->getLoweredFunctionType()->getParameters();
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      SILArgument *A = Args[i];
      ParameterConvention P = SignatureParams[i].getConvention();

      DEBUG(llvm::dbgs() << "VISITING ARGUMENT: " << *A);

      if (P != ParameterConvention::Direct_Owned)
        continue;

      TopDownRefCountState &State = BBState.getTopDownRefCountState(Args[i]);
      State.initWithArg(A);
    }
  }

  // For each instruction I in BB...
  for (auto &I : BB) {

    DEBUG(llvm::dbgs() << "VISITING:\n    " << I);

    if (isAutoreleasePoolCall(I)) {
      BBState.clear();
      continue;
    }

    SILValue Op;

    // If I is a ref count increment instruction...
    if (isRefCountIncrement(I)) {
      // map its operand to a newly initialized or reinitialized ref count
      // state and continue...
      Op = stripCasts(I.getOperand(0));
      TopDownRefCountState &State = BBState.getTopDownRefCountState(Op);
      NestingDetected |= State.initWithInst(&I);

      DEBUG(llvm::dbgs() << "    REF COUNT INCREMENT! Known Safe: "
                         << (State.isKnownSafe() ? "yes" : "no") << "\n");

      // Continue processing in case this increment could be a CanUse for a
      // different pointer.
    }

    // If we have a reference count decrement...
    if (isRefCountDecrement(I)) {
      // Look up the state associated with its operand...
      Op = stripCasts(I.getOperand(0));
      TopDownRefCountState &RefCountState = BBState.getTopDownRefCountState(Op);

      DEBUG(llvm::dbgs() << "    REF COUNT DECREMENT!\n");

      // If the state is already initialized to contain a reference count
      // increment of the same type (i.e. retain_value, release_value or
      // strong_retain, strong_release), then remove the state from the map
      // and add the retain/release pair to the delete list and continue.
      if (RefCountState.isRefCountInstMatchedToTrackedInstruction(&I)) {
        // Copy the current value of ref count state into the result map.
        DecToIncStateMap[&I] = RefCountState;
        DEBUG(llvm::dbgs() << "    MATCHING INCREMENT:\n"
                           << RefCountState.getValue());

        // Clear the ref count state in case we see more operations on this
        // ref counted value. This is for safety reasons.
        RefCountState.clear();
      } else {
        if (RefCountState.isTrackingRefCountInst()) {
          DEBUG(llvm::dbgs() << "    FAILED MATCH INCREMENT:\n"
                             << RefCountState.getValue());
        } else {
          DEBUG(llvm::dbgs() << "    FAILED MATCH. NO INCREMENT.\n");
        }
      }

      // Otherwise we continue processing the reference count decrement to
      // see if the decrement can affect any other pointers that we are
      // tracking.
    }

    // For all other (reference counted value, ref count state) we are
    // tracking...
    for (auto &OtherState : BBState.getTopDownStates()) {
      // If the state we are visiting is for the pointer we just visited, bail.
      if (Op && OtherState.first == Op)
        continue;

      // If the other state is not tracking anything, bail.
      if (!OtherState.second.isTrackingRefCount())
        continue;

      // Check if the instruction we are visiting could potentially decrement
      // the reference counted value we are tracking... in a manner that could
      // cause us to change states. If we do change states continue...
      if (OtherState.second.handlePotentialDecrement(&I, AA)) {
        DEBUG(llvm::dbgs() << "    Found Potential Decrement:\n        "
                           << OtherState.second.getValue());
        continue;
      }

      // Otherwise check if the reference counted value we are tracking
      // could be used by the given instruction.
      if (OtherState.second.handlePotentialUser(&I, AA))
        DEBUG(llvm::dbgs() << "    Found Potential Use:\n        "
                           << OtherState.second.getValue());
    }
  }

  return NestingDetected;
}

void
swift::arc::ARCSequenceDataflowEvaluator::mergePredecessors(ARCBBState &BBState,
                                                            SILBasicBlock *BB) {
  bool HasAtLeastOnePred = false;

  // For each successor of BB...
  for (auto Pred : BB->getPreds()) {
    auto *PredBB = Pred;

    // If the precessor is the head of a backedge in our traversal, clear any
    // state we are tracking now and clear the state of the basic block. There
    // is some sort of control flow here that we do not understand.
    if (BackedgeMap[PredBB].count(BB)) {
      BBState.clear();
      break;
    }

    // Otherwise, lookup the BBState associated with the predecessor and merge
    // the predecessor in.
    auto I = TopDownBBStates.find(PredBB);

    // If we can not lookup the BBState then the BB was not in the post order
    // implying that it is unreachable. LLVM will ensure that the BB is removed
    // if we do not reach it at the SIL level. Since it is unreachable, ignore
    // it.
    if (I == TopDownBBStates.end())
      continue;

    // If we found the state but the state is for a trap BB, skip it. Trap BBs
    // leak all reference counts and do not reference reference semantic objects
    // in any manner.
    if (I->second.isTrapBB())
      continue;

    if (!HasAtLeastOnePred) {
      BBState.initPredTopDown(I->second);
    } else {
      BBState.mergePredTopDown(I->second);
    }
    HasAtLeastOnePred = true;
  }
}

bool swift::arc::ARCSequenceDataflowEvaluator::processTopDown() {
  bool NestingDetected = false;

  DEBUG(llvm::dbgs() << "<<<< Processing Top Down! >>>>\n");

  // For each BB in our reverse post order...
  for (auto *BB : POTA->getReversePostOrder(&F)) {

    DEBUG(llvm::dbgs() << "Processing BB#: " << BBToBBID[BB] << "\n");

    // Grab the BBState associated with it and set it to be the current BB.
    ARCBBState &BBState = TopDownBBStates.find(BB)->second;
    BBState.init(BB);

    DEBUG(llvm::dbgs() << "Merging Predecessors!\n");
    mergePredecessors(BBState, BB);

    // Then perform the basic block optimization.
    NestingDetected |= processBBTopDown(BBState, DecToIncStateMap, AA);
  }

  return NestingDetected;
}

//===----------------------------------------------------------------------===//
//                             Bottom Up Dataflow
//===----------------------------------------------------------------------===//

/// Analyze a single BB for refcount inc/dec instructions.
///
/// If anything was found it will be added to DecToIncStateMap.
///
/// NestingDetected will be set to indicate that the block needs to be
/// reanalyzed if code motion occurs.
static bool processBBBottomUp(
    ARCBBState &BBState,
    BlotMapVector<SILInstruction *, BottomUpRefCountState> &IncToDecStateMap,
    AliasAnalysis *AA) {
  DEBUG(llvm::dbgs() << ">>>> Bottom Up!\n");
  SILBasicBlock &BB = BBState.getBB();

  bool NestingDetected = false;

  // For each non terminator instruction I in BB visited in reverse...
  for (auto II = std::next(BB.rbegin()), IE = BB.rend(); II != IE;) {
    SILInstruction &I = *II;
    ++II;

    DEBUG(llvm::dbgs() << "VISITING:\n    " << I);

    if (isAutoreleasePoolCall(I)) {
      BBState.clear();
      continue;
    }

    SILValue Op;

    // If I is a ref count decrement instruction...
    if (isRefCountDecrement(I)) {
      // map its operand to a newly initialized or reinitialized ref count
      // state and continue...
      Op = stripCasts(I.getOperand(0));
      BottomUpRefCountState &State = BBState.getBottomUpRefCountState(Op);
      NestingDetected |= State.initWithInst(&I);

      DEBUG(llvm::dbgs() << "    REF COUNT DECREMENT! Known Safe: "
                         << (State.isKnownSafe() ? "yes" : "no") << "\n");

      // Continue on to see if our reference decrement could potentially affect
      // any other pointers via a use or a decrement.
    }

    // If we have a reference count decrement...
    if (isRefCountIncrement(I)) {
      // Look up the state associated with its operand...
      Op = stripCasts(I.getOperand(0));
      BottomUpRefCountState &RefCountState =
          BBState.getBottomUpRefCountState(Op);

      DEBUG(llvm::dbgs() << "    REF COUNT INCREMENT!\n");

      // If the state is already initialized to contain a reference count
      // increment of the same type (i.e. retain_value, release_value or
      // strong_retain, strong_release), then remove the state from the map
      // and add the retain/release pair to the delete list and continue.
      if (RefCountState.isRefCountInstMatchedToTrackedInstruction(&I)) {
        // Copy the current value of ref count state into the result map.
        IncToDecStateMap[&I] = RefCountState;
        DEBUG(llvm::dbgs() << "    MATCHING DECREMENT:"
                           << RefCountState.getValue());

        // Clear the ref count state in case we see more operations on this
        // ref counted value. This is for safety reasons.
        RefCountState.clear();
      } else {
        if (RefCountState.isTrackingRefCountInst()) {
          DEBUG(llvm::dbgs()
                << "    FAILED MATCH DECREMENT:" << RefCountState.getValue());
        } else {
          DEBUG(llvm::dbgs() << "    FAILED MATCH DECREMENT. Not tracking a "
                                "decrement.\n");
        }
      }

      // Otherwise we continue processing the reference count decrement to
      // see if the increment can act as a use for other values.
    }

    // For all other (reference counted value, ref count state) we are
    // tracking...
    for (auto &OtherState : BBState.getBottomupStates()) {
      // If this is the state associated with the instruction that we are
      // currently visiting, bail.
      if (Op && OtherState.first == Op)
        continue;

      // If this state is not tracking anything, skip it.
      if (!OtherState.second.isTrackingRefCount())
        continue;

      // Check if the instruction we are visiting could potentially decrement
      // the reference counted value we are tracking... in a manner that could
      // cause us to change states. If we do change states continue...
      if (OtherState.second.handlePotentialDecrement(&I, AA)) {
        DEBUG(llvm::dbgs() << "    Found Potential Decrement:\n        "
                           << OtherState.second.getValue());
        continue;
      }

      // Otherwise check if the reference counted value we are tracking
      // could be used by the given instruction.
      if (OtherState.second.handlePotentialUser(&I, AA))
        DEBUG(llvm::dbgs() << "    Found Potential Use:\n        "
                           << OtherState.second.getValue());
    }
  }

  return NestingDetected;
}

void
swift::arc::ARCSequenceDataflowEvaluator::mergeSuccessors(ARCBBState &BBState,
                                                          SILBasicBlock *BB) {
  // Grab the backedge set for our BB.
  auto &BackEdgeSet = BackedgeMap[BB];

  // For each successor of BB...
  ArrayRef<SILSuccessor> Succs = BB->getSuccs();
  bool HasAtLeastOneSucc = false;
  for (unsigned i = 0, e = Succs.size(); i != e; ++i) {
    // If it does not have a basic block associated with it...
    auto *SuccBB = Succs[i].getBB();

    // Skip it.
    if (!SuccBB)
      continue;

    // If the BB is the head of a backedge in our traversal, clear any state
    // we are tracking now and clear the state of the basic block. There is
    // some sort of control flow here that we do not understand.
    if (BackEdgeSet.count(SuccBB)) {
      BBState.clear();
      break;
    }

    // Otherwise, lookup the BBState associated with the successor and merge
    // the successor in.
    auto I = BottomUpBBStates.find(SuccBB);
    assert(I != BottomUpBBStates.end());

    if (I->second.isTrapBB())
      continue;

    if (!HasAtLeastOneSucc) {
      BBState.initSuccBottomUp(I->second);
    } else {
      BBState.mergeSuccBottomUp(I->second);
    }
    HasAtLeastOneSucc = true;
  }
}

bool swift::arc::ARCSequenceDataflowEvaluator::processBottomUp() {
  bool NestingDetected = false;

  DEBUG(llvm::dbgs() << "<<<< Processing Bottom Up! >>>>\n");

  // For each BB in our post order...
  for (auto *BB : POTA->getPostOrder(&F)) {

    DEBUG(llvm::dbgs() << "Processing BB#: " << BBToBBID[BB] << "\n");

    // Grab the BBState associated with it and set it to be the current BB.
    ARCBBState &BBState = BottomUpBBStates.find(BB)->second;
    BBState.init(BB);

    DEBUG(llvm::dbgs() << "Merging Successors!\n");
    mergeSuccessors(BBState, BB);

    // Then perform the basic block optimization.
    NestingDetected |= processBBBottomUp(BBState, IncToDecStateMap, AA);
  }

  return NestingDetected;
}

//===----------------------------------------------------------------------===//
//                 Top Level ARC Sequence Dataflow Evaluator
//===----------------------------------------------------------------------===//

void swift::arc::ARCSequenceDataflowEvaluator::init() {
  // Initialize the post order data structure.
#ifndef NDEBUG
  unsigned Count = 0;
  for (auto &BB : F) {
    BBToBBID[&BB] = Count++;      
  }
#endif

  // Then iterate through it in reverse to perform the post order, looking for
  // backedges.
  llvm::DenseSet<SILBasicBlock *> VisitedSet;
  unsigned i = 0;
  for (SILBasicBlock *BB : POTA->getReversePostOrder(&F)) {
    VisitedSet.insert(BB);

    BottomUpBBStates[i].first = BB;
    BottomUpBBStates[i].second.init(BB);
    TopDownBBStates[i].first = BB;
    TopDownBBStates[i].second.init(BB);
    ++i;

    for (auto &Succ : BB->getSuccs())
      if (SILBasicBlock *SuccBB = Succ.getBB())
        if (VisitedSet.count(SuccBB))
          BackedgeMap[BB].insert(SuccBB);
  }

  BottomUpBBStates.sort();
  TopDownBBStates.sort();
}

bool swift::arc::ARCSequenceDataflowEvaluator::run() {
  bool NestingDetected = processBottomUp();
  NestingDetected |= processTopDown();

  return NestingDetected;
}

void swift::arc::ARCBBState::initializeTrapStatus() {
  auto II = BB->begin();
  auto *BFRI = dyn_cast<BuiltinFunctionRefInst>(&*II);
  if (!BFRI || !BFRI->getName().str().equals("int_trap"))
    return;
  ++II;

  auto *AI = dyn_cast<ApplyInst>(&*II);
  if (!AI || AI->getCallee() != SILValue(BFRI))
    return;
  ++II;

  IsTrapBB = isa<UnreachableInst>(&*II);
}
