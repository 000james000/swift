//===--- SerializeSIL.cpp - Read and write SIL ----------------------------===//
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

#include "SILFormat.h"
#include "Serialization.h"
#include "swift/AST/Module.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"

// This is a template-only header; eventually it should move to llvm/Support.
#include "clang/Basic/OnDiskHashTable.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::serialization;
using namespace swift::serialization::sil_block;

static unsigned toStableStringEncoding(StringLiteralInst::Encoding encoding) {
  switch (encoding) {
  case StringLiteralInst::Encoding::UTF8: return SIL_UTF8;
  case StringLiteralInst::Encoding::UTF16: return SIL_UTF16;
  }
  llvm_unreachable("bad string encoding");
}

static unsigned toStableSILLinkage(SILLinkage linkage) {
  switch (linkage) {
  case SILLinkage::Public: return SIL_LINKAGE_PUBLIC;
  case SILLinkage::Hidden: return SIL_LINKAGE_HIDDEN;
  case SILLinkage::Shared: return SIL_LINKAGE_SHARED;
  case SILLinkage::Private: return SIL_LINKAGE_PRIVATE;
  case SILLinkage::PublicExternal: return SIL_LINKAGE_PUBLIC_EXTERNAL;
  case SILLinkage::HiddenExternal: return SIL_LINKAGE_HIDDEN_EXTERNAL;
  }
  llvm_unreachable("bad linkage");
}

namespace {
    /// Used to serialize the on-disk func hash table.
  class FuncTableInfo {
  public:
    using key_type = Identifier;
    using key_type_ref = key_type;
    using data_type = DeclID;
    using data_type_ref = const data_type &;

    uint32_t ComputeHash(key_type_ref key) {
      assert(!key.empty());
      return llvm::HashString(key.str());
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      using namespace clang::io;
      uint32_t keyLength = key.str().size();
      uint32_t dataLength = sizeof(DeclID);
      Emit16(out, keyLength);
      Emit16(out, dataLength);
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      out << key.str();
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(sizeof(DeclID) <= 32, "DeclID too large");
      using namespace clang::io;
      Emit32(out, data);
    }
  };

  class SILSerializer {
    Serializer &S;
    ASTContext &Ctx;

    llvm::BitstreamWriter &Out;

    /// A reusable buffer for emitting records.
    SmallVector<uint64_t, 64> ScratchRecord;

    /// In case we want to encode the relative of InstID vs ValueID.
    ValueID InstID = 0;

    llvm::DenseMap<const ValueBase*, ValueID> ValueIDs;
    ValueID addValueRef(SILValue SV) {
      return addValueRef(SV.getDef());
    }
    ValueID addValueRef(const ValueBase *Val);

  public:
    using TableData = FuncTableInfo::data_type;
    using Table = llvm::DenseMap<FuncTableInfo::key_type, TableData>;
  private:
    /// FuncTable maps function name to an ID.
    Table FuncTable;
    std::vector<BitOffset> Funcs;
    /// The current function ID.
    DeclID FuncID = 1;

    /// Maps class name to a VTable ID.
    Table VTableList;
    /// Holds the list of VTables.
    std::vector<BitOffset> VTableOffset;
    DeclID VTableID = 1;

    /// Maps global variable name to an ID.
    Table GlobalVarList;
    /// Holds the list of SIL global variables.
    std::vector<BitOffset> GlobalVarOffset;
    DeclID GlobalVarID = 1;

    /// Maps witness table identifier to an ID.
    Table WitnessTableList;
    /// Holds the list of WitnessTables.
    std::vector<BitOffset> WitnessTableOffset;
    DeclID WitnessTableID = 1;

    /// Give each SILBasicBlock a unique ID.
    llvm::DenseMap<const SILBasicBlock*, unsigned> BasicBlockMap;

    /// Functions that we've emitted a reference to.
    llvm::SmallSet<const SILFunction *, 16> FuncsToDeclare;

    std::array<unsigned, 256> SILAbbrCodes;
    template <typename Layout>
    void registerSILAbbr() {
      using AbbrArrayTy = decltype(SILAbbrCodes);
      static_assert(Layout::Code <= std::tuple_size<AbbrArrayTy>::value,
                    "layout has invalid record code");
      SILAbbrCodes[Layout::Code] = Layout::emitAbbrev(Out);
      DEBUG(llvm::dbgs() << "SIL abbre code " << SILAbbrCodes[Layout::Code]
                         << " for layout " << Layout::Code << "\n");
    }

    bool ShouldSerializeAll;

    /// Helper function to update ListOfValues for MethodInst. Format:
    /// Attr, SILDeclRef (DeclID, Kind, uncurryLevel, IsObjC), and an operand.
    void handleMethodInst(const MethodInst *MI, SILValue operand,
                          SmallVectorImpl<ValueID> &ListOfValues);

    void writeSILFunction(const SILFunction &F, bool DeclOnly = false);
    void writeSILBasicBlock(const SILBasicBlock &BB);
    void writeSILInstruction(const SILInstruction &SI);
    void writeVTable(const SILVTable &vt);
    void writeGlobalVar(const SILGlobalVariable &g);
    void writeWitnessTable(const SILWitnessTable &wt);
    void writeTables();

  public:
    SILSerializer(Serializer &S, ASTContext &Ctx,
                  llvm::BitstreamWriter &Out, bool serializeAll)
      : S(S), Ctx(Ctx), Out(Out), ShouldSerializeAll(serializeAll) {}

    void writeModule(const SILModule *SILMod);
  };
} // end anonymous namespace

/// We enumerate all values in a SILFunction beforehand to correctly
/// handle forward references of values.
ValueID SILSerializer::addValueRef(const ValueBase *Val) {
  if (!Val || isa<SILUndef>(Val))
    return 0;

  ValueID id = ValueIDs[Val];
  assert(id != 0 && "We should have assigned a value ID to each value.");
  return id;
}

void SILSerializer::writeSILFunction(const SILFunction &F, bool DeclOnly) {
  DEBUG(llvm::dbgs() << "Serialize SIL:\n";
        F.dump());
  ValueIDs.clear();
  InstID = 0;

  FuncTable[Ctx.getIdentifier(F.getName())] = FuncID++;
  Funcs.push_back(Out.GetCurrentBitNo());
  unsigned abbrCode = SILAbbrCodes[SILFunctionLayout::Code];
  TypeID FnID = S.addTypeRef(F.getLoweredType().getSwiftType());
  DEBUG(llvm::dbgs() << "SILFunction @" << Out.GetCurrentBitNo() <<
        " abbrCode " << abbrCode << " FnID " << FnID << "\n");
  SILFunctionLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                toStableSILLinkage(F.getLinkage()),
                                (unsigned)F.isTransparent(),
                                FnID);
  if (DeclOnly)
    return;

  // Write the body's context archetypes, unless we don't actually have a body.
  if (!F.isExternalDeclaration()) {
    if (auto gp = F.getContextGenericParams()) {
      // If we have outer parameters, first serialize the decl context of their
      // parent.
      if (GenericParamList *outerParams = gp->getOuterParameters()) {
        DeclID D = S.addDeclRef(S.getGenericContext(outerParams));
        unsigned abbrCode = SILAbbrCodes[SILGenericOuterParamsLayout::Code];
        SILGenericOuterParamsLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                                D);
      }

      S.writeGenericParams(gp, SILAbbrCodes);
    }
  }

  // Assign a unique ID to each basic block of the SILFunction.
  unsigned BasicID = 0;
  BasicBlockMap.clear();
  // Assign a value ID to each SILInstruction that has value and to each basic
  // block argument.
  unsigned ValueID = 0;
  for (const SILBasicBlock &BB : F) {
    BasicBlockMap.insert(std::make_pair(&BB, BasicID++));

    for (auto I = BB.bbarg_begin(), E = BB.bbarg_end(); I != E; ++I)
      ValueIDs[static_cast<const ValueBase*>(*I)] = ++ValueID;

    for (const SILInstruction &SI : BB)
      if (SI.hasValue())
        ValueIDs[&SI] = ++ValueID;
  }

  for (const SILBasicBlock &BB : F)
    writeSILBasicBlock(BB);
}

