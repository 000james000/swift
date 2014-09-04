//===--- SILBuilder.h - Class for creating SIL Constructs --------*- C++ -*-==//
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

#ifndef SWIFT_SIL_SILBUILDER_H
#define SWIFT_SIL_SILBUILDER_H

#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"

namespace swift {

class SILBuilder {
  /// BB - If this is non-null, the instruction is inserted in the specified
  /// basic block, at the specified InsertPt.  If null, created instructions
  /// are not auto-inserted.
  SILFunction &F;
  SILBasicBlock *BB;
  SILBasicBlock::iterator InsertPt;

  /// InsertedInstrs - If this pointer is non-null, then any inserted
  /// instruction is recorded in this list.
  SmallVectorImpl<SILInstruction*> *InsertedInstrs = nullptr;
public:
  SILBuilder(SILFunction &F) : F(F), BB(0) {}

  explicit SILBuilder(SILInstruction *I,
                      SmallVectorImpl<SILInstruction*> *InsertedInstrs = 0)
    : F(*I->getParent()->getParent()), InsertedInstrs(InsertedInstrs) {
    setInsertionPoint(I);
  }

  explicit SILBuilder(SILBasicBlock *BB,
                      SmallVectorImpl<SILInstruction*> *InsertedInstrs = 0)
    : F(*BB->getParent()), InsertedInstrs(InsertedInstrs) {
    setInsertionPoint(BB);
  }

  SILBuilder(SILBasicBlock *BB, SILBasicBlock::iterator InsertPt,
             SmallVectorImpl<SILInstruction*> *InsertedInstrs = 0)
    : F(*BB->getParent()), InsertedInstrs(InsertedInstrs) {
    setInsertionPoint(BB, InsertPt);
  }

  SILFunction &getFunction() const { return F; }
  SILModule &getModule() const { return F.getModule(); }
  ASTContext &getASTContext() const { return F.getASTContext(); }
  const Lowering::TypeLowering &getTypeLowering(SILType T) const {
    return F.getModule().getTypeLowering(T);
  }

  //===--------------------------------------------------------------------===//
  // Insertion Point Management
  //===--------------------------------------------------------------------===//

  bool hasValidInsertionPoint() const { return BB != nullptr; }
  SILBasicBlock *getInsertionBB() { return BB; }
  SILBasicBlock::iterator getInsertionPoint() { return InsertPt; }

  /// insertingAtEndOfBlock - Return true if the insertion point is at the end
  /// of the current basic block.  False if we're inserting before an existing
  /// instruction.
  bool insertingAtEndOfBlock() const {
    assert(hasValidInsertionPoint() &&
           "Must have insertion point to ask about it");
    return InsertPt == BB->end();
  }
  
  /// clearInsertionPoint - Clear the insertion point: created instructions will
  /// not be inserted into a block.
  void clearInsertionPoint() {
    BB = nullptr;
  }

  /// setInsertionPoint - Set the insertion point.
  void setInsertionPoint(SILBasicBlock *BB, SILBasicBlock::iterator InsertPt) {
    this->BB = BB;
    this->InsertPt = InsertPt;
  }

  /// setInsertionPoint - Set the insertion point to insert before the specified
  /// instruction.
  void setInsertionPoint(SILInstruction *I) {
    setInsertionPoint(I->getParent(), I);
  }

  /// setInsertionPoint - Set the insertion point to insert at the end of the
  /// specified block.
  void setInsertionPoint(SILBasicBlock *BB) {
    setInsertionPoint(BB, BB->end());
  }
  
  SILBasicBlock *getInsertionPoint() const {
    return BB;
  }

  //===--------------------------------------------------------------------===//
  // Instruction Tracking
  //===--------------------------------------------------------------------===//

  /// Clients of SILBuilder who want to know about any newly created
  /// instructions can install a SmallVector into the builder to collect them.
  void setTrackingList(SmallVectorImpl<SILInstruction*> *II) {
    InsertedInstrs = II;
  }

  SmallVectorImpl<SILInstruction*> *getTrackingList() {
    return InsertedInstrs;
  }

  //===--------------------------------------------------------------------===//
  // Type remapping
  //===--------------------------------------------------------------------===//

  static SILType getPartialApplyResultType(SILType Ty, unsigned ArgCount,
                                           SILModule &M,
                                           ArrayRef<Substitution> subs);

  //===--------------------------------------------------------------------===//
  // CFG Manipulation
  //===--------------------------------------------------------------------===//

  /// moveBlockToEnd - Move a block to a new position in its function.
  void moveBlockTo(SILBasicBlock *BB, SILFunction::iterator IP) {
    SILFunction *F = BB->getParent();
    auto &Blocks = F->getBlocks();
    Blocks.splice(IP, Blocks, BB);
  }

  /// moveBlockToEnd - Reorder a block to the end of its containing function.
  void moveBlockToEnd(SILBasicBlock *BB) {
    moveBlockTo(BB, BB->getParent()->end());
  }

  /// \brief Move the specified block to the end of the function and reset the
  /// insertion point to point to the first instruction in the emitted block.
  ///
  /// Assumes that no insertion point is currently active.
  void emitBlock(SILBasicBlock *BB);

  /// \brief Move the specified block to the current insertion point (which
  /// is the end of the function if there is no insertion point) and reset the
  /// insertion point to point to the first instruction in the emitted block.
  void emitBlock(SILBasicBlock *BB, SILLocation BranchLoc);

