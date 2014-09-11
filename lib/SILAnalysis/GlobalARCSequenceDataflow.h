//===- GlobalARCSequenceDataflow.h - ARC Sequence Flow Analysis -*- C++ -*-===//
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

#include "ReferenceCountState.h"
#include "swift/SILAnalysis/PostOrderAnalysis.h"
#include "swift/Basic/BlotMapVector.h"
#include "swift/Basic/PreallocatedMap.h"

namespace swift {

class SILFunction;
class AliasAnalysis;

} // end swift namespace

namespace swift {
namespace arc {

/// \brief Per-BasicBlock state.
class ARCBBState {
public:
  using TopDownMapTy = BlotMapVector<SILValue, TopDownRefCountState>;
  using BottomUpMapTy = BlotMapVector<SILValue, BottomUpRefCountState>;

private:
  /// The basic block that this bbstate corresponds to.
  SILBasicBlock *BB;

  /// The top-down traversal uses this to record information known about a
  /// pointer at the bottom of each block.
  TopDownMapTy PtrToTopDownState;

  /// The bottom-up traversal uses this to record information known about a
  /// pointer at the top of each block.
  BottomUpMapTy PtrToBottomUpState;

  /// Is this a BB that is a trap?
  bool IsTrapBB;

public:
  ARCBBState() : BB() {}
  ARCBBState(SILBasicBlock *BB) : BB(BB) {}

  void init(SILBasicBlock *NewBB) {
    assert(NewBB && "Cannot set NewBB to a nullptr.");
    BB = NewBB;
    IsTrapBB = false;
    initializeTrapStatus();
  }

  /// Is this BB a BB that fits the canonical form of a trap?
  ///
  /// The canonical form of a trap is:
  ///
  ///   %0 = builtin_function_ref "int_trap"
  ///   apply %0()
  ///   unreachable
  ///
  /// This can not have any uses of reference counted values since the frontend
  /// just leaks at that point.
  bool isTrapBB() const { return IsTrapBB; }

  /// Top Down Iterators
  using topdown_iterator = TopDownMapTy::iterator;
  using topdown_const_iterator = TopDownMapTy::const_iterator;
  topdown_iterator topdown_begin() { return PtrToTopDownState.begin(); }
  topdown_iterator topdown_end() { return PtrToTopDownState.end(); }
  topdown_const_iterator topdown_begin() const {
    return PtrToTopDownState.begin();
  }
  topdown_const_iterator topdown_end() const { return PtrToTopDownState.end(); }
  Range<topdown_iterator> getTopDownStates() {
    return make_range(topdown_begin(), topdown_end());
  }

  /// Bottom up iteration.
  using bottomup_iterator = BottomUpMapTy::iterator;
  using bottomup_const_iterator = BottomUpMapTy::const_iterator;
  bottomup_iterator bottomup_begin() { return PtrToBottomUpState.begin(); }
  bottomup_iterator bottomup_end() { return PtrToBottomUpState.end(); }
  bottomup_const_iterator bottomup_begin() const {
    return PtrToBottomUpState.begin();
  }
  bottomup_const_iterator bottomup_end() const {
    return PtrToBottomUpState.end();
  }
  Range<bottomup_iterator> getBottomupStates() {
    return make_range(bottomup_begin(), bottomup_end());
  }

  /// Attempt to find the PtrState object describing the top down state for
  /// pointer Arg. Return a new initialized PtrState describing the top down
  /// state for Arg if we do not find one.
  TopDownRefCountState &getTopDownRefCountState(SILValue Ptr) {
    return PtrToTopDownState[Ptr];
  }

  /// Attempt to find the PtrState object describing the bottom up state for
  /// pointer Arg. Return a new initialized PtrState describing the bottom up
  /// state for Arg if we do not find one.
  BottomUpRefCountState &getBottomUpRefCountState(SILValue Ptr) {
    return PtrToBottomUpState[Ptr];
  }

  /// Blot \p Ptr.
  void clearBottomUpRefCountState(SILValue Ptr) {
    PtrToBottomUpState.blot(Ptr);
  }

  /// Blot \p Ptr.
  void clearTopDownRefCountState(SILValue Ptr) {
    PtrToTopDownState.blot(Ptr);
  }

  void clearTopDownState() { PtrToTopDownState.clear(); }
  void clearBottomUpState() { PtrToBottomUpState.clear(); }

  /// Clear both the bottom up *AND* top down state.
  void clear() {
    clearTopDownState();
    clearBottomUpState();
  }

  /// Returns a reference to the basic block that we are tracking.
  SILBasicBlock &getBB() const { return *BB; }

  /// Merge in the state of the successor basic block. This is currently a stub.
  void mergeSuccBottomUp(ARCBBState &SuccBB);

