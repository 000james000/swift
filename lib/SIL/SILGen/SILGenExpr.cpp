//===--- SILGenExpr.cpp - Implements Lowering of ASTs -> SIL for Exprs ----===//
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

#include "SILGen.h"
#include "swift/AST/AST.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/TypeLowering.h"
#include "Condition.h"
#include "Initialization.h"
#include "OwnershipConventions.h"
#include "LValue.h"
#include "RValue.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace Lowering;

namespace {
  class CleanupRValue : public Cleanup {
    SILValue rv;
  public:
    CleanupRValue(SILValue rv) : rv(rv) {}
    
    void emit(SILGenFunction &gen) override {
      gen.emitReleaseRValue(SILLocation(), rv);
    }
  };
  
  class CleanupTemporaryAllocation : public Cleanup {
    SILValue alloc;
  public:
    CleanupTemporaryAllocation(SILValue alloc) : alloc(alloc) {}
    
    void emit(SILGenFunction &gen) override {
      gen.B.createDeallocVar(SILLocation(), alloc);
    }
  };
  
  class CleanupMaterializedValue : public Cleanup {
    SILValue address;
  public:
    CleanupMaterializedValue(SILValue address) : address(address) {}
    
    void emit(SILGenFunction &gen) override {
      SILValue tmpValue = gen.B.createLoad(SILLocation(), address);
      gen.emitReleaseRValue(SILLocation(), tmpValue);
    }
  };
  
  class CleanupMaterializedAddressOnlyValue : public Cleanup {
    SILValue address;
  public:
    CleanupMaterializedAddressOnlyValue(SILValue address) : address(address) {}
    
    void emit(SILGenFunction &gen) override {
      gen.B.createDestroyAddr(SILLocation(), address);
    }
  };
} // end anonymous namespace

ManagedValue SILGenFunction::emitManagedRValueWithCleanup(SILValue v) {
  if (getTypeLoweringInfo(v.getType().getSwiftRValueType()).isTrivial(SGM.M)) {
    return ManagedValue(v, ManagedValue::Unmanaged);
  } else if (v.getType().isAddressOnly(SGM.M)) {
    Cleanups.pushCleanup<CleanupMaterializedAddressOnlyValue>(v);
    return ManagedValue(v, getCleanupsDepth());
  } else {
    Cleanups.pushCleanup<CleanupRValue>(v);
    return ManagedValue(v, getCleanupsDepth());
  }
}

void SILGenFunction::emitExprInto(Expr *E, Initialization *I) {
  // FIXME: actually emit into the initialization. The initialization should
  // be passed down in the context argument to visit, and it should be the
  // visit*Expr method's responsibility to store to it if possible.
  RValue result = visit(E, SGFContext(I));
  if (result)
    std::move(result).forwardInto(*this, I);
}

RValue SILGenFunction::visit(swift::Expr *E) {
  return visit(E, SGFContext());
}

RValue SILGenFunction::visitApplyExpr(ApplyExpr *E, SGFContext C) {
  return emitApplyExpr(E, C);
}

SILValue SILGenFunction::emitEmptyTuple(SILLocation loc) {
  return B.createTuple(loc,
                       getLoweredType(TupleType::getEmpty(SGM.M.getASTContext())),
                       {});
}

SILValue SILGenFunction::emitGlobalFunctionRef(SILLocation loc,
                                               SILConstant constant) {
  assert(!LocalConstants.count(constant) &&
         "emitting ref to local constant without context?!");
  if (constant.hasDecl() &&
      isa<BuiltinModule>(constant.getDecl()->getDeclContext())) {
    return B.createBuiltinFunctionRef(loc, cast<FuncDecl>(constant.getDecl()),
                                      SGM.getConstantType(constant));
  }
  
  return B.createFunctionRef(loc, SGM.getFunction(constant));
}

SILValue SILGenFunction::emitUnmanagedFunctionRef(SILLocation loc,
                                               SILConstant constant) {
  // If this is a reference to a local constant, grab it.
  if (LocalConstants.count(constant)) {
    return LocalConstants[constant];
  }
  
  // Otherwise, use a global FunctionRefInst.
  return emitGlobalFunctionRef(loc, constant);
}

ManagedValue SILGenFunction::emitFunctionRef(SILLocation loc,
                                             SILConstant constant) {
  // If this is a reference to a local constant, grab it.
  if (LocalConstants.count(constant)) {
    SILValue v = LocalConstants[constant];
    emitRetainRValue(loc, v);
    return emitManagedRValueWithCleanup(v);
  }
  
  // Otherwise, use a global FunctionRefInst.
  SILValue c = emitGlobalFunctionRef(loc, constant);
  return ManagedValue(c, ManagedValue::Unmanaged);
}

static ManagedValue emitGlobalVariable(SILGenFunction &gen,
                                       SILLocation loc, VarDecl *var) {
  assert(!var->getDeclContext()->isLocalContext() &&
         "not a global variable!");
  assert(!var->isProperty() &&
         "not a physical global variable!");
  
  // FIXME: Always emit global variables directly. Eventually we want "true"
  // global variables to be indirectly accessed so that they can be initialized
  // on demand.
  SILValue addr = gen.B.createGlobalAddr(loc, var,
                          gen.getLoweredType(var->getType()).getAddressType());
  return ManagedValue(addr, ManagedValue::LValue);
}

ManagedValue SILGenFunction::emitReferenceToDecl(SILLocation loc,
                                                 ValueDecl *decl,
                                                 Type declType,
                                                 unsigned uncurryLevel) {
  if (!declType) declType = decl->getType();
  
  // If this is a reference to a type, produce a metatype.
  if (isa<TypeDecl>(decl)) {
    assert(decl->getType()->is<MetaTypeType>() &&
           "type declref does not have metatype type?!");
    assert((uncurryLevel == SILConstant::ConstructAtNaturalUncurryLevel
            || uncurryLevel == 0)
           && "uncurry level doesn't make sense for types");
    return ManagedValue(B.createMetatype(loc, getLoweredType(declType)),
                        ManagedValue::Unmanaged);
  }
  
  // If this is a reference to a var, produce an address.
  if (VarDecl *var = dyn_cast<VarDecl>(decl)) {
    assert((uncurryLevel == SILConstant::ConstructAtNaturalUncurryLevel
            || uncurryLevel == 0)
           && "uncurry level doesn't make sense for vars");

    assert(!var->isProperty() &&
           "property accessors should go through ");
    
    // For local decls, use the address we allocated.
    if (VarLocs.count(decl)) {
      return ManagedValue(VarLocs[decl].address, ManagedValue::LValue);
    }
    // If this is a global variable, invoke its accessor function to get its
    // address.
    return emitGlobalVariable(*this, loc, var);
  }
  
  // If the referenced decl isn't a VarDecl, it should be a constant of some
  // sort.
  assert(!decl->getTypeOfReference()->is<LValueType>() &&
         "unexpected lvalue decl ref?!");
  
  // If the referenced decl is a local func with context, then the SILConstant
  // uncurry level is one deeper (for the context vars).
  if (auto *fd = dyn_cast<FuncDecl>(decl)) {
    if (!fd->getCaptures().empty()
        && uncurryLevel != SILConstant::ConstructAtNaturalUncurryLevel)
      ++uncurryLevel;
  }

  return emitFunctionRef(loc, SILConstant(decl, uncurryLevel));
}

RValue SILGenFunction::visitDeclRefExpr(DeclRefExpr *E, SGFContext C) {
  if (E->getType()->is<LValueType>())
    return emitLValueAsRValue(E);
  return RValue(*this, emitReferenceToDecl(E, E->getDecl(), E->getType(), 0));
}

RValue SILGenFunction::visitSuperRefExpr(SuperRefExpr *E, SGFContext C) {
  if (E->getType()->is<LValueType>())
    return emitLValueAsRValue(E);
  return RValue(*this, emitReferenceToDecl(E, E->getThis(), E->getType(), 0));
}

RValue SILGenFunction::visitOtherConstructorDeclRefExpr(
                                OtherConstructorDeclRefExpr *E, SGFContext C) {
  // This should always be a child of an ApplyExpr and so will be emitted by
  // SILGenApply.
  llvm_unreachable("unapplied reference to constructor?!");
}

RValue SILGenFunction::visitIntegerLiteralExpr(IntegerLiteralExpr *E,
                                               SGFContext C) {
  return RValue(*this,
              ManagedValue(B.createIntegerLiteral(E), ManagedValue::Unmanaged));
}
RValue SILGenFunction::visitFloatLiteralExpr(FloatLiteralExpr *E,
                                             SGFContext C) {
  return RValue(*this,
              ManagedValue(B.createFloatLiteral(E), ManagedValue::Unmanaged));
}
RValue SILGenFunction::visitCharacterLiteralExpr(CharacterLiteralExpr *E,
                                                 SGFContext C)
{
  return RValue(*this,
            ManagedValue(B.createIntegerLiteral(E), ManagedValue::Unmanaged));
}
RValue SILGenFunction::visitStringLiteralExpr(StringLiteralExpr *E,
                                              SGFContext C) {
  SILType ty = getLoweredLoadableType(E->getType());
  SILValue string = B.createStringLiteral(E, ty);
  return RValue(*this, ManagedValue(string, ManagedValue::Unmanaged));
}

ManagedValue SILGenFunction::emitLoad(SILLocation loc,
                                      SILValue addr,
                                      SGFContext C,
                                      bool isTake) {
  if (addr.getType().isAddressOnly(SGM.M)) {
    // Copy the address-only value.
    SILValue copy = getBufferForExprResult(loc, addr.getType(), C);
    B.createCopyAddr(loc, addr, copy,
                     isTake,
                     /*isInitialize*/ true);
    
    return emitManagedRValueWithCleanup(copy);
  }
  
  // Load the loadable value, and retain it if we aren't taking it.
  SILValue loadedV = B.createLoad(loc, addr);
  if (!isTake)
    emitRetainRValue(loc, loadedV);
  return emitManagedRValueWithCleanup(loadedV);
}

RValue SILGenFunction::visitLoadExpr(LoadExpr *E, SGFContext C) {
  // No need to write back to a loaded lvalue.
  DisableWritebackScope scope(*this);
  
  LValue lv = emitLValue(E->getSubExpr());
  SILValue addr = emitAddressOfLValue(E, lv).getUnmanagedValue();  
  return RValue(*this, emitLoad(E, addr, C, /*isTake*/ false));
}