  /// splitBlockForFallthrough - Prepare for the insertion of a terminator.  If
  /// the builder's insertion point is at the end of the current block (as when
  /// SILGen is creating the initial code for a function), just create and
  /// return a new basic block that will be later used for the continue point.
  ///
  /// If the insertion point is valid (i.e., pointing to an existing
  /// instruction) then split the block at that instruction and return the
  /// continuation block.
  SILBasicBlock *splitBlockForFallthrough();

  //===--------------------------------------------------------------------===//
  // SILInstruction Creation Methods
  //===--------------------------------------------------------------------===//

  AllocStackInst *createAllocStack(SILLocation Loc, SILType elementType) {
    Loc.markAsPrologue();
    return insert(new (F.getModule()) AllocStackInst(Loc, elementType, F));
  }

  AllocRefInst *createAllocRef(SILLocation Loc, SILType elementType, bool objc){
    Loc.markAsPrologue();
    return insert(new (F.getModule()) AllocRefInst(Loc, elementType, F, objc));
  }

  AllocRefDynamicInst *createAllocRefDynamic(SILLocation loc, SILValue operand,
                                             SILType type, bool objc) {
    loc.markAsPrologue();
    return insert(new (F.getModule()) AllocRefDynamicInst(loc, operand, type, 
                                                          objc));
  }
  
  AllocBoxInst *createAllocBox(SILLocation Loc, SILType ElementType) {
    Loc.markAsPrologue();
    return insert(new (F.getModule())
                    AllocBoxInst(Loc, ElementType, F));
  }

  AllocArrayInst *createAllocArray(SILLocation Loc, SILType ElementType,
                                   SILValue NumElements) {
    Loc.markAsPrologue();
    return insert(new (F.getModule())
                    AllocArrayInst(Loc, ElementType, NumElements, F));
  }

  ApplyInst *createApply(SILLocation Loc, SILValue Fn,
                         SILType SubstFnTy,
                         SILType Result,
                         ArrayRef<Substitution> Subs,
                         ArrayRef<SILValue> Args,
                         bool Transparent = false) {
    return insert(ApplyInst::create(Loc, Fn, SubstFnTy, Result,
                                    Subs, Args, Transparent, F));
  }
  
  ApplyInst *createApply(SILLocation Loc, SILValue Fn, ArrayRef<SILValue> Args,
                         bool Transparent = false) {
    auto FnTy = Fn.getType();
    return createApply(Loc, Fn, FnTy,
                       FnTy.castTo<SILFunctionType>()
                         ->getResult()
                         .getSILType(),
                       ArrayRef<Substitution>(), Args, Transparent);
  }


  PartialApplyInst *createPartialApply(SILLocation Loc, SILValue Fn,
                                       SILType SubstFnTy,
                                       ArrayRef<Substitution> Subs,
                                       ArrayRef<SILValue> Args,
                                       SILType ClosureTy) {
    return insert(PartialApplyInst::create(Loc, Fn, SubstFnTy,
                                           Subs, Args, ClosureTy, F));
  }

  BuiltinFunctionRefInst *createBuiltinFunctionRef(SILLocation loc,
                                                   Identifier Id,
                                                   SILType ty) {
    return insert(new (F.getModule()) BuiltinFunctionRefInst(loc, Id, ty));
  }
  BuiltinFunctionRefInst *createBuiltinFunctionRef(SILLocation loc,
                                                   StringRef Str,
                                                   SILType ty) {
    return createBuiltinFunctionRef(loc, getASTContext().getIdentifier(Str),ty);
  }

  FunctionRefInst *createFunctionRef(SILLocation loc, SILFunction *f) {
    return insert(new (F.getModule()) FunctionRefInst(loc, f));
  }
  GlobalAddrInst *createGlobalAddr(SILLocation loc, VarDecl *g, SILType ty) {
    return insert(new (F.getModule()) GlobalAddrInst(loc, g, ty));
  }
  SILGlobalAddrInst *createSILGlobalAddr(SILLocation loc, SILGlobalVariable *g){
    return insert(new (F.getModule()) SILGlobalAddrInst(loc, g));
  }

  IntegerLiteralInst *createIntegerLiteral(IntegerLiteralExpr *E) {
    return insert(IntegerLiteralInst::create(E, F));
  }
  IntegerLiteralInst *createIntegerLiteral(CharacterLiteralExpr *E) {
    return insert(IntegerLiteralInst::create(E, F));
  }
  IntegerLiteralInst *createIntegerLiteral(SILLocation Loc, SILType Ty,
                                           intmax_t Value) {
    return insert(IntegerLiteralInst::create(Loc, Ty, Value, F));
  }
  IntegerLiteralInst *createIntegerLiteral(SILLocation Loc, SILType Ty,
                                           const APInt &Value) {
    return insert(IntegerLiteralInst::create(Loc, Ty, Value, F));
  }

  FloatLiteralInst *createFloatLiteral(FloatLiteralExpr *E) {
    return insert(FloatLiteralInst::create(E, F));
  }
  FloatLiteralInst *createFloatLiteral(SILLocation Loc, SILType Ty,
                                       const APFloat &Value) {
    return insert(FloatLiteralInst::create(Loc, Ty, Value, F));
  }
  
  StringLiteralInst *createStringLiteral(SILLocation loc, StringRef text,
                                         StringLiteralInst::Encoding encoding) {
    return insert(StringLiteralInst::create(loc, text, encoding, F));
  }

  StringLiteralInst *createStringLiteral(SILLocation loc, const Twine& text,
                                         StringLiteralInst::Encoding encoding) {
    SmallVector<char, 256> Out;
    return insert(StringLiteralInst::create(loc,
                                            text.toStringRef(Out),
                                            encoding,
                                            F));
  }

  LoadInst *createLoad(SILLocation Loc, SILValue LV) {
    return insert(new (F.getModule()) LoadInst(Loc, LV));
  }

