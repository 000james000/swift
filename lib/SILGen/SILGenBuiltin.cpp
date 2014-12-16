//===--- SILGenBuiltin.cpp - SIL generation for builtin call sites  -------===//
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

#include "SpecializedEmitter.h"

#include "RValue.h"
#include "SILGenFunction.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Module.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/SIL/SILUndef.h"

using namespace swift;
using namespace Lowering;

static ManagedValue emitBuiltinRetain(SILGenFunction &gen,
                                       SILLocation loc,
                                       ArrayRef<Substitution> substitutions,
                                       ArrayRef<ManagedValue> args,
                                       SGFContext C) {
  // The value was produced at +1; we can produce an unbalanced
  // retain simply by disabling the cleanup.
  args[0].forward(gen);
  return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));    
}

static ManagedValue emitBuiltinRelease(SILGenFunction &gen,
                                       SILLocation loc,
                                       ArrayRef<Substitution> substitutions,
                                       ArrayRef<ManagedValue> args,
                                       SGFContext C) {
  // The value was produced at +1, so to produce an unbalanced
  // release we need to leave the cleanup intact and then do a *second*
  // release.
  gen.B.createReleaseValue(loc, args[0].getValue());
  return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));    
}

static ManagedValue emitBuiltinAutorelease(SILGenFunction &gen,
                                           SILLocation loc,
                                           ArrayRef<Substitution> substitutions,
                                           ArrayRef<ManagedValue> args,
                                           SGFContext C) {
  // The value was produced at +1, so to produce an unbalanced
  // autorelease we need to leave the cleanup intact.
  gen.B.createAutoreleaseValue(loc, args[0].getValue());
  return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));    
}

static bool requireIsOptionalNativeObject(SILGenFunction &gen,
                                           SILLocation loc,
                                           Type type) {
  if (auto valueType = type->getOptionalObjectType())
    if (valueType->is<BuiltinNativeObjectType>())
      return true;

  gen.SGM.diagnose(loc, diag::invalid_sil_builtin,
              "type of pin handle must be Optional<Builtin.NativeObject>");
  return false;
}

static ManagedValue emitBuiltinTryPin(SILGenFunction &gen,
                                      SILLocation loc,
                                      ArrayRef<Substitution> subs,
                                      ArrayRef<ManagedValue> args,
                                      SGFContext C) {
  assert(args.size() == 1);

  if (!requireIsOptionalNativeObject(gen, loc, subs[0].getReplacement())) {
    return gen.emitUndef(loc, subs[0].getReplacement());
  }

  // The value was produced at +1, but pinning is only a conditional
  // retain, so we have to leave the cleanup in place.  TODO: try to
  // emit the argument at +0.
  SILValue result = gen.B.createStrongPin(loc, args[0].getValue());

  // The handle, if non-null, is effectively +1.
  return gen.emitManagedRValueWithCleanup(result);
}

static ManagedValue emitBuiltinUnpin(SILGenFunction &gen,
                                     SILLocation loc,
                                     ArrayRef<Substitution> subs,
                                     ArrayRef<ManagedValue> args,
                                     SGFContext C) {
  assert(args.size() == 1);

  if (requireIsOptionalNativeObject(gen, loc, subs[0].getReplacement())) {
    // Unpinning takes responsibility for the +1 handle.
    gen.B.createStrongUnpin(loc, args[0].forward(gen));
  }

  return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));
}

/// Specialized emitter for Builtin.load and Builtin.take.
static ManagedValue emitBuiltinLoadOrTake(SILGenFunction &gen,
                                          SILLocation loc,
                                          ArrayRef<Substitution> substitutions,
                                          ArrayRef<ManagedValue> args,
                                          SGFContext C,
                                          IsTake_t isTake) {
  assert(substitutions.size() == 1 && "load should have single substitution");
  assert(args.size() == 1 && "load should have a single argument");
  
  // The substitution gives the type of the load.  This is always a
  // first-class type; there is no way to e.g. produce a @weak load
  // with this builtin.
  auto &rvalueTL = gen.getTypeLowering(substitutions[0].getReplacement());
  SILType loadedType = rvalueTL.getLoweredType();

  // Convert the pointer argument to a SIL address.
  SILValue addr = gen.B.createPointerToAddress(loc, args[0].getUnmanagedValue(),
                                               loadedType.getAddressType());
  // Perform the load.
  return gen.emitLoad(loc, addr, rvalueTL, C, isTake);
}