SILValue SILGenFunction::emitTemporaryAllocation(SILLocation loc,
                                                 SILType ty) {
  SILValue tmpMem = B.createAllocVar(loc, ty);
  Cleanups.pushCleanup<CleanupTemporaryAllocation>(tmpMem);
  return tmpMem;
}

SILValue SILGenFunction::getBufferForExprResult(
                                    SILLocation loc, SILType ty, SGFContext C) {
  // If we have a single-buffer "emit into" initialization, use that for the
  // result.
  if (Initialization *I = C.getEmitInto()) {
    switch (I->kind) {
    case Initialization::Kind::AddressBinding:
      llvm_unreachable("can't emit into address binding");

    case Initialization::Kind::Ignored:
      break;
      
    case Initialization::Kind::Tuple:
      // FIXME: For a single-element tuple, we could emit into the single field.
      
      // The tuple initialization isn't contiguous, so we can't emit directly
      // into it.
      break;

    case Initialization::Kind::SingleBuffer:
      // Emit into the buffer.
      return I->getAddress();
    }
  }
  
  // If we couldn't emit into an Initialization, emit into a temporary
  // allocation.
  return emitTemporaryAllocation(loc, ty);
}

Materialize SILGenFunction::emitMaterialize(SILLocation loc, ManagedValue v) {
  assert(!v.isLValue() && "materializing an lvalue?!");
  // Address-only values are already materialized.
  if (v.getType().isAddressOnly(SGM.M)) {
    return Materialize{v.getValue(), v.getCleanup()};
  }
  
  assert(!v.getType().isAddress() &&
         "can't materialize a reference");
  
  // We don't use getBufferForExprResult here because the result of a
  // MaterializeExpr is *not* the value, but an lvalue reference to the value.
  SILValue tmpMem = emitTemporaryAllocation(loc, v.getType());
  v.forwardInto(*this, loc, tmpMem);
  
  CleanupsDepth valueCleanup = CleanupsDepth::invalid();
  if (!getTypeLoweringInfo(v.getType().getSwiftType()).isTrivial(SGM.M)) {
    Cleanups.pushCleanup<CleanupMaterializedValue>(tmpMem);
    valueCleanup = getCleanupsDepth();
  }
  
  return Materialize{tmpMem, valueCleanup};
}

RValue SILGenFunction::visitMaterializeExpr(MaterializeExpr *E, SGFContext C) {
  // Always an lvalue.
  return emitLValueAsRValue(E);
}

RValue SILGenFunction::visitDerivedToBaseExpr(DerivedToBaseExpr *E,
                                                    SGFContext C) {
  ManagedValue original = visit(E->getSubExpr()).getAsSingleValue(*this);
  SILValue converted = B.createUpcast(E,
                                   original.getValue(),
                                   getLoweredType(E->getType()));
  return RValue(*this, ManagedValue(converted, original.getCleanup()));
}

RValue SILGenFunction::visitMetatypeConversionExpr(MetatypeConversionExpr *E,
                                                   SGFContext C) {
  SILValue metaBase = visit(E->getSubExpr()).getUnmanagedSingleValue(*this);
  return RValue(*this,
                ManagedValue(B.createUpcast(E, metaBase,
                                          getLoweredLoadableType(E->getType())),
                             ManagedValue::Unmanaged));
}

RValue SILGenFunction::visitArchetypeToSuperExpr(ArchetypeToSuperExpr *E,
                                                 SGFContext C) {
  ManagedValue archetype = visit(E->getSubExpr()).getAsSingleValue(*this);
  // Replace the cleanup with a new one on the base class value so we always use
  // concrete retain/release operations.
  SILValue base = B.createArchetypeRefToSuper(E,
                                        archetype.forward(*this),
                                        getLoweredLoadableType(E->getType()));
  return RValue(*this, emitManagedRValueWithCleanup(base));
}

RValue SILGenFunction::visitRequalifyExpr(RequalifyExpr *E, SGFContext C) {
  assert(E->getType()->is<LValueType>() && "non-lvalue requalify");
  // Ignore lvalue qualifiers.
  return visit(E->getSubExpr());
}

RValue SILGenFunction::visitFunctionConversionExpr(FunctionConversionExpr *e,
                                                   SGFContext C)
{
  ManagedValue original = visit(e->getSubExpr()).getAsSingleValue(*this);
  
  // Retain the thinness of the original function type.
  Type destTy = e->getType();
  if (original.getType().castTo<FunctionType>()->isThin())
    destTy = getThinFunctionType(destTy);
  
  SILValue converted = B.createConvertFunction(e, original.getValue(),
                                               getLoweredType(destTy));
  return RValue(*this, ManagedValue(converted, original.getCleanup()));
}

namespace {
  /// An Initialization representing the concrete value buffer inside an
  /// existential container.
  class ExistentialValueInitialization : public SingleInitializationBase {
    SILValue valueAddr;
  public:
    ExistentialValueInitialization(SILValue valueAddr)
      : SingleInitializationBase(valueAddr.getType().getSwiftRValueType()),
        valueAddr(valueAddr)
    {}
    
    SILValue getAddressOrNull() override {
      return valueAddr;
    }
    
    void finishInitialization(SILGenFunction &gen) {
      // FIXME: Disable the DeinitExistential cleanup and enable the
      // DestroyAddr cleanup for the existential container.
    }
  };
}

static RValue emitClassBoundErasure(SILGenFunction &gen, ErasureExpr *E) {
  ManagedValue sub = gen.visit(E->getSubExpr()).getAsSingleValue(gen);
  SILType resultTy = gen.getLoweredLoadableType(E->getType());
  
  SILValue v;
  
  if (E->getSubExpr()->getType()->isExistentialType())
    // If the source value is already of protocol type, we can use
    // upcast_existential_ref to steal the already-initialized witness tables
    // and concrete value.
    v = gen.B.createUpcastExistentialRef(E, sub.getValue(), resultTy);
  else
    // Otherwise, create a new existential container value around the class
    // instance.
    v = gen.B.createInitExistentialRef(E, resultTy, sub.getValue(),
                                       E->getConformances());

  return RValue(gen, ManagedValue(v, sub.getCleanup()));
}

static RValue emitAddressOnlyErasure(SILGenFunction &gen, ErasureExpr *E,
                                     SGFContext C) {
  // FIXME: Need to stage cleanups here. If code fails between
  // InitExistential and initializing the value, clean up using
  // DeinitExistential.
  
  // Allocate the existential.
  SILValue existential = gen.getBufferForExprResult(E,
                                              gen.getLoweredType(E->getType()),
                                              C);
  
  if (E->getSubExpr()->getType()->isExistentialType()) {
    // If the source value is already of a protocol type, we can use
    // upcast_existential to steal its already-initialized witness tables and
    // concrete value.
    ManagedValue subExistential
      = gen.visit(E->getSubExpr()).getAsSingleValue(gen);

    gen.B.createUpcastExistential(E, subExistential.getValue(), existential,
                                  /*isTake=*/subExistential.hasCleanup());
  } else {
    // Otherwise, we need to initialize a new existential container from
    // scratch.
    
    // Allocate the concrete value inside the container.
    SILValue valueAddr = gen.B.createInitExistential(E, existential,
                                gen.getLoweredType(E->getSubExpr()->getType()),
                                E->getConformances());
    // Initialize the concrete value in-place.
    InitializationPtr init(new ExistentialValueInitialization(valueAddr));
    gen.emitExprInto(E->getSubExpr(), init.get());
    init->finishInitialization(gen);
  }
  
  return RValue(gen, gen.emitManagedRValueWithCleanup(existential));
}

RValue SILGenFunction::visitErasureExpr(ErasureExpr *E, SGFContext C) {
  if (E->getType()->isClassExistentialType())
    return emitClassBoundErasure(*this, E);
  return emitAddressOnlyErasure(*this, E, C);
}

RValue SILGenFunction::visitCoerceExpr(CoerceExpr *E, SGFContext C) {
  return visit(E->getSubExpr(), C);
}

namespace {
  class CleanupUsedExistentialContainer : public Cleanup {
    SILValue existential;
  public:
    CleanupUsedExistentialContainer(SILValue existential)
      : existential(existential) {}
    
    void emit(SILGenFunction &gen) override {
      gen.B.createDeinitExistential(SILLocation(), existential);
    }
  };
}

/// \brief Emit the cast instruction appropriate to the kind of checked cast.
///
/// \param loc          The AST location associated with the operation.
/// \param originalMV   The value to cast.
/// \param origTy       The original AST-level type.
/// \param castTy       The destination type.
/// \param kind         The semantics of the cast.
/// \param mode         Whether to emit an unconditional or conditional cast.
/// \param useCastValue If true, the cleanup on the original value will be
///                     disabled, and the callee will be expected to take
///                     ownership of the returned value. If false, the original
///                     value's cleanup is left intact, and an unowned reference
///                     or address is returned.
SILValue SILGenFunction::emitCheckedCast(SILLocation loc,
                                         ManagedValue originalMV,
                                         Type origTy,
                                         Type castTy,
                                         CheckedCastKind kind,
                                         CheckedCastMode mode,
                                         bool useCastValue) {
  SILValue original = useCastValue
    ? originalMV.forward(*this)
    : originalMV.getValue();
  
  switch (kind) {
  case CheckedCastKind::Unresolved:
  case CheckedCastKind::InvalidCoercible:
    llvm_unreachable("invalid checked cast?!");
      
  case CheckedCastKind::Downcast:
    return B.createDowncast(loc, original,
                            getLoweredLoadableType(castTy), mode);
  case CheckedCastKind::SuperToArchetype:
    return B.createSuperToArchetypeRef(loc, original,
                                       getLoweredLoadableType(castTy), mode);
  case CheckedCastKind::ArchetypeToArchetype:
  case CheckedCastKind::ArchetypeToConcrete:
    if (origTy->castTo<ArchetypeType>()->requiresClass()) {
      return B.createDowncastArchetypeRef(loc, original,
                                getLoweredLoadableType(castTy), mode);
    } else {
      SILType loweredTy = getLoweredType(castTy);
      SILValue cast = B.createDowncastArchetypeAddr(loc, original,
                                             loweredTy.getAddressType(), mode);
      if (useCastValue && loweredTy.isLoadable(F.getModule()))
        cast = B.createLoad(loc, cast);
      return cast;
    }
  
  case CheckedCastKind::ExistentialToArchetype:
  case CheckedCastKind::ExistentialToConcrete:
    if (origTy->isClassExistentialType()) {
      return B.createDowncastExistentialRef(loc, original,
                                getLoweredLoadableType(castTy), mode);
    } else {
      // Project the concrete value address out of the container.
      SILType loweredTy = getLoweredType(castTy);
      SILValue cast = B.createProjectDowncastExistentialAddr(loc, original,
                                             loweredTy.getAddressType(), mode);
      if (useCastValue) {
        if (loweredTy.isLoadable(F.getModule()))
          cast = B.createLoad(loc, cast);
      
        // We'll pass on ownership of the contained value, but we still need to
        // deallocate the existential buffer when we're done.
        Cleanups.pushCleanup<CleanupUsedExistentialContainer>(original);
      }

      return cast;
    }
  }
}

