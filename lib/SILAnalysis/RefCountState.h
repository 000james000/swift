//===--- RefCountState.h - Represents a Reference Count -----*- C++ -*-----===//
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

#ifndef SWIFT_SILANALYSIS_REFCOUNTSTATE_H
#define SWIFT_SILANALYSIS_REFCOUNTSTATE_H

#include "RCStateTransition.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SILAnalysis/ARCAnalysis.h"
#include <algorithm>

namespace swift {
class AliasAnalysis;
} // end namespace swift

//===----------------------------------------------------------------------===//
//                              Ref Count State
//===----------------------------------------------------------------------===//

namespace swift {

/// A struct that abstracts over reference counts manipulated by strong_retain,
/// retain_value, strong_release,
template <typename ImplStruct>
struct RefCountState {
  using InstructionSet = llvm::SmallPtrSet<SILInstruction *, 4>;

  /// Return the SILValue that represents the RCRoot that we are
  /// tracking.
  SILValue RCRoot;

  /// The last state transition that this RefCountState went through. None if we
  /// have not see any transition on this ref count yet.
  llvm::Optional<RCStateTransition> Transition;

  /// Was the pointer we are tracking known incremented when we visited the
  /// current increment we are tracking? In that case we know that it is safe
  /// to move the inner retain over instructions that may decrement ref counts
  /// since the outer retain will keep the reference counted value alive.
  bool KnownSafe = false;

  /// The latest point we can move Instruction without moving it over an
  /// instruction that might be able to decrement the value with reference
  /// semantics.
  InstructionSet InsertPts;

  /// Have we performed any partial merges of insertion points? We can not
  /// perform two partial merges in a row unless we are able to reason about
  /// control dependency (which avoid for now).
  bool Partial = false;

  /// Return this as the CRTP substruct.
  ImplStruct *asImpl() { return static_cast<ImplStruct *>(this); }
  ImplStruct *asImpl() const {
    return const_cast<RefCountState<ImplStruct> *>(this)->asImpl();
  }

  RefCountState() {}
  ~RefCountState() {}

  /// Initializes/reinitialized the state for I. If we reinitialize we return
  /// true.
  bool initWithInst(SILInstruction *I) {
    // Are we already tracking a ref count modification?
    bool Nested = isTrackingRefCount();

    Transition = RCStateTransition(I);

    // Initialize value.
    RCRoot = I->getOperand(0).stripCasts();

    // This retain is known safe if the operand we are tracking was already
    // known incremented previously. This occurs when you have nested
    // increments.
    KnownSafe = isRefCountStateModified();

    // Clear our insertion point list.
    InsertPts.clear();

    return Nested;
  }

  /// Uninitialize the current state.
  void clear() {
    RCRoot = SILValue();
    Transition = None;
    KnownSafe = false;
    Partial = false;
    InsertPts.clear();
  }

  /// Can we gaurantee that the given reference counted value has been modified?
  bool isRefCountStateModified() const {
    return asImpl()->isRefCountStateModified();
  }

  /// Is this ref count initialized and tracking a ref count ptr.
  bool isTrackingRefCount() const {
    return asImpl()->isTrackingRefCount();
  }

  /// Are we tracking an instruction currently? This returns false when given an
  /// uninitialized ReferenceCountState.
  bool isTrackingRefCountInst() const {
    return asImpl()->isTrackingRefCountInst();
  }

  /// Are we tracking a source of ref counts? This currently means that we are
  /// tracking an argument that is @owned. In the future this will include
  /// return values of functions that are @owned.
  bool isTrackingRefCountSource() const {
    return asImpl()->isTrackingRefCountSource();
  }

  /// Return the increment we are tracking.
  Range<InstructionSet::iterator> getInstructions() const {
    return asImpl()->getInstructions();
  }

  /// Returns true if I is in the instructions we are tracking.
  bool containsInstruction(SILInstruction *I) const {
    return asImpl()->containsInstruction(I);
  }

  /// Return the value with reference semantics that is the operand of our
  /// increment.
  SILValue getRCRoot() const {
    assert(RCRoot && "Value should never be null here");
    return RCRoot;
  }

  /// Returns true if we have a valid value that we are tracking.
  bool hasRCRoot() const {
    return RCRoot.isValid();
  }