static ManagedValue emitBuiltinLoad(SILGenFunction &gen,
                                    SILLocation loc,
                                    ArrayRef<Substitution> substitutions,
                                    ArrayRef<ManagedValue> args,
                                    SGFContext C) {
  return emitBuiltinLoadOrTake(gen, loc, substitutions, args, C, IsNotTake);
}

static ManagedValue emitBuiltinTake(SILGenFunction &gen,
                                    SILLocation loc,
                                    ArrayRef<Substitution> substitutions,
                                    ArrayRef<ManagedValue> args,
                                    SGFContext C) {
  return emitBuiltinLoadOrTake(gen, loc, substitutions, args, C, IsTake);
}

/// Specialized emitter for Builtin.destroy.
static ManagedValue emitBuiltinDestroy(SILGenFunction &gen,
                                       SILLocation loc,
                                       ArrayRef<Substitution> substitutions,
                                       ArrayRef<ManagedValue> args,
                                       SGFContext C) {
  assert(args.size() == 2 && "destroy should have two arguments");
  assert(substitutions.size() == 1 &&
         "destroy should have a single substitution");
  // The substitution determines the type of the thing we're destroying.
  auto &ti = gen.getTypeLowering(substitutions[0].getReplacement());
  
  // Destroy is a no-op for trivial types.
  if (ti.isTrivial())
    return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));
  
  SILType destroyType = ti.getLoweredType();

  // Convert the pointer argument to a SIL address.
  SILValue addr =
    gen.B.createPointerToAddress(loc, args[1].getUnmanagedValue(),
                                 destroyType.getAddressType());
  
  // Destroy the value indirectly. Canonicalization will promote to loads
  // and releases if appropriate.
  gen.B.emitDestroyAddr(loc, addr);
  
  return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));
}

/// Specialized emitter for Builtin.assign and Builtin.init.
static ManagedValue emitBuiltinAssignOrInit(SILGenFunction &gen,
                                      SILLocation loc,
                                      ArrayRef<Substitution> substitutions,
                                      ArrayRef<ManagedValue> args,
                                      SGFContext C,
                                      bool isInitialization) {
  assert(args.size() >= 2 && "assign should have two arguments");
  assert(substitutions.size() == 1 &&
         "assign should have a single substitution");

  // The substitution determines the type of the thing we're destroying.
  CanType assignFormalType = substitutions[0].getReplacement()->getCanonicalType();
  SILType assignType = gen.getLoweredType(assignFormalType);
  
  // Convert the destination pointer argument to a SIL address.
  SILValue addr = gen.B.createPointerToAddress(loc,
                                               args.back().getUnmanagedValue(),
                                               assignType.getAddressType());
  
  // Build the value to be assigned, reconstructing tuples if needed.
  ManagedValue src = RValue(args.slice(0, args.size() - 1), assignFormalType)
    .getAsSingleValue(gen, loc);
  
  if (isInitialization)
    src.forwardInto(gen, loc, addr);
  else
    src.assignInto(gen, loc, addr);
  return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));
}

static ManagedValue emitBuiltinAssign(SILGenFunction &gen,
                                      SILLocation loc,
                                      ArrayRef<Substitution> substitutions,
                                      ArrayRef<ManagedValue> args,
                                      SGFContext C) {
  return emitBuiltinAssignOrInit(gen, loc, substitutions, args, C,
                                 /*isInitialization*/ false);
}

static ManagedValue emitBuiltinInit(SILGenFunction &gen,
                                    SILLocation loc,
                                    ArrayRef<Substitution> substitutions,
                                    ArrayRef<ManagedValue> args,
                                    SGFContext C) {
  return emitBuiltinAssignOrInit(gen, loc, substitutions, args, C,
                                 /*isInitialization*/ true);
}

/// Specialized emitter for Builtin.fixLifetime.
static ManagedValue emitBuiltinFixLifetime(SILGenFunction &gen,
                                           SILLocation loc,
                                           ArrayRef<Substitution> substitutions,
                                           ArrayRef<ManagedValue> args,
                                           SGFContext C) {
  for (auto arg : args) {
    gen.B.createFixLifetime(loc, arg.getValue());
  }
  return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));
}