RValue SILGenFunction::visitUnconditionalCheckedCastExpr(
                                               UnconditionalCheckedCastExpr *E,
                                               SGFContext C) {
  // Disable the original cleanup because the cast-to type is more specific and
  // should have a more efficient cleanup.
  ManagedValue original = visit(E->getSubExpr()).getAsSingleValue(*this);
  SILValue cast = emitCheckedCast(E, original,
                                  E->getSubExpr()->getType(),
                                  E->getCastTypeLoc().getType(),
                                  E->getCastKind(),
                                  CheckedCastMode::Unconditional,
                                  /*useCastValue*/ true);
  return RValue(*this, emitManagedRValueWithCleanup(cast));
}

RValue SILGenFunction::visitIsaExpr(IsaExpr *E, SGFContext C) {
  // Cast the value using a conditional cast.
  ManagedValue original = visit(E->getSubExpr()).getAsSingleValue(*this);
  SILValue cast = emitCheckedCast(E, original,
                                  E->getSubExpr()->getType(),
                                  E->getCastTypeLoc().getType(),
                                  E->getCastKind(),
                                  CheckedCastMode::Conditional,
                                  /*useCastValue*/ false);
  // Check the result.
  SILValue is = B.createIsNonnull(E, cast,
                                  getLoweredLoadableType(E->getType()));
  return RValue(*this, emitManagedRValueWithCleanup(is));
}

RValue SILGenFunction::visitParenExpr(ParenExpr *E, SGFContext C) {
  return visit(E->getSubExpr(), C);
}

static ManagedValue emitVarargs(SILGenFunction &gen,
                                SILLocation loc,
                                Type baseTy,
                                ArrayRef<ManagedValue> elements,
                                Expr *VarargsInjectionFn) {
  SILValue numEltsVal = gen.B.createIntegerLiteral(SILLocation(),
                      SILType::getBuiltinIntegerType(64, gen.F.getASTContext()),
                      elements.size());
  AllocArrayInst *allocArray = gen.B.createAllocArray(loc,
                                                  gen.getLoweredType(baseTy),
                                                  numEltsVal);
  // The first result is the owning ObjectPointer for the array.
  ManagedValue objectPtr
    = gen.emitManagedRValueWithCleanup(SILValue(allocArray, 0));
  // The second result is a RawPointer to the base address of the array.
  SILValue basePtr(allocArray, 1);

  for (size_t i = 0, size = elements.size(); i < size; ++i) {
    SILValue eltPtr = basePtr;
    if (i != 0) {
      SILValue index = gen.B.createIntegerLiteral(loc,
                  SILType::getBuiltinIntegerType(64, gen.F.getASTContext()), i);
      eltPtr = gen.B.createIndexAddr(loc, basePtr, index);
    }
    ManagedValue v = elements[i];
    v.forwardInto(gen, loc, eltPtr);
  }

  return gen.emitArrayInjectionCall(objectPtr, basePtr,
                                    numEltsVal, VarargsInjectionFn);
}

RValue SILGenFunction::visitTupleExpr(TupleExpr *E, SGFContext C) {
  // If we have an Initialization, emit the tuple elements into its elements.
  if (Initialization *I = C.getEmitInto()) {
    SmallVector<InitializationPtr, 4> subInitializationBuf;
    auto subInitializations = I->getSubInitializations(*this,
                                                       subInitializationBuf);
    assert(subInitializations.size() == E->getElements().size() &&
           "initialization for tuple has wrong number of elements");
    for (unsigned i = 0, size = subInitializations.size(); i < size; ++i) {
      emitExprInto(E->getElements()[i], subInitializations[i].get());
    }
    I->finishInitialization(*this);
    return RValue();
  }
  
  RValue result(E->getType()->getCanonicalType());
  for (Expr *elt : E->getElements()) {
    result.addElement(visit(elt));
  }
  return result;
}

RValue SILGenFunction::visitSpecializeExpr(SpecializeExpr *E,
                                           SGFContext C) {
  SILValue unspecialized
    = visit(E->getSubExpr()).getUnmanagedSingleValue(*this);
  SILType specializedType
    = getLoweredLoadableType(getThinFunctionType(E->getType()));
  SILValue spec = B.createSpecialize(E,
                                     unspecialized,
                                     E->getSubstitutions(),
                                     specializedType);
  return RValue(*this, ManagedValue(spec, ManagedValue::Unmanaged));
}

RValue SILGenFunction::visitAddressOfExpr(AddressOfExpr *E,
                                          SGFContext C) {
  return emitLValueAsRValue(E);
}

ManagedValue SILGenFunction::emitMethodRef(SILLocation loc,
                                           SILValue thisValue,
                                           SILConstant methodConstant,
                                           ArrayRef<Substitution> innerSubs) {
  // FIXME: Emit dynamic dispatch instruction (class_method, super_method, etc.)
  // if needed.
  
  SILValue methodValue = B.createFunctionRef(loc,
                                             SGM.getFunction(methodConstant));
  SILType methodType = SGM.getConstantType(methodConstant.atUncurryLevel(0));
  
  /// If the 'this' type is a bound generic, specialize the method ref with
  /// its substitutions.
  ArrayRef<Substitution> outerSubs;
  
  Type innerMethodTy = methodType.castTo<AnyFunctionType>()->getResult();
  
  if (!innerSubs.empty()) {
    // Specialize the inner method type.
    // FIXME: This assumes that 'innerSubs' is an identity mapping, which is
    // true for generic allocating constructors calling initializers but not in
    // general.
    
    PolymorphicFunctionType *innerPFT
      = innerMethodTy->castTo<PolymorphicFunctionType>();
    innerMethodTy = FunctionType::get(innerPFT->getInput(),
                                      innerPFT->getResult(),
                                      F.getASTContext());
  }
  
  Type outerMethodTy = FunctionType::get(thisValue.getType().getSwiftType(),
                                         innerMethodTy,
                                         /*isAutoClosure*/ false,
                                         /*isBlock*/ false,
                                         /*isThin*/ true,
                                         methodType.getAbstractCC(),
                                         F.getASTContext());

  if (BoundGenericType *bgt = thisValue.getType().getAs<BoundGenericType>())
    outerSubs = bgt->getSubstitutions();
  
  if (!innerSubs.empty() || !outerSubs.empty()) {
    // Specialize the generic method.
    ArrayRef<Substitution> allSubs;
    if (outerSubs.empty())
      allSubs = innerSubs;
    else if (innerSubs.empty())
      allSubs = outerSubs;
    else {
      Substitution *allSubsBuf
        = F.getASTContext().Allocate<Substitution>(outerSubs.size()
                                                  + innerSubs.size());
      std::memcpy(allSubsBuf,
                  outerSubs.data(), outerSubs.size() * sizeof(Substitution));
      std::memcpy(allSubsBuf + outerSubs.size(),
                  innerSubs.data(), innerSubs.size() * sizeof(Substitution));
      allSubs = {allSubsBuf, outerSubs.size()+innerSubs.size()};
    }
    
    SILType specType = getLoweredLoadableType(outerMethodTy,
                                              methodConstant.uncurryLevel);
    
    methodValue = B.createSpecialize(loc, methodValue, allSubs, specType);
  }

  return ManagedValue(methodValue, ManagedValue::Unmanaged);
}

RValue SILGenFunction::visitMemberRefExpr(MemberRefExpr *E,
                                          SGFContext C) {
  return emitLValueAsRValue(E);
}

RValue SILGenFunction::visitGenericMemberRefExpr(GenericMemberRefExpr *E,
                                                 SGFContext C)
{
  if (E->getBase()->getType()->is<MetaTypeType>()) {
    assert(E->getType()->is<MetaTypeType>() &&
           "generic_member_ref of metatype should give metatype");
    // If the base and member are metatypes, emit an associated_metatype inst
    // to extract the associated type from the type metadata.
    SILValue baseMetatype = visit(E->getBase()).getUnmanagedSingleValue(*this);
    return RValue(*this,
                  ManagedValue(B.createAssociatedMetatype(E,
                             baseMetatype,
                             getLoweredLoadableType(E->getType())),
                        ManagedValue::Unmanaged));

  }
  return emitLValueAsRValue(E);
}

RValue SILGenFunction::visitArchetypeMemberRefExpr(ArchetypeMemberRefExpr *E,
                                                   SGFContext C) {
  SILValue archetype = visit(E->getBase()).getUnmanagedSingleValue(*this);
  assert((archetype.getType().isAddress() ||
          archetype.getType().is<MetaTypeType>()) &&
         "archetype must be an address or metatype");
  // FIXME: curried archetype
  // FIXME: archetype properties
  (void)archetype;
  llvm_unreachable("unapplied archetype method not implemented");
}

RValue SILGenFunction::visitExistentialMemberRefExpr(
                                                 ExistentialMemberRefExpr *E,
                                                 SGFContext C) {
  SILValue existential = visit(E->getBase()).getUnmanagedSingleValue(*this);
  //SILValue projection = B.createProjectExistential(E, existential);
  //SILValue method = emitProtocolMethod(E, existential);
  // FIXME: curried existential
  // FIXME: existential properties
  (void)existential;
  llvm_unreachable("unapplied protocol method not implemented");
}

RValue SILGenFunction::visitDotSyntaxBaseIgnoredExpr(
                                                  DotSyntaxBaseIgnoredExpr *E,
                                                  SGFContext C) {
  visit(E->getLHS());
  return visit(E->getRHS());
}

RValue SILGenFunction::visitModuleExpr(ModuleExpr *E, SGFContext C) {
  SILValue module = B.createModule(E, getLoweredLoadableType(E->getType()));
  return RValue(*this, ManagedValue(module, ManagedValue::Unmanaged));
}

