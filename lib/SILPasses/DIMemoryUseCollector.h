//===--- DIMemoryUseCollector.h - Memory use information for DI -*- C++ -*-===//
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
// This file declares logic used by definitive analysis related passes that look
// at all the instructions that access a memory object.  This is quite specific
// to definitive analysis in that it is tuple element sensitive instead of
// relying on SROA.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILPASSES_DI_MEMORY_USE_COLLECTOR_H
#define SWIFT_SILPASSES_DI_MEMORY_USE_COLLECTOR_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/APInt.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILType.h"

namespace swift {
  class SILBuilder;

/// DIMemoryObjectInfo - This struct holds information about the memory object
/// being analyzed that is required to correctly break it down into elements.
///
/// This includes a collection of utilities for reasoning about (potentially
/// recursively) exploded aggregate elements, and computing access paths and
/// indexes into the flattened namespace.
///
/// The flattened namespace is assigned lexicographically.  For example, in:
///   (Int, ((Float, (), Double)))
/// the Int member is numbered 0, the Float is numbered 1, and the Double is
/// numbered 2.  Empty tuples don't get numbered since they contain no state.
///
/// Structs and classes have their elements exploded when we are analyzing the
/// 'self' member in an initializer for the aggregate.
///
/// Derived classes have an additional field at the end that models whether or
/// not super.init() has been called or not.
class DIMemoryObjectInfo {
public:
  /// This is the instruction that represents the memory.  It is either an
  /// allocation (alloc_box, alloc_stack) or a mark_uninitialized.
  SILInstruction *MemoryInst;

  /// This is the base type of the memory allocation.
  SILType MemorySILType;
  
  /// This is true if the memory being analyzed represents the 'self' value in
  /// an initializer.
  bool IsSelfOfInitializer;

  /// This is the count of elements being analyzed.  For memory objects that are
  /// tuples, this is the flattened element count.  For 'self' members in init
  /// methods, this is the local field count (+1 for derive classes).
  unsigned NumElements;
public:

  DIMemoryObjectInfo(SILInstruction *MemoryInst);

  SILLocation getLoc() const { return MemoryInst->getLoc(); }
  SILFunction &getFunction() const { return *MemoryInst->getFunction(); }

  /// Return the first instruction of the function containing the memory object.
  SILInstruction *getFunctionEntryPoint() const;

  CanType getType() const {
    return MemorySILType.getSwiftRValueType();
  }

  SILValue getAddress() const {
    if (isa<MarkUninitializedInst>(MemoryInst))
      return SILValue(MemoryInst, 0);
    return SILValue(MemoryInst, 1);
  }

  /// getNumMemoryElements - Return the number of elements, without the extra
  /// "super.init" tracker in initializers of derived classes.
  unsigned getNumMemoryElements() const {
    return NumElements - (unsigned)isDerivedClassSelf();
  }

  bool isEnumSelf() const {
    if (auto *MUI = dyn_cast<MarkUninitializedInst>(MemoryInst))
      if (MUI->getKind() == MarkUninitializedInst::RootSelf &&
          isa<EnumDecl>(getType()->getAnyNominal()))
        return true;
    return false;
  }
  
  /// isDerivedClassSelf - Return true if this memory object is the 'self' of
  /// a derived class init method.
  bool isDerivedClassSelf() const {
    return IsSelfOfInitializer &&
           cast<MarkUninitializedInst>(MemoryInst)->getKind() ==
             MarkUninitializedInst::DerivedSelf;
  }

  /// emitElementAddress - Given an element number (in the flattened sense)
  /// return a pointer to a leaf element of the specified number.
  SILValue emitElementAddress(unsigned TupleEltNo, SILLocation Loc,
                              SILBuilder &B) const;

  /// getElementType - Return the swift type of the specified element.
  CanType getElementType(unsigned EltNo) const;

  /// Push the symbolic path name to the specified element number onto the
  /// specified std::string.
  void getPathStringToElement(unsigned Element, std::string &Result) const;
};


enum DIUseKind {
  // The instruction is a Load.
  Load,
  
  // The instruction is either an initialization or an assignment, we don't
  // know which.  This classification only happens with values of trivial type
  // where the different isn't significant.
  InitOrAssign,
  
  // The instruction is an initialization of the tuple element.
  Initialization,
  
  // The instruction is an assignment, overwriting an already initialized
  // value.
  Assign,
  
  // The instruction is a store to a member of a larger struct value.
  PartialStore,
  
  /// An indirect 'inout' parameter of an Apply instruction.
  InOutUse,
  
  /// An indirect 'in' parameter of an Apply instruction.
  IndirectIn,
  
  /// This instruction is a general escape of the value, e.g. a call to a
  /// closure that captures it.
  Escape,

  /// This instruction is the start of an access to the superclass when we are
  /// analyzing 'self'.
  Superclass
};

/// This struct represents a single classified access to the memory object
/// being analyzed, along with classification information about the access.
struct DIMemoryUse {
  /// This is the instruction accessing the memory.
  SILInstruction *Inst;
  
  /// This is what kind of access it is, load, store, escape, etc.
  DIUseKind Kind;
  
  /// For memory objects of (potentially recursive) tuple type, this keeps
  /// track of which tuple elements are affected.
  unsigned short FirstElement, NumElements;
  
  DIMemoryUse(SILInstruction *Inst, DIUseKind Kind, unsigned FE, unsigned NE)
  : Inst(Inst), Kind(Kind), FirstElement(FE), NumElements(NE) {
    assert(FE == FirstElement && NumElements == NE &&
           "more than 64K elements not supported yet");
  }
  
  DIMemoryUse() : Inst(nullptr) {}
  
  bool isInvalid() const { return Inst == nullptr; }
  bool isValid() const { return Inst != nullptr; }

  bool usesElement(unsigned i) const {
    return i >= FirstElement && i < FirstElement+NumElements;
  }
  
  /// onlyTouchesTrivialElements - Return true if all of the accessed elements
  /// have trivial type.
  bool onlyTouchesTrivialElements(const DIMemoryObjectInfo &MemoryInfo) const;
  
  /// getElementBitmask - Return a bitmask with the touched tuple elements
  /// set.
  APInt getElementBitmask(unsigned NumMemoryTupleElements) const {
    return APInt::getBitsSet(NumMemoryTupleElements, FirstElement,
                             FirstElement+NumElements);
  }
};

/// collectDIElementUsesFrom - Analyze all uses of the specified allocation
/// instruction (alloc_box, alloc_stack or mark_uninitialized), classifying them
/// and storing the information found into the Uses and Releases lists.
void collectDIElementUsesFrom(const DIMemoryObjectInfo &MemoryInfo,
                              SmallVectorImpl<DIMemoryUse> &Uses,
                              SmallVectorImpl<SILInstruction*> &Releases,
                              bool isDefiniteInitFinished);

} // end namespace swift

#endif