void SILSerializer::writeSILBasicBlock(const SILBasicBlock &BB) {
  SmallVector<DeclID, 4> Args;
  for (auto I = BB.bbarg_begin(), E = BB.bbarg_end(); I != E; ++I) {
    SILArgument *SA = *I;
    DeclID tId = S.addTypeRef(SA->getType().getSwiftRValueType());
    DeclID vId = addValueRef(static_cast<const ValueBase*>(SA));
    Args.push_back(tId);
    Args.push_back((unsigned)SA->getType().getCategory());
    Args.push_back(vId);
  }

  unsigned abbrCode = SILAbbrCodes[SILBasicBlockLayout::Code];
  SILBasicBlockLayout::emitRecord(Out, ScratchRecord, abbrCode, Args);

  for (const SILInstruction &SI : BB)
    writeSILInstruction(SI);
}

/// Add SILDeclRef to ListOfValues, so we can reconstruct it at
/// deserialization.
static void handleSILDeclRef(Serializer &S, const SILDeclRef &Ref,
                             SmallVectorImpl<ValueID> &ListOfValues) {
  ListOfValues.push_back(S.addDeclRef(Ref.getDecl()));
  ListOfValues.push_back((unsigned)Ref.kind);
  ListOfValues.push_back((unsigned)Ref.getResilienceExpansion());
  ListOfValues.push_back(Ref.uncurryLevel);
  ListOfValues.push_back(Ref.isForeign);
}

/// Helper function to update ListOfValues for MethodInst. Format:
/// Attr, SILDeclRef (DeclID, Kind, uncurryLevel, IsObjC), and an operand.
void SILSerializer::handleMethodInst(const MethodInst *MI,
                                     SILValue operand,
                                     SmallVectorImpl<ValueID> &ListOfValues) {
  ListOfValues.push_back(MI->isVolatile());
  handleSILDeclRef(S, MI->getMember(), ListOfValues);
  ListOfValues.push_back(
      S.addTypeRef(operand.getType().getSwiftRValueType()));
  ListOfValues.push_back((unsigned)operand.getType().getCategory());
  ListOfValues.push_back(addValueRef(operand));
  ListOfValues.push_back(operand.getResultNumber());
}