  StoreInst *createStore(SILLocation Loc, SILValue Src, SILValue DestAddr) {
    return insert(new (F.getModule()) StoreInst(Loc, Src, DestAddr));
  }
  AssignInst *createAssign(SILLocation Loc, SILValue Src, SILValue DestAddr) {
    return insert(new (F.getModule()) AssignInst(Loc, Src, DestAddr));
  }

  MarkUninitializedInst *createMarkUninitialized(SILLocation loc, SILValue src,
                                                 MarkUninitializedInst::Kind k){
    return insert(new (F.getModule()) MarkUninitializedInst(loc, src, k));
  }
  MarkUninitializedInst *createMarkUninitializedVar(SILLocation loc,
                                                    SILValue src) {
    return createMarkUninitialized(loc, src, MarkUninitializedInst::Var);
  }
  MarkUninitializedInst *createMarkUninitializedRootSelf(SILLocation loc,
                                                         SILValue src) {
    return createMarkUninitialized(loc, src, MarkUninitializedInst::RootSelf);
  }

  MarkFunctionEscapeInst *createMarkFunctionEscape(SILLocation loc,
                                                   ArrayRef<SILValue> vars) {
    return insert(MarkFunctionEscapeInst::create(loc, vars, F));
  }
  DebugValueInst *createDebugValue(SILLocation loc, SILValue src) {
    return insert(new (F.getModule()) DebugValueInst(loc, src));
  }
  DebugValueAddrInst *createDebugValueAddr(SILLocation loc, SILValue src) {
    return insert(new (F.getModule()) DebugValueAddrInst(loc, src));
  }

  LoadWeakInst *createLoadWeak(SILLocation loc, SILValue src, IsTake_t isTake) {
    return insert(new (F.getModule()) LoadWeakInst(loc, src, isTake));
  }

  StoreWeakInst *createStoreWeak(SILLocation loc, SILValue value,
                                 SILValue dest, IsInitialization_t isInit) {
    return insert(new (F.getModule()) StoreWeakInst(loc, value, dest, isInit));
  }

  CopyAddrInst *createCopyAddr(SILLocation loc, SILValue srcAddr,
                               SILValue destAddr, IsTake_t isTake,
                               IsInitialization_t isInitialize) {
    assert(srcAddr.getType() == destAddr.getType());
    return insert(new (F.getModule())
                    CopyAddrInst(loc, srcAddr, destAddr, isTake, isInitialize));
  }
  
  ConvertFunctionInst *createConvertFunction(SILLocation Loc, SILValue Op,
                                             SILType Ty) {
    return insert(new (F.getModule()) ConvertFunctionInst(Loc, Op, Ty));
  }

  UpcastInst *createUpcast(SILLocation Loc, SILValue Op, SILType Ty) {
    return insert(new (F.getModule()) UpcastInst(Loc, Op, Ty));
  }
  
  AddressToPointerInst *createAddressToPointer(SILLocation Loc, SILValue Op,
                                               SILType Ty) {
    return insert(new (F.getModule()) AddressToPointerInst(Loc, Op, Ty));
  }
  
  PointerToAddressInst *createPointerToAddress(SILLocation Loc, SILValue Op,
                                               SILType Ty) {
    return insert(new (F.getModule()) PointerToAddressInst(Loc, Op, Ty));
  }
  
  UncheckedRefCastInst *createUncheckedRefCast(SILLocation Loc, SILValue Op,
                                                   SILType Ty) {
    return insert(new (F.getModule()) UncheckedRefCastInst(Loc, Op, Ty));
  }
  
  UncheckedAddrCastInst *createUncheckedAddrCast(SILLocation Loc, SILValue Op,
                                               SILType Ty) {
    return insert(new (F.getModule()) UncheckedAddrCastInst(Loc, Op, Ty));
  }

  UncheckedTrivialBitCastInst *createUncheckedTrivialBitCast(SILLocation Loc,
                                                             SILValue Op,
                                                             SILType Ty) {
    return insert(new (F.getModule()) UncheckedTrivialBitCastInst(Loc, Op, Ty));
  }

  UncheckedRefBitCastInst *createUncheckedRefBitCast(SILLocation Loc,
                                                     SILValue Op,
                                                     SILType Ty) {
    return insert(new (F.getModule()) UncheckedRefBitCastInst(Loc, Op, Ty));
  }

  RefToRawPointerInst *createRefToRawPointer(SILLocation Loc, SILValue Op,
                                             SILType Ty) {
    return insert(new (F.getModule()) RefToRawPointerInst(Loc, Op, Ty));
  }
  
  RawPointerToRefInst *createRawPointerToRef(SILLocation Loc, SILValue Op,
                                             SILType Ty) {
    return insert(new (F.getModule()) RawPointerToRefInst(Loc, Op, Ty));
  }

  ThinToThickFunctionInst *createThinToThickFunction(SILLocation Loc,
                                                     SILValue Op, SILType Ty) {
    return insert(new (F.getModule()) ThinToThickFunctionInst(Loc, Op, Ty));
  }

  ThickToObjCMetatypeInst *createThickToObjCMetatype(SILLocation Loc,
                                                     SILValue Op, SILType Ty) {
    return insert(new (F.getModule()) ThickToObjCMetatypeInst(Loc, Op, Ty));
  }

  ObjCToThickMetatypeInst *createObjCToThickMetatype(SILLocation Loc,
                                                     SILValue Op, SILType Ty) {
    return insert(new (F.getModule()) ObjCToThickMetatypeInst(Loc, Op, Ty));
  }