RValue SILGenFunction::visitSubscriptExpr(SubscriptExpr *E,
                                          SGFContext C) {
  return emitLValueAsRValue(E);
}

RValue SILGenFunction::visitGenericSubscriptExpr(GenericSubscriptExpr *E,
                                                 SGFContext C)
{
  return emitLValueAsRValue(E);
}

RValue SILGenFunction::visitTupleElementExpr(TupleElementExpr *E,
                                             SGFContext C) {
  if (E->getType()->is<LValueType>()) {
    return emitLValueAsRValue(E);
  } else {
    return visit(E->getBase()).extractElement(E->getFieldNumber());
  }
}

RValue SILGenFunction::visitTupleShuffleExpr(TupleShuffleExpr *E,
                                             SGFContext C) {
  /* TODO:
  // If we're emitting into an initialization, we can try shuffling the
  // elements of the initialization.
  if (Initialization *I = C.getEmitInto()) {
    emitTupleShuffleExprInto(*this, E, I);
    return RValue();
  }
   */

  // Emit the sub-expression tuple and destructure it into elements.
  SmallVector<RValue, 4> elements;
  visit(E->getSubExpr()).extractElements(elements);
  
  // Prepare a new tuple to hold the shuffled result.
  RValue result(E->getType()->getCanonicalType());
  
  auto outerFields = E->getType()->castTo<TupleType>()->getFields();
  auto shuffleIndexIterator = E->getElementMapping().begin(),
    shuffleIndexEnd = E->getElementMapping().end();
  for (auto &field : outerFields) {
    assert(shuffleIndexIterator != shuffleIndexEnd &&
           "ran out of shuffle indexes before running out of fields?!");
    int shuffleIndex = *shuffleIndexIterator++;
    
    // If the shuffle index is DefaultInitialize, we're supposed to use the
    // default value.
    if (shuffleIndex == TupleShuffleExpr::DefaultInitialize) {
      // If magic identifiers like __FILE__ are expanded in this default
      // argument, have them use the location of this expression, not their
      // location.
      llvm::SaveAndRestore<SourceLoc> Save(overrideLocationForMagicIdentifiers);
      overrideLocationForMagicIdentifiers = E->getStartLoc();
      
      assert(field.hasInit() && "no default initializer for field!");
      result.addElement(visit(field.getInit()->getExpr()));
      continue;
    }

    // If the shuffle index is FirstVariadic, it is the beginning of the list of
    // varargs inputs.  Save this case for last.
    if (shuffleIndex != TupleShuffleExpr::FirstVariadic) {
      // Map from a different tuple element.
      result.addElement(std::move(elements[shuffleIndex]));
      continue;
    }

    assert(field.isVararg() && "Cannot initialize nonvariadic element");
    
    // Okay, we have a varargs tuple element.  All the remaining elements feed
    // into the varargs portion of this, which is then constructed into a Slice
    // through an informal protocol captured by the InjectionFn in the
    // TupleShuffleExpr.
    assert(E->getVarargsInjectionFunction() &&
           "no injection function for varargs tuple?!");
    SmallVector<ManagedValue, 4> variadicValues;
    
    while (shuffleIndexIterator != shuffleIndexEnd) {
      unsigned sourceField = *shuffleIndexIterator++;
      variadicValues.push_back(
                     std::move(elements[sourceField]).getAsSingleValue(*this));
    }
    
    ManagedValue varargs = emitVarargs(*this, E, field.getVarargBaseTy(),
                                       variadicValues,
                                       E->getVarargsInjectionFunction());
    result.addElement(RValue(*this, varargs));
    break;
  }
  
  return result;
}

static void emitScalarToTupleExprInto(SILGenFunction &gen,
                                      ScalarToTupleExpr *E,
                                      Initialization *I) {
  auto outerFields = E->getType()->castTo<TupleType>()->getFields();
  bool isScalarFieldVariadic = outerFields[E->getScalarField()].isVararg();

  // Decompose the initialization.
  SmallVector<InitializationPtr, 4> subInitializationBuf;
  auto subInitializations = I->getSubInitializations(gen, subInitializationBuf);
  assert(subInitializations.size() == outerFields.size() &&
         "initialization size does not match tuple size?!");
  
  // If the scalar field isn't variadic, emit it into the destination field of
  // the tuple.
  Initialization *scalarInit = subInitializations[E->getScalarField()].get();
  if (!isScalarFieldVariadic)
    gen.emitExprInto(E->getSubExpr(), scalarInit);
  else {
    // Otherwise, create the vararg and store it to the vararg field.
    ManagedValue scalar = gen.visit(E->getSubExpr()).getAsSingleValue(gen);
    ManagedValue varargs = emitVarargs(gen, E, E->getSubExpr()->getType(),
                                       scalar, E->getVarargsInjectionFunction());
    varargs.forwardInto(gen, E, scalarInit->getAddress());
    scalarInit->finishInitialization(gen);
  }
  
  // Emit the non-scalar fields.
  for (unsigned i = 0, e = outerFields.size(); i != e; ++i) {
    if (i == E->getScalarField())
      continue;
    // Fill the vararg field with an empty array.
    if (outerFields[i].isVararg()) {
      assert(i == e - 1 && "vararg isn't last?!");
      ManagedValue varargs = emitVarargs(gen, E, outerFields[i].getVarargBaseTy(),
                                         {}, E->getVarargsInjectionFunction());
      varargs.forwardInto(gen, E, subInitializations[i]->getAddress());
      subInitializations[i]->finishInitialization(gen);
    }
    // Evaluate default initializers in-place.
    else {
      assert(outerFields[i].hasInit() &&
             "no default initializer in non-scalar field of scalar-to-tuple?!");
      gen.emitExprInto(outerFields[i].getInit()->getExpr(),
                       subInitializations[i].get());
    }
  }
  
  // Finish the aggregate initialization.
  I->finishInitialization(gen);
}

RValue SILGenFunction::visitScalarToTupleExpr(ScalarToTupleExpr *E,
                                              SGFContext C) {
  // If we're emitting into an Initialization, we can decompose the
  // initialization.
  if (Initialization *I = C.getEmitInto()) {
    emitScalarToTupleExprInto(*this, E, I);
    return RValue();
  }
  
  // Emit the scalar member.
  RValue scalar = visit(E->getSubExpr());

  // Prepare a tuple rvalue to house the result.
  RValue result(E->getType()->getCanonicalType());
  
  // Create a tuple around the scalar along with any
  // default values or varargs.
  auto outerFields = E->getType()->castTo<TupleType>()->getFields();
  for (unsigned i = 0, e = outerFields.size(); i != e; ++i) {
    // Handle the variadic argument. If we didn't emit the scalar field yet,
    // it goes into the variadic array; otherwise, the variadic array is empty.
    if (outerFields[i].isVararg()) {
      assert(i == e - 1 && "vararg isn't last?!");
      ManagedValue varargs;
      if (!scalar.isUsed())
        varargs = emitVarargs(*this, E, outerFields[i].getVarargBaseTy(),
                              std::move(scalar).getAsSingleValue(*this),
                              E->getVarargsInjectionFunction());
      else
        varargs = emitVarargs(*this, E, outerFields[i].getVarargBaseTy(),
                              {}, E->getVarargsInjectionFunction());
      result.addElement(RValue(*this, varargs));
      break;
    }

    // Add the scalar to the tuple in the right place.
    else if (i == E->getScalarField()) {
      result.addElement(std::move(scalar));
    }
    // Fill in the other fields with their default initializers.
    else {
      assert(outerFields[i].hasInit() &&
             "no default initializer in non-scalar field of scalar-to-tuple?!");
      result.addElement(visit(outerFields[i].getInit()->getExpr()));
    }
  }

  return result;
}

RValue SILGenFunction::visitNewArrayExpr(NewArrayExpr *E, SGFContext C) {
  SILValue NumElements = visit(E->getBounds()[0].Value)
    .getAsSingleValue(*this)
    .getValue();

  // Allocate the array.
  AllocArrayInst *AllocArray = B.createAllocArray(E,
                                            getLoweredType(E->getElementType()),
                                            NumElements);

  ManagedValue ObjectPtr
    = emitManagedRValueWithCleanup(SILValue(AllocArray, 0));
  SILValue BasePtr(AllocArray, 1);

  // FIXME: We need to initialize the elements of the array that are now
  // allocated.

  // Finally, build and return a Slice instance using the object
  // header/base/count.
  return RValue(*this, emitArrayInjectionCall(ObjectPtr, BasePtr, NumElements,
                                              E->getInjectionFunction()));
}

SILValue SILGenFunction::emitMetatypeOfValue(SILLocation loc, SILValue base) {
  // For class, archetype, and protocol types, look up the dynamic metatype.
  SILType metaTy = getLoweredLoadableType(
    MetaTypeType::get(base.getType().getSwiftRValueType(), F.getASTContext()));
  if (base.getType().getSwiftType()->getClassOrBoundGenericClass()) {
    return B.createClassMetatype(loc, metaTy, base);
  } else if (base.getType().getSwiftRValueType()->is<ArchetypeType>()) {
    return B.createArchetypeMetatype(loc, metaTy, base);
  } else if (base.getType().getSwiftRValueType()->isExistentialType()) {
    return B.createProtocolMetatype(loc, metaTy, base);
  }
  // Otherwise, ignore the base and return the static metatype.
  return B.createMetatype(loc, metaTy);
}

RValue SILGenFunction::visitMetatypeExpr(MetatypeExpr *E, SGFContext C) {
  // Evaluate the base if present.
  SILValue metatype;
  
  if (E->getBase()) {
    SILValue base = visit(E->getBase()).getAsSingleValue(*this).getValue();
    metatype = emitMetatypeOfValue(E, base);
  } else {
    metatype = B.createMetatype(E, getLoweredLoadableType(E->getType()));
  }
  
  return RValue(*this, ManagedValue(metatype, ManagedValue::Unmanaged));
}