void SILSerializer::writeSILInstruction(const SILInstruction &SI) {
  switch (SI.getKind()) {
  case ValueKind::SILArgument:
  case ValueKind::SILUndef:
    llvm_unreachable("not an instruction");

  case ValueKind::UnreachableInst: {
    unsigned abbrCode = SILAbbrCodes[SILInstNoOperandLayout::Code];
    SILInstNoOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                       (unsigned)SI.getKind());
    break;
  }
  case ValueKind::InitExistentialInst:
  case ValueKind::InitExistentialRefInst: {
    SILValue operand;
    SILType Ty;
    ArrayRef<ProtocolConformance*> conformances;
    SmallVector<ProtocolDecl *, 4> protocols;

    CanType existentialType;
    switch (SI.getKind()) {
    default: assert(0 && "out of sync with parent");
    case ValueKind::InitExistentialInst: {
      auto &IEI = cast<InitExistentialInst>(SI);
      operand = IEI.getOperand();
      Ty = IEI.getConcreteType();
      conformances = IEI.getConformances();
      existentialType = IEI.getOperand().getType().getSwiftRValueType();
      break;
    }
    case ValueKind::InitExistentialRefInst: {
      auto &IERI = cast<InitExistentialRefInst>(SI);
      operand = IERI.getOperand();
      Ty = IERI.getType();
      conformances = IERI.getConformances();
      existentialType = IERI.getType().getSwiftRValueType();
      break;
    }
    }

    // Retrieve the protocols.
    assert(existentialType->isExistentialType() && "Not an existential type?");
    existentialType->isExistentialType(protocols);

    unsigned abbrCode = SILAbbrCodes[SILInitExistentialLayout::Code];
    SILInitExistentialLayout::emitRecord(Out, ScratchRecord, abbrCode,
       (unsigned)SI.getKind(),
       S.addTypeRef(Ty.getSwiftRValueType()),
       (unsigned)Ty.getCategory(),
       S.addTypeRef(operand.getType().getSwiftRValueType()),
       (unsigned)operand.getType().getCategory(),
       addValueRef(operand),
       operand.getResultNumber(),
       conformances.size());

    for (unsigned i = 0, n = conformances.size(); i != n; ++i) {
      S.writeConformance(protocols[i], conformances[i], nullptr, SILAbbrCodes);
    }
    break;
  }
  case ValueKind::DeallocBoxInst:
  case ValueKind::ValueMetatypeInst:
  case ValueKind::ExistentialMetatypeInst:
  case ValueKind::AllocArrayInst: {
    SILValue operand;
    SILType Ty;
    switch (SI.getKind()) {
    default: assert(0 && "Out of sync with parent switch");
    case ValueKind::ValueMetatypeInst:
      operand = cast<ValueMetatypeInst>(&SI)->getOperand();
      Ty = cast<ValueMetatypeInst>(&SI)->getType();
      break;
    case ValueKind::ExistentialMetatypeInst:
      operand = cast<ExistentialMetatypeInst>(&SI)->getOperand();
      Ty = cast<ExistentialMetatypeInst>(&SI)->getType();
      break;
    case ValueKind::DeallocBoxInst:
      operand = cast<DeallocBoxInst>(&SI)->getOperand();
      Ty = cast<DeallocBoxInst>(&SI)->getElementType();
      break;
    case ValueKind::AllocArrayInst:
      operand = cast<AllocArrayInst>(&SI)->getNumElements();
      Ty = cast<AllocArrayInst>(&SI)->getElementType();
      break;
    }
    unsigned abbrCode = SILAbbrCodes[SILOneTypeOneOperandLayout::Code];
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(Ty.getSwiftRValueType()),
        (unsigned)Ty.getCategory(),
        S.addTypeRef(operand.getType().getSwiftRValueType()),
        (unsigned)operand.getType().getCategory(),
        addValueRef(operand),
        operand.getResultNumber());
    break;
  }
  case ValueKind::AllocBoxInst: {
    const AllocBoxInst *ABI = cast<AllocBoxInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeLayout::Code];
    SILOneTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                      (unsigned)SI.getKind(),
                      S.addTypeRef(ABI->getElementType().getSwiftRValueType()),
                      (unsigned)ABI->getElementType().getCategory());
    break;
  }
  case ValueKind::AllocRefInst: {
    const AllocRefInst *ARI = cast<AllocRefInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeValuesLayout::Code];
    ValueID Args[1] = { ARI->isObjC() };
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                       (unsigned)SI.getKind(),
                                       S.addTypeRef(
                                         ARI->getType().getSwiftRValueType()),
                                       (unsigned)ARI->getType().getCategory(),
                                       llvm::makeArrayRef(Args));
    break;
  }
  case ValueKind::AllocRefDynamicInst: {
    const AllocRefDynamicInst* ARD = cast<AllocRefDynamicInst>(&SI);
    unsigned flags = 0;
    if (ARD->isObjC())
      flags = 1;
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        (unsigned)SI.getKind(), flags,
        S.addTypeRef(ARD->getType().getSwiftRValueType()),
        (unsigned)ARD->getType().getCategory(),
        S.addTypeRef(ARD->getOperand().getType().getSwiftRValueType()),
        (unsigned)ARD->getOperand().getType().getCategory(),
        addValueRef(ARD->getOperand()),
        ARD->getOperand().getResultNumber());
    break;
  }
  case ValueKind::AllocStackInst: {
    const AllocStackInst *ASI = cast<AllocStackInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeLayout::Code];
    SILOneTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                      (unsigned)SI.getKind(),
                      S.addTypeRef(ASI->getElementType().getSwiftRValueType()),
                      (unsigned)ASI->getElementType().getCategory());
    break;
  }
  case ValueKind::ApplyInst: {
    // Format: attributes such as transparent and number of substitutions,
    // the callee's substituted and unsubstituted types, a value for
    // the callee and a list of values for the arguments. Each value in the list
    // is represented with 2 IDs: ValueID and ValueResultNumber. The record
    // is followed by the substitution list.
    const ApplyInst *AI = cast<ApplyInst>(&SI);
    SmallVector<ValueID, 4> Args;
    for (auto Arg: AI->getArguments()) {
      Args.push_back(addValueRef(Arg));
      Args.push_back(Arg.getResultNumber());
    }
    SILInstApplyLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstApplyLayout::Code], 0/*PartialApply*/,
        (unsigned)AI->isTransparent(),
        AI->getSubstitutions().size(),
        S.addTypeRef(AI->getCallee().getType().getSwiftRValueType()),
        S.addTypeRef(AI->getSubstCalleeType()),
        addValueRef(AI->getCallee()), AI->getCallee().getResultNumber(),
        Args);
    S.writeSubstitutions(AI->getSubstitutions(), SILAbbrCodes);
    break;
  }
  case ValueKind::PartialApplyInst: {
    const PartialApplyInst *PAI = cast<PartialApplyInst>(&SI);
        SmallVector<ValueID, 4> Args;
    for (auto Arg: PAI->getArguments()) {
      Args.push_back(addValueRef(Arg));
      Args.push_back(Arg.getResultNumber());
    }
    SILInstApplyLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstApplyLayout::Code], 1/*PartialApply*/,
        0 /*IsTransparent*/,
        PAI->getSubstitutions().size(),
        S.addTypeRef(PAI->getCallee().getType().getSwiftRValueType()),
        S.addTypeRef(PAI->getSubstCalleeType()),
        addValueRef(PAI->getCallee()), PAI->getCallee().getResultNumber(),
        Args);
    S.writeSubstitutions(PAI->getSubstitutions(), SILAbbrCodes);
    break;
  }
  case ValueKind::BuiltinFunctionRefInst: {
    // Format: FuncDecl and type. Use SILOneOperandLayout.
    const BuiltinFunctionRefInst *BFR = cast<BuiltinFunctionRefInst>(&SI);
    SILOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(BFR->getType().getSwiftRValueType()),
        (unsigned)BFR->getType().getCategory(),
        S.addIdentifierRef(BFR->getName()), 0);
    break;
  }
  case ValueKind::GlobalAddrInst: {
    // Format: VarDecl and type. Use SILOneOperandLayout.
    const GlobalAddrInst *GAI = cast<GlobalAddrInst>(&SI);
    SILOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(GAI->getType().getSwiftRValueType()),
        (unsigned)GAI->getType().getCategory(),
        S.addDeclRef(GAI->getGlobal()), 0);
    break;
  }
  case ValueKind::SILGlobalAddrInst: {
    // Format: Name and type. Use SILOneOperandLayout.
    const SILGlobalAddrInst *GAI = cast<SILGlobalAddrInst>(&SI);
    SILOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(GAI->getType().getSwiftRValueType()),
        (unsigned)GAI->getType().getCategory(),
        S.addIdentifierRef(
            Ctx.getIdentifier(GAI->getReferencedGlobal()->getName())),
        0);
    break;
  }
  case ValueKind::BranchInst: {
    // Format: destination basic block ID, a list of arguments. Use
    // SILOneTypeValuesLayout.
    const BranchInst *BrI = cast<BranchInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : BrI->getArgs()) {
      ListOfValues.push_back(S.addTypeRef(Elt.getType().getSwiftRValueType()));
      ListOfValues.push_back((unsigned)Elt.getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
      ListOfValues.push_back(Elt.getResultNumber());
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        BasicBlockMap[BrI->getDestBB()], 0, ListOfValues);
    break;
  }
  case ValueKind::CondBranchInst: {
    // Format: condition, true basic block ID, a list of arguments, false basic
    // block ID, a list of arguments. Use SILOneTypeValuesLayout: the type is
    // for condition, the list has value for condition, true basic block ID,
    // false basic block ID, number of true arguments, and a list of true|false
    // arguments.
    const CondBranchInst *CBI = cast<CondBranchInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    ListOfValues.push_back(addValueRef(CBI->getCondition()));
    ListOfValues.push_back(CBI->getCondition().getResultNumber());
    ListOfValues.push_back(BasicBlockMap[CBI->getTrueBB()]);
    ListOfValues.push_back(BasicBlockMap[CBI->getFalseBB()]);
    ListOfValues.push_back(CBI->getTrueArgs().size());
    for (auto Elt : CBI->getTrueArgs()) {
      ListOfValues.push_back(S.addTypeRef(Elt.getType().getSwiftRValueType()));
      ListOfValues.push_back((unsigned)Elt.getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
      ListOfValues.push_back(Elt.getResultNumber());
    }
    for (auto Elt : CBI->getFalseArgs()) {
      ListOfValues.push_back(S.addTypeRef(Elt.getType().getSwiftRValueType()));
      ListOfValues.push_back((unsigned)Elt.getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
      ListOfValues.push_back(Elt.getResultNumber());
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(CBI->getCondition().getType().getSwiftRValueType()),
        (unsigned)CBI->getCondition().getType().getCategory(),
        ListOfValues);
    break;
  }
  case ValueKind::SwitchEnumInst:
  case ValueKind::SwitchEnumAddrInst: {
    // Format: condition, a list of cases (EnumElementDecl + Basic Block ID),
    // default basic block ID. Use SILOneTypeValuesLayout: the type is
    // for condition, the list has value for condition, hasDefault, default
    // basic block ID, a list of (DeclID, BasicBlock ID).
    const SwitchEnumInstBase *SOI = cast<SwitchEnumInstBase>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    ListOfValues.push_back(addValueRef(SOI->getOperand()));
    ListOfValues.push_back(SOI->getOperand().getResultNumber());
    ListOfValues.push_back((unsigned)SOI->hasDefault());
    if (SOI->hasDefault())
      ListOfValues.push_back(BasicBlockMap[SOI->getDefaultBB()]);
    else
      ListOfValues.push_back(0);

    for (unsigned i = 0, e = SOI->getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILBasicBlock *dest;
      std::tie(elt, dest) = SOI->getCase(i);
      ListOfValues.push_back(S.addDeclRef(elt));
      ListOfValues.push_back(BasicBlockMap[dest]);
    }
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(SOI->getOperand().getType().getSwiftRValueType()),
        (unsigned)SOI->getOperand().getType().getCategory(),
        ListOfValues);
    break;
  }
  case ValueKind::SwitchIntInst: {
    // Format: condition, a list of cases (APInt + Basic Block ID),
    // default basic block ID. Use SILOneTypeValuesLayout: the type is
    // for condition, the list contains value for condition, hasDefault, default
    // basic block ID, a list of (APInt(Identifier ID), BasicBlock ID).
    const SwitchIntInst *SII = cast<SwitchIntInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    ListOfValues.push_back(addValueRef(SII->getOperand()));
    ListOfValues.push_back(SII->getOperand().getResultNumber());
    ListOfValues.push_back((unsigned)SII->hasDefault());
    if (SII->hasDefault())
      ListOfValues.push_back(BasicBlockMap[SII->getDefaultBB()]);
    else
      ListOfValues.push_back(0);

    for (unsigned i = 0, e = SII->getNumCases(); i < e; ++i) {
      APInt value;
      SILBasicBlock *dest;
      std::tie(value, dest) = SII->getCase(i);
      StringRef Str = value.toString(10, true);
      ListOfValues.push_back(S.addIdentifierRef(Ctx.getIdentifier(Str)));
      ListOfValues.push_back(BasicBlockMap[dest]);
    }
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(SII->getOperand().getType().getSwiftRValueType()),
        (unsigned)SII->getOperand().getType().getCategory(),
        ListOfValues);
    break;
  }
  case ValueKind::CondFailInst:
  case ValueKind::CopyValueInst:
  case ValueKind::DestroyValueInst:
  case ValueKind::DeallocStackInst:
  case ValueKind::DeallocRefInst:
  case ValueKind::DeinitExistentialInst:
  case ValueKind::DestroyAddrInst:
  case ValueKind::IsNonnullInst:
  case ValueKind::LoadInst:
  case ValueKind::LoadWeakInst:
  case ValueKind::MarkUninitializedInst:
  case ValueKind::StrongReleaseInst:
  case ValueKind::StrongRetainInst:
  case ValueKind::StrongRetainAutoreleasedInst:
  case ValueKind::AutoreleaseReturnInst:
  case ValueKind::StrongRetainUnownedInst:
  case ValueKind::UnownedRetainInst:
  case ValueKind::UnownedReleaseInst:
  case ValueKind::ReturnInst:
  case ValueKind::DebugValueInst:
  case ValueKind::DebugValueAddrInst: {
    unsigned Attr = 0;
    if (auto *LWI = dyn_cast<LoadWeakInst>(&SI))
      Attr = LWI->isTake();
    else if (auto *MUI = dyn_cast<MarkUninitializedInst>(&SI))
      Attr = (unsigned)MUI->getKind();
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                 (unsigned)SI.getKind(), Attr,
                 S.addTypeRef(SI.getOperand(0).getType().getSwiftRValueType()),
                 (unsigned)SI.getOperand(0).getType().getCategory(),
                 addValueRef(SI.getOperand(0)),
                 SI.getOperand(0).getResultNumber());
    break;
  }
  case ValueKind::FunctionRefInst: {
    // Use SILOneOperandLayout to specify the function type and the function
    // name (IdentifierID).
    const FunctionRefInst *FRI = cast<FunctionRefInst>(&SI);
    SILFunction *ReferencedFunction = FRI->getReferencedFunction();
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(FRI->getType().getSwiftRValueType()),
        (unsigned)FRI->getType().getCategory(),
        S.addIdentifierRef(Ctx.getIdentifier(ReferencedFunction->getName())),
        0);

    // Make sure we declare the referenced function.
    FuncsToDeclare.insert(ReferencedFunction);
    break;
  }
  case ValueKind::IndexAddrInst:
  case ValueKind::IndexRawPointerInst:
  case ValueKind::UpcastExistentialInst: {
    SILValue operand, operand2;
    unsigned Attr = 0;
    if (SI.getKind() == ValueKind::IndexRawPointerInst) {
      const IndexRawPointerInst *IRP = cast<IndexRawPointerInst>(&SI);
      operand = IRP->getBase();
      operand2 = IRP->getIndex();
    } else if (SI.getKind() == ValueKind::UpcastExistentialInst) {
      Attr = cast<UpcastExistentialInst>(&SI)->isTakeOfSrc();
      operand = cast<UpcastExistentialInst>(&SI)->getSrcExistential();
      operand2 = cast<UpcastExistentialInst>(&SI)->getDestExistential();
    } else {
      const IndexAddrInst *IAI = cast<IndexAddrInst>(&SI);
      operand = IAI->getBase();
      operand2 = IAI->getIndex();
    }
    SILTwoOperandsLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILTwoOperandsLayout::Code],
        (unsigned)SI.getKind(), Attr,
        S.addTypeRef(operand.getType().getSwiftRValueType()),
        (unsigned)operand.getType().getCategory(),
        addValueRef(operand), operand.getResultNumber(),
        S.addTypeRef(operand2.getType().getSwiftRValueType()),
        (unsigned)operand2.getType().getCategory(),
        addValueRef(operand2), operand2.getResultNumber());
    break;
  }
  case ValueKind::StringLiteralInst: {
    auto SLI = cast<StringLiteralInst>(&SI);
    StringRef Str = SLI->getValue();
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    unsigned encoding = toStableStringEncoding(SLI->getEncoding());
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                    (unsigned)SI.getKind(), encoding, 0, 0,
                                    S.addIdentifierRef(Ctx.getIdentifier(Str)),
                                    0);
    break;
  }
  case ValueKind::FloatLiteralInst:
  case ValueKind::IntegerLiteralInst: {
    // Use SILOneOperandLayout to specify the type and the literal.
    std::string Str;
    SILType Ty;
    switch (SI.getKind()) {
    default: assert(0 && "Out of sync with parent switch");
    case ValueKind::IntegerLiteralInst:
      Str = cast<IntegerLiteralInst>(&SI)->getValue().toString(10, true);
      Ty = cast<IntegerLiteralInst>(&SI)->getType();
      break;
    case ValueKind::FloatLiteralInst:
      Str = cast<FloatLiteralInst>(&SI)->getBits().toString(16,
                                                            /*Signed*/false);
      Ty = cast<FloatLiteralInst>(&SI)->getType();
      break;
    }
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(Ty.getSwiftRValueType()),
        (unsigned)Ty.getCategory(),
        S.addIdentifierRef(Ctx.getIdentifier(Str)),
        0);
    break;
  }
  case ValueKind::MarkFunctionEscapeInst: {
    // Format: a list of typed values. A typed value is expressed by 4 IDs:
    // TypeID, TypeCategory, ValueID, ValueResultNumber.
    const MarkFunctionEscapeInst *MFE = cast<MarkFunctionEscapeInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : MFE->getElements()) {
      ListOfValues.push_back(S.addTypeRef(Elt.getType().getSwiftRValueType()));
      ListOfValues.push_back((unsigned)Elt.getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
      ListOfValues.push_back(Elt.getResultNumber());
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(), 0, 0, ListOfValues);
    break;
  }
  case ValueKind::MetatypeInst: {
    const MetatypeInst *MI = cast<MetatypeInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeLayout::Code];
    SILOneTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                      (unsigned)SI.getKind(),
                      S.addTypeRef(MI->getType().getSwiftRValueType()),
                      (unsigned)MI->getType().getCategory());
    break;
  }
  case ValueKind::ProjectExistentialInst: {
    const ProjectExistentialInst *PEI = cast<ProjectExistentialInst>(&SI);
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(PEI->getType().getSwiftRValueType()),
        (unsigned)PEI->getType().getCategory(),
        S.addTypeRef(PEI->getOperand().getType().getSwiftRValueType()),
        (unsigned)PEI->getOperand().getType().getCategory(),
        addValueRef(PEI->getOperand()),
        PEI->getOperand().getResultNumber());
    break;
  }
  case ValueKind::ProjectExistentialRefInst: {
    const ProjectExistentialRefInst *PEI = cast<ProjectExistentialRefInst>(&SI);
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(PEI->getType().getSwiftRValueType()),
        (unsigned)PEI->getType().getCategory(),
        S.addTypeRef(PEI->getOperand().getType().getSwiftRValueType()),
        (unsigned)PEI->getOperand().getType().getCategory(),
        addValueRef(PEI->getOperand()),
        PEI->getOperand().getResultNumber());
    break;
  }
  case ValueKind::OpenExistentialInst: {
    const OpenExistentialInst *OEI = cast<OpenExistentialInst>(&SI);
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(OEI->getType().getSwiftRValueType()),
        (unsigned)OEI->getType().getCategory(),
        S.addTypeRef(OEI->getOperand().getType().getSwiftRValueType()),
        (unsigned)OEI->getOperand().getType().getCategory(),
        addValueRef(OEI->getOperand()),
        OEI->getOperand().getResultNumber());
    break;
  }
  case ValueKind::OpenExistentialRefInst: {
    const OpenExistentialRefInst *OEI = cast<OpenExistentialRefInst>(&SI);
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(OEI->getType().getSwiftRValueType()),
        (unsigned)OEI->getType().getCategory(),
        S.addTypeRef(OEI->getOperand().getType().getSwiftRValueType()),
        (unsigned)OEI->getOperand().getType().getCategory(),
        addValueRef(OEI->getOperand()),
        OEI->getOperand().getResultNumber());
    break;
  }
  // Conversion instructions.
  case ValueKind::RefToObjectPointerInst:
  case ValueKind::UpcastInst:
  case ValueKind::AddressToPointerInst:
  case ValueKind::PointerToAddressInst:
  case ValueKind::ObjectPointerToRefInst:
  case ValueKind::RefToRawPointerInst:
  case ValueKind::RawPointerToRefInst:
  case ValueKind::RefToUnownedInst:
  case ValueKind::UnownedToRefInst:
  case ValueKind::ThinToThickFunctionInst:
  case ValueKind::ThickToObjCMetatypeInst:
  case ValueKind::ObjCToThickMetatypeInst:
  case ValueKind::BridgeToBlockInst:
  case ValueKind::ConvertFunctionInst:
  case ValueKind::UpcastExistentialRefInst: {
    SILValue operand;
    SILType Ty;
    switch (SI.getKind()) {
    default: assert(0 && "Out of sync with parent switch");
    case ValueKind::RefToObjectPointerInst:
      operand = cast<RefToObjectPointerInst>(&SI)->getOperand();
      Ty = cast<RefToObjectPointerInst>(&SI)->getType();
      break;
    case ValueKind::UpcastInst:
      operand = cast<UpcastInst>(&SI)->getOperand();
      Ty = cast<UpcastInst>(&SI)->getType();
      break;
    case ValueKind::AddressToPointerInst:
      operand = cast<AddressToPointerInst>(&SI)->getOperand();
      Ty = cast<AddressToPointerInst>(&SI)->getType();
      break;
    case ValueKind::PointerToAddressInst:
      operand = cast<PointerToAddressInst>(&SI)->getOperand();
      Ty = cast<PointerToAddressInst>(&SI)->getType();
      break;
    case ValueKind::ObjectPointerToRefInst:
      operand = cast<ObjectPointerToRefInst>(&SI)->getOperand();
      Ty = cast<ObjectPointerToRefInst>(&SI)->getType();
      break;
    case ValueKind::RefToRawPointerInst:
      operand = cast<RefToRawPointerInst>(&SI)->getOperand();
      Ty = cast<RefToRawPointerInst>(&SI)->getType();
      break;
    case ValueKind::RawPointerToRefInst:
      operand = cast<RawPointerToRefInst>(&SI)->getOperand();
      Ty = cast<RawPointerToRefInst>(&SI)->getType();
      break;
    case ValueKind::RefToUnownedInst:
      operand = cast<RefToUnownedInst>(&SI)->getOperand();
      Ty = cast<RefToUnownedInst>(&SI)->getType();
      break;
    case ValueKind::UnownedToRefInst:
      operand = cast<UnownedToRefInst>(&SI)->getOperand();
      Ty = cast<UnownedToRefInst>(&SI)->getType();
      break;
    case ValueKind::ThinToThickFunctionInst:
      operand = cast<ThinToThickFunctionInst>(&SI)->getOperand();
      Ty = cast<ThinToThickFunctionInst>(&SI)->getType();
      break;
    case ValueKind::ThickToObjCMetatypeInst:
      operand = cast<ThickToObjCMetatypeInst>(&SI)->getOperand();
      Ty = cast<ThickToObjCMetatypeInst>(&SI)->getType();
      break;
    case ValueKind::ObjCToThickMetatypeInst:
      operand = cast<ObjCToThickMetatypeInst>(&SI)->getOperand();
      Ty = cast<ObjCToThickMetatypeInst>(&SI)->getType();
      break;
    case ValueKind::BridgeToBlockInst:
      operand = cast<BridgeToBlockInst>(&SI)->getOperand();
      Ty = cast<BridgeToBlockInst>(&SI)->getType();
      break;
    case ValueKind::ConvertFunctionInst:
      operand = cast<ConvertFunctionInst>(&SI)->getOperand();
      Ty = cast<ConvertFunctionInst>(&SI)->getType();
      break;
    case ValueKind::UpcastExistentialRefInst:
      operand = cast<UpcastExistentialRefInst>(&SI)->getOperand();
      Ty = cast<UpcastExistentialRefInst>(&SI)->getType();
      break;
    }
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(Ty.getSwiftRValueType()),
        (unsigned)Ty.getCategory(),
        S.addTypeRef(operand.getType().getSwiftRValueType()),
        (unsigned)operand.getType().getCategory(),
        addValueRef(operand), operand.getResultNumber());
    break;
  }
  // Checked Conversion instructions.
  case ValueKind::UnconditionalCheckedCastInst: {
    auto CI = cast<UnconditionalCheckedCastInst>(&SI);
    SILInstCastLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstCastLayout::Code],
        (unsigned)SI.getKind(), (unsigned)CI->getCastKind(),
        S.addTypeRef(CI->getType().getSwiftRValueType()),
        (unsigned)CI->getType().getCategory(),
        S.addTypeRef(CI->getOperand().getType().getSwiftRValueType()),
        (unsigned)CI->getOperand().getType().getCategory(),
        addValueRef(CI->getOperand()), CI->getOperand().getResultNumber());
    break;
  }

  case ValueKind::AssignInst:
  case ValueKind::CopyAddrInst:
  case ValueKind::StoreInst:
  case ValueKind::StoreWeakInst: {
    SILValue operand, value;
    unsigned Attr = 0;
    if (SI.getKind() == ValueKind::StoreWeakInst) {
      Attr = cast<StoreWeakInst>(&SI)->isInitializationOfDest();
      operand = cast<StoreWeakInst>(&SI)->getDest();
      value = cast<StoreWeakInst>(&SI)->getSrc();
    } else if (SI.getKind() == ValueKind::StoreInst) {
      operand = cast<StoreInst>(&SI)->getDest();
      value = cast<StoreInst>(&SI)->getSrc();
    } else if (SI.getKind() == ValueKind::AssignInst) {
      operand = cast<AssignInst>(&SI)->getDest();
      value = cast<AssignInst>(&SI)->getSrc();
    } else if (SI.getKind() == ValueKind::CopyAddrInst) {
      const CopyAddrInst *CAI = cast<CopyAddrInst>(&SI);
      Attr = (CAI->isInitializationOfDest() << 1) | CAI->isTakeOfSrc();
      operand = cast<CopyAddrInst>(&SI)->getDest();
      value = cast<CopyAddrInst>(&SI)->getSrc();
    } else
      llvm_unreachable("switch out of sync");

    unsigned abbrCode = SILAbbrCodes[SILOneValueOneOperandLayout::Code];
    SILOneValueOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                  (unsigned)SI.getKind(), Attr, addValueRef(value),
                  value.getResultNumber(),
                  S.addTypeRef(operand.getType().getSwiftRValueType()),
                  (unsigned)operand.getType().getCategory(),
                  addValueRef(operand),
                  operand.getResultNumber());
    break;
  }
  case ValueKind::RefElementAddrInst:
  case ValueKind::StructElementAddrInst:
  case ValueKind::StructExtractInst:
  case ValueKind::InitEnumDataAddrInst:
  case ValueKind::TakeEnumDataAddrInst:
  case ValueKind::InjectEnumAddrInst: {
    // Has a typed valueref and a field decl. We use SILOneValueOneOperandLayout
    // where the field decl is streamed as a ValueID.
    SILValue operand;
    Decl *tDecl;
    switch (SI.getKind()) {
    default: assert(0 && "Out of sync with parent switch");
    case ValueKind::RefElementAddrInst:
      operand = cast<RefElementAddrInst>(&SI)->getOperand();
      tDecl = cast<RefElementAddrInst>(&SI)->getField();
      break;
    case ValueKind::StructElementAddrInst:
      operand = cast<StructElementAddrInst>(&SI)->getOperand();
      tDecl = cast<StructElementAddrInst>(&SI)->getField();
      break;
    case ValueKind::StructExtractInst:
      operand = cast<StructExtractInst>(&SI)->getOperand();
      tDecl = cast<StructExtractInst>(&SI)->getField();
      break;
    case ValueKind::InitEnumDataAddrInst:
      operand = cast<InitEnumDataAddrInst>(&SI)->getOperand();
      tDecl = cast<InitEnumDataAddrInst>(&SI)->getElement();
      break;
    case ValueKind::TakeEnumDataAddrInst:
      operand = cast<TakeEnumDataAddrInst>(&SI)->getOperand();
      tDecl = cast<TakeEnumDataAddrInst>(&SI)->getElement();
      break;
    case ValueKind::InjectEnumAddrInst:
      operand = cast<InjectEnumAddrInst>(&SI)->getOperand();
      tDecl = cast<InjectEnumAddrInst>(&SI)->getElement();
      break;
    }
    SILOneValueOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneValueOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0, S.addDeclRef(tDecl), 0,
        S.addTypeRef(operand.getType().getSwiftRValueType()),
        (unsigned)operand.getType().getCategory(),
        addValueRef(operand), operand.getResultNumber());
    break;
  }
  case ValueKind::StructInst: {
    // Format: a type followed by a list of typed values. A typed value is
    // expressed by 4 IDs: TypeID, TypeCategory, ValueID, ValueResultNumber.
    const StructInst *StrI = cast<StructInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : StrI->getElements()) {
      ListOfValues.push_back(S.addTypeRef(Elt.getType().getSwiftRValueType()));
      ListOfValues.push_back((unsigned)Elt.getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
      ListOfValues.push_back(Elt.getResultNumber());
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(StrI->getType().getSwiftRValueType()),
        (unsigned)StrI->getType().getCategory(), ListOfValues);
    break;
  }
  case ValueKind::TupleElementAddrInst:
  case ValueKind::TupleExtractInst: {
    SILValue operand;
    unsigned FieldNo;
    switch (SI.getKind()) {
    default: assert(0 && "Out of sync with parent switch");
    case ValueKind::TupleElementAddrInst:
      operand = cast<TupleElementAddrInst>(&SI)->getOperand();
      FieldNo = cast<TupleElementAddrInst>(&SI)->getFieldNo();
      break;
    case ValueKind::TupleExtractInst:
      operand = cast<TupleExtractInst>(&SI)->getOperand();
      FieldNo = cast<TupleExtractInst>(&SI)->getFieldNo();
      break;
    }

    // Use OneTypeOneOperand layout where the field number is stored in TypeID.
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        FieldNo, 0,
        S.addTypeRef(operand.getType().getSwiftRValueType()),
        (unsigned)operand.getType().getCategory(),
        addValueRef(operand), operand.getResultNumber());
    break;
  }
  case ValueKind::TupleInst: {
    // Format: a type followed by a list of values. A value is expressed by
    // 2 IDs: ValueID, ValueResultNumber.
    const TupleInst *TI = cast<TupleInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : TI->getElements()) {
      ListOfValues.push_back(addValueRef(Elt));
      ListOfValues.push_back(Elt.getResultNumber());
    }

    unsigned abbrCode = SILAbbrCodes[SILOneTypeValuesLayout::Code];
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(),
        S.addTypeRef(TI->getType().getSwiftRValueType()),
        (unsigned)TI->getType().getCategory(),
        ListOfValues);
    break;
  }
  case ValueKind::EnumInst: {
    // Format: a type, an operand and a decl ID. Use SILTwoOperandsLayout: type,
    // (DeclID + hasOperand), and an operand.
    const EnumInst *UI = cast<EnumInst>(&SI);
    TypeID OperandTy = UI->hasOperand() ?
      S.addTypeRef(UI->getOperand().getType().getSwiftRValueType()) : (TypeID)0;
    unsigned OperandTyCategory = UI->hasOperand() ?
        (unsigned)UI->getOperand().getType().getCategory() : 0;
    SILTwoOperandsLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILTwoOperandsLayout::Code], (unsigned)SI.getKind(), 0,
        S.addTypeRef(UI->getType().getSwiftRValueType()),
        (unsigned)UI->getType().getCategory(),
        S.addDeclRef(UI->getElement()), UI->hasOperand(),
        OperandTy, OperandTyCategory,
        UI->hasOperand() ? addValueRef(UI->getOperand()) : (ValueID)0,
        UI->hasOperand() ? UI->getOperand().getResultNumber() : 0);
    break;
  }
  case ValueKind::WitnessMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel, IsObjC), and a type.
    const WitnessMethodInst *AMI = cast<WitnessMethodInst>(&SI);
    SILType Ty = AMI->getLookupType();
    SILType Ty2 = AMI->getType(0);

    SmallVector<ValueID, 7> ListOfValues;
    ListOfValues.push_back(AMI->isVolatile());
    handleSILDeclRef(S, AMI->getMember(), ListOfValues);
    ListOfValues.push_back(S.addTypeRef(Ty2.getSwiftRValueType()));
    ListOfValues.push_back((unsigned)Ty2.getCategory());

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getSwiftRValueType()),
        (unsigned)Ty.getCategory(), ListOfValues);

    if (AMI->getConformance())
      S.writeConformance(
               cast<ProtocolDecl>(AMI->getMember().getDecl()->getDeclContext()),
               AMI->getConformance(),
               nullptr,
               SILAbbrCodes);

    break;
  }
  case ValueKind::ProtocolMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel, IsObjC),
    // and an operand.
    const ProtocolMethodInst *PMI = cast<ProtocolMethodInst>(&SI);
    SILType Ty = PMI->getType();
    SmallVector<ValueID, 9> ListOfValues;
    handleMethodInst(PMI, PMI->getOperand(), ListOfValues);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getSwiftRValueType()),
        (unsigned)Ty.getCategory(), ListOfValues);
    break;
  }
  case ValueKind::ClassMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel, IsObjC),
    // and an operand.
    const ClassMethodInst *CMI = cast<ClassMethodInst>(&SI);
    SILType Ty = CMI->getType();
    SmallVector<ValueID, 9> ListOfValues;
    handleMethodInst(CMI, CMI->getOperand(), ListOfValues);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getSwiftRValueType()),
        (unsigned)Ty.getCategory(), ListOfValues);
    break;
  }
  case ValueKind::SuperMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel, IsObjC),
    // and an operand.
    const SuperMethodInst *SMI = cast<SuperMethodInst>(&SI);
    SILType Ty = SMI->getType();
    SmallVector<ValueID, 9> ListOfValues;
    handleMethodInst(SMI, SMI->getOperand(), ListOfValues);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getSwiftRValueType()),
        (unsigned)Ty.getCategory(), ListOfValues);
    break;
  }
  case ValueKind::DynamicMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel, IsObjC),
    // and an operand.
    const DynamicMethodInst *DMI = cast<DynamicMethodInst>(&SI);
    SILType Ty = DMI->getType();
    SmallVector<ValueID, 9> ListOfValues;
    handleMethodInst(DMI, DMI->getOperand(), ListOfValues);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getSwiftRValueType()),
        (unsigned)Ty.getCategory(), ListOfValues);
    break;
  }
  case ValueKind::DynamicMethodBranchInst: {
    // Format: a typed value, a SILDeclRef, a BasicBlock ID for method,
    // a BasicBlock ID for no method. Use SILOneTypeValuesLayout.
    const DynamicMethodBranchInst *DMB = cast<DynamicMethodBranchInst>(&SI);
    SmallVector<ValueID, 8> ListOfValues;
    ListOfValues.push_back(addValueRef(DMB->getOperand()));
    ListOfValues.push_back(DMB->getOperand().getResultNumber());
    handleSILDeclRef(S, DMB->getMember(), ListOfValues);
    ListOfValues.push_back(BasicBlockMap[DMB->getHasMethodBB()]);
    ListOfValues.push_back(BasicBlockMap[DMB->getNoMethodBB()]);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(DMB->getOperand().getType().getSwiftRValueType()),
        (unsigned)DMB->getOperand().getType().getCategory(), ListOfValues);
    break;
  }
  case ValueKind::CheckedCastBranchInst: {
    // Format: the cast kind, a typed value, a BasicBlock ID for success,
    // a BasicBlock ID for failure. Uses SILOneTypeValuesLayout.
    const CheckedCastBranchInst *CBI = cast<CheckedCastBranchInst>(&SI);
    SmallVector<ValueID, 8> ListOfValues;
    ListOfValues.push_back((unsigned)CBI->getCastKind());
    ListOfValues.push_back(addValueRef(CBI->getOperand()));
    ListOfValues.push_back(CBI->getOperand().getResultNumber());
    ListOfValues.push_back(
               S.addTypeRef(CBI->getOperand().getType().getSwiftRValueType()));
    ListOfValues.push_back((unsigned)CBI->getOperand().getType().getCategory());
    ListOfValues.push_back(BasicBlockMap[CBI->getSuccessBB()]);
    ListOfValues.push_back(BasicBlockMap[CBI->getFailureBB()]);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
             SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
             S.addTypeRef(CBI->getCastType().getSwiftRValueType()),
             (unsigned)CBI->getCastType().getCategory(),
             ListOfValues);
    break;
  }
  }
  // Non-void values get registered in the value table.
  if (SI.hasValue()) {
    addValueRef(&SI);
    ++InstID;
  }
}