/// Specialized emitter for Builtin.castToNativeObject.
static ManagedValue emitBuiltinCastToNativeObject(SILGenFunction &gen,
                                         SILLocation loc,
                                         ArrayRef<Substitution> substitutions,
                                         ArrayRef<ManagedValue> args,
                                         SGFContext C) {
  assert(args.size() == 1 && "cast should have a single argument");
  assert(substitutions.size() == 1 && "cast should have a type substitution");
  
  // Take the reference type argument and cast it to NativeObject.
  SILType objPointerType = SILType::getNativeObjectType(gen.F.getASTContext());

  // Bail if the source type is not a class reference of some kind.
  if (!substitutions[0].getReplacement()->mayHaveSuperclass() &&
      !substitutions[0].getReplacement()->isClassExistentialType()) {
    gen.SGM.diagnose(loc, diag::invalid_sil_builtin,
                     "castToNativeObject source must be a class");
    SILValue undef = SILUndef::get(objPointerType, gen.SGM.M);
    return ManagedValue::forUnmanaged(undef);
  }
  
  // Save the cleanup on the argument so we can forward it onto the cast
  // result.
  auto cleanup = args[0].getCleanup();
  
  SILValue arg = args[0].getValue();

  // If the argument is existential, open it.
  if (substitutions[0].getReplacement()->isClassExistentialType()) {
    Type openedTy
      = ArchetypeType::getOpened(substitutions[0].getReplacement());
    SILType loweredOpenedTy = gen.getLoweredLoadableType(openedTy);
    arg = gen.B.createOpenExistentialRef(loc, arg, loweredOpenedTy);
  }

  SILValue result = gen.B.createUncheckedRefCast(loc, arg, objPointerType);
  // Return the cast result with the original cleanup.
  return ManagedValue(result, cleanup);
}

/// Specialized emitter for Builtin.castFromNativeObject.
static ManagedValue emitBuiltinCastFromNativeObject(SILGenFunction &gen,
                                         SILLocation loc,
                                         ArrayRef<Substitution> substitutions,
                                         ArrayRef<ManagedValue> args,
                                         SGFContext C) {
  assert(args.size() == 1 && "cast should have a single argument");
  assert(substitutions.size() == 1 &&
         "cast should have a single substitution");

  // The substitution determines the destination type.
  SILType destType = gen.getLoweredType(substitutions[0].getReplacement());
  
  // Bail if the source type is not a class reference of some kind.
  if (!substitutions[0].getReplacement()->isBridgeableObjectType()
      || !destType.isObject()) {
    gen.SGM.diagnose(loc, diag::invalid_sil_builtin,
                     "castFromNativeObject dest must be an object type");
    // Recover by propagating an undef result.
    SILValue result = SILUndef::get(destType, gen.SGM.M);
    return ManagedValue::forUnmanaged(result);
  }
  
  // Save the cleanup on the argument so we can forward it onto the cast
  // result.
  auto cleanup = args[0].getCleanup();

  // Take the reference type argument and cast it.
  SILValue result = gen.B.createUncheckedRefCast(loc, args[0].getValue(),
                                                   destType);
  // Return the cast result with the original cleanup.
  return ManagedValue(result, cleanup);
}

/// Specialized emitter for Builtin.bridgeToRawPointer.
static ManagedValue emitBuiltinBridgeToRawPointer(SILGenFunction &gen,
                                        SILLocation loc,
                                        ArrayRef<Substitution> substitutions,
                                        ArrayRef<ManagedValue> args,
                                        SGFContext C) {
  assert(args.size() == 1 && "bridge should have a single argument");
  
  // Take the reference type argument and cast it to RawPointer.
  // RawPointers do not have ownership semantics, so the cleanup on the
  // argument remains.
  SILType rawPointerType = SILType::getRawPointerType(gen.F.getASTContext());
  SILValue result = gen.B.createRefToRawPointer(loc, args[0].getValue(),
                                                rawPointerType);
  return ManagedValue::forUnmanaged(result);
}