  /// The latest point we can move the increment without bypassing instructions
  /// that may have reference semantics.
  Range<InstructionSet::iterator> getInsertPts() const {
    return {InsertPts.begin(), InsertPts.end()};
  }

  /// This retain is known safe if the operand we are tracking was already
  /// known incremented previously. This occurs when you have nested
  /// increments.
  bool isKnownSafe() const { return KnownSafe; }

  /// This reference count state is partial if we found a partial merge of
  /// insertion points. This stymies our ability to move instructions due to
  /// potential control dependency issues.
  bool isPartial() const { return Partial; }

  /// Check if PotentialGuaranteedUser can use the reference count associated
  /// with the value we are tracking. If so advance the state's sequence
  /// appropriately and return true. Otherwise return false.
  bool handlePotentialGuaranteedUser(SILInstruction *PotentialGuaranteedUser,
                                     AliasAnalysis *AA) {
    // If we are not tracking a ref count, just return false.
    if (!isTrackingRefCount())
      return false;

    // If at the current lattice state, we don't care if the value we are
    // tracking can be decremented or used, just return false.
    //
    // This causes us to only perform alias queries when we are at a lattice
    // state where the alias queries will actually be used.
    if (!asImpl()->valueCanBeGuaranteedUsedGivenLatticeState())
      return false;

    // If we can prove that Other can not use the pointer we are tracking,
    // return...
    if (!mayGuaranteedUseValue(PotentialGuaranteedUser, getRCRoot(), AA))
      return false;

    // Otherwise, allow the CRTP substruct to update itself given we have a
    // potential decrement.
    return asImpl()->handleGuaranteedUser(PotentialGuaranteedUser);
  }

  /// Check if PotentialDecrement can decrement the reference count associated
  /// with the value we are tracking. If so advance the state's sequence
  /// appropriately and return true. Otherwise return false.
  bool handlePotentialDecrement(SILInstruction *PotentialDecrement,
                                AliasAnalysis *AA) {
    // If we are not tracking a ref count, just return false.
    if (!isTrackingRefCount())
      return false;

    // If at the current lattice state, we don't care if the value we are
    // tracking can be decremented, just return false.
    //
    // This causes us to only perform alias queries when we are at a lattice
    // state where the alias queries will actually be used.
    if (!asImpl()->valueCanBeDecrementedGivenLatticeState())
      return false;

    // If we can prove that Other can not use the pointer we are tracking,
    // return...
    if (!mayDecrementRefCount(PotentialDecrement, getRCRoot(), AA))
      return false;

    // Otherwise, allow the CRTP substruct to update itself given we have a
    // potential decrement.
    return asImpl()->handleDecrement(PotentialDecrement);
  }

  /// Check if PotentialUser could be a use of the reference counted value that
  /// requires user to be alive. If so advance the state's sequence
  /// appropriately and return true. Otherwise return false.
  bool handlePotentialUser(SILInstruction *PotentialUser, AliasAnalysis *AA) {
    // If we are not tracking a ref count, just return false.
    if (!isTrackingRefCount())
      return false;

    // If at the current lattice state, we don't care if the value we are
    // tracking can be used, just return false.
    //
    // This causes us to only perform alias queries when we are at a lattice
    // state where the alias queries will actually be used.
    if (!asImpl()->valueCanBeUsedGivenLatticeState())
      return false;

    if (!mayUseValue(PotentialUser, getRCRoot(), AA))
      return false;

    return asImpl()->handleUser(PotentialUser);
  }

  /// Returns true if the passed in ref count inst matches the ref count inst
  /// we are tracking. This handles generically retains/release.
  bool isRefCountInstMatchedToTrackedInstruction(SILInstruction *RefCountInst) {
    // If we are not tracking any state transitions bail.
    if (!Transition.hasValue())
      return false;

    // Otherwise, ask the transition state if this instruction causes a
    // transition that can be matched with the transition in order to eliminate
    // the transition.
    if (!Transition->matchingInst(RefCountInst))
      return false;

    // If we have a match, handle it.
    return asImpl()->handleRefCountInstMatch(RefCountInst);
  }
};

//===----------------------------------------------------------------------===//
//                         Bottom Up Ref Count State
//===----------------------------------------------------------------------===//

struct BottomUpRefCountState : RefCountState<BottomUpRefCountState> {
  using SuperTy = RefCountState<BottomUpRefCountState>;