/// Depending on the RecordKind, we write the SILFunction table, the global
/// variable table, the table for SILVTable, or the table for SILWitnessTable.
static void writeTable(const sil_index_block::ListLayout &List,
                       sil_index_block::RecordKind kind,
                       const SILSerializer::Table &table) {
  assert((kind == sil_index_block::SIL_FUNC_NAMES ||
          kind == sil_index_block::SIL_VTABLE_NAMES ||
          kind == sil_index_block::SIL_GLOBALVAR_NAMES ||
          kind == sil_index_block::SIL_WITNESSTABLE_NAMES) &&
         "SIL function table, global, vtable and witness table are supported");
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    clang::OnDiskChainedHashTableGenerator<FuncTableInfo> generator;
    for (auto &entry : table)
      generator.insert(entry.first, entry.second);

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0.
    clang::io::Emit32(blobStream, 0);
    tableOffset = generator.Emit(blobStream);
  }
  SmallVector<uint64_t, 8> scratch;
  List.emit(scratch, kind, tableOffset, hashTableBlob);
}

void SILSerializer::writeTables() {
  sil_index_block::ListLayout List(Out);
  sil_index_block::OffsetLayout Offset(Out);
  if (!FuncTable.empty()) {
    writeTable(List, sil_index_block::SIL_FUNC_NAMES, FuncTable);
    Offset.emit(ScratchRecord, sil_index_block::SIL_FUNC_OFFSETS, Funcs);
  }

  if (!VTableList.empty()) {
    writeTable(List, sil_index_block::SIL_VTABLE_NAMES, VTableList);
    Offset.emit(ScratchRecord, sil_index_block::SIL_VTABLE_OFFSETS,
                VTableOffset);
  }

  if (!GlobalVarList.empty()) {
    writeTable(List, sil_index_block::SIL_GLOBALVAR_NAMES, GlobalVarList);
    Offset.emit(ScratchRecord, sil_index_block::SIL_GLOBALVAR_OFFSETS,
                GlobalVarOffset);
  }

  if (!WitnessTableList.empty()) {
    writeTable(List, sil_index_block::SIL_WITNESSTABLE_NAMES, WitnessTableList);
    Offset.emit(ScratchRecord, sil_index_block::SIL_WITNESSTABLE_OFFSETS,
                WitnessTableOffset);
  }
}