/// Specialized emitter for Builtin.bridgeFromRawPointer.
static ManagedValue emitBuiltinBridgeFromRawPointer(SILGenFunction &gen,
                                        SILLocation loc,
                                        ArrayRef<Substitution> substitutions,
                                        ArrayRef<ManagedValue> args,
                                        SGFContext C) {
  assert(substitutions.size() == 1 &&
         "bridge should have a single substitution");
  assert(args.size() == 1 && "bridge should have a single argument");
  
  // The substitution determines the destination type.
  // FIXME: Archetype destination type?
  auto &destLowering = gen.getTypeLowering(substitutions[0].getReplacement());
  assert(destLowering.isLoadable());
  SILType destType = destLowering.getLoweredType();

  // Take the raw pointer argument and cast it to the destination type.
  SILValue result = gen.B.createRawPointerToRef(loc, args[0].getUnmanagedValue(),
                                                destType);
  // The result has ownership semantics, so retain it with a cleanup.
  return gen.emitManagedRetain(loc, result, destLowering);
}

/// Specialized emitter for Builtin.addressof.
static ManagedValue emitBuiltinAddressOf(SILGenFunction &gen,
                                         SILLocation loc,
                                         ArrayRef<Substitution> substitutions,
                                         ArrayRef<ManagedValue> args,
                                         SGFContext C) {
  assert(args.size() == 1 && "addressof should have a single argument");
  
  // Take the address argument and cast it to RawPointer.
  SILType rawPointerType = SILType::getRawPointerType(gen.F.getASTContext());
  SILValue result = gen.B.createAddressToPointer(loc,
                                                 args[0].getUnmanagedValue(),
                                                 rawPointerType);
  return ManagedValue::forUnmanaged(result);
}

/// Specialized emitter for Builtin.gep.
static ManagedValue emitBuiltinGep(SILGenFunction &gen,
                                   SILLocation loc,
                                   ArrayRef<Substitution> substitutions,
                                   ArrayRef<ManagedValue> args,
                                   SGFContext C) {
  assert(args.size() == 2 && "gep should be given two arguments");
  
  SILValue offsetPtr = gen.B.createIndexRawPointer(loc,
                                                 args[0].getUnmanagedValue(),
                                                 args[1].getUnmanagedValue());
  return ManagedValue::forUnmanaged(offsetPtr);
}

/// Specialized emitter for Builtin.condfail.
static ManagedValue emitBuiltinCondFail(SILGenFunction &gen,
                                        SILLocation loc,
                                        ArrayRef<Substitution> substitutions,
                                        ArrayRef<ManagedValue> args,
                                        SGFContext C) {
  assert(args.size() == 1 && "condfail should be given one argument");
  
  gen.B.createCondFail(loc, args[0].getUnmanagedValue());
  return ManagedValue::forUnmanaged(gen.emitEmptyTuple(loc));
}

/// Specialized emitter for Builtin.reinterpretCast.
static ManagedValue emitBuiltinReinterpretCast(SILGenFunction &gen,
                                         SILLocation loc,
                                         ArrayRef<Substitution> substitutions,
                                         ArrayRef<ManagedValue> args,
                                         SGFContext C) {
  assert(args.size() == 1 && "reinterpretCast should be given one argument");
  assert(substitutions.size() == 2 && "reinterpretCast should have two subs");
  
  auto &fromTL = gen.getTypeLowering(substitutions[0].getReplacement());
  auto &toTL = gen.getTypeLowering(substitutions[1].getReplacement());
  
  // If casting between address-only types, cast the address.
  if (!fromTL.isLoadable() || !toTL.isLoadable()) {
    SILValue fromAddr;
    
    // If the from value is loadable, move it to a buffer.
    if (fromTL.isLoadable()) {
      fromAddr = gen.emitTemporaryAllocation(loc, args[0].getValue().getType());
      gen.B.createStore(loc, args[0].getValue(), fromAddr);
    } else {
      fromAddr = args[0].getValue();
    }
    
    auto toAddr = gen.B.createUncheckedAddrCast(loc, fromAddr,
                                      toTL.getLoweredType().getAddressType());
    
    SILValue toValue;
    // Load the destination value if it's loadable.
    if (toTL.isLoadable()) {
      toValue = gen.B.createLoad(loc, toAddr);
    } else {
      toValue = toAddr;
    }
    
    // Forward it along with the original cleanup.
    // TODO: Could try to pick which of the original or destination types has
    // a cheaper cleanup.
    if (toTL.isTrivial())
      return ManagedValue::forUnmanaged(toValue);
    
    return ManagedValue(toValue, args[0].getCleanup());
  }
  
  // If the destination is trivial, do a trivial bitcast, leaving the cleanup
  // on the original value intact.
  // TODO: Could try to pick which of the original or destination types has
  // a cheaper cleanup.
  if (toTL.isTrivial()) {
    SILValue in = args[0].getValue();
    SILValue out = gen.B.createUncheckedTrivialBitCast(loc, in,
                                                       toTL.getLoweredType());
    return ManagedValue::forUnmanaged(out);
  }
  
  // Otherwise, do a reference-counting-identical bitcast, forwarding the
  // cleanup onto the new value.
  SILValue in = args[0].getValue();
  SILValue out = gen.B.createUncheckedRefBitCast(loc, in,
                                                 toTL.getLoweredType());
  return ManagedValue(out, args[0].getCleanup());
}