  /// Sequence of states that a value with reference semantics can go through
  /// when visiting decrements bottom up. The reason why I have this separate
  /// from TopDownSubstruct is I think it gives more clarity to the algorithm
  /// by giving it typed form.
  enum class LatticeState {
    None,               ///< The pointer has no information associated with it.
    Decremented,        ///< The pointer will be decremented.
    MightBeUsed,        ///< The pointer will be used and then at this point
                        ///  be decremented
    MightBeDecremented, ///< The pointer might be decremented again implying
                        ///  that we can not, without being known safe remove
                        ///  this decrement.
  };

  /// Current place in the sequence of the value.
  LatticeState LatState = LatticeState::None;

  /// Initializes/reinitialized the state for I. If we reinitialize we return
  /// true.
  bool initWithInst(SILInstruction *I) {
    assert((isa<StrongReleaseInst>(I) || isa<ReleaseValueInst>(I)) &&
           "strong_release and release_value are only supported.");

    bool NestingDetected = SuperTy::initWithInst(I);

    // If we know that there is another decrement on the same pointer that has
    // not been matched up to an increment, then the pointer must have a
    // reference count of at least 2 before this decrement. This implies it is
    // known safe.
    KnownSafe = NestingDetected;

    // Set our lattice state to be incremented.
    LatState = LatticeState::Decremented;

    return NestingDetected;
  }

  /// Uninitialize the current state.
  void clear() {
    LatState = LatticeState::None;
    SuperTy::clear();
  }

  /// Can we gaurantee that the given reference counted value has been modified?
  bool isRefCountStateModified() const {
    return LatState == LatticeState::Decremented;
  }

  /// Is this ref count initialized and tracking a ref count ptr.
  bool isTrackingRefCount() const { return Transition.hasValue(); }

  /// Are we tracking an instruction currently? This returns false when given an
  /// uninitialized ReferenceCountState.
  bool isTrackingRefCountInst() const {
    return Transition.hasValue() && Transition->isMutator();
  }

  /// Are we tracking a source of ref counts? This currently means that we are
  /// tracking an argument that is @owned. In the future this will include
  /// return values of functions that are @owned.
  bool isTrackingRefCountSource() const {
    return Transition.hasValue() && Transition->isEndPoint();
  }

  /// Returns true if I is an instruction that we are tracking.
  bool containsInstruction(SILInstruction *I) const {
    return Transition.hasValue() && Transition->containsMutator(I);
  }

  /// Return the increment we are tracking.
  RCStateTransition::mutator_range getInstructions() const {
    return Transition->getMutators();
  }

  /// Returns true if given the current lattice state, do we care if the value
  /// we are tracking is decremented.
  bool valueCanBeDecrementedGivenLatticeState() const {
    return LatState == LatticeState::MightBeUsed;
  }

  /// If advance the state's sequence appropriately for a decrement. If we do
  /// advance return true. Otherwise return false.
  bool handleDecrement(SILInstruction *PotentialDecrement) {
    switch (LatState) {
    case LatticeState::MightBeUsed:
      LatState = LatticeState::MightBeDecremented;
      return true;
    case LatticeState::None:
    case LatticeState::MightBeDecremented:
    case LatticeState::Decremented:
      return false;
    }
  }

  /// Returns true if given the current lattice state, do we care if the value
  /// we are tracking is used.
  bool valueCanBeUsedGivenLatticeState() const {
    return LatState == LatticeState::Decremented;
  }

  /// Given the current lattice state, if we have seen a use, advance the
  /// lattice state. Return true if we do so and false otherwise.
  bool handleUser(SILInstruction *PotentialUser) {
    assert(valueCanBeUsedGivenLatticeState() &&
           "Must be able to be used at this point of the lattice.");
    // Advance the sequence...
    switch (LatState) {
    case LatticeState::Decremented:
      LatState = LatticeState::MightBeUsed;
      assert(InsertPts.empty() && "If we are decremented, we should have no "
                                  "insertion points.");
      InsertPts.insert(std::next(SILBasicBlock::iterator(PotentialUser)));
      return true;
    case LatticeState::MightBeUsed:
    case LatticeState::MightBeDecremented:
    case LatticeState::None:
      return false;
    }
  }