void SILSerializer::writeGlobalVar(const SILGlobalVariable &g) {
  GlobalVarList[Ctx.getIdentifier(g.getName())] = GlobalVarID++;
  GlobalVarOffset.push_back(Out.GetCurrentBitNo());
  TypeID TyID = S.addTypeRef(g.getLoweredType().getSwiftType());
  GlobalVarLayout::emitRecord(Out, ScratchRecord,
                              SILAbbrCodes[GlobalVarLayout::Code],
                              toStableSILLinkage(g.getLinkage()),
                              TyID);
}

void SILSerializer::writeVTable(const SILVTable &vt) {
  VTableList[vt.getClass()->getName()] = VTableID++;
  VTableOffset.push_back(Out.GetCurrentBitNo());
  VTableLayout::emitRecord(Out, ScratchRecord, SILAbbrCodes[VTableLayout::Code],
                           S.addDeclRef(vt.getClass()));

  for (auto &entry : vt.getEntries()) {
    SmallVector<ValueID, 4> ListOfValues;
    handleSILDeclRef(S, entry.first, ListOfValues);
    FuncsToDeclare.insert(entry.second);
    // Each entry is a pair of SILDeclRef and SILFunction.
    VTableEntryLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[VTableEntryLayout::Code],
        // SILFunction name
        S.addIdentifierRef(Ctx.getIdentifier(entry.second->getName())),
        ListOfValues);
  }
}