  ObjCProtocolInst *createObjCProtocol(SILLocation Loc,
                                       ProtocolDecl *P,
                                       SILType Ty) {
    return insert(new (F.getModule()) ObjCProtocolInst(Loc, P, Ty));
  }
  
  UnownedToRefInst *createUnownedToRef(SILLocation loc,
                                       SILValue op, SILType ty) {
    return insert(new (F.getModule()) UnownedToRefInst(loc, op, ty));
  }

  RefToUnownedInst *createRefToUnowned(SILLocation loc,
                                       SILValue op, SILType ty) {
    return insert(new (F.getModule()) RefToUnownedInst(loc, op, ty));
  }

  UnmanagedToRefInst *createUnmanagedToRef(SILLocation loc,
                                           SILValue op, SILType ty) {
    return insert(new (F.getModule()) UnmanagedToRefInst(loc, op, ty));
  }

  RefToUnmanagedInst *createRefToUnmanaged(SILLocation loc,
                                           SILValue op, SILType ty) {
    return insert(new (F.getModule()) RefToUnmanagedInst(loc, op, ty));
  }

  IsNonnullInst *createIsNonnull(SILLocation loc,
                                 SILValue operand) {
    return insert(new (F.getModule())
                    IsNonnullInst(loc, operand,
                      SILType::getBuiltinIntegerType(1, getASTContext())));
  }

  UnconditionalCheckedCastInst *createUnconditionalCheckedCast(SILLocation loc,
                                                               SILValue op,
                                                               SILType destTy) {
    return insert(new (F.getModule())
                    UnconditionalCheckedCastInst(loc, op, destTy));
  }

  UnconditionalCheckedCastAddrInst *
  createUnconditionalCheckedCastAddr(SILLocation loc,
                                     CastConsumptionKind consumption,
                                     SILValue src, CanType sourceType,
                                     SILValue dest, CanType targetType) {
    return insert(new (F.getModule())
                    UnconditionalCheckedCastAddrInst(loc, consumption,
                                                     src, sourceType,
                                                     dest, targetType));
  }
  
  RetainValueInst *createRetainValue(SILLocation loc, SILValue operand) {
    return insert(new (F.getModule()) RetainValueInst(loc, operand));
  }

  ReleaseValueInst *createReleaseValue(SILLocation loc, SILValue operand) {
    return insert(new (F.getModule()) ReleaseValueInst(loc, operand));
  }

  AutoreleaseValueInst *createAutoreleaseValue(SILLocation loc, SILValue operand) {
    return insert(new (F.getModule()) AutoreleaseValueInst(loc, operand));
  }

  StructInst *createStruct(SILLocation Loc, SILType Ty,
                           ArrayRef<SILValue> Elements) {
    return insert(StructInst::create(Loc, Ty, Elements, F));
  }

  TupleInst *createTuple(SILLocation Loc, SILType Ty,
                         ArrayRef<SILValue> Elements) {
    return insert(TupleInst::create(Loc, Ty, Elements, F));
  }
  
  EnumInst *createEnum(SILLocation Loc, SILValue Operand,
                       EnumElementDecl *Element, SILType Ty) {
    return insert(new (F.getModule()) EnumInst(Loc, Operand, Element, Ty));
  }
  
  EnumIsTagInst *createEnumIsTag(SILLocation Loc, SILValue Operand,
                                 EnumElementDecl *Element, SILType Ty) {
    return insert(new (F.getModule()) EnumIsTagInst(Loc, Operand, Element, Ty));
  }

  InitEnumDataAddrInst *createInitEnumDataAddr(SILLocation Loc, SILValue Operand,
                                       EnumElementDecl *Element, SILType Ty) {
    return insert(
          new (F.getModule()) InitEnumDataAddrInst(Loc, Operand, Element, Ty));
  }

  UncheckedEnumDataInst *createUncheckedEnumData(SILLocation Loc, SILValue Operand,
                                       EnumElementDecl *Element, SILType Ty) {
    return insert(
          new (F.getModule()) UncheckedEnumDataInst(Loc, Operand, Element, Ty));
  }

  UncheckedTakeEnumDataAddrInst *createUncheckedTakeEnumDataAddr(SILLocation Loc, SILValue Operand,
                                       EnumElementDecl *Element, SILType Ty) {
    return insert(
          new (F.getModule()) UncheckedTakeEnumDataAddrInst(Loc, Operand, Element, Ty));
  }

  InjectEnumAddrInst *createInjectEnumAddr(SILLocation Loc, SILValue Operand,
                                             EnumElementDecl *Element) {
    return insert(new (F.getModule())
                    InjectEnumAddrInst(Loc, Operand, Element));
  }
  
  TupleExtractInst *createTupleExtract(SILLocation Loc, SILValue Operand,
                                       unsigned FieldNo, SILType ResultTy) {
    return insert(new (F.getModule())
                    TupleExtractInst(Loc, Operand, FieldNo, ResultTy));
  }

  TupleExtractInst *createTupleExtract(SILLocation Loc, SILValue Operand,
                                       unsigned FieldNo) {
    auto type = Operand.getType().getTupleElementType(FieldNo);
    return createTupleExtract(Loc, Operand, FieldNo, type);
  }

  TupleElementAddrInst *createTupleElementAddr(SILLocation Loc,
                                               SILValue Operand,
                                               unsigned FieldNo,
                                               SILType ResultTy) {
    return insert(new (F.getModule())
                    TupleElementAddrInst(Loc, Operand, FieldNo, ResultTy));
  }