  /// Initialize this BB with the state of the successor basic block. This is
  /// called on a basic block's state and then any other successors states are
  /// merged in. This is currently a stub.
  void initSuccBottomUp(ARCBBState &SuccBB);

  /// Merge in the state of the predecessor basic block. This is currently a stub.
  void mergePredTopDown(ARCBBState &PredBB);

  /// Initialize the state for this BB with the state of its predecessor
  /// BB. Used to create an initial state before we merge in other
  /// predecessors. This is currently a stub.
  void initPredTopDown(ARCBBState &PredBB);

private:
  void initializeTrapStatus();
};

/// A class that implements the ARC sequence data flow.
class ARCSequenceDataflowEvaluator {
  /// The SILFunction that we are applying the dataflow to.
  SILFunction &F;

  /// The alias analysis that we are using for alias queries.
  AliasAnalysis *AA;

  /// The post order analysis we are using for computing post orders, reverse
  /// post orders.
  PostOrderAnalysis *POTA;

  /// An analysis which computes the identity root of a SILValue(), i.e. the
  /// dominating origin SILValue of the reference count that by retaining or
  /// releasing this value one is affecting.
  RCIdentityAnalysis *RCIA;

  /// The map from dataflow terminating decrements -> increment dataflow state.
  BlotMapVector<SILInstruction *, TopDownRefCountState> &DecToIncStateMap;

  /// The map from dataflow terminating increment -> decrement dataflow state.
  BlotMapVector<SILInstruction *, BottomUpRefCountState> &IncToDecStateMap;

#ifndef NDEBUG
  /// A map from a SIL Basic Block to its id in the BB list. This matches what
  /// is printed out in SIL files.
  llvm::DenseMap<SILBasicBlock *, unsigned> BBToBBID;
#endif

  /// A map mapping the head to a tail of a backedge. We only compute this once
  /// in the lifetime of this class.
  llvm::DenseMap<SILBasicBlock *,
                 llvm::SmallPtrSet<SILBasicBlock *, 4>> BackedgeMap;

  /// A type which maps a basic block to an ARCBBState.
  using BBToARCStateMapTy = PreallocatedMap<SILBasicBlock *, ARCBBState>;

  /// Map from a basic block to its bottom up dataflow state.
  BBToARCStateMapTy BottomUpBBStates;

  /// Map from basic block to its top down dataflow state.
  BBToARCStateMapTy TopDownBBStates;

public:
  ARCSequenceDataflowEvaluator(
      SILFunction &F, AliasAnalysis *AA, PostOrderAnalysis *POTA,
      RCIdentityAnalysis *RCIA,
      BlotMapVector<SILInstruction *, TopDownRefCountState> &DecToIncStateMap,
      BlotMapVector<SILInstruction *, BottomUpRefCountState> &IncToDecStateMap)
      : F(F), AA(AA), POTA(POTA), RCIA(RCIA),
        DecToIncStateMap(DecToIncStateMap), IncToDecStateMap(IncToDecStateMap),
#ifndef NDEBUG
        BBToBBID(),
#endif
        BackedgeMap(),
        BottomUpBBStates(POTA->size(&F),
                         [](const BBToARCStateMapTy::PairTy &P1,
                            const BBToARCStateMapTy::PairTy &P2) {
                           return P1.first < P2.first;
                         }),
        TopDownBBStates(POTA->size(&F),
                        [](const BBToARCStateMapTy::PairTy &P1,
                           const BBToARCStateMapTy::PairTy &P2) {
                          return P1.first < P2.first;
                        }) {}

  /// Initialize the dataflow evaluator state.
  void init();

  /// Run the dataflow evaluator.
  bool run();

  /// Clear all of the states we are tracking for the various basic blocks.
  void clear() {
    assert(BottomUpBBStates.size() == TopDownBBStates.size()
           && "These should be one to one mapped to basic blocks so should"
           " have the same size");
    for (unsigned i = 0, e = BottomUpBBStates.size(); i != e; ++i) {
      BottomUpBBStates[i].second.clear();
      TopDownBBStates[i].second.clear();
    }
  }

  SILFunction *getFunction() const { return &F; }

private:
  /// Perform the bottom up data flow.
  bool processBottomUp();

  /// Perform the top down dataflow.
  bool processTopDown();

  /// Merge in the bottomupstate for any successors of BB into BBState. This is
  /// a stub currently.
  void mergeSuccessors(ARCBBState &BBState, SILBasicBlock *BB);

  /// Merge in the top down state for any predecessors of BB into BBState. This
  /// is a stub currently.
  void mergePredecessors(ARCBBState &BBState, SILBasicBlock *BB);
};

} // end arc namespace
} // end swift namespace