/// Generates an identifier for a given NormalProtocolConformance. We use
/// the identifier to look for a witness table in sil_index block.
static Identifier getIdOfConformance(const NormalProtocolConformance *npc) {
  // FIXME: generates a better name. Right now, we don't support look up
  // a specific witness table in deserialization. So this function and the
  // corresponding sil_index block is not used.
  return npc->getProtocol()->getName();
}

void SILSerializer::writeWitnessTable(const SILWitnessTable &wt) {
  WitnessTableList[getIdOfConformance(wt.getConformance())] = WitnessTableID++;
  WitnessTableOffset.push_back(Out.GetCurrentBitNo());
  WitnessTableLayout::emitRecord(Out, ScratchRecord,
                           SILAbbrCodes[WitnessTableLayout::Code],
                           S.addTypeRef(wt.getConformance()->getType()));

  S.writeConformance(wt.getConformance()->getProtocol(), wt.getConformance(),
                     nullptr, SILAbbrCodes);
  for (auto &entry : wt.getEntries()) {
    if (entry.getKind() == SILWitnessTable::BaseProtocol) {
      auto &baseWitness = entry.getBaseProtocolWitness();
      WitnessBaseEntryLayout::emitRecord(Out, ScratchRecord,
          SILAbbrCodes[WitnessBaseEntryLayout::Code],
          S.addDeclRef(baseWitness.Requirement),
          S.addTypeRef(baseWitness.Witness->getType()));
      S.writeConformance(baseWitness.Witness->getProtocol(),
                         baseWitness.Witness, nullptr, SILAbbrCodes);
      continue;
    }
    if (entry.getKind() == SILWitnessTable::AssociatedTypeProtocol) {
      auto &assoc = entry.getAssociatedTypeProtocolWitness();
      WitnessAssocProtocolLayout::emitRecord(Out, ScratchRecord,
          SILAbbrCodes[WitnessAssocProtocolLayout::Code],
          S.addDeclRef(assoc.Requirement),
          S.addDeclRef(assoc.Protocol),
          assoc.Witness ? S.addTypeRef(assoc.Witness->getType()) : (TypeID)0);
      if (assoc.Witness)
        S.writeConformance(assoc.Witness->getProtocol(),
                           assoc.Witness, nullptr, SILAbbrCodes);
      continue;
    }
    if (entry.getKind() == SILWitnessTable::AssociatedType) {
      auto &assoc = entry.getAssociatedTypeWitness();
      WitnessAssocEntryLayout::emitRecord(Out, ScratchRecord,
          SILAbbrCodes[WitnessAssocEntryLayout::Code],
          S.addDeclRef(assoc.Requirement),
          S.addTypeRef(assoc.Witness));
      continue;
    }
    auto &methodWitness = entry.getMethodWitness();
    SmallVector<ValueID, 4> ListOfValues;
    handleSILDeclRef(S, methodWitness.Requirement, ListOfValues);
    FuncsToDeclare.insert(methodWitness.Witness);
    WitnessMethodEntryLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[WitnessMethodEntryLayout::Code],
        // SILFunction name
        S.addIdentifierRef(Ctx.getIdentifier(methodWitness.Witness->getName())),
        ListOfValues);
  }
}