  TupleElementAddrInst *createTupleElementAddr(SILLocation Loc,
                                               SILValue Operand,
                                               unsigned FieldNo) {
    return insert(new (F.getModule())
                  TupleElementAddrInst(Loc, Operand, FieldNo,
                            Operand.getType().getTupleElementType(FieldNo)));
  }
  
  StructExtractInst *createStructExtract(SILLocation Loc,
                                         SILValue Operand,
                                         VarDecl *Field,
                                         SILType ResultTy) {
    return insert(new (F.getModule())
                    StructExtractInst(Loc, Operand, Field, ResultTy));
  }

  StructExtractInst *createStructExtract(SILLocation Loc,
                                         SILValue Operand,
                                         VarDecl *Field) {
    auto type = Operand.getType().getFieldType(Field, F.getModule());
    return createStructExtract(Loc, Operand, Field, type);
  }


  StructElementAddrInst *createStructElementAddr(SILLocation Loc,
                                                 SILValue Operand,
                                                 VarDecl *Field,
                                                 SILType ResultTy) {
    return insert(new (F.getModule())
                    StructElementAddrInst(Loc, Operand, Field, ResultTy));
  }
  
  StructElementAddrInst *createStructElementAddr(SILLocation Loc,
                                                 SILValue Operand,
                                                 VarDecl *Field) {
    auto ResultTy = Operand.getType().getFieldType(Field, F.getModule());
    return createStructElementAddr(Loc, Operand, Field, ResultTy);
  }
  
  RefElementAddrInst *createRefElementAddr(SILLocation Loc, SILValue Operand,
                                           VarDecl *Field, SILType ResultTy) {
    return insert(new (F.getModule())
                    RefElementAddrInst(Loc, Operand, Field, ResultTy));
  }
  RefElementAddrInst *createRefElementAddr(SILLocation Loc, SILValue Operand,
                                           VarDecl *Field) {
    auto ResultTy = Operand.getType().getFieldType(Field, F.getModule());
    return createRefElementAddr(Loc, Operand, Field, ResultTy);
  }

  ClassMethodInst *createClassMethod(SILLocation Loc, SILValue Operand,
                                     SILDeclRef Member, SILType MethodTy,
                                     bool Volatile = false) {
    return insert(new (F.getModule())
                    ClassMethodInst(Loc, Operand, Member, MethodTy, Volatile));
  }
  
  SuperMethodInst *createSuperMethod(SILLocation Loc, SILValue Operand,
                                     SILDeclRef Member, SILType MethodTy,
                                     bool Volatile = false) {
    return insert(new (F.getModule())
                    SuperMethodInst(Loc, Operand, Member, MethodTy, Volatile));
  }

  WitnessMethodInst *createWitnessMethod(SILLocation Loc, SILType LookupTy,
                                         ProtocolConformance *Conformance,
                                         SILDeclRef Member,
                                         SILType MethodTy,
                                         bool Volatile = false)
  {
    return insert(WitnessMethodInst::create(Loc, LookupTy, Conformance, Member,
                                            MethodTy, &F, Volatile));
  }
  
  ProtocolMethodInst *createProtocolMethod(SILLocation Loc, SILValue Operand,
                                           SILDeclRef Member, SILType MethodTy,
                                           bool Volatile = false) {
    return insert(new (F.getModule())
                    ProtocolMethodInst(Loc, Operand, Member, MethodTy,
                                       Volatile));
  }

  DynamicMethodInst *createDynamicMethod(SILLocation Loc, SILValue Operand,
                                         SILDeclRef Member, SILType MethodTy,
                                         bool Volatile = false) {
    return insert(new (F.getModule())
                  DynamicMethodInst(Loc, Operand, Member, MethodTy,
                                    Volatile));
  }

  ProjectExistentialInst *createProjectExistential(SILLocation Loc,
                                                   SILValue Operand,
                                                   SILType SelfTy) {
    return insert(new (F.getModule())
                    ProjectExistentialInst(Loc, Operand, SelfTy));
  }
  
  ProjectExistentialRefInst *createProjectExistentialRef(SILLocation Loc,
                                                         SILValue Operand,
                                                         SILType Ty) {
    return insert(new (F.getModule())
                    ProjectExistentialRefInst(Loc, Operand, Ty));
  }

  OpenExistentialInst *createOpenExistential(SILLocation Loc, SILValue Operand,
                                             SILType SelfTy) {
    return insert(new (F.getModule())
                    OpenExistentialInst(Loc, Operand, SelfTy));
  }
  
  OpenExistentialRefInst *createOpenExistentialRef(SILLocation Loc,
                                                   SILValue Operand,
                                                   SILType Ty) {
    return insert(new (F.getModule())
                    OpenExistentialRefInst(Loc, Operand, Ty));
  }

  InitExistentialInst *
  createInitExistential(SILLocation Loc,
                        SILValue Existential,
                        SILType ConcreteType,
                        ArrayRef<ProtocolConformance*> Conformances) {
    return insert(InitExistentialInst::create(Loc, Existential, ConcreteType,
                                              Conformances, &F));
  }
  
  InitExistentialRefInst *
  createInitExistentialRef(SILLocation Loc, SILType ExistentialType,
                           SILValue Concrete,
                           ArrayRef<ProtocolConformance*> Conformances) {
    return insert(InitExistentialRefInst::create(Loc, ExistentialType, Concrete,
                                                 Conformances, &F));
  }
  
  UpcastExistentialInst *createUpcastExistential(SILLocation Loc,
                                 SILValue SrcExistential,
                                 SILValue DestExistential,
                                 IsTake_t isTakeOfSrc) {
    return insert(new (F.getModule())
                    UpcastExistentialInst(Loc, SrcExistential, DestExistential,
                                          isTakeOfSrc));
  }
  