ManagedValue SILGenFunction::emitClosureForCapturingExpr(SILLocation loc,
                                             SILConstant constant,
                                             ArrayRef<Substitution> forwardSubs,
                                             CapturingExpr *body) {
  // FIXME: Stash the capture args somewhere and curry them on demand rather
  // than here.
  assert(((constant.uncurryLevel == 1 && !body->getCaptures().empty())
          || (constant.uncurryLevel == 0 && body->getCaptures().empty()))
         && "curried local functions not yet supported");
  
  SILValue functionRef = emitGlobalFunctionRef(loc, constant);
  
  // Forward substitutions from the outer scope.
  
  // FIXME: ImplicitClosureExprs appear to always have null parent decl
  // contexts, so getFunctionTypeWithCaptures is unable to find contextual
  // generic parameters for them. The getAs null check here should be
  // unnecessary.
  auto *pft = SGM.getConstantType(constant).getAs<PolymorphicFunctionType>();
  
  if (pft && !forwardSubs.empty()) {
    FunctionType *specialized = FunctionType::get(pft->getInput(),
                                                  pft->getResult(),
                                                  /*autoClosure*/ false,
                                                  /*isBlock*/ false,
                                                  /*isThin*/ true,
                                                  pft->getAbstractCC(),
                                                  F.getASTContext());
    functionRef = B.createSpecialize(loc, functionRef, forwardSubs,
                                     getLoweredLoadableType(specialized));
  }

  auto captures = body->getCaptures();
  if (!captures.empty()) {    
    llvm::SmallVector<SILValue, 4> capturedArgs;
    for (ValueDecl *capture : captures) {
      switch (getDeclCaptureKind(capture)) {
        case CaptureKind::Box: {
          // LValues are captured as both the box owning the value and the
          // address of the value.
          assert(VarLocs.count(capture) &&
                 "no location for captured var!");
          
          VarLoc const &vl = VarLocs[capture];
          assert(vl.box && "no box for captured var!");
          assert(vl.address && "no address for captured var!");
          B.createRetain(loc, vl.box);
          capturedArgs.push_back(vl.box);
          capturedArgs.push_back(vl.address);
          break;
        }
        case CaptureKind::Byref: {
          // Byrefs are captured by address only.
          assert(VarLocs.count(capture) &&
                 "no location for captured byref!");
          capturedArgs.push_back(VarLocs[capture].address);
          break;
        }
        case CaptureKind::Constant: {
          // SILValue is a constant such as a local func. Pass on the reference.
          ManagedValue v = emitReferenceToDecl(loc, capture);
          capturedArgs.push_back(v.forward(*this));
          break;
        }
        case CaptureKind::GetterSetter: {
          // Pass the setter and getter closure references on.
          ManagedValue v = emitFunctionRef(loc, SILConstant(capture,
                                                   SILConstant::Kind::Setter));
          capturedArgs.push_back(v.forward(*this));
          SWIFT_FALLTHROUGH;
        }
        case CaptureKind::Getter: {
          // Pass the getter closure reference on.
          ManagedValue v = emitFunctionRef(loc, SILConstant(capture,
                                                   SILConstant::Kind::Getter));
          capturedArgs.push_back(v.forward(*this));
          break;
        }
      }
    }
    
    SILType closureTy = getLoweredLoadableType(body->getType());
    return emitManagedRValueWithCleanup(
                    B.createPartialApply(loc, functionRef, capturedArgs,
                                         closureTy));
  } else {
    return ManagedValue(functionRef, ManagedValue::Unmanaged);
  }
}

RValue SILGenFunction::visitFuncExpr(FuncExpr *e, SGFContext C) {
  // Generate the local function body.
  SGM.emitFunction(e, e);

  // Generate the closure (if any) for the function reference.
  return RValue(*this, emitClosureForCapturingExpr(e, SILConstant(e),
                                                   getForwardingSubstitutions(),
                                                   e));
}

RValue SILGenFunction::visitPipeClosureExpr(PipeClosureExpr *e, SGFContext C) {
  // Generate the closure function.
  SGM.emitClosure(e);

  // Generate the closure value (if any) for the closure expr's function
  // reference.
  return RValue(*this, emitClosureForCapturingExpr(e, SILConstant(e),
                                                   getForwardingSubstitutions(),
                                                   e));
}

RValue SILGenFunction::visitClosureExpr(ClosureExpr *e, SGFContext C) {
  // Generate the closure body.
  SGM.emitClosure(e);
  
  // Generate the closure value (if any) for the closure expr's function
  // reference.
  return RValue(*this, emitClosureForCapturingExpr(e, SILConstant(e),
                                                   getForwardingSubstitutions(),
                                                   e));
}

void SILGenFunction::emitFunction(FuncExpr *fe) {
  emitProlog(fe, fe->getBodyParamPatterns(), fe->getResultType(F.getASTContext()));
  visit(fe->getBody());
}

void SILGenFunction::emitClosure(PipeClosureExpr *ce) {
  emitProlog(ce, ce->getParams(), ce->getResultType());
  visit(ce->getBody());
}

void SILGenFunction::emitClosure(ClosureExpr *ce) {
  emitProlog(ce, ce->getParamPatterns(),
             ce->getType()->castTo<FunctionType>()->getResult());

  // Closure expressions implicitly return the result of their body expression.
  emitReturnExpr(ce, ce->getBody());
  
  assert(!B.hasValidInsertionPoint() &&
         "returning closure body did not terminate closure?!");
}

bool SILGenFunction::emitEpilogBB(SILLocation loc) {
  assert(epilogBB && "no epilog bb to emit?!");
  
  // If the epilog was not branched to at all, just unwind like a "return"
  // and emit the epilog into the current BB.
  if (epilogBB->pred_empty()) {
    epilogBB->eraseFromParent();

    // If the current bb is terminated then the epilog is just unreachable.
    if (!B.hasValidInsertionPoint())
      return false;
    
    Cleanups.emitCleanupsForReturn(loc);
  } else {
    // If the body didn't explicitly return, we need to branch out of it as if
    // returning. emitReturnAndCleanups will do that.
    if (B.hasValidInsertionPoint())
      Cleanups.emitReturnAndCleanups(loc, SILValue());
    // Emit the epilog into the epilog bb.
    B.emitBlock(epilogBB);
  }
  return true;
}

void SILGenFunction::emitDestructor(ClassDecl *cd, DestructorDecl *dd) {
  SILValue thisValue = emitDestructorProlog(cd, dd);

  // Create a basic block to jump to for the implicit destruction behavior
  // of releasing the elements and calling the base class destructor.
  // We won't actually emit the block until we finish with the destructor body.
  epilogBB = new (SGM.M) SILBasicBlock(&F);
  
  // Emit the destructor body, if any.
  if (dd)
    visit(dd->getBody());
  
  if (!emitEpilogBB(dd))
    return;
  
  // Release our members.
  // FIXME: generic params
  // FIXME: Can a destructor always consider its fields fragile like this?
  for (Decl *member : cd->getMembers()) {
    if (VarDecl *vd = dyn_cast<VarDecl>(member)) {
      if (vd->isProperty())
        continue;
      const TypeLoweringInfo &ti = getTypeLoweringInfo(vd->getType());
      if (!ti.isTrivial(SGM.M)) {
        SILValue addr = B.createRefElementAddr(dd, thisValue, vd,
                                          ti.getLoweredType().getAddressType());
        if (ti.isAddressOnly(SGM.M)) {
          B.createDestroyAddr(dd, addr);
        } else {
          SILValue field = B.createLoad(dd, addr);
          emitReleaseRValue(dd, field);
        }
      }
    }
  }
  
  // If we have a base class, invoke its destructor.
  SILType objectPtrTy = SILType::getObjectPointerType(F.getASTContext());
  if (Type baseTy = cd->getBaseClass()) {
    ClassDecl *baseClass = baseTy->getClassOrBoundGenericClass();
    
    // FIXME: We can't sensibly call up to ObjC dealloc methods right now
    // because they aren't really destroying destructors.
    if (baseClass->hasClangNode() && baseClass->isObjC()) {
      thisValue = B.createRefToObjectPointer(dd, thisValue, objectPtrTy);
      B.createReturn(dd, thisValue);
      return;
    }
    
    SILConstant dtorConstant =
      SILConstant(baseClass, SILConstant::Kind::Destroyer);
    SILType baseSILTy = getLoweredLoadableType(baseTy);
    SILValue baseThis = B.createUpcast(dd, thisValue, baseSILTy);
    ManagedValue dtorValue = emitMethodRef(dd, baseThis, dtorConstant,
                                           /*innerSubstitutions*/ {});
    thisValue = B.createApply(dd, dtorValue.forward(*this),
                              objectPtrTy,
                              baseThis);
  } else {
    thisValue = B.createRefToObjectPointer(dd, thisValue, objectPtrTy);
  }
  B.createReturn(dd, thisValue);
}

static void emitConstructorMetatypeArg(SILGenFunction &gen,
                                       ConstructorDecl *ctor) {
  // In addition to the declared arguments, the constructor implicitly takes
  // the metatype as its first argument, like a static function.
  Type metatype = ctor->getType()->castTo<AnyFunctionType>()->getInput();
  new (gen.F.getModule()) SILArgument(gen.getLoweredType(metatype),
                                      gen.F.begin());
}

static RValue emitImplicitValueConstructorArg(SILGenFunction &gen,
                                              SILLocation loc,
                                              Type ty) {
  SILType argTy = gen.getLoweredType(ty);
  
  // Restructure tuple arguments.
  if (TupleType *tupleTy = argTy.getAs<TupleType>()) {
    RValue tuple(tupleTy->getCanonicalType());
    for (auto &field : tupleTy->getFields())
      tuple.addElement(
                   emitImplicitValueConstructorArg(gen, loc, field.getType()));

    return tuple;
  } else {
    SILValue arg = new (gen.F.getModule()) SILArgument(gen.getLoweredType(ty),
                                                       gen.F.begin());
    return RValue(gen, ManagedValue(arg, ManagedValue::Unmanaged));
  }
}

namespace {
  class ImplicitValueInitialization : public SingleInitializationBase {
    SILValue slot;
  public:
    ImplicitValueInitialization(SILValue slot, Type type)
      : SingleInitializationBase(type), slot(slot)
    {}
    
    SILValue getAddressOrNull() override {
      return slot;
    };
  };
}

static void emitImplicitValueDefaultConstructor(SILGenFunction &gen,
                                                ConstructorDecl *ctor) {
  emitConstructorMetatypeArg(gen, ctor);

  SILType thisTy
    = gen.getLoweredType(ctor->getImplicitThisDecl()->getType());
  
  // FIXME: We should actually elementwise default-construct the elements.
  if (thisTy.isAddressOnly(gen.SGM.M)) {
    SILValue resultSlot
      = new (gen.F.getModule()) SILArgument(thisTy, gen.F.begin());
    gen.B.createInitializeVar(ctor, resultSlot, /*canDefaultConstruct*/ false);
    gen.B.createReturn(ctor, gen.emitEmptyTuple(ctor));
  } else {
    SILValue addr = gen.B.createAllocVar(ctor, thisTy);
    gen.B.createInitializeVar(ctor, addr, /*canDefaultConstruct*/ false);
    SILValue result = gen.B.createLoad(ctor, addr);
    gen.B.createReturn(ctor, result);
  }
}

