//===--- SimplifyInstruction.cpp - Fold instructions ----------------------===//
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

#define DEBUG_TYPE "sil-simplify"
#include "swift/SILAnalysis/ValueTracking.h"
#include "swift/SILPasses/Utils/Local.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"

using namespace swift;
using namespace swift::PatternMatch;

namespace swift {
  class ASTContext;
}

namespace {
  class InstSimplifier : public SILInstructionVisitor<InstSimplifier, SILValue>{
  public:
    SILValue visitSILInstruction(SILInstruction *I) { return SILValue(); }

    SILValue visitTupleExtractInst(TupleExtractInst *TEI);
    SILValue visitStructExtractInst(StructExtractInst *SEI);
    SILValue visitEnumInst(EnumInst *EI);
    SILValue visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI);
    SILValue visitAddressToPointerInst(AddressToPointerInst *ATPI);
    SILValue visitPointerToAddressInst(PointerToAddressInst *PTAI);
    SILValue visitRefToRawPointerInst(RefToRawPointerInst *RRPI);
    SILValue
    visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI);
    SILValue visitUncheckedRefCastInst(UncheckedRefCastInst *OPRI);
    SILValue visitUncheckedAddrCastInst(UncheckedAddrCastInst *UACI);
    SILValue visitStructInst(StructInst *SI);
    SILValue visitTupleInst(TupleInst *SI);
    SILValue visitApplyInst(ApplyInst *AI);
    SILValue visitUpcastInst(UpcastInst *UI);
    SILValue visitUncheckedRefBitCastInst(UncheckedRefBitCastInst *URBCI);
    SILValue
    visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI);

    SILValue simplifyOverflowBuiltin(ApplyInst *AI,
                                     BuiltinFunctionRefInst *FR);
  };
} // end anonymous namespace

SILValue InstSimplifier::visitStructInst(StructInst *SI) {
  // Ignore empty structs.
  if (SI->getNumOperands() < 1)
    return SILValue();

  // Optimize structs that are generated from struct_extract instructions
  // from the same struct.
  if (auto *Ex0 = dyn_cast<StructExtractInst>(SI->getOperand(0))) {
    // Check that the constructed struct and the extracted struct are of the
    // same type.
    if (SI->getType() != Ex0->getOperand().getType())
      return SILValue();

    // Check that all of the operands are extracts of the correct kind.
    for (unsigned i = 0, e = SI->getNumOperands(); i < e; i++) {
      auto *Ex = dyn_cast<StructExtractInst>(SI->getOperand(i));
      // Must be an extract.
      if (!Ex)
        return SILValue();

      // Extract from the same struct as the first extract_inst.
      if (Ex0->getOperand() != Ex->getOperand())
        return SILValue();

      // And the order of the field must be identical to the construction order.
      if (Ex->getFieldNo() != i)
        return SILValue();
    }

    return Ex0->getOperand();
  }
  
  return SILValue();
}

SILValue InstSimplifier::visitTupleInst(TupleInst *TI) {
  // Ignore empty tuples.
  if (TI->getNumOperands() < 1)
    return SILValue();

  // Optimize tuples that are generated from tuple_extract instructions
  // from the same tuple.
  if (auto *Ex0 = dyn_cast<TupleExtractInst>(TI->getOperand(0))) {
    // Check that the constructed tuple and the extracted tuple are of the
    // same type.
    if (TI->getType() != Ex0->getOperand().getType())
      return SILValue();

    // Check that all of the operands are extracts of the correct kind.
    for (unsigned i = 0, e = TI->getNumOperands(); i < e; i++) {
      auto *Ex = dyn_cast<TupleExtractInst>(TI->getOperand(i));
      // Must be an extract.
      if (!Ex)
        return SILValue();

      // Extract from the same struct as the first extract_inst.
      if (Ex0->getOperand() != Ex->getOperand())
        return SILValue();

      // And the order of the field must be identical to the construction order.
      if (Ex->getFieldNo() != i)
        return SILValue();
    }

    return Ex0->getOperand();
  }

  return SILValue();
}

SILValue InstSimplifier::visitTupleExtractInst(TupleExtractInst *TEI) {
  // tuple_extract(tuple(x, y), 0) -> x
  if (TupleInst *TheTuple = dyn_cast<TupleInst>(TEI->getOperand()))
    return TheTuple->getElements()[TEI->getFieldNo()];

  // tuple_extract(apply([add|sub|...]overflow(x,y)),  0) -> x
  // tuple_extract(apply(checked_trunc(ext(x))), 0) -> x
  if (TEI->getFieldNo() == 0)
    if (ApplyInst *AI = dyn_cast<ApplyInst>(TEI->getOperand()))
      if (auto *BFRI = dyn_cast<BuiltinFunctionRefInst>(AI->getCallee()))
        return simplifyOverflowBuiltin(AI, BFRI);

  return SILValue();
}