  UpcastExistentialRefInst *createUpcastExistentialRef(SILLocation Loc,
                                 SILValue Operand,
                                 SILType Ty) {
    return insert(new (F.getModule())
                    UpcastExistentialRefInst(Loc, Operand, Ty));
  }
  
  DeinitExistentialInst *createDeinitExistential(SILLocation Loc,
                                                 SILValue Existential) {
    return insert(new (F.getModule()) DeinitExistentialInst(Loc, Existential));
  }
  
  ProjectBlockStorageInst *createProjectBlockStorage(SILLocation Loc,
                                                     SILValue Storage) {
    auto CaptureTy =
      Storage.getType().castTo<SILBlockStorageType>()->getCaptureAddressType();
    return createProjectBlockStorage(Loc, Storage, CaptureTy);
  }
  ProjectBlockStorageInst *createProjectBlockStorage(SILLocation Loc,
                                                     SILValue Storage,
                                                     SILType CaptureTy) {
    return insert(new (F.getModule()) ProjectBlockStorageInst(Loc, Storage,
                                                              CaptureTy));
  }
  
  InitBlockStorageHeaderInst *createInitBlockStorageHeader(SILLocation Loc,
                                                         SILValue BlockStorage,
                                                         SILValue InvokeFunction,
                                                         SILType BlockType) {
    return insert(new (F.getModule()) InitBlockStorageHeaderInst(Loc,
                                                                 BlockStorage,
                                                                 InvokeFunction,
                                                                 BlockType));
  }
  
  MetatypeInst *createMetatype(SILLocation Loc, SILType Metatype) {
    return insert(new (F.getModule()) MetatypeInst(Loc, Metatype));
  }
  
  ObjCMetatypeToObjectInst *createObjCMetatypeToObject(SILLocation Loc,
                                                       SILValue Op,
                                                       SILType Ty) {
    return insert(new (F.getModule()) ObjCMetatypeToObjectInst(Loc, Op, Ty));
  }
  
  ObjCExistentialMetatypeToObjectInst *
  createObjCExistentialMetatypeToObject(SILLocation Loc,
                                        SILValue Op,
                                        SILType Ty) {
    return insert(new (F.getModule())
                    ObjCExistentialMetatypeToObjectInst(Loc, Op, Ty));
  }
  
  ValueMetatypeInst *createValueMetatype(SILLocation Loc,
                                                 SILType Metatype,
                                                 SILValue Base) {
    return insert(new (F.getModule())
                    ValueMetatypeInst(Loc, Metatype, Base));
  }
  
  ExistentialMetatypeInst *createExistentialMetatype(SILLocation Loc,
                                               SILType Metatype,
                                               SILValue Base) {
    return insert(new (F.getModule()) ExistentialMetatypeInst(Loc, Metatype,Base));
  }
  
  CopyBlockInst *createCopyBlock(SILLocation Loc, SILValue Operand) {
    return insert(new (F.getModule()) CopyBlockInst(Loc, Operand));
  }
  StrongRetainInst *createStrongRetain(SILLocation Loc, SILValue Operand) {
    return insert(new (F.getModule()) StrongRetainInst(Loc, Operand));
  }
  StrongReleaseInst *createStrongRelease(SILLocation Loc, SILValue Operand) {
    return insert(new (F.getModule()) StrongReleaseInst(Loc, Operand));
  }
  StrongRetainAutoreleasedInst *
  createStrongRetainAutoreleased(SILLocation Loc, SILValue Operand) {
    return insert(new (F.getModule()) StrongRetainAutoreleasedInst(Loc, Operand));
  }
  StrongRetainUnownedInst *createStrongRetainUnowned(SILLocation Loc,
                                                     SILValue Operand) {
    return insert(new (F.getModule()) StrongRetainUnownedInst(Loc, Operand));
  }
  UnownedRetainInst *createUnownedRetain(SILLocation Loc,
                                         SILValue Operand) {
    return insert(new (F.getModule()) UnownedRetainInst(Loc, Operand));
  }
  UnownedReleaseInst *createUnownedRelease(SILLocation Loc,
                                           SILValue Operand) {
    return insert(new (F.getModule()) UnownedReleaseInst(Loc, Operand));
  }
  FixLifetimeInst *createFixLifetime(SILLocation Loc,
                                     SILValue Operand) {
    return insert(new (F.getModule()) FixLifetimeInst(Loc, Operand));
  }
  void emitFixLifetime(SILLocation Loc, SILValue Operand) {
    if (getTypeLowering(Operand.getType()).isTrivial())
      return;
    createFixLifetime(Loc, Operand);
  }
  
  DeallocStackInst *createDeallocStack(SILLocation loc, SILValue operand) {
    return insert(new (F.getModule()) DeallocStackInst(loc, operand));
  }
  DeallocRefInst *createDeallocRef(SILLocation loc, SILValue operand) {
    return insert(new (F.getModule()) DeallocRefInst(loc, operand));
  }
  DeallocBoxInst *createDeallocBox(SILLocation loc, SILType eltType,
                                   SILValue operand) {
    return insert(new (F.getModule()) DeallocBoxInst(loc, eltType, operand));
  }
  DestroyAddrInst *createDestroyAddr(SILLocation Loc, SILValue Operand) {
    return insert(new (F.getModule()) DestroyAddrInst(Loc, Operand));
  }
  
  //===--------------------------------------------------------------------===//
  // Runtime failure
  //===--------------------------------------------------------------------===//
  
  CondFailInst *createCondFail(SILLocation Loc, SILValue Operand) {
    return insert(new (F.getModule()) CondFailInst(Loc, Operand));
  }