static bool canAlwaysSerializeLinkage(SILLinkage linkage) {
  switch (linkage) {
  case SILLinkage::Public:
  case SILLinkage::PublicExternal:
  case SILLinkage::Hidden:
  case SILLinkage::HiddenExternal:
    return true;
  case SILLinkage::Shared:
  case SILLinkage::Private:
    return false;
  }
}

// Check if F transitively references a global, function, vtable, or witness
// table with private linkage.
//
// FIXME: When vtables/witness tables get linkage, update this.
static bool
transitivelyReferencesPotentiallyUnserializableLinkage(const SILFunction &F) {
  for (auto &BB : F)
    for (auto &I : BB) {
      if (auto *GA = dyn_cast<SILGlobalAddrInst>(&I))
        if (!canAlwaysSerializeLinkage(GA->getReferencedGlobal()->getLinkage()))
          return true;
      if (auto *FRI = dyn_cast<FunctionRefInst>(&I))
        if (!canAlwaysSerializeLinkage(FRI->getReferencedFunction()->getLinkage()))
          return true;
    }
  return false;
}

void SILSerializer::writeModule(const SILModule *SILMod) {
  {
    BCBlockRAII subBlock(Out, SIL_BLOCK_ID, 6);
    registerSILAbbr<SILFunctionLayout>();
    registerSILAbbr<SILBasicBlockLayout>();
    registerSILAbbr<SILOneValueOneOperandLayout>();
    registerSILAbbr<SILOneTypeLayout>();
    registerSILAbbr<SILOneOperandLayout>();
    registerSILAbbr<SILOneTypeOneOperandLayout>();
    registerSILAbbr<SILInitExistentialLayout>();
    registerSILAbbr<SILOneTypeValuesLayout>();
    registerSILAbbr<SILTwoOperandsLayout>();
    registerSILAbbr<SILInstApplyLayout>();
    registerSILAbbr<SILInstNoOperandLayout>();

    registerSILAbbr<VTableLayout>();
    registerSILAbbr<VTableEntryLayout>();
    registerSILAbbr<GlobalVarLayout>();
    registerSILAbbr<WitnessTableLayout>();
    registerSILAbbr<WitnessMethodEntryLayout>();
    registerSILAbbr<WitnessBaseEntryLayout>();
    registerSILAbbr<WitnessAssocProtocolLayout>();
    registerSILAbbr<WitnessAssocEntryLayout>();
    registerSILAbbr<SILGenericOuterParamsLayout>();

    registerSILAbbr<SILInstCastLayout>();

    // Register the abbreviation codes so these layouts can exist in both
    // decl blocks and sil blocks.
    // We have to make sure BOUND_GENERIC_SUBSTITUTION does not overlap with
    // SIL-specific records.
    registerSILAbbr<decls_block::BoundGenericSubstitutionLayout>();
    registerSILAbbr<decls_block::NoConformanceLayout>();
    registerSILAbbr<decls_block::NormalProtocolConformanceLayout>();
    registerSILAbbr<decls_block::SpecializedProtocolConformanceLayout>();
    registerSILAbbr<decls_block::InheritedProtocolConformanceLayout>();
    registerSILAbbr<decls_block::GenericParamListLayout>();
    registerSILAbbr<decls_block::GenericParamLayout>();
    registerSILAbbr<decls_block::GenericRequirementLayout>();
    registerSILAbbr<decls_block::LastGenericRequirementLayout>();

    for (const SILGlobalVariable &g : SILMod->getSILGlobals())
      writeGlobalVar(g);

    // Write out VTables first because it may require serializations of
    // non-transparent SILFunctions (body is not needed).
    // Go through all SILVTables in SILMod, and if it is fragile, write out the
    // VTable.
    for (const SILVTable &vt : SILMod->getVTables()) {
      const ClassDecl *cd = vt.getClass();
      if (ShouldSerializeAll ||
          cd->getAttrs().getResilienceKind() == Resilience::Fragile)
        writeVTable(vt);
    }

    // Write out WitnessTables. For now, write out only if EnableSerializeAll.
    for (const SILWitnessTable &wt : SILMod->getWitnessTables()) {
      if (ShouldSerializeAll)
        writeWitnessTable(wt);
    }

    // Helper function for whether to emit a function body.
    auto shouldEmitFunctionBody = [&](const SILFunction &F) {
      // If F is a declaration, it has no body to emit...
      if (F.empty())
        return false;

      // If F is transparent, we should always emit its body.
      if (F.isTransparent())
        return true;

      // Emit the function body if F is a shared function referenced in this
      // module. This is needed specifically to handle the bodies of closures.
      // FIXME: This is order-dependent.
      if (F.getLinkage() == SILLinkage::Shared && FuncsToDeclare.count(&F))
        return true;

      // If F has private linkage or transitively references a global, function,
      // vtable, or witnesstable with private linkage, do not serialize it.
      //
      // FIXME: *NOTE* vtables and witness tables do not have linkage currently,
      // but will at some point in the near future.
      if (!canAlwaysSerializeLinkage(F.getLinkage()) ||
          transitivelyReferencesPotentiallyUnserializableLinkage(F))
        return false;

      // Otherwise serialize the body of the function only if we are asked to
      // serialize everything.
      return ShouldSerializeAll;
    };

    // Go through all the SILFunctions in SILMod and write out any
    // mandatory function bodies.
    for (const SILFunction &F : *SILMod) {
      if (shouldEmitFunctionBody(F))
        writeSILFunction(F);
    }

    // Now write function declarations for every function we've
    // emitted a reference to without emitting a function body for.
    for (const SILFunction &F : *SILMod) {
      if (!shouldEmitFunctionBody(F) && FuncsToDeclare.count(&F))
        writeSILFunction(F, true);
    }
  }
  {
    BCBlockRAII restoreBlock(Out, SIL_INDEX_BLOCK_ID, 4);
    writeTables();
  }
}

void Serializer::writeSIL(const SILModule *SILMod, bool serializeAllSIL) {
  if (!SILMod)
    return;

  SILSerializer SILSer(*this, M->Ctx, Out, serializeAllSIL);
  SILSer.writeModule(SILMod);

}