/// Specialized emitter for Builtin.castToBridgeObject.
static ManagedValue emitBuiltinCastToBridgeObject(SILGenFunction &gen,
                                                  SILLocation loc,
                                                  ArrayRef<Substitution> subs,
                                                  ArrayRef<ManagedValue> args,
                                                  SGFContext C) {
  assert(args.size() == 2 && "cast should have two arguments");
  assert(subs.size() == 1 && "cast should have a type substitution");
  
  // Take the reference type argument and cast it to BridgeObject.
  SILType objPointerType = SILType::getBridgeObjectType(gen.F.getASTContext());

  // Bail if the source type is not a class reference of some kind.
  if (!subs[0].getReplacement()->mayHaveSuperclass() &&
      !subs[0].getReplacement()->isClassExistentialType()) {
    gen.SGM.diagnose(loc, diag::invalid_sil_builtin,
                     "castToNativeObject source must be a class");
    SILValue undef = SILUndef::get(objPointerType, gen.SGM.M);
    return ManagedValue::forUnmanaged(undef);
  }
  
  // Save the cleanup on the argument so we can forward it onto the cast
  // result.
  auto refCleanup = args[0].getCleanup();
  SILValue ref = args[0].getValue();
  SILValue bits = args[1].getUnmanagedValue();
  
  // If the argument is existential, open it.
  if (subs[0].getReplacement()->isClassExistentialType()) {
    Type openedTy
      = ArchetypeType::getOpened(subs[0].getReplacement());
    SILType loweredOpenedTy = gen.getLoweredLoadableType(openedTy);
    ref = gen.B.createOpenExistentialRef(loc, ref, loweredOpenedTy);
  }
  
  SILValue result = gen.B.createRefToBridgeObject(loc, ref, bits);
  return ManagedValue(result, refCleanup);
}

/// Specialized emitter for Builtin.castReferenceFromBridgeObject.
static ManagedValue emitBuiltinCastReferenceFromBridgeObject(
                                                  SILGenFunction &gen,
                                                  SILLocation loc,
                                                  ArrayRef<Substitution> subs,
                                                  ArrayRef<ManagedValue> args,
                                                  SGFContext C) {
  assert(args.size() == 1 && "cast should have one argument");
  assert(subs.size() == 1 && "cast should have a type substitution");

  // The substitution determines the destination type.
  SILType destType = gen.getLoweredType(subs[0].getReplacement());
  
  // Bail if the source type is not a class reference of some kind.
  if (!subs[0].getReplacement()->isBridgeableObjectType()
      || !destType.isObject()) {
    gen.SGM.diagnose(loc, diag::invalid_sil_builtin,
                 "castReferenceFromBridgeObject dest must be an object type");
    // Recover by propagating an undef result.
    SILValue result = SILUndef::get(destType, gen.SGM.M);
    return ManagedValue::forUnmanaged(result);
  }
  
  SILValue result = gen.B.createBridgeObjectToRef(loc, args[0].forward(gen),
                                                  destType);
  return gen.emitManagedRValueWithCleanup(result);
}
static ManagedValue emitBuiltinCastBitPatternFromBridgeObject(
                                                  SILGenFunction &gen,
                                                  SILLocation loc,
                                                  ArrayRef<Substitution> subs,
                                                  ArrayRef<ManagedValue> args,
                                                  SGFContext C) {
  assert(args.size() == 1 && "cast should have one argument");
  assert(subs.empty() && "cast should not have subs");

  SILType wordType = SILType::getBuiltinWordType(gen.getASTContext());
  SILValue result = gen.B.createBridgeObjectToWord(loc, args[0].getValue(),
                                                   wordType);
  return ManagedValue::forUnmanaged(result);
}