  BuiltinFunctionRefInst *createBuiltinTrap(SILLocation Loc) {
    ASTContext &AST = F.getModule().getASTContext();
    // builtin_function_ref "int_trap" : $@thin @noreturn () -> ()
    auto bfrInfo = SILFunctionType::ExtInfo(AbstractCC::Freestanding,
                                          SILFunctionType::Representation::Thin,
                                          /*noreturn*/ true,
                                          /*autoclosure*/ false);
    SILResultInfo Result(TupleType::getEmpty(AST), ResultConvention::Unowned);
    auto bfrFnType = SILFunctionType::get(nullptr, bfrInfo,
                                          ParameterConvention::Direct_Owned,
                                          ArrayRef<SILParameterInfo>(), Result,
                                          AST);
    return createBuiltinFunctionRef(Loc, "int_trap",
                                    SILType::getPrimitiveObjectType(bfrFnType));
  }

  //===--------------------------------------------------------------------===//
  // Array indexing instructions
  //===--------------------------------------------------------------------===//

  IndexAddrInst *createIndexAddr(SILLocation loc,
                                 SILValue Operand, SILValue Index) {
    return insert(new (F.getModule()) IndexAddrInst(loc, Operand, Index));
  }

  IndexRawPointerInst *createIndexRawPointer(SILLocation loc,
                                             SILValue Operand, SILValue Index) {
    return insert(new (F.getModule()) IndexRawPointerInst(loc, Operand, Index));
  }
  
  //===--------------------------------------------------------------------===//
  // Terminator SILInstruction Creation Methods
  //===--------------------------------------------------------------------===//

  UnreachableInst *createUnreachable(SILLocation Loc) {
    return insertTerminator(new (F.getModule()) UnreachableInst(Loc));
  }

  ReturnInst *createReturn(SILLocation Loc, SILValue ReturnValue) {
    return insertTerminator(new (F.getModule()) ReturnInst(Loc, ReturnValue));
  }

  AutoreleaseReturnInst *createAutoreleaseReturn(SILLocation Loc,
                                                 SILValue ReturnValue) {
    return insertTerminator(new (F.getModule())
                              AutoreleaseReturnInst(Loc, ReturnValue));
  }
  
  CondBranchInst *createCondBranch(SILLocation Loc, SILValue Cond,
                                   SILBasicBlock *Target1,
                                   SILBasicBlock *Target2) {
    return insertTerminator(CondBranchInst::create(Loc, Cond,
                                                   Target1, Target2, F));
  }
    
  CondBranchInst *createCondBranch(SILLocation Loc, SILValue Cond,
                                   SILBasicBlock *Target1,
                                   ArrayRef<SILValue> Args1,
                                   SILBasicBlock *Target2,
                                   ArrayRef<SILValue> Args2) {
    return insertTerminator(CondBranchInst::create(Loc, Cond,
                                                   Target1, Args1,
                                                   Target2, Args2,
                                                   F));
  }

  CondBranchInst *createCondBranch(SILLocation Loc, SILValue Cond,
                                   SILBasicBlock *Target1,
                                   OperandValueArrayRef Args1,
                                   SILBasicBlock *Target2,
                                   OperandValueArrayRef Args2) {
    SmallVector<SILValue, 6> ArgsCopy1;
    SmallVector<SILValue, 6> ArgsCopy2;
    
    ArgsCopy1.reserve(Args1.size());
    ArgsCopy2.reserve(Args2.size());

    for (auto I = Args1.begin(), E = Args1.end(); I != E; ++I)
      ArgsCopy1.push_back(*I);
    for (auto I = Args2.begin(), E = Args2.end(); I != E; ++I)
      ArgsCopy2.push_back(*I);

    return insertTerminator(CondBranchInst::create(Loc, Cond,
                                                   Target1, ArgsCopy1,
                                                   Target2, ArgsCopy2,
                                                   F));
  }

  BranchInst *createBranch(SILLocation Loc, SILBasicBlock *TargetBlock) {
    return insertTerminator(BranchInst::create(Loc, TargetBlock, F));
  }

  BranchInst *createBranch(SILLocation Loc, SILBasicBlock *TargetBlock,
                           ArrayRef<SILValue> Args) {
    return insertTerminator(BranchInst::create(Loc, TargetBlock, Args, F));
  }

  BranchInst *createBranch(SILLocation Loc, SILBasicBlock *TargetBlock,
                           OperandValueArrayRef Args);

  SwitchIntInst *createSwitchInt(SILLocation Loc, SILValue Operand,
         SILBasicBlock *DefaultBB,
         ArrayRef<std::pair<APInt, SILBasicBlock*>> CaseBBs) {
    return insertTerminator(SwitchIntInst::create(Loc, Operand, DefaultBB,
                                                  CaseBBs, F));
  }

  SwitchEnumInst *createSwitchEnum(SILLocation Loc, SILValue Operand,
         SILBasicBlock *DefaultBB,
         ArrayRef<std::pair<EnumElementDecl*, SILBasicBlock*>> CaseBBs) {
    return insertTerminator(SwitchEnumInst::create(Loc, Operand, DefaultBB,
                                                    CaseBBs, F));
  }

  SwitchEnumAddrInst *
  createSwitchEnumAddr(SILLocation Loc, SILValue Operand,
         SILBasicBlock *DefaultBB,
         ArrayRef<std::pair<EnumElementDecl*, SILBasicBlock*>> CaseBBs) {
    return insertTerminator(
              SwitchEnumAddrInst::create(Loc, Operand, DefaultBB,
                                                     CaseBBs, F));
  }