static void emitImplicitValueConstructor(SILGenFunction &gen,
                                         ConstructorDecl *ctor) {
  auto *TP = cast<TuplePattern>(ctor->getArguments());
  SILType thisTy
    = gen.getLoweredType(ctor->getImplicitThisDecl()->getType());

  if (TP->getFields().empty()) {
    // Emit a default constructor.
    return emitImplicitValueDefaultConstructor(gen, ctor);
  }

  // Emit the indirect return argument, if any.
  SILValue resultSlot;
  if (thisTy.isAddressOnly(gen.SGM.M))
    resultSlot = new (gen.F.getModule()) SILArgument(thisTy, gen.F.begin());
  
  // Emit the elementwise arguments.
  SmallVector<RValue, 4> elements;
  for (size_t i = 0, size = TP->getFields().size(); i < size; ++i) {
    auto *P = cast<TypedPattern>(TP->getFields()[i].getPattern());
    
    elements.push_back(
                     emitImplicitValueConstructorArg(gen, ctor, P->getType()));
  }

  emitConstructorMetatypeArg(gen, ctor);
  
  // If we have an indirect return slot, initialize it in-place in the implicit
  // return slot.
  if (resultSlot) {
    auto *decl = cast<StructDecl>(thisTy.getSwiftRValueType()
                                  ->getNominalOrBoundGenericNominal());
    unsigned memberIndex = 0;
    
    auto findNextPhysicalField = [&] {
      while (memberIndex < decl->getMembers().size()) {
        if (auto *vd = dyn_cast<VarDecl>(decl->getMembers()[memberIndex])) {
          if (!vd->isProperty())
            break;
        }
        ++memberIndex;
      }
    };
    findNextPhysicalField();
    
    for (size_t i = 0, size = elements.size(); i < size; ++i) {
      assert(memberIndex < decl->getMembers().size() &&
             "not enough physical struct members for value constructor?!");
      SILType argTy = gen.getLoweredType(elements[i].getType());

      // Store each argument in the corresponding element of 'this'.
      auto *field = cast<VarDecl>(decl->getMembers()[memberIndex]);
      SILValue slot = gen.B.createStructElementAddr(ctor, resultSlot,
                                                    cast<VarDecl>(field),
                                                    argTy.getAddressType());
      InitializationPtr init(new ImplicitValueInitialization(slot,
                                                         elements[i].getType()));
      std::move(elements[i]).forwardInto(gen, init.get());
      ++memberIndex;
      findNextPhysicalField();
    }
    gen.B.createReturn(ctor, gen.emitEmptyTuple(ctor));
    return;
  }
  
  // Otherwise, build a struct value directly from the elements.
  SmallVector<SILValue, 4> eltValues;
  for (RValue &rv : elements) {
    eltValues.push_back(std::move(rv).forwardAsSingleValue(gen));
  }
  
  SILValue thisValue = gen.B.createStruct(ctor, thisTy, eltValues);
  gen.B.createReturn(ctor, thisValue);
  return;
}

void SILGenFunction::emitValueConstructor(ConstructorDecl *ctor) {
  // If there's no body, this is the implicit elementwise constructor.
  if (!ctor->getBody())
    return emitImplicitValueConstructor(*this, ctor);
  
  // Emit the prolog.
  emitProlog(ctor->getArguments(), ctor->getImplicitThisDecl()->getType());
  emitConstructorMetatypeArg(*this, ctor);
  
  // Get the 'this' decl and type.
  VarDecl *thisDecl = ctor->getImplicitThisDecl();
  SILType thisTy = getLoweredType(thisDecl->getType());
  assert(!thisTy.hasReferenceSemantics() && "can't emit a ref type ctor here");
  assert(!ctor->getAllocThisExpr() && "alloc_this expr for value type?!");

  // Emit a local variable for 'this'.
  // FIXME: The (potentially partially initialized) variable would need to be
  // cleaned up on an error unwind.
  
  // If we don't need to heap-allocate the local 'this' and we're returning
  // indirectly, we can emplace 'this' in the return slot.
  bool canConstructInPlace
    = thisDecl->hasFixedLifetime() && IndirectReturnAddress.isValid();
  if (canConstructInPlace)
    VarLocs[thisDecl] = {SILValue(), IndirectReturnAddress};
  else
    emitLocalVariable(thisDecl);

  SILValue thisLV = VarLocs[thisDecl].address;
  
  // Emit a default initialization of the this value.
  // Note that this initialization *cannot* be lowered to a
  // default constructor--we're already in a constructor!
  B.createInitializeVar(ctor, thisLV, /*CanDefaultConstruct*/ false);
  
  // Create a basic block to jump to for the implicit 'this' return.
  // We won't emit this until after we've emitted the body.
  epilogBB = new (SGM.M) SILBasicBlock(&F);

  // Emit the constructor body.
  visit(ctor->getBody());
  
  // Return 'this' in the epilog.
  if (!emitEpilogBB(ctor))
    return;

  // If we constructed in-place, we're done.
  if (canConstructInPlace) {
    B.createReturn(ctor, emitEmptyTuple(ctor));
    return;
  }
  
  // If 'this' is address-only, copy 'this' into the indirect return slot.
  if (thisTy.isAddressOnly(SGM.M)) {
    assert(IndirectReturnAddress &&
           "no indirect return for address-only ctor?!");
    SILValue thisBox = VarLocs[thisDecl].box;
    assert(thisBox &&
           "address-only non-heap this should have been allocated in-place");
    // We have to do a non-take copy because someone else may be using the box.
    B.createCopyAddr(ctor, thisLV, IndirectReturnAddress,
                     /*isTake=*/ false,
                     /*isInit=*/ true);
    B.createRelease(ctor, thisBox);
    B.createReturn(ctor, emitEmptyTuple(ctor));
    return;
  }

  // Otherwise, load and return the final 'this' value.
  SILValue thisValue = B.createLoad(ctor, thisLV);
  if (SILValue thisBox = VarLocs[thisDecl].box) {
    // We have to do a retain because someone else may be using the box.
    emitRetainRValue(ctor, thisValue);
    B.createRelease(ctor, thisBox);
  } else {
    // We can just take ownership from the stack slot and consider it
    // deinitialized.
    B.createDeallocVar(ctor, thisLV);
  }
  B.createReturn(ctor, thisValue);
}

namespace {
  // Unlike the ArgumentInitVisitor, this visitor generates arguments but leaves
  // them destructured instead of storing them to lvalues so that the
  // argument set can be easily forwarded to another function.
  class ArgumentForwardVisitor
    : public PatternVisitor<ArgumentForwardVisitor>
  {
    SILGenFunction &gen;
    SmallVectorImpl<SILValue> &args;
  public:
    ArgumentForwardVisitor(SILGenFunction &gen,
                           SmallVectorImpl<SILValue> &args)
      : gen(gen), args(args) {}
    
    void makeArgument(Type ty) {
      assert(ty && "no type?!");
      // Destructure tuple arguments.
      if (TupleType *tupleTy = ty->getAs<TupleType>()) {
        for (auto &field : tupleTy->getFields())
          makeArgument(field.getType());
      } else {
        SILValue arg = new (gen.F.getModule()) SILArgument(gen.getLoweredType(ty),
                                                       gen.F.begin());
        args.push_back(arg);
      }
    }

    void visitParenPattern(ParenPattern *P) {
      visit(P->getSubPattern());
    }
    
    void visitTypedPattern(TypedPattern *P) {
      // FIXME: work around a bug in visiting the "this" argument of methods
      if (isa<NamedPattern>(P->getSubPattern()))
        makeArgument(P->getType());
      else
        visit(P->getSubPattern());
    }
    
    void visitTuplePattern(TuplePattern *P) {
      for (auto &elt : P->getFields())
        visit(elt.getPattern());
    }
    
    void visitAnyPattern(AnyPattern *P) {
      makeArgument(P->getType());
    }
    
    void visitNamedPattern(NamedPattern *P) {
      makeArgument(P->getType());
    }
    
#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) \
    void visit##Id##Pattern(Id##Pattern *) { \
      llvm_unreachable("pattern not valid in argument binding"); \
    }
#include "swift/AST/PatternNodes.def"

  };
} // end anonymous namespace

ArrayRef<Substitution>
SILGenFunction::buildForwardingSubstitutions(GenericParamList *gp) {
  if (!gp)
    return {};
  
  ASTContext &C = F.getASTContext();
  ArrayRef<ArchetypeType*> params = gp->getAllArchetypes();
  
  size_t paramCount = params.size();
  Substitution *resultBuf = C.Allocate<Substitution>(paramCount);
  MutableArrayRef<Substitution> results{resultBuf, paramCount};
  
  for (size_t i = 0; i < paramCount; ++i) {
    // FIXME: better way to do this?
    ArchetypeType *archetype = params[i];
    // "Check conformance" on each declared protocol to build a
    // conformance map.
    SmallVector<ProtocolConformance*, 2> conformances;
    
    for (ProtocolDecl *conformsTo : archetype->getConformsTo()) {
      (void)conformsTo;
      conformances.push_back(nullptr);
    }
    
    // Build an identity mapping with the derived conformances.
    auto replacement = SubstitutedType::get(archetype, archetype, C);
    results[i] = {archetype, replacement,
                  C.AllocateCopy(conformances)};
  }
  
  return results;
}