  /// Returns true if given the current lattice state, do we care if the value
  /// we are tracking is used.
  bool valueCanBeGuaranteedUsedGivenLatticeState() const {
    switch (LatState) {
    case LatticeState::None:
    case LatticeState::MightBeDecremented:
      return false;
    case LatticeState::Decremented:
    case LatticeState::MightBeUsed:
      return true;
    }
  }

  /// Given the current lattice state, if we have seen a use, advance the
  /// lattice state. Return true if we do so and false otherwise.
  bool handleGuaranteedUser(SILInstruction *PotentialGuaranteedUser) {
    assert(valueCanBeGuaranteedUsedGivenLatticeState() &&
           "Must be able to be used at this point of the lattice.");
    // Advance the sequence...
    switch (LatState) {
    // If were decremented, insert the insertion point.
    case LatticeState::Decremented: {
      assert(InsertPts.empty() && "If we are decremented, we should have no "
                                  "insertion points.");
      auto Iter = SILBasicBlock::iterator(PotentialGuaranteedUser);
      InsertPts.insert(std::next(Iter));
      LatState = LatticeState::MightBeDecremented;
      return true;
    }
    case LatticeState::MightBeUsed:
      // If we have a might be used, we already created an insertion point
      // earlier. Just move to MightBeDecremented.
      LatState = LatticeState::MightBeDecremented;
      return true;
    case LatticeState::MightBeDecremented:
    case LatticeState::None:
      return false;
    }
  }

  /// We have a matching ref count inst. Return true if we advance the sequence
  /// and false otherwise.
  bool handleRefCountInstMatch(SILInstruction *RefCountInst) {
    // Otherwise modify the state appropriately in preparation for removing the
    // increment, decrement pair.
    switch (LatState) {
    case LatticeState::None:
      return false;
    case LatticeState::Decremented:
    case LatticeState::MightBeUsed:
      // Unset InsertPt so we remove retain release pairs instead of
      // performing code motion.
      InsertPts.clear();
      SWIFT_FALLTHROUGH;
    case LatticeState::MightBeDecremented:
      return true;
    }
  }

  bool merge(const BottomUpRefCountState &Other);
};

//===----------------------------------------------------------------------===//
//                          Top Down Ref Count State
//===----------------------------------------------------------------------===//

struct TopDownRefCountState : RefCountState<TopDownRefCountState> {
  using SuperTy = RefCountState<TopDownRefCountState>;

  /// Sequence of states that a value with reference semantics can go through
  /// when visiting decrements bottom up. The reason why I have this separate
  /// from BottomUpRefCountState is I think it gives more clarity to the
  /// algorithm by giving it typed form.
  enum class LatticeState {
    None,               ///< The pointer has no information associated with it.
    Incremented,        ///< The pointer has been incremented.
    MightBeDecremented, ///< The pointer has been incremented and might be
                        ///  decremented. be decremented again implying
    MightBeUsed,        ///< The pointer has been incremented,
  };

  /// Current place in the sequence of the value.
  LatticeState LatState = LatticeState::None;

  /// Initializes/reinitialized the state for I. If we reinitialize we return
  /// true.
  bool initWithInst(SILInstruction *I) {
    assert((isa<StrongRetainInst>(I) || isa<RetainValueInst>(I)) &&
           "strong_retain and retain_value are only supported.");

    bool NestingDetected = SuperTy::initWithInst(I);

    // Set our lattice state to be incremented.
    LatState = LatticeState::Incremented;

    return NestingDetected;
  }

  void initWithArg(SILArgument *Arg) {
    LatState = LatticeState::Incremented;
    Transition = RCStateTransition(Arg);
    RCRoot = Arg;
    KnownSafe = false;
    InsertPts.clear();
  }

  /// Uninitialize the current state.
  void clear() {
    Transition = None;
    LatState = LatticeState::None;
    SuperTy::clear();
  }

  /// Can we gaurantee that the given reference counted value has been modified?
  bool isRefCountStateModified() const {
    return LatState == LatticeState::Incremented;
  }

  /// Is this ref count initialized and tracking a ref count ptr.
  bool isTrackingRefCount() const { return Transition.hasValue(); }

  /// Are we tracking an instruction currently? This returns false when given an
  /// uninitialized ReferenceCountState.
  bool isTrackingRefCountInst() const {
    return Transition.hasValue() && Transition->isMutator();
  }

  /// Are we tracking a source of ref counts? This currently means that we are
  /// tracking an argument that is @owned. In the future this will include
  /// return values of functions that are @owned.
  bool isTrackingRefCountSource() const {
    return Transition.hasValue() && Transition->isEndPoint();
  }

