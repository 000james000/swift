//===--- GenProto.h - Swift IR generation for prototypes --------*- C++ -*-===//
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
//  This file provides the private interface to the protocol-emission code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENPROTO_H
#define SWIFT_IRGEN_GENPROTO_H

namespace llvm {
  class Type;
}

namespace swift {
  class CanType;
  class FuncDecl;
  class ProtocolConformance;
  struct SILDeclRef;
  class SILType;
  class SILFunction;

namespace irgen {
  class AbstractCallee;
  class Address;
  class Explosion;
  class CallEmission;
  class IRGenFunction;
  class IRGenModule;
  class TypeInfo;

  /// Emit the metadata and witness table initialization for an allocated
  /// opaque existential container.
  Address emitOpaqueExistentialContainerInit(IRGenFunction &IGF,
                                   Address dest,
                                   SILType destType,
                                   CanType formalSrcType,
                                   SILType loweredSrcType,
                                   ArrayRef<ProtocolConformance*> conformances);

  /// Emit an existential metatype container from a metatype value
  /// as an explosion.
  void emitExistentialMetatypeContainer(IRGenFunction &IGF,
                                        Explosion &out,
                                        SILType outType,
                                        llvm::Value *metatype,
                                        SILType metatypeType,
                                 ArrayRef<ProtocolConformance*> conformances);
  
  /// Emit a class existential container from a class instance value
  /// as an explosion.
  void emitClassExistentialContainer(IRGenFunction &IGF,
                                 Explosion &out,
                                 SILType outType,
                                 llvm::Value *instance,
                                 CanType instanceFormalType,
                                 SILType instanceLoweredType,
                                 ArrayRef<ProtocolConformance*> conformances);

  /// "Deinitialize" an existential container whose contained value is allocated
  /// but uninitialized, by deallocating the buffer owned by the container if any.
  void emitOpaqueExistentialContainerDeinit(IRGenFunction &IGF,
                                            Address container,
                                            SILType type);
  
  /// Emit a projection from an existential container address to the address
  /// of its concrete value buffer.
  ///
  /// \param openedArchetype If non-null, the archetype that will capture the
  /// metadata and witness tables produced by projecting the archetype.
  Address emitOpaqueExistentialProjection(IRGenFunction &IGF,
                                          Address base,
                                          SILType baseTy,
                                          CanArchetypeType openedArchetype);
  
  /// Extract the instance pointer from a class existential value.
  ///
  /// \param openedArchetype If non-null, the archetype that will capture the
  /// metadata and witness tables produced by projecting the archetype.
  llvm::Value *emitClassExistentialProjection(IRGenFunction &IGF,
                                              Explosion &base,
                                              SILType baseTy,
                                              CanArchetypeType openedArchetype);

  /// Extract the metatype pointer from an existential metatype value.
  ///
  /// \param openedTy If non-null, a metatype of the archetype that
  ///   will capture the metadata and witness tables
  llvm::Value *emitExistentialMetatypeProjection(IRGenFunction &IGF,
                                                 Explosion &base,
                                                 SILType baseTy,
                                                 CanType openedTy);

  /// Extract the method pointer from an archetype's witness table
  /// as a function value.
  void emitWitnessMethodValue(IRGenFunction &IGF,
                              CanType baseTy,
                              SILDeclRef member,
                              ProtocolConformance *conformance,
                              Explosion &out);
  
  /// Add the witness arguments necessary for calling a function with
  /// the given generics clause.
  void expandPolymorphicSignature(IRGenModule &IGM,
                                  CanSILFunctionType type,
                                  SmallVectorImpl<llvm::Type*> &types);

  /// In the prelude of a generic function, perform the bindings for a
  /// generics clause.
  void emitPolymorphicParameters(IRGenFunction &IGF,
                                 SILFunction &Fn,
                                 Explosion &args);
  
  /// Perform the metadata bindings necessary to emit a generic value witness.
  void emitPolymorphicParametersForGenericValueWitness(IRGenFunction &IGF,
                                                       NominalTypeDecl *ntd,
                                                       llvm::Value *selfMeta);

  /// When calling a polymorphic call, pass the arguments for the
  /// generics clause.
  void emitPolymorphicArguments(IRGenFunction &IGF,
                                CanSILFunctionType origType,
                                CanSILFunctionType substType,
                                ArrayRef<Substitution> subs,
                                Explosion &args);

  /// True if a type has a generic-parameter-dependent value witness table.
  /// Currently, this is true if the size and/or alignment of the type is
  /// dependent on its generic parameters.
  bool hasDependentValueWitnessTable(IRGenModule &IGM, CanType ty);
  
  /// Emit a value-witness table for the given type, which is assumed
  /// to be non-dependent.
  llvm::Constant *emitValueWitnessTable(IRGenModule &IGM, CanType type);

  /// Emit the elements of a dependent value witness table template into a
  /// vector.
  void emitDependentValueWitnessTablePattern(IRGenModule &IGM,
                                    CanType abstractType,
                                    SmallVectorImpl<llvm::Constant*> &fields);

  /// Emit references to the witness tables for the substituted type
  /// in the given substitution.
  void emitWitnessTableRefs(IRGenFunction &IGF, const Substitution &sub,
                            SmallVectorImpl<llvm::Value *> &out);

  /// Emit a witness table reference.
  llvm::Value *emitWitnessTableRef(IRGenFunction &IGF,
                                   CanArchetypeType archetype,
                                   ProtocolDecl *protocol);

  /// Emit a dynamic metatype lookup for the given archetype.
  llvm::Value *emitDynamicTypeOfOpaqueArchetype(IRGenFunction &IGF,
                                                Address archetypeAddr,
                                                SILType archetypeType);
  
  /// Emit the existential metatype of an opaque existential value.
  void emitMetatypeOfOpaqueExistential(IRGenFunction &IGF, Address addr,
                                       SILType type, Explosion &out);
  
  /// Emit the existential metatype of a class existential value.
  void emitMetatypeOfClassExistential(IRGenFunction &IGF,
                                      Explosion &value,
                                      SILType type, Explosion &out);
  
  std::pair<Address, llvm::Value*>
  emitIndirectExistentialProjectionWithMetadata(IRGenFunction &IGF,
                                                Address base,
                                                SILType baseTy,
                                                CanType openedArchetype);

  /// True if the protocol requires a witness table for method dispatch.
  bool requiresProtocolWitnessTable(ProtocolDecl *protocol);
  
} // end namespace irgen
} // end namespace swift

#endif