void SILGenFunction::emitClassConstructorAllocator(ConstructorDecl *ctor) {
  // Emit the prolog. Since we're just going to forward our args directly
  // to the initializer, don't allocate local variables for them.

  SmallVector<SILValue, 8> args;
  
  // Forward the constructor arguments.
  ArgumentForwardVisitor(*this, args).visit(ctor->getArguments());

  emitConstructorMetatypeArg(*this, ctor);

  // Allocate the "this" value.
  VarDecl *thisDecl = ctor->getImplicitThisDecl();
  SILType thisTy = getLoweredType(thisDecl->getType());
  assert(thisTy.hasReferenceSemantics() &&
         "can't emit a value type ctor here");
  SILValue thisValue;
  if (ctor->getAllocThisExpr()) {
    FullExpr allocThisScope(Cleanups);
    // If the constructor has an alloc-this expr, emit it to get "this".
    thisValue = visit(ctor->getAllocThisExpr()).forwardAsSingleValue(*this);
    assert(thisValue.getType() == thisTy &&
           "alloc-this expr type did not match this type?!");
  } else {
    // Otherwise, just emit an alloc_ref instruction for the default allocation
    // path.
    // FIXME: should have a cleanup in case of exception
    thisValue = B.createAllocRef(ctor, thisTy);
  }
  args.push_back(thisValue);

  // Call the initializer.
  SILConstant initConstant = SILConstant(ctor, SILConstant::Kind::Initializer);
  auto forwardingSubs = buildForwardingSubstitutions(ctor->getGenericParams());
  ManagedValue initVal = emitMethodRef(ctor, thisValue, initConstant,
                                       forwardingSubs);
  
  SILValue initedThisValue
    = B.createApply(ctor, initVal.forward(*this), thisTy, args);
  
  // Return the initialized 'this'.
  B.createReturn(ctor, initedThisValue);
}

static void emitClassImplicitConstructorInitializer(SILGenFunction &gen,
                                                    ConstructorDecl *ctor) {
  // The default constructor is currently a no-op. Just return back 'this'.
  // FIXME: We should default-construct fields maybe?

  assert(cast<TuplePattern>(ctor->getArguments())->getNumFields() == 0
         && "implicit class ctor has arguments?!");

  VarDecl *thisDecl = ctor->getImplicitThisDecl();
  SILType thisTy = gen.getLoweredLoadableType(thisDecl->getType());
  SILValue thisArg = new (gen.SGM.M) SILArgument(thisTy, gen.F.begin());
  assert(thisTy.hasReferenceSemantics() &&
         "can't emit a value type ctor here");
  
  gen.B.createReturn(ctor, thisArg);
}

void SILGenFunction::emitClassConstructorInitializer(ConstructorDecl *ctor) {
  // If there's no body, this is the implicit constructor.
  if (!ctor->getBody())
    return emitClassImplicitConstructorInitializer(*this, ctor);
  
  // Emit the prolog for the non-this arguments.
  emitProlog(ctor->getArguments(), TupleType::getEmpty(F.getASTContext()));
  
  // Emit the 'this' argument and make an lvalue for it.
  VarDecl *thisDecl = ctor->getImplicitThisDecl();
  SILType thisTy = getLoweredLoadableType(thisDecl->getType());
  SILValue thisArg = new (SGM.M) SILArgument(thisTy, F.begin());
  assert(thisTy.hasReferenceSemantics() &&
         "can't emit a value type ctor here");

  // FIXME: The (potentially partially initialized) value here would need to be
  // cleaned up on a constructor failure unwinding.
  emitLocalVariable(thisDecl);
  SILValue thisLV = VarLocs[thisDecl].address;
  emitStore(ctor, ManagedValue(thisArg, ManagedValue::Unmanaged), thisLV);
  
  // Create a basic block to jump to for the implicit 'this' return.
  // We won't emit the block until after we've emitted the body.
  epilogBB = new (SGM.M) SILBasicBlock(&F);
  
  // Emit the constructor body.
  visit(ctor->getBody());
  
  // Return 'this' in the epilog.
  if (!emitEpilogBB(ctor))
    return;

  // Load and return the final 'this'.
  SILValue thisValue = B.createLoad(ctor, thisLV);
  if (SILValue thisBox = VarLocs[thisDecl].box) {
    // We have to do a retain because someone else may be using the box.
    emitRetainRValue(ctor, thisValue);
    B.createRelease(ctor, thisBox);
  } else {
    // We can just take ownership from the stack slot and consider it
    // deinitialized.
    B.createDeallocVar(ctor, thisLV);
  }
  B.createReturn(ctor, thisValue);
}

static void forwardCaptureArgs(SILGenFunction &gen,
                               SmallVectorImpl<SILValue> &args,
                               ValueDecl *capture) {
  ASTContext &c = capture->getASTContext();
  
  auto addSILArgument = [&](SILType t) {
    args.push_back(new (gen.SGM.M) SILArgument(t, gen.F.begin()));
  };

  switch (getDeclCaptureKind(capture)) {
  case CaptureKind::Box: {
    SILType ty = gen.getLoweredType(capture->getTypeOfReference());
    // Forward the captured owning ObjectPointer.
    addSILArgument(SILType::getObjectPointerType(c));
    // Forward the captured value address.
    addSILArgument(ty);
    break;
  }
  case CaptureKind::Byref: {
    // Forward the captured address.
    SILType ty = gen.getLoweredType(capture->getTypeOfReference());
    addSILArgument(ty);
    break;
  }
  case CaptureKind::Constant: {
    // Forward the captured value.
    SILType ty = gen.getLoweredType(capture->getType());
    addSILArgument(ty);
    break;
  }
  case CaptureKind::GetterSetter: {
    // Forward the captured setter.
    Type setTy = gen.SGM.Types.getPropertyType(SILConstant::Kind::Setter,
                                               capture->getType());
    addSILArgument(gen.getLoweredType(setTy));
    SWIFT_FALLTHROUGH;
  }
  case CaptureKind::Getter: {
    // Forward the captured getter.
    Type getTy = gen.SGM.Types.getPropertyType(SILConstant::Kind::Getter,
                                               capture->getType());
    addSILArgument(gen.getLoweredType(getTy));
    break;
  }
  }
}

void SILGenFunction::emitCurryThunk(FuncExpr *fe,
                                    SILConstant from, SILConstant to) {
  SmallVector<SILValue, 8> curriedArgs;
  
  unsigned paramCount = from.uncurryLevel + 1;
  
  /// Forward implicit closure context arguments.
  bool hasCaptures = !fe->getCaptures().empty();
  if (hasCaptures)
    --paramCount;
  
  auto forwardCaptures = [&] {
    if (hasCaptures)
      for (auto capture : fe->getCaptures())
        forwardCaptureArgs(*this, curriedArgs, capture);
  };

  // Forward the curried formal arguments.
  auto forwardedPatterns = fe->getBodyParamPatterns().slice(0, paramCount);
  ArgumentForwardVisitor forwarder(*this, curriedArgs);
  UncurryDirection direction
    = SGM.Types.getUncurryDirection(F.getAbstractCC());
  switch (direction) {
  case UncurryDirection::LeftToRight:
    forwardCaptures();
    for (auto *paramPattern : forwardedPatterns)
      forwarder.visit(paramPattern);
    break;

  case UncurryDirection::RightToLeft:
    for (auto *paramPattern : reversed(forwardedPatterns))
      forwarder.visit(paramPattern);
    forwardCaptures();
    break;
  }
  
  // FIXME: Forward archetypes and specialize if the function is generic.
  
  // Partially apply the next uncurry level and return the result closure.
  auto toFn = B.createFunctionRef(fe, SGM.getFunction(to));
  SILType resultTy
    = SGM.getConstantType(from).getFunctionTypeInfo(SGM.M)->getResultType();
  auto toClosure = B.createPartialApply(fe, toFn, curriedArgs, resultTy);
  B.createReturn(fe, toClosure);
}

RValue SILGenFunction::
visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E,
                                   SGFContext C) {
  return visit(E->getSemanticExpr());
}

RValue SILGenFunction::
visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *E, SGFContext C) {
  ASTContext &Ctx = SGM.M.getASTContext();
  SILType Ty = getLoweredLoadableType(E->getType());
  llvm::SMLoc Loc;
  
  // If "overrideLocationForMagicIdentifiers" is set, then we use it as the
  // location point for these magic identifiers.
  if (overrideLocationForMagicIdentifiers.isValid())
    Loc = overrideLocationForMagicIdentifiers.Value;
  else
    Loc = E->getStartLoc().Value;
  
  switch (E->getKind()) {
  case MagicIdentifierLiteralExpr::File: {
    int BufferID = Ctx.SourceMgr.FindBufferContainingLoc(Loc);
    assert(BufferID != -1 && "MagicIdentifierLiteral has invalid location");
    
    StringRef Value =
      Ctx.SourceMgr.getMemoryBuffer(BufferID)->getBufferIdentifier();
    
    return RValue(*this, ManagedValue(B.createStringLiteral(E, Ty, Value),
                                      ManagedValue::Unmanaged));
  }
  case MagicIdentifierLiteralExpr::Line: {
    unsigned Value = Ctx.SourceMgr.getLineAndColumn(Loc).first;
    return RValue(*this,
                  ManagedValue(B.createIntegerLiteral(E, Ty, Value),
                               ManagedValue::Unmanaged));
  }
  case MagicIdentifierLiteralExpr::Column: {
    unsigned Value = Ctx.SourceMgr.getLineAndColumn(Loc).second;
    return RValue(*this,
                  ManagedValue(B.createIntegerLiteral(E, Ty, Value),
                               ManagedValue::Unmanaged));
  }
  }
}

RValue SILGenFunction::visitCollectionExpr(CollectionExpr *E, SGFContext C) {
  return visit(E->getSemanticExpr());
}

RValue SILGenFunction::visitRebindThisInConstructorExpr(
                                RebindThisInConstructorExpr *E, SGFContext C) {
  // FIXME: Use a different instruction from 'downcast'. IRGen can make
  // "rebind this" into a no-op if the called constructor is a Swift one.
  ManagedValue newThis = visit(E->getSubExpr()).getAsSingleValue(*this);
  if (!newThis.getType().getSwiftRValueType()
        ->isEqual(E->getThis()->getType())) {
    assert(!newThis.getType().isAddress() &&
           newThis.getType().hasReferenceSemantics() &&
           "delegating ctor type mismatch for non-reference type?!");
    CleanupsDepth newThisCleanup = newThis.getCleanup();
    SILValue newThisValue = B.createDowncast(E, newThis.getValue(),
                              getLoweredLoadableType(E->getThis()->getType()),
                              CheckedCastMode::Unconditional);
    newThis = ManagedValue(newThisValue, newThisCleanup);
  }
  
  SILValue thisAddr = emitReferenceToDecl(E, E->getThis()).getUnmanagedValue();
  newThis.assignInto(*this, E, thisAddr);
  
  return emitEmptyTupleRValue(E);
}

RValue SILGenFunction::visitArchetypeSubscriptExpr(
                                     ArchetypeSubscriptExpr *E, SGFContext C) {
  llvm_unreachable("not implemented");
}