SILValue InstSimplifier::visitStructExtractInst(StructExtractInst *SEI) {
  // struct_extract(struct(x, y), x) -> x
  if (StructInst *Struct = dyn_cast<StructInst>(SEI->getOperand()))
    return Struct->getFieldValue(SEI->getField());
  
  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI) {
  // (unchecked_enum_data (enum payload)) -> payload
  if (EnumInst *EI = dyn_cast<EnumInst>(UEDI->getOperand())) {
    if (EI->getElement() != UEDI->getElement())
      return SILValue();

    assert(EI->hasOperand() &&
           "Should only get data from an enum with payload.");
    return EI->getOperand();
  }

  return SILValue();
}

// Simplify
//   %1 = unchecked_enum_data %0 : $Optional<C>, #Optional.Some!enumelt.1 // user: %27
//   %2 = enum $Optional<C>, #Optional.Some!enumelt.1, %1 : $C // user: %28
// to %0 since we are building the same enum.
static SILValue simplifyEnumFromUncheckedEnumData(EnumInst *EI) {
  assert(EI->hasOperand() && "Expected an enum with an operand!");

  auto *UEDI = dyn_cast<UncheckedEnumDataInst>(EI->getOperand());
  if (!UEDI || UEDI->getElement() != EI->getElement())
    return SILValue();

  return UEDI->getOperand();
}

SILValue InstSimplifier::visitEnumInst(EnumInst *EI) {
  if (EI->hasOperand())
    return simplifyEnumFromUncheckedEnumData(EI);

  // Simplify enum insts to the value from a switch_enum when possible, e.g.
  // for
  //   switch_enum %0 : $Bool, case #Bool.true!enumelt: bb1
  // bb1:
  //   %1 = enum $Bool, #Bool.true!enumelt
  //
  // we'll return %0
  auto *BB = EI->getParent();
  auto *Pred = BB->getSinglePredecessor();
  if (!Pred)
    return SILValue();

  if (auto *SEI = dyn_cast<SwitchEnumInst>(Pred->getTerminator())) {
    if (EI->getType() != SEI->getOperand().getType())
      return SILValue();

    if (BB == SEI->getCaseDestination(EI->getElement()))
      return SEI->getOperand();
  }

  return SILValue();
}