static ManagedValue emitBuiltinMarkDependence(SILGenFunction &gen,
                                              SILLocation loc,
                                              ArrayRef<Substitution> subs,
                                              ArrayRef<ManagedValue> args,
                                              SGFContext C) {
  assert(args.size() == 2 && "markDependence should have two value args");
  assert(subs.size() == 2 && "markDependence should have two generic args");

  SILValue result =
    gen.B.createMarkDependence(loc, args[0].forward(gen), args[1].getValue());
  return gen.emitManagedRValueWithCleanup(result);
}

/// Specialized emitter for type traits.
template<TypeTraitResult (TypeBase::*Trait)(),
         BuiltinValueKind Kind>
static ManagedValue emitBuiltinTypeTrait(SILGenFunction &gen,
                                        SILLocation loc,
                                        ArrayRef<Substitution> substitutions,
                                        ArrayRef<ManagedValue> args,
                                        SGFContext C) {
  assert(substitutions.size() == 1
         && "type trait should take a single type parameter");
  assert(args.size() == 1
         && "type trait should take a single argument");
  
  unsigned result;
  
  auto traitTy = substitutions[0].getReplacement()->getCanonicalType();
  
  switch ((traitTy.getPointer()->*Trait)()) {
  // If the type obviously has or lacks the trait, emit a constant result.
  case TypeTraitResult::IsNot:
    result = 0;
    break;
  case TypeTraitResult::Is:
    result = 1;
    break;
      
  // If not, emit the builtin call normally. Specialization may be able to
  // eliminate it later, or we'll lower it away at IRGen time.
  case TypeTraitResult::CanBe: {
    auto &C = gen.getASTContext();
    auto int8Ty = BuiltinIntegerType::get(8, C)->getCanonicalType();
    auto apply = gen.B.createBuiltin(loc,
                                     C.getIdentifier(getBuiltinName(Kind)),
                                     SILType::getPrimitiveObjectType(int8Ty),
                                     substitutions, args[0].getValue());
    
    return ManagedValue::forUnmanaged(apply);
  }
  }
  
  // Produce the result as an integer literal constant.
  auto val = gen.B.createIntegerLiteral(
      loc, SILType::getBuiltinIntegerType(8, gen.getASTContext()),
      (uintmax_t)result);
  return ManagedValue::forUnmanaged(val);
}  

Optional<SpecializedEmitter>
SpecializedEmitter::forDecl(SILGenModule &SGM, SILDeclRef function) {
  // Only consider standalone declarations in the Builtin module.
  if (function.kind != SILDeclRef::Kind::Func)
    return None;
  if (!function.hasDecl())
    return None;  
  ValueDecl *decl = function.getDecl();
  if (!isa<BuiltinUnit>(decl->getDeclContext()))
    return None;

  const BuiltinInfo &builtin = SGM.M.getBuiltinInfo(decl->getName());
  switch (builtin.ID) {
  // All the non-SIL, non-type-trait builtins should use the
  // named-builtin logic, which just emits the builtin as a call to a
  // builtin function.  This includes builtins that aren't even declared
  // in Builtins.def, i.e. all of the LLVM intrinsics.
  //
  // We do this in a separate pass over Builtins.def to avoid creating
  // a bunch of identical cases.
#define BUILTIN(Id, Name, Attrs)                                            \
  case BuiltinValueKind::Id:
#define BUILTIN_SIL_OPERATION(Id, Name, Overload)
#define BUILTIN_TYPE_TRAIT_OPERATION(Id, Name)
#include "swift/AST/Builtins.def"
  case BuiltinValueKind::None:
    return SpecializedEmitter(decl->getName());

  // Do a second pass over Builtins.def, ignoring all the cases for
  // which we emitted something above.
#define BUILTIN(Id, Name, Attrs)

  // Use specialized emitters for SIL builtins.
#define BUILTIN_SIL_OPERATION(Id, Name, Overload)                           \
  case BuiltinValueKind::Id:                                                \
    return SpecializedEmitter(&emitBuiltin##Id);
  
  // Lower away type trait builtins when they're trivially solvable.
#define BUILTIN_TYPE_TRAIT_OPERATION(Id, Name)                              \
  case BuiltinValueKind::Id:                                                \
    return SpecializedEmitter(&emitBuiltinTypeTrait<&TypeBase::Name,        \
                                                    BuiltinValueKind::Id>);

#include "swift/AST/Builtins.def"
  }
  llvm_unreachable("bad builtin kind");
}