  /// Returns true if I is an instruction that we are tracking.
  bool containsInstruction(SILInstruction *I) const {
    return Transition.hasValue() && Transition->containsMutator(I);
  }

  /// Return the increment we are tracking.
  RCStateTransition::mutator_range getInstructions() const {
    return Transition->getMutators();
  }

  /// Returns true if given the current lattice state, do we care if the value
  /// we are tracking is decremented.
  bool valueCanBeDecrementedGivenLatticeState() const {
    return LatState == LatticeState::Incremented;
  }

  /// If advance the state's sequence appropriately for a decrement. If we do
  /// advance return true. Otherwise return false.
  bool handleDecrement(SILInstruction *PotentialDecrement) {
    switch (LatState) {
    case LatticeState::Incremented:
      LatState = LatticeState::MightBeDecremented;
      InsertPts.insert(PotentialDecrement);
      return true;
    case LatticeState::None:
    case LatticeState::MightBeDecremented:
    case LatticeState::MightBeUsed:
      return false;
    }
  }

  /// Returns true if given the current lattice state, do we care if the value
  /// we are tracking is used.
  bool valueCanBeUsedGivenLatticeState() const {
    return LatState == LatticeState::MightBeDecremented;
  }

  /// Given the current lattice state, if we have seen a use, advance the
  /// lattice state. Return true if we do so and false otherwise.
  bool handleUser(SILInstruction *PotentialUser) {
    assert(valueCanBeUsedGivenLatticeState() &&
           "Must be able to be used at this point of the lattice.");

    // Otherwise advance the sequence...
    switch (LatState) {
    case LatticeState::MightBeDecremented:
      LatState = LatticeState::MightBeUsed;
      return true;
    case LatticeState::Incremented:
    case LatticeState::None:
    case LatticeState::MightBeUsed:
      return false;
    }
  }

  /// Returns true if given the current lattice state, do we care if the value
  /// we are tracking is used.
  bool valueCanBeGuaranteedUsedGivenLatticeState() const {
    switch (LatState) {
    case LatticeState::None:
    case LatticeState::MightBeUsed:
      return false;
    case LatticeState::Incremented:
    case LatticeState::MightBeDecremented:
      return true;
    }
  }

  /// Given the current lattice state, if we have seen a use, advance the
  /// lattice state. Return true if we do so and false otherwise.
  bool handleGuaranteedUser(SILInstruction *PotentialGuaranteedUser) {
    assert(valueCanBeGuaranteedUsedGivenLatticeState() &&
           "Must be able to be used at this point of the lattice.");
    // Advance the sequence...
    switch (LatState) {
    // If were decremented, insert the insertion point.
    case LatticeState::Incremented: {
      assert(InsertPts.empty() && "If we are decremented, we should have no "
                                  "insertion points.");
      LatState = LatticeState::MightBeUsed;
      InsertPts.insert(PotentialGuaranteedUser);
      return true;
    }
    case LatticeState::MightBeDecremented:
      // If we have a might be used, we already created an insertion point
      // earlier. Just move to MightBeDecremented.
      LatState = LatticeState::MightBeUsed;
      return true;
    case LatticeState::MightBeUsed:
    case LatticeState::None:
      return false;
    }
  }

  /// We have a matching ref count inst. Return true if we advance the sequence
  /// and false otherwise.
  bool handleRefCountInstMatch(SILInstruction *RefCountInst) {
    // Otherwise modify the state appropriately in preparation for removing the
    // increment, decrement pair.
    switch (LatState) {
    case LatticeState::None:
      return false;
    case LatticeState::Incremented:
    case LatticeState::MightBeDecremented:
      // Unset InsertPt so we remove retain release pairs instead of performing
      // code motion.
      InsertPts.clear();
      SWIFT_FALLTHROUGH;
    case LatticeState::MightBeUsed:
      return true;
    }
  }

  bool merge(const TopDownRefCountState &Other);
};

} // end swift namespace

namespace llvm {

raw_ostream &operator<<(raw_ostream &OS,
                        swift::BottomUpRefCountState::LatticeState S);
raw_ostream &operator<<(raw_ostream &OS,
                        swift::TopDownRefCountState::LatticeState S);

} // end namespace llvm

#endif