SILValue InstSimplifier::visitAddressToPointerInst(AddressToPointerInst *ATPI) {
  // (address_to_pointer (pointer_to_address x)) -> x
  if (auto *PTAI = dyn_cast<PointerToAddressInst>(ATPI->getOperand()))
    if (PTAI->getType() == ATPI->getOperand().getType())
      return PTAI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitPointerToAddressInst(PointerToAddressInst *PTAI) {
  // (pointer_to_address (address_to_pointer x)) -> x
  if (auto *ATPI = dyn_cast<AddressToPointerInst>(PTAI->getOperand()))
    if (ATPI->getOperand().getType() == PTAI->getType())
      return ATPI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitRefToRawPointerInst(RefToRawPointerInst *RefToRaw) {
  // Perform the following simplification:
  //
  // (ref_to_raw_pointer (raw_pointer_to_ref x)) -> x
  //
  // *NOTE* We don't need to check types here.
  if (auto *RawToRef = dyn_cast<RawPointerToRefInst>(&*RefToRaw->getOperand()))
    return RawToRef->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI) {
  // (UCCI downcast (upcast x #type1 to #type2) #type2 to #type1) -> x
  if (auto *upcast = dyn_cast<UpcastInst>(UCCI->getOperand()))
    if (UCCI->getType() == upcast->getOperand().getType())
      return upcast->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedRefCastInst(UncheckedRefCastInst *OPRI) {
  // (unchecked-ref-cast Y->X (unchecked-ref-cast x X->Y)) -> x
  if (auto *ROPI = dyn_cast<UncheckedRefCastInst>(&*OPRI->getOperand()))
    if (ROPI->getOperand().getType() == OPRI->getType())
      return ROPI->getOperand();

  // (unchecked-ref-cast Y->X (upcast x X->Y)) -> x
  if (auto *UI = dyn_cast<UpcastInst>(OPRI->getOperand()))
    if (UI->getOperand().getType() == OPRI->getType())
      return UI->getOperand();

  // (unchecked-ref-cast X->X x) -> x
  if (OPRI->getOperand().getType() == OPRI->getType())
    return OPRI->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedAddrCastInst(UncheckedAddrCastInst *UACI) {
  // (unchecked-addr-cast Y->X (unchecked-addr-cast x X->Y)) -> x
  if (auto *OtherUACI = dyn_cast<UncheckedAddrCastInst>(&*UACI->getOperand()))
    if (OtherUACI->getOperand().getType() == UACI->getType())
      return OtherUACI->getOperand();

  // (unchecked-addr-cast X->X x) -> x
  if (UACI->getOperand().getType() == UACI->getType())
    return UACI->getOperand();

  return SILValue();
}

SILValue InstSimplifier::visitUpcastInst(UpcastInst *UI) {
  // (upcast Y->X (unchecked-ref-cast x X->Y)) -> x
  if (auto *URCI = dyn_cast<UncheckedRefCastInst>(UI->getOperand()))
    if (URCI->getOperand().getType() == UI->getType())
      return URCI->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedRefBitCastInst(UncheckedRefBitCastInst *URBCI) {
  // (unchecked_ref_bit_cast X->X x) -> x
  if (URBCI->getOperand().getType() == URBCI->getType())
    return URBCI->getOperand();

  // (unchecked_ref_bit_cast Y->X (unchecked_ref_bit_cast X->Y x)) -> x
  if (auto *Op = dyn_cast<UncheckedRefBitCastInst>(URBCI->getOperand()))
    if (Op->getOperand().getType() == URBCI->getType())
      return Op->getOperand();

  return SILValue();
}

SILValue
InstSimplifier::
visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI) {
  // (unchecked_trivial_bit_cast X->X x) -> x
  if (UTBCI->getOperand().getType() == UTBCI->getType())
    return UTBCI->getOperand();

  // (unchecked_trivial_bit_cast Y->X (unchecked_trivial_bit_cast X->Y x)) -> x
  if (auto *Op = dyn_cast<UncheckedTrivialBitCastInst>(UTBCI->getOperand()))
    if (Op->getOperand().getType() == UTBCI->getType())
      return Op->getOperand();

  return SILValue();
}


static SILValue simplifyBuiltin(ApplyInst *AI,
                                BuiltinFunctionRefInst *FR) {
  const IntrinsicInfo &Intrinsic = FR->getIntrinsicInfo();

  switch (Intrinsic.ID) {
  default:
    // TODO: Handle some of the llvm intrinsics here.
    return SILValue();
  case llvm::Intrinsic::not_intrinsic:
    break;
  case llvm::Intrinsic::expect:
    // If we have an expect optimizer hint with a constant value input,
    // there is nothing left to expect so propagate the input, i.e.,
    //
    // apply(expect, constant, _) -> constant.
    if (auto *Literal = dyn_cast<IntegerLiteralInst>(AI->getArgument(0)))
      return Literal;
    return SILValue();
  }

  // Otherwise, it should be one of the builtin functions.
  OperandValueArrayRef Args = AI->getArguments();
  const BuiltinInfo &Builtin = FR->getBuiltinInfo();

  switch (Builtin.ID) {
  default: break;

  case BuiltinValueKind::TruncOrBitCast: {
    const SILValue &Op = Args[0];
    SILValue Result;
    // trunc(extOrBitCast(x)) -> x
    if (match(Op, m_ExtOrBitCast(m_SILValue(Result)))) {
      // Truncated back to the same bits we started with.
      if (Result->getType(0) == AI->getType())
        return Result;
    }

    // trunc(tuple_extract(conversion(extOrBitCast(x))))) -> x
    if (match(Op, m_TupleExtractInst(
                   m_CheckedConversion(
                     m_ExtOrBitCast(m_SILValue(Result))), 0))) {
      // If the top bit of Result is known to be 0, then
      // it is safe to replace the whole patterb by original bits of x
      if (Result->getType(0) == AI->getType()) {
        if (auto signBit = computeSignBit(Result))
          if (!signBit.getValue())
            return Result;
      }
    }
    return SILValue();
  }
  }
  return SILValue();
}

/// Simplify an apply of the builtin canBeClass to either 0 or 1
/// when we can statically determine the result.
SILValue InstSimplifier::visitApplyInst(ApplyInst *AI) {
  auto *BFRI = dyn_cast<BuiltinFunctionRefInst>(AI->getCallee());
  if (BFRI)
    return simplifyBuiltin(AI, BFRI);
  return SILValue();
}

/// \brief Simplify arithmetic intrinsics with overflow and known identity
/// constants such as 0 and 1.
/// If this returns a value other than SILValue() then the instruction was
/// simplified to a value which doesn't overflow.  The overflow case is handled
/// in SILCombine.
static SILValue simplifyBinaryWithOverflow(ApplyInst *AI,
                                           llvm::Intrinsic::ID ID) {
  OperandValueArrayRef Args = AI->getArguments();
  assert(Args.size() >= 2);

  const SILValue &Op1 = Args[0];
  const SILValue &Op2 = Args[1];

  IntegerLiteralInst *IntOp1 = dyn_cast<IntegerLiteralInst>(Op1);
  IntegerLiteralInst *IntOp2 = dyn_cast<IntegerLiteralInst>(Op2);

  // If both ops are not constants, we cannot do anything.
  // FIXME: Add cases where we can do something, eg, (x - x) -> 0
  if (!IntOp1 && !IntOp2)
    return SILValue();

  // Calculate the result.

  switch (ID) {
  default: llvm_unreachable("Invalid case");
  case llvm::Intrinsic::sadd_with_overflow:
  case llvm::Intrinsic::uadd_with_overflow:
    // 0 + X -> X
    if (match(Op1, m_Zero()))
      return Op2;
    // X + 0 -> X
    if (match(Op2, m_Zero()))
      return Op1;
    return SILValue();
  case llvm::Intrinsic::ssub_with_overflow:
  case llvm::Intrinsic::usub_with_overflow:
    // X - 0 -> X
    if (match(Op2, m_Zero()))
      return Op1;
    return SILValue();
  case llvm::Intrinsic::smul_with_overflow:
  case llvm::Intrinsic::umul_with_overflow:
    // 0 * X -> 0
    if (match(Op1, m_Zero()))
      return Op1;
    // X * 0 -> 0
    if (match(Op2, m_Zero()))
      return Op2;
    // 1 * X -> X
    if (match(Op1, m_One()))
      return Op2;
    // X * 1 -> X
    if (match(Op2, m_One()))
      return Op1;
    return SILValue();
  }
  return SILValue();
}

/// Simplify operations that may overflow. All such operations return a tuple.
/// This function simplifies such operations, but returns only the first
/// element of a tuple. It looks strange at the first glance, but this
/// is OK, because this function is invoked only internally when processing
/// tuple_extract instructions. Therefore the result of this function
/// is used for simplifications like tuple_extract(x, 0) -> simplified(x)
SILValue InstSimplifier::simplifyOverflowBuiltin(ApplyInst *AI,
                                                 BuiltinFunctionRefInst *FR) {
  const IntrinsicInfo &Intrinsic = FR->getIntrinsicInfo();

  // If it's an llvm intrinsic, fold the intrinsic.
  switch (Intrinsic.ID) {
  default:
    return SILValue();
  case llvm::Intrinsic::not_intrinsic:
    break;
  case llvm::Intrinsic::sadd_with_overflow:
  case llvm::Intrinsic::uadd_with_overflow:
  case llvm::Intrinsic::ssub_with_overflow:
  case llvm::Intrinsic::usub_with_overflow:
  case llvm::Intrinsic::smul_with_overflow:
  case llvm::Intrinsic::umul_with_overflow:
    return simplifyBinaryWithOverflow(AI, Intrinsic.ID);
  }

  // Otherwise, it should be one of the builtin functions.
  const BuiltinInfo &Builtin = FR->getBuiltinInfo();

  switch (Builtin.ID) {
  default: break;

  case BuiltinValueKind::SUCheckedConversion:
  case BuiltinValueKind::USCheckedConversion: {
    OperandValueArrayRef Args = AI->getArguments();
    const SILValue &Op = Args[0];
    if (auto signBit = computeSignBit(Op))
      if (!signBit.getValue())
        return Op;
    SILValue Result;
    // CheckedConversion(ExtOrBitCast(x)) -> x
    if (match(AI, m_CheckedConversion(m_ExtOrBitCast(m_SILValue(Result)))))
      if (Result->getType(0) == AI->getType().getTupleElementType(0)) {
        assert (!computeSignBit(Result).getValue() && "Sign bit should be 0");
        return Result;
      }
    }
    break;

  case BuiltinValueKind::UToSCheckedTrunc:
  case BuiltinValueKind::UToUCheckedTrunc:
  case BuiltinValueKind::SToUCheckedTrunc:
  case BuiltinValueKind::SToSCheckedTrunc: {
    SILValue Result;
    // CheckedTrunc(Ext(x)) -> x
    if (match(AI, m_CheckedTrunc(m_Ext(m_SILValue(Result)))))
      if (Result->getType(0) == AI->getType().getTupleElementType(0))
        if (auto signBit = computeSignBit(Result))
          if (!signBit.getValue())
            return Result;
    }
    break;

      // Check and simplify binary arithmetic with overflow.
#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(id, name, _, attrs, overload) \
case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
      return simplifyBinaryWithOverflow(AI,
                          getLLVMIntrinsicIDForBuiltinWithOverflow(Builtin.ID));

  }
  return SILValue();
}

/// \brief Try to simplify the specified instruction, performing local
/// analysis of the operands of the instruction, without looking at its uses
/// (e.g. constant folding).  If a simpler result can be found, it is
/// returned, otherwise a null SILValue is returned.
///
SILValue swift::simplifyInstruction(SILInstruction *I) {
  return InstSimplifier().visit(I);
}
