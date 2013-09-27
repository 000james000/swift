//===--- SILVisitor.h - Defines the SILVisitor class -------------*- C++ -*-==//
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
// This file defines the SILVisitor class, used for walking SIL code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILVISITOR_H
#define SWIFT_SIL_SILVISITOR_H

#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILArgument.h"
#include "llvm/Support/ErrorHandling.h"

namespace swift {

/// SILVisitor - This is a simple visitor class for Swift SIL nodes, allowing
/// clients to walk over entire SIL functions, blocks, or instructions.
template<typename ImplClass, typename ValueRetTy = void>
class SILVisitor {
public:
#define VALUE(CLASS, PARENT)              \
  case ValueKind::CLASS:                  \
    return static_cast<ImplClass*>(this)  \
    ->visit##CLASS(static_cast<CLASS*>(V));

  ValueRetTy visit(ValueBase *V) {
    switch (V->getKind()) {
#include "swift/SIL/SILNodes.def"
    }
    llvm_unreachable("Not reachable, all cases handled");
  }
  ValueRetTy visit(SILValue V) {
    return visit(V.getDef());
  }

  // Define default dispatcher implementations chain to parent nodes.
#define VALUE(CLASS, PARENT)                   \
ValueRetTy visit##CLASS(CLASS *I) {            \
  return static_cast<ImplClass*>(this)->visit##PARENT(I);  \
}

#define ABSTRACT_VALUE(CLASS, PARENT)                       \
ValueRetTy visit##CLASS(CLASS *I) {                         \
  return static_cast<ImplClass*>(this)->visit##PARENT(I);   \
}
#include "swift/SIL/SILNodes.def"

  void visitSILBasicBlock(SILBasicBlock *BB) {
    for (auto argI = BB->bbarg_begin(), argEnd = BB->bbarg_end();
         argI != argEnd;
         ++argI)
      visit(*argI);
      
    for (auto &I : *BB)
      visit(&I);
  }
  void visitSILBasicBlock(SILBasicBlock &BB) {
    this->ImplClass::visitSILBasicBlock(&BB);
  }

  void visitSILFunction(SILFunction *F) {
    for (auto &BB : *F)
      this->ImplClass::visitSILBasicBlock(&BB);
  }
  void visitSILFunction(SILFunction &F) {
    this->ImplClass::visitSILFunction(&F);
  }
};

/// A simple convenience class for a visitor that should only visit
/// SIL instructions.
template<typename ImplClass, typename ValueRetTy = void>
class SILInstructionVisitor : public SILVisitor<ImplClass, ValueRetTy> {
public:
  ValueRetTy visitSILArgument(SILArgument *A) {
    llvm_unreachable("should only be visiting instructions");
  }

  ValueRetTy visit(SILInstruction *I) {
    return SILVisitor<ImplClass, ValueRetTy>::visit(I);
  }
};

} // end namespace swift

#endif