  DynamicMethodBranchInst *
  createDynamicMethodBranch(SILLocation Loc, SILValue Operand,
                            SILDeclRef Member,
                            SILBasicBlock *HasMethodBB,
                            SILBasicBlock *NoMethodBB) {
    return insertTerminator(
             DynamicMethodBranchInst::create(Loc, Operand, Member, HasMethodBB,
                                             NoMethodBB, F));
  }
  
  CheckedCastBranchInst *createCheckedCastBranch(SILLocation loc,
                                                 bool isExact,
                                                 SILValue op,
                                                 SILType destTy,
                                                 SILBasicBlock *successBB,
                                                 SILBasicBlock *failureBB) {
    return insertTerminator(new (F.getModule())
                              CheckedCastBranchInst(loc, isExact, op, destTy,
                                                    successBB, failureBB));
  }

  CheckedCastAddrBranchInst *createCheckedCastAddrBranch(SILLocation loc,
                                              CastConsumptionKind consumption,
                                                         SILValue src,
                                                         CanType sourceType,
                                                         SILValue dest,
                                                         CanType targetType,
                                                 SILBasicBlock *successBB,
                                                 SILBasicBlock *failureBB) {
    return insertTerminator(new (F.getModule())
                              CheckedCastAddrBranchInst(loc, consumption,
                                                        src, sourceType,
                                                        dest, targetType,
                                                        successBB, failureBB));
  }

  //===--------------------------------------------------------------------===//
  // Memory management helpers
  //===--------------------------------------------------------------------===//

  /// Try to fold a destroy_addr operation into the previous instructions, or
  /// generate an explicit one if that fails.  If this inserts a new
  /// instruction, it returns it, otherwise it returns null.
  DestroyAddrInst *emitDestroyAddr(SILLocation Loc, SILValue Operand);

  /// Perform a strong_release instruction at the current location, attempting
  /// to fold it locally into nearby retain instructions or emitting an explicit
  /// strong release if necessary.  If this inserts a new instruction, it
  /// returns it, otherwise it returns null.
  StrongReleaseInst *emitStrongRelease(SILLocation Loc, SILValue Operand);

  /// Emit a release_value instruction at the current location, attempting to
  /// fold it locally into another nearby retain_value instruction.  This
  /// returns the new instruction if it inserts one, otherwise it returns null.
  ///
  /// This instruction doesn't handle strength reduction of release_value into
  /// a noop / strong_release / unowned_release.  For that, use the
  /// emitReleaseValueOperation method below or use the TypeLowering API.
  ReleaseValueInst *emitReleaseValue(SILLocation Loc, SILValue Operand);


  /// Convenience function for calling emitRetain on the type lowering
  /// for the non-address value.
  void emitRetainValueOperation(SILLocation loc, SILValue v) {
    assert(!v.getType().isAddress());
    auto &lowering = getTypeLowering(v.getType());
    return lowering.emitRetainValue(*this, loc, v);
  }
  
  /// Convenience function for calling TypeLowering.emitRelease on the type
  /// lowering for the non-address value.
  void emitReleaseValueOperation(SILLocation loc, SILValue v) {
    assert(!v.getType().isAddress());
    auto &lowering = getTypeLowering(v.getType());
    lowering.emitReleaseValue(*this, loc, v);
  }


  SILValue emitTupleExtract(SILLocation Loc, SILValue Operand,
                            unsigned FieldNo, SILType ResultTy) {
    // Fold tuple_extract(tuple(x,y,z),2)
    if (auto *TI = dyn_cast<TupleInst>(Operand))
      return TI->getOperand(FieldNo);
    
    return createTupleExtract(Loc, Operand, FieldNo, ResultTy);
  }
  
  SILValue emitTupleExtract(SILLocation Loc, SILValue Operand,
                            unsigned FieldNo) {
    return emitTupleExtract(Loc, Operand, FieldNo,
                            Operand.getType().getTupleElementType(FieldNo));
  }
  
  SILValue emitStructExtract(SILLocation Loc, SILValue Operand,
                             VarDecl *Field, SILType ResultTy) {
    if (auto *SI = dyn_cast<StructInst>(Operand))
      return SI->getFieldValue(Field);
    
    return createStructExtract(Loc, Operand, Field, ResultTy);
  }
  
  SILValue emitStructExtract(SILLocation Loc, SILValue Operand, VarDecl *Field){
    auto type = Operand.getType().getFieldType(Field, F.getModule());
    return emitStructExtract(Loc, Operand, Field, type);
  }

  SILValue emitThickToObjCMetatype(SILLocation Loc, SILValue Op, SILType Ty);
  SILValue emitObjCToThickMetatype(SILLocation Loc, SILValue Op, SILType Ty);

  //===--------------------------------------------------------------------===//
  // Private Helper Methods
  //===--------------------------------------------------------------------===//

private:
  /// insert - This is a template to avoid losing type info on the result.
  template <typename T>
  T *insert(T *TheInst) {
    insertImpl(TheInst);
    return TheInst;
  }
  
  /// insertTerminator - This is the same as insert, but clears the insertion
  /// point after doing the insertion.  This is used by terminators, since it
  /// isn't valid to insert something after a terminator.
  template <typename T>
  T *insertTerminator(T *TheInst) {
    insertImpl(TheInst);
    clearInsertionPoint();
    return TheInst;
  }

  void insertImpl(SILInstruction *TheInst) {
    if (BB == 0) return;

    // If the SILBuilder client wants to know about new instructions, record
    // this.
    if (InsertedInstrs)
      InsertedInstrs->push_back(TheInst);

    BB->getInstList().insert(InsertPt, TheInst);
  }
};

} // end swift namespace

#endif
