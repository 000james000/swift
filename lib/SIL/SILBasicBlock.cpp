//===--- SILBasicBlock.cpp - Basic blocks for high-level SIL code ----------==//
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
// This file defines the high-level BasicBlocks used for Swift SIL code.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
using namespace swift;

//===----------------------------------------------------------------------===//
// SILArgument Implementation
//===----------------------------------------------------------------------===//

SILArgument::SILArgument(SILType Ty, SILBasicBlock *ParentBB, const ValueDecl *D)
  : ValueBase(ValueKind::SILArgument, Ty), ParentBB(ParentBB), Decl(D) {
  // Function arguments need to have a decl.
  assert(
    !ParentBB->getParent()->isBare() &&
    ParentBB->getParent()->size() == 1
          ? D != nullptr
          : true );
  ParentBB->addArgument(this);
}


SILFunction *SILArgument::getFunction() {
  return getParent()->getParent();
}
const SILFunction *SILArgument::getFunction() const {
  return getParent()->getParent();
}

SILModule &SILArgument::getModule() const {
  return getFunction()->getModule();
}

//===----------------------------------------------------------------------===//
// SILBasicBlock Implementation
//===----------------------------------------------------------------------===//

SILBasicBlock::SILBasicBlock(SILFunction *parent, SILBasicBlock *afterBB)
  : Parent(parent), PredList(0) {
  if (afterBB) {
    parent->getBlocks().insertAfter(afterBB, this);
  } else {
    parent->getBlocks().push_back(this);
  }
}
SILBasicBlock::~SILBasicBlock() {
  // iplist's destructor is going to destroy the InstList.
}

SILModule &SILBasicBlock::getModule() const {
  return getParent()->getModule();
}

/// eraseFromParent - This method unlinks 'self' from the containing SIL and
/// deletes it.
///
void SILBasicBlock::eraseFromParent() {
  getParent()->getBlocks().erase(this);
}

/// Replace the ith BB argument with a new one with type Ty (and optional
/// ValueDecl D).
SILArgument *SILBasicBlock::replaceBBArg(unsigned i, SILType Ty, ValueDecl *D) {
  SILModule &M = getParent()->getModule();
  assert(BBArgList[i]->use_empty() && "Expected no uses of the old BB arg!");

  auto *NewArg = new (M) SILArgument(Ty, D);
  NewArg->setParent(this);
  BBArgList[i] = NewArg;

  return NewArg;
}

SILArgument *SILBasicBlock::createArgument(SILType Ty) {
  return new (getModule()) SILArgument(Ty, this);
}

/// \brief Splits a basic block into two at the specified instruction.
///
/// Note that all the instructions BEFORE the specified iterator
/// stay as part of the original basic block. The old basic block is left
/// without a terminator.
SILBasicBlock *SILBasicBlock::splitBasicBlock(iterator I) {
  SILBasicBlock *New = new (Parent->getModule()) SILBasicBlock(Parent);
  SILFunction::iterator Where = std::next(SILFunction::iterator(this));
  SILFunction::iterator First = SILFunction::iterator(New);
  if (Where != First)
    Parent->getBlocks().splice(Where, Parent->getBlocks(), First);
  // Move all of the specified instructions from the original basic block into
  // the new basic block.
  New->getInstList().splice(New->end(), this->getInstList(), I, end());
  return New;
}

/// \brief Splits a basic block into two at the specified instruction and
/// inserts an unconditional branch from the old basic block to the new basic
/// block.
SILBasicBlock *SILBasicBlock::splitBasicBlockAndBranch(iterator I,
                                                       SILLocation BranchLoc) {
  SILBasicBlock *New = splitBasicBlock(I);
  getInstList().insert(getInstList().end(),
                       BranchInst::create(BranchLoc, New, *getParent()));
  return New;
}

/// \brief Move the basic block to after the specified basic block in the IR.
void SILBasicBlock::moveAfter(SILBasicBlock *After) {
  assert(getParent() && getParent() == After->getParent() &&
         "Blocks must be in the same function");
  auto InsertPt = std::next(SILFunction::iterator(After));
  auto &BlkList = getParent()->getBlocks();
  BlkList.splice(InsertPt, BlkList, this);
}