RValue SILGenFunction::visitExistentialSubscriptExpr(
                                   ExistentialSubscriptExpr *E, SGFContext C) {
  llvm_unreachable("not implemented");
}

RValue SILGenFunction::visitBridgeToBlockExpr(BridgeToBlockExpr *E,
                                              SGFContext C) {
  SILValue func = visit(E->getSubExpr()).forwardAsSingleValue(*this);
  // Thicken thin function value if necessary.
  // FIXME: This should go away when Swift typechecking learns how to handle
  // thin functions.
  func = emitGeneralizedValue(E, func);
  
  // Emit the bridge_to_block instruction.
  SILValue block = B.createBridgeToBlock(E, func,
                                         getLoweredLoadableType(E->getType()));
  return RValue(*this, emitManagedRValueWithCleanup(block));
}

RValue SILGenFunction::visitIfExpr(IfExpr *E, SGFContext C) {
  // FIXME: We could avoid imploding and reexploding tuples here.
  // FIXME: "emit into" optimization
  
  Condition cond = emitCondition(E, E->getCondExpr(),
                                 /*hasFalse*/ true,
                                 /*invertCondition*/ false,
                                 getLoweredType(E->getType()));
  
  cond.enterTrue(B);
  SILValue trueValue;
  {
    FullExpr trueScope(Cleanups);
    trueValue = visit(E->getThenExpr()).forwardAsSingleValue(*this);
  }
  cond.exitTrue(B, trueValue);
  
  cond.enterFalse(B);
  SILValue falseValue;
  {
    FullExpr falseScope(Cleanups);
    falseValue = visit(E->getElseExpr()).forwardAsSingleValue(*this);
  }
  cond.exitFalse(B, falseValue);
  
  SILBasicBlock *cont = cond.complete(B);
  assert(cont && "no continuation block for if expr?!");
  
  SILValue result = cont->bbarg_begin()[0];
  
  return RValue(*this, emitManagedRValueWithCleanup(result));
}

RValue SILGenFunction::visitZeroValueExpr(ZeroValueExpr *E, SGFContext C) {
  SILValue zero = B.createBuiltinZero(E,
                                      getLoweredLoadableType(E->getType()));
  return RValue(*this, ManagedValue(zero, ManagedValue::Unmanaged));
}

RValue SILGenFunction::visitDefaultValueExpr(DefaultValueExpr *E, SGFContext C) {
  return visit(E->getSubExpr(), C);
}

SILValue SILGenFunction::emitGeneralizedValue(SILLocation loc, SILValue v) {
  // Thicken thin functions.
  if (v.getType().is<AnyFunctionType>() &&
      v.getType().castTo<AnyFunctionType>()->isThin()) {
    // Thunk functions to the standard "freestanding" calling convention.
    if (v.getType().getAbstractCC() != AbstractCC::Freestanding) {
      auto freestandingType = getThinFunctionType(v.getType().getSwiftType(),
                                                  AbstractCC::Freestanding);
      SILType freestandingSILType = getLoweredLoadableType(freestandingType, 0);
      v = B.createConvertCC(loc, v, freestandingSILType);
    }
    
    Type thickTy = getThickFunctionType(v.getType().getSwiftType(),
                                        AbstractCC::Freestanding);
    
    v = B.createThinToThickFunction(loc, v,
                                    getLoweredLoadableType(thickTy));
  }
  
  return v;
}

static ManagedValue emitBridgeStringToNSString(SILGenFunction &gen,
                                               SILLocation loc,
                                               ManagedValue str) {
  // func convertStringToNSString([byref] String) -> NSString
  SILValue stringToNSStringFn
    = gen.emitGlobalFunctionRef(loc, gen.SGM.getStringToNSStringFn());
  
  // Materialize the string so we can pass a reference.
  // Assume StringToNSString won't consume or modify the string, so leave the
  // cleanup on the original value intact.
  SILValue strTemp = gen.emitTemporaryAllocation(loc,
                                                 str.getType());
  gen.B.createStore(loc, str.getValue(), strTemp);
  
  SILValue nsstr = gen.B.createApply(loc, stringToNSStringFn,
                           gen.getLoweredType(gen.SGM.Types.getNSStringType()),
                           strTemp);
  return gen.emitManagedRValueWithCleanup(nsstr);
}

static ManagedValue emitBridgeNSStringToString(SILGenFunction &gen,
                                               SILLocation loc,
                                               ManagedValue nsstr) {
  // func convertNSStringToString(NSString, [byref] String) -> ()
  SILValue nsstringToStringFn
    = gen.emitGlobalFunctionRef(loc, gen.SGM.getNSStringToStringFn());
  
  // Allocate and initialize a temporary to receive the result String.
  SILValue strTemp = gen.emitTemporaryAllocation(loc,
                             gen.getLoweredType(gen.SGM.Types.getStringType()));
  gen.B.createInitializeVar(loc, strTemp, true);
  
  SILValue args[2] = {nsstr.forward(gen), strTemp};
  gen.B.createApply(loc, nsstringToStringFn,
                    gen.SGM.Types.getEmptyTupleType(),
                    args);
  
  // Load the result string, taking ownership of the value. There's no cleanup
  // on the value in the temporary allocation.
  SILValue str = gen.B.createLoad(loc, strTemp);
  return gen.emitManagedRValueWithCleanup(str);
}

static ManagedValue emitBridgeBoolToObjCBool(SILGenFunction &gen,
                                             SILLocation loc,
                                             ManagedValue swiftBool) {
  // func convertBoolToObjCBool(Bool) -> ObjCBool
  SILValue boolToObjCBoolFn
    = gen.emitGlobalFunctionRef(loc, gen.SGM.getBoolToObjCBoolFn());
  
  SILType resultTy =gen.getLoweredLoadableType(gen.SGM.Types.getObjCBoolType());
  
  SILValue result = gen.B.createApply(loc, boolToObjCBoolFn,
                                      resultTy, swiftBool.forward(gen));
  return gen.emitManagedRValueWithCleanup(result);
}

static ManagedValue emitBridgeObjCBoolToBool(SILGenFunction &gen,
                                             SILLocation loc,
                                             ManagedValue objcBool) {
  // func convertObjCBoolToBool(ObjCBool) -> Bool
  SILValue objcBoolToBoolFn
    = gen.emitGlobalFunctionRef(loc, gen.SGM.getObjCBoolToBoolFn());
  
  SILType resultTy = gen.getLoweredLoadableType(gen.SGM.Types.getBoolType());
  
  SILValue result = gen.B.createApply(loc, objcBoolToBoolFn,
                                      resultTy, objcBool.forward(gen));
  return gen.emitManagedRValueWithCleanup(result);
}

ManagedValue SILGenFunction::emitNativeToBridgedValue(SILLocation loc,
                                                      ManagedValue v,
                                                      AbstractCC destCC,
                                                      CanType bridgedTy) {
  // First, generalize the value representation.
  SILValue generalized = emitGeneralizedValue(loc, v.getValue());
  v = ManagedValue(generalized, v.getCleanup());

  switch (destCC) {
  case AbstractCC::Freestanding:
  case AbstractCC::Method:
    // No additional bridging needed for native functions.
    return v;
  case AbstractCC::C:
  case AbstractCC::ObjCMethod:
    // If the input is a native type with a bridged mapping, convert it.
#define BRIDGE_TYPE(BridgedModule,BridgedType, NativeModule,NativeType) \
    if (v.getType().getSwiftType() == SGM.Types.get##NativeType##Type() \
        && bridgedTy == SGM.Types.get##BridgedType##Type()) {           \
      return emitBridge##NativeType##To##BridgedType(*this, loc, v);    \
    }
#include "swift/SIL/BridgedTypes.def"
    return v;
  }
}

ManagedValue SILGenFunction::emitBridgedToNativeValue(SILLocation loc,
                                                      ManagedValue v,
                                                      AbstractCC srcCC,
                                                      CanType nativeTy) {
  switch (srcCC) {
  case AbstractCC::Freestanding:
  case AbstractCC::Method:
    // No additional bridging needed for native functions.
    return v;

  case AbstractCC::C:
  case AbstractCC::ObjCMethod:
    // If the output is a bridged type, convert it back to a native type.
#define BRIDGE_TYPE(BridgedModule,BridgedType, NativeModule,NativeType)  \
    if (v.getType().getSwiftType() == SGM.Types.get##BridgedType##Type() \
        && nativeTy == SGM.Types.get##NativeType##Type()) {              \
      return emitBridge##BridgedType##To##NativeType(*this, loc, v);     \
    }
#include "swift/SIL/BridgedTypes.def"
    return v;
  }
}

void SILGenFunction::emitStore(SILLocation loc, ManagedValue src,
                               SILValue destAddr) {
  SILValue fwdSrc = src.forward(*this);
  // If we store a function value, we lose its thinness.
  // FIXME: This should go away when Swift typechecking learns how to handle
  // thin functions.
  fwdSrc = emitGeneralizedValue(loc, fwdSrc);  
  B.createStore(loc, fwdSrc, destAddr);
}

RValue SILGenFunction::emitEmptyTupleRValue(SILLocation loc) {
  return RValue(CanType(TupleType::getEmpty(F.getASTContext())));
}

/// Destructure (potentially) recursive assignments into tuple expressions
/// down to their scalar stores.
static void emitAssignExprRecursive(AssignExpr *S, RValue &&Src, Expr *Dest,
                                    SILGenFunction &Gen) {
  // If the destination is a tuple, recursively destructure.
  if (TupleExpr *TE = dyn_cast<TupleExpr>(Dest)) {
    SmallVector<RValue, 4> elements;
    std::move(Src).extractElements(elements);
    unsigned EltNo = 0;
    for (Expr *DestElem : TE->getElements()) {
      emitAssignExprRecursive(S,
                              std::move(elements[EltNo++]),
                              DestElem, Gen);
    }
    return;
  }
  
  // Otherwise, emit the scalar assignment.
  LValue DstLV = Gen.emitLValue(Dest);
  Gen.emitAssignToLValue(S, std::move(Src), DstLV);
}


RValue SILGenFunction::visitAssignExpr(AssignExpr *E, SGFContext C) {
  FullExpr scope(Cleanups);

  // Handle tuple destinations by destructuring them if present.
  emitAssignExprRecursive(E, visit(E->getSrc()), E->getDest(), *this);
  
  return emitEmptyTupleRValue(E);
}

