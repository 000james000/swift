//===--- GenMeta.cpp - IR generation for metadata constructs --------------===//
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
//  This file implements IR generation for metadata constructs like
//  metatypes and modules.  These is presently always trivial, but in
//  the future we will likely have some sort of physical
//  representation for at least some metatypes.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Substitution.h"
#include "swift/AST/Types.h"
#include "swift/ABI/MetadataValues.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/ADT/SmallString.h"

#include "Address.h"
#include "Callee.h"
#include "ClassMetadataLayout.h"
#include "FixedTypeInfo.h"
#include "GenClass.h"
#include "GenPoly.h"
#include "GenProto.h"
#include "GenStruct.h"
#include "IRGenModule.h"
#include "IRGenDebugInfo.h"
#include "Linking.h"
#include "ScalarTypeInfo.h"
#include "StructMetadataLayout.h"
#include "StructLayout.h"
#include "EnumMetadataLayout.h"

#include "GenMeta.h"

using namespace swift;
using namespace irgen;

/// Produce a constant to place in a metatype's isa field
/// corresponding to the given metadata kind.
static llvm::ConstantInt *getMetadataKind(IRGenModule &IGM,
                                          MetadataKind kind) {
  return llvm::ConstantInt::get(IGM.MetadataKindTy, uint8_t(kind));
}

/// Emit a reference to the Swift metadata for an Objective-C class.
static llvm::Value *emitObjCMetadataRef(IRGenFunction &IGF,
                                        ClassDecl *theClass) {
  // Derive a pointer to the Objective-C class.
  auto classPtr = IGF.IGM.getAddrOfObjCClass(theClass, NotForDefinition);

  // Fetch the metadata for that class.
  auto call = IGF.Builder.CreateCall(IGF.IGM.getGetObjCClassMetadataFn(),
                                     classPtr);
  call->setDoesNotThrow();
  call->setDoesNotAccessMemory();
  call->setCallingConv(IGF.IGM.RuntimeCC);
  return call;
}

namespace {
  /// A structure for collecting generic arguments for emitting a
  /// nominal metadata reference.  The structure produced here is
  /// consumed by swift_getGenericMetadata() and must correspond to
  /// the fill operations that the compiler emits for the bound decl.
  struct GenericArguments {
    /// The values to use to initialize the arguments structure.
    SmallVector<llvm::Value *, 8> Values;
    SmallVector<llvm::Type *, 8> Types;

    void collect(IRGenFunction &IGF, BoundGenericType *type) {
      // Add all the argument archetypes.
      // TODO: only the *primary* archetypes
      // TODO: not archetypes from outer contexts
      // TODO: but we are partially determined by the outer context!
      for (auto &sub : type->getSubstitutions(/*FIXME:*/nullptr, nullptr)) {
        CanType subbed = sub.Replacement->getCanonicalType();
        Values.push_back(IGF.emitTypeMetadataRef(subbed));
      }

      // All of those values are metadata pointers.
      Types.append(Values.size(), IGF.IGM.TypeMetadataPtrTy);

      // Add protocol witness tables for all those archetypes.
      for (auto &sub : type->getSubstitutions(/*FIXME:*/nullptr, nullptr))
        emitWitnessTableRefs(IGF, sub, Values);

      // All of those values are witness table pointers.
      Types.append(Values.size() - Types.size(), IGF.IGM.WitnessTablePtrTy);
    }
  };
}

static bool isMetadataIndirect(IRGenModule &IGM, NominalTypeDecl *theDecl) {
  // FIXME
  return false;
}

/// Attempts to return a constant heap metadata reference for a
/// nominal type.
llvm::Constant *irgen::tryEmitConstantHeapMetadataRef(IRGenModule &IGM,
                                                      CanType type) {
  assert(isa<NominalType>(type) || isa<BoundGenericType>(type));

  // We can't do this for any types with generic parameters, either
  // directly or inherited from the context.
  if (isa<BoundGenericType>(type))
    return nullptr;
  auto theDecl = cast<NominalType>(type)->getDecl();
  if (theDecl->getGenericParamsOfContext())
    return nullptr;

  if (auto theClass = dyn_cast<ClassDecl>(theDecl))
    if (!hasKnownSwiftMetadata(IGM, theClass))
      return IGM.getAddrOfObjCClass(theClass, NotForDefinition);

  if (isMetadataIndirect(IGM, theDecl))
    return nullptr;

  return IGM.getAddrOfTypeMetadata(type, false, false);
}

/// Returns a metadata reference for a class type.
static llvm::Value *emitNominalMetadataRef(IRGenFunction &IGF,
                                           NominalTypeDecl *theDecl,
                                           CanType theType) {
  // If this is a class that might not have Swift metadata, we need to
  // transform it.
  if (auto theClass = dyn_cast<ClassDecl>(theDecl)) {
    if (!hasKnownSwiftMetadata(IGF.IGM, theClass)) {
      assert(!theDecl->getGenericParamsOfContext() &&
             "ObjC class cannot be generic");
      return emitObjCMetadataRef(IGF, theClass);
    }
  }

  auto generics = isa<ProtocolDecl>(theDecl)
                    ? nullptr
                    : theDecl->getGenericParamsOfContext();

  bool isPattern = (generics != nullptr);
  assert(!isPattern || isa<BoundGenericType>(theType));
  assert(isPattern || isa<NominalType>(theType));

  // If this is generic, check to see if we've maybe got a local
  // reference already.
  if (isPattern) {
    if (auto cache = IGF.tryGetLocalTypeData(theType, LocalTypeData::Metatype))
      return cache;
  }

  bool isIndirect = isMetadataIndirect(IGF.IGM, theDecl);

  // Grab a reference to the metadata or metadata template.
  CanType declaredType = theDecl->getDeclaredType()->getCanonicalType();
  llvm::Value *metadata = IGF.IGM.getAddrOfTypeMetadata(declaredType,
                                                        isIndirect, isPattern);

  // If it's indirected, go ahead and load the true value to use.
  // TODO: startup performance might force this to be some sort of
  // lazy check.
  if (isIndirect) {
    auto addr = Address(metadata, IGF.IGM.getPointerAlignment());
    metadata = IGF.Builder.CreateLoad(addr, "metadata.direct");
  }

  // If we don't have generic parameters, that's all we need.
  if (!generics) {
    assert(metadata->getType() == IGF.IGM.TypeMetadataPtrTy);
    return metadata;
  }

  // Okay, we need to call swift_getGenericMetadata.
  assert(metadata->getType() == IGF.IGM.TypeMetadataPatternPtrTy);

  // Grab the substitutions.
  auto boundGeneric = cast<BoundGenericType>(theType);
  assert(boundGeneric->getDecl() == theDecl);

  GenericArguments genericArgs;
  genericArgs.collect(IGF, boundGeneric);

  // Slam that information directly into the generic arguments buffer.
  auto argsBufferTy =
    llvm::StructType::get(IGF.IGM.LLVMContext, genericArgs.Types);
  Address argsBuffer = IGF.createAlloca(argsBufferTy,
                                        IGF.IGM.getPointerAlignment(),
                                        "generic.arguments");
  for (unsigned i = 0, e = genericArgs.Values.size(); i != e; ++i) {
    Address elt = IGF.Builder.CreateStructGEP(argsBuffer, i,
                                              IGF.IGM.getPointerSize() * i);
    IGF.Builder.CreateStore(genericArgs.Values[i], elt);
  }

  // Cast to void*.
  llvm::Value *arguments =
    IGF.Builder.CreateBitCast(argsBuffer.getAddress(), IGF.IGM.Int8PtrTy);

  // Make the call.
  auto result = IGF.Builder.CreateCall2(IGF.IGM.getGetGenericMetadataFn(),
                                        metadata, arguments);
  result->setDoesNotThrow();

  // FIXME: Save scope type metadata.
  return result;
}

/// Is the given class known to have Swift-compatible metadata?
bool irgen::hasKnownSwiftMetadata(IRGenModule &IGM, ClassDecl *theClass) {
  // For now, the fact that a declaration was not implemented in Swift
  // is enough to conclusively force us into a slower path.
  // Eventually we might have an attribute here or something based on
  // the deployment target.
  return hasKnownSwiftImplementation(IGM, theClass);
}

/// Is the given class known to have an implementation in Swift?
bool irgen::hasKnownSwiftImplementation(IRGenModule &IGM, ClassDecl *theClass) {
  return !theClass->hasClangNode();
}

/// Is the given method known to be callable by vtable lookup?
bool irgen::hasKnownVTableEntry(IRGenModule &IGM, FuncDecl *theMethod) {
  auto theClass = dyn_cast<ClassDecl>(theMethod->getDeclContext());
  if (!theClass) {
    assert(theMethod->hasClangNode() && "overriding a non-imported method");
    return false;
  }
  return hasKnownSwiftImplementation(IGM, theClass);
}

/// Emit a string encoding the labels in the given tuple type.
static llvm::Constant *getTupleLabelsString(IRGenModule &IGM,
                                            CanTupleType type) {
  bool hasLabels = false;
  llvm::SmallString<128> buffer;
  for (auto &elt : type->getFields()) {
    if (elt.hasName()) {
      hasLabels = true;
      buffer.append(elt.getName().str());
    }

    // Each label is space-terminated.
    buffer += ' ';
  }

  // If there are no labels, use a null pointer.
  if (!hasLabels) {
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }

  // Otherwise, create a new string literal.
  // This method implicitly adds a null terminator.
  return IGM.getAddrOfGlobalString(buffer);
}

namespace {
  /// A visitor class for emitting a reference to a metatype object.
  class EmitTypeMetadataRef
    : public CanTypeVisitor<EmitTypeMetadataRef, llvm::Value *> {
  private:
    IRGenFunction &IGF;
  public:
    EmitTypeMetadataRef(IRGenFunction &IGF) : IGF(IGF) {}

#define TREAT_AS_OPAQUE(KIND)                          \
    llvm::Value *visit##KIND##Type(KIND##Type *type) { \
      return visitOpaqueType(CanType(type));           \
    }
    TREAT_AS_OPAQUE(BuiltinInteger)
    TREAT_AS_OPAQUE(BuiltinFloat)
    TREAT_AS_OPAQUE(BuiltinRawPointer)
#undef TREAT_AS_OPAQUE

    llvm::Value *emitDirectMetadataRef(CanType type) {
      return IGF.IGM.getAddrOfTypeMetadata(type,
                                           /*indirect*/ false,
                                           /*pattern*/ false);
    }

    /// The given type should use opaque type info.  We assume that
    /// the runtime always provides an entry for such a type;  right
    /// now, that mapping is as one of the integer types.
    llvm::Value *visitOpaqueType(CanType type) {
      auto &opaqueTI = cast<FixedTypeInfo>(IGF.IGM.getTypeInfoForLowered(type));
      assert(opaqueTI.getFixedSize() ==
             Size(opaqueTI.getFixedAlignment().getValue()));
      assert(opaqueTI.getFixedSize().isPowerOf2());
      auto numBits = 8 * opaqueTI.getFixedSize().getValue();
      auto intTy = BuiltinIntegerType::get(numBits, IGF.IGM.Context);
      return emitDirectMetadataRef(CanType(intTy));
    }

    llvm::Value *visitBuiltinObjectPointerType(CanBuiltinObjectPointerType type) {
      return emitDirectMetadataRef(type);
    }

    llvm::Value *visitBuiltinObjCPointerType(CanBuiltinObjCPointerType type) {
      return emitDirectMetadataRef(type);
    }

    llvm::Value *visitBuiltinVectorType(CanBuiltinVectorType type) {
      return emitDirectMetadataRef(type);
    }

    llvm::Value *visitNominalType(CanNominalType type) {
      assert(!type->isExistentialType());
      return emitNominalMetadataRef(IGF, type->getDecl(), type);
    }

    llvm::Value *visitBoundGenericType(CanBoundGenericType type) {
      assert(!type->isExistentialType());
      return emitNominalMetadataRef(IGF, type->getDecl(), type);
    }

    llvm::Value *visitTupleType(CanTupleType type) {
      if (auto cached = tryGetLocal(type))
        return cached;

      // I think the sanest thing to do here is drop labels, but maybe
      // that's not correct.  If so, that's really unfortunate in a
      // lot of ways.

      // Er, varargs bit?  Should that go in?


      switch (type->getNumElements()) {
      case 0: {// Special case the empty tuple, just use the global descriptor.
        llvm::Constant *fullMetadata = IGF.IGM.getEmptyTupleMetadata();
        llvm::Constant *indices[] = {
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0),
          llvm::ConstantInt::get(IGF.IGM.Int32Ty, 1)
        };
        return llvm::ConstantExpr::getInBoundsGetElementPtr(fullMetadata,
                                                            indices);
      }

      case 1:
          // For metadata purposes, we consider a singleton tuple to be
          // isomorphic to its element type.
        return visit(type.getElementType(0));

      case 2: {
        // Find the metadata pointer for this element.
        llvm::Value *elt0Metadata = visit(type.getElementType(0));
        llvm::Value *elt1Metadata = visit(type.getElementType(1));

        llvm::Value *args[] = {
          elt0Metadata, elt1Metadata,
          getTupleLabelsString(IGF.IGM, type),
          llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
        };

        auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleMetadata2Fn(),
                                           args);
        call->setDoesNotThrow();
        call->setCallingConv(IGF.IGM.RuntimeCC);
        return setLocal(CanType(type), call);
      }

      case 3: {
        // Find the metadata pointer for this element.
        llvm::Value *elt0Metadata = visit(type.getElementType(0));
        llvm::Value *elt1Metadata = visit(type.getElementType(1));
        llvm::Value *elt2Metadata = visit(type.getElementType(2));

        llvm::Value *args[] = {
          elt0Metadata, elt1Metadata, elt2Metadata,
          getTupleLabelsString(IGF.IGM, type),
          llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
        };

        auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleMetadata3Fn(),
                                           args);
        call->setDoesNotThrow();
        call->setCallingConv(IGF.IGM.RuntimeCC);
        return setLocal(CanType(type), call);
      }
      default:
        // TODO: use a caching entrypoint (with all information
        // out-of-line) for non-dependent tuples.

        llvm::Value *pointerToFirst = nullptr; // appease -Wuninitialized

        auto elements = type.getElementTypes();
        auto arrayTy = llvm::ArrayType::get(IGF.IGM.TypeMetadataPtrTy,
                                            elements.size());
        Address buffer = IGF.createAlloca(arrayTy,IGF.IGM.getPointerAlignment(),
                                          "tuple-elements");
        for (unsigned i = 0, e = elements.size(); i != e; ++i) {
          // Find the metadata pointer for this element.
          llvm::Value *eltMetadata = visit(elements[i]);

          // GEP to the appropriate element and store.
          Address eltPtr = IGF.Builder.CreateStructGEP(buffer, i,
                                                     IGF.IGM.getPointerSize());
          IGF.Builder.CreateStore(eltMetadata, eltPtr);

          // Remember the GEP to the first element.
          if (i == 0) pointerToFirst = eltPtr.getAddress();
        }

        llvm::Value *args[] = {
          llvm::ConstantInt::get(IGF.IGM.SizeTy, elements.size()),
          pointerToFirst,
          getTupleLabelsString(IGF.IGM, type),
          llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
        };

        auto call = IGF.Builder.CreateCall(IGF.IGM.getGetTupleMetadataFn(),
                                           args);
        call->setDoesNotThrow();
        call->setCallingConv(IGF.IGM.RuntimeCC);

        return setLocal(type, call);
      }
    }

    llvm::Value *visitPolymorphicFunctionType(CanPolymorphicFunctionType type) {
      IGF.unimplemented(SourceLoc(),
                        "metadata ref for polymorphic function type");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }

    llvm::Value *visitGenericFunctionType(CanGenericFunctionType type) {
      IGF.unimplemented(SourceLoc(),
                        "metadata ref for generic function type");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }

    llvm::Value *visitFunctionType(CanFunctionType type) {
      if (auto metatype = tryGetLocal(type))
        return metatype;

      // TODO: use a caching entrypoint (with all information
      // out-of-line) for non-dependent functions.

      auto argMetadata = visit(type.getInput());
      auto resultMetadata = visit(type.getResult());

      auto call = IGF.Builder.CreateCall2(IGF.IGM.getGetFunctionMetadataFn(),
                                          argMetadata, resultMetadata);
      call->setDoesNotThrow();
      call->setCallingConv(IGF.IGM.RuntimeCC);

      return setLocal(CanType(type), call);
    }

    llvm::Value *visitArrayType(CanArrayType type) {
      IGF.unimplemented(SourceLoc(), "metadata ref for array type");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }

    llvm::Value *visitMetatypeType(CanMetatypeType type) {
      if (auto metatype = tryGetLocal(type))
        return metatype;

      auto instMetadata = visit(type.getInstanceType());
      auto call = IGF.Builder.CreateCall(IGF.IGM.getGetMetatypeMetadataFn(),
                                         instMetadata);
      call->setDoesNotThrow();
      call->setCallingConv(IGF.IGM.RuntimeCC);

      return setLocal(type, call);
    }

    llvm::Value *visitModuleType(CanModuleType type) {
      IGF.unimplemented(SourceLoc(), "metadata ref for module type");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }
      
    llvm::Value *emitExistentialTypeMetadata(CanType type) {
      SmallVector<ProtocolDecl*, 2> protocols;
      bool isExistential = type->isExistentialType(protocols);
      assert(isExistential); (void)isExistential;
      
      // Collect references to the protocol descriptors.
      auto descriptorArrayTy
        = llvm::ArrayType::get(IGF.IGM.ProtocolDescriptorPtrTy,
                               protocols.size());
      Address descriptorArray = IGF.createAlloca(descriptorArrayTy,
                                                 IGF.IGM.getPointerAlignment(),
                                                 "protocols");
      descriptorArray = IGF.Builder.CreateBitCast(descriptorArray,
                               IGF.IGM.ProtocolDescriptorPtrTy->getPointerTo());
      
      unsigned index = 0;
      for (auto *p : protocols) {
        llvm::Value *ref = emitProtocolDescriptorRef(IGF, p);
        Address slot = IGF.Builder.CreateConstArrayGEP(descriptorArray,
                                               index, IGF.IGM.getPointerSize());
        IGF.Builder.CreateStore(ref, slot);
        ++index;
      }
      
      auto call = IGF.Builder.CreateCall2(IGF.IGM.getGetExistentialMetadataFn(),
                                        IGF.IGM.getSize(Size(protocols.size())),
                                        descriptorArray.getAddress());
      call->setDoesNotThrow();
      call->setCallingConv(IGF.IGM.RuntimeCC);
      return setLocal(type, call);
    }

    llvm::Value *visitProtocolType(CanProtocolType type) {
      return emitExistentialTypeMetadata(type);
    }
      
    llvm::Value *visitProtocolCompositionType(CanProtocolCompositionType type) {
      return emitExistentialTypeMetadata(type);
    }

    llvm::Value *visitReferenceStorageType(CanReferenceStorageType type) {
      IGF.unimplemented(SourceLoc(), "metadata ref for ref storage type");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }
    llvm::Value *visitSILFunctionType(CanSILFunctionType type) {
      IGF.unimplemented(SourceLoc(), "metadata ref for SIL function type");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }

    llvm::Value *visitArchetypeType(CanArchetypeType type) {
      return IGF.getLocalTypeData(type, LocalTypeData::Metatype);
    }

    llvm::Value *visitGenericTypeParamType(CanGenericTypeParamType type) {
      IGF.unimplemented(SourceLoc(), "metadata ref for generic type parameter");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }

    llvm::Value *visitDependentMemberType(CanDependentMemberType type) {
      IGF.unimplemented(SourceLoc(), "metadata ref for dependent member type");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }

    llvm::Value *visitLValueType(CanLValueType type) {
      llvm_unreachable("should have been lowered by SILGen");
    }
    llvm::Value *visitInOutType(CanInOutType type) {
      IGF.unimplemented(SourceLoc(), "metadata ref for @inout type");
      return llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy);
    }

    /// Try to find the metatype in local data.
    llvm::Value *tryGetLocal(CanType type) {
      return IGF.tryGetLocalTypeData(type, LocalTypeData::Metatype);
    }

    /// Set the metatype in local data.
    llvm::Value *setLocal(CanType type, llvm::Value *metatype) {
      // FIXME: Save scope type metadata.
      return metatype;
    }
  };
}

/// Produce the type metadata pointer for the given type.
llvm::Value *IRGenFunction::emitTypeMetadataRef(CanType type) {
  return EmitTypeMetadataRef(*this).visit(type);
}

llvm::Value *IRGenFunction::emitTypeMetadataRef(SILType type) {
  return emitTypeMetadataRef(type.getSwiftRValueType());
}

/// Produce the heap metadata pointer for the given class type.  For
/// Swift-defined types, this is equivalent to the metatype for the
/// class, but for Objective-C-defined types, this is the class
/// object.
llvm::Value *irgen::emitClassHeapMetadataRef(IRGenFunction &IGF, CanType type) {
  assert(isa<ClassType>(type) || isa<BoundGenericClassType>(type));

  // ObjC-defined classes will always be top-level non-generic classes.

  if (auto classType = dyn_cast<ClassType>(type)) {
    auto theClass = classType->getDecl();
    if (hasKnownSwiftMetadata(IGF.IGM, theClass))
      return EmitTypeMetadataRef(IGF).visitClassType(classType);
    return IGF.IGM.getAddrOfObjCClass(theClass, NotForDefinition);
  }

  auto classType = cast<BoundGenericClassType>(type);
  assert(hasKnownSwiftMetadata(IGF.IGM, classType->getDecl()));
  return EmitTypeMetadataRef(IGF).visitBoundGenericClassType(classType);
}

llvm::Value *irgen::emitClassHeapMetadataRef(IRGenFunction &IGF, SILType type) {
  return emitClassHeapMetadataRef(IGF, type.getSwiftRValueType());
}

namespace {
  /// A CRTP type visitor for deciding whether the metatype for a type
  /// has trivial representation.
  struct HasTrivialMetatype : CanTypeVisitor<HasTrivialMetatype, bool> {
    /// Class metatypes have non-trivial representation due to the
    /// possibility of subclassing.
    bool visitClassType(CanClassType type) {
      return false;
    }
    bool visitBoundGenericClassType(CanBoundGenericClassType type) {
      return false;
    }

    /// Archetype metatypes have non-trivial representation in case
    /// they instantiate to a class metatype.
    bool visitArchetypeType(CanArchetypeType type) {
      return false;
    }
    
    /// All levels of class metatypes support subtyping.
    bool visitMetatypeType(CanMetatypeType type) {
      return visit(type.getInstanceType());
    }

    /// Existential metatypes have non-trivial representation because
    /// they can refer to an arbitrary metatype. Everything else is trivial.
    bool visitType(CanType type) {
      return !type->isExistentialType();
    }
  };
}

/// Does the metatype for the given type have a trivial representation?
bool IRGenModule::isTrivialMetatype(CanMetatypeType metaTy) {
  // FIXME: We still need to handle unlowered metatypes from the AST for
  // IRGen protocol witnesses. This can go away (with the HasTrivialMetatype
  // visitor) when we enable SIL witnesses.
  if (!metaTy->hasThin())
    return HasTrivialMetatype().visit(metaTy.getInstanceType());
  return metaTy->isThin();
}

/// Emit a DeclRefExpr which refers to a metatype.
void irgen::emitMetatypeRef(IRGenFunction &IGF, CanMetatypeType type,
                            Explosion &explosion) {
  // Some metatypes have trivial representation.
  if (type->isThin())
    return;

  // Otherwise, emit a metadata reference.
  llvm::Value *metadata = IGF.emitTypeMetadataRef(type.getInstanceType());
  explosion.add(metadata);
}

/*****************************************************************************/
/** Nominal Type Descriptor Emission *****************************************/
/*****************************************************************************/

namespace {
  template<class Impl>
  class NominalTypeDescriptorBuilderBase {
    Impl &asImpl() { return *static_cast<Impl*>(this); }

  protected:
    IRGenModule &IGM;
    std::vector<llvm::Constant*> Fields;
    
  public:
    NominalTypeDescriptorBuilderBase(IRGenModule &IGM)
      : IGM(IGM) {}
    
    void layout() {
      asImpl().addKind();
      asImpl().addName();
      asImpl().addKindDependentFields();
      asImpl().addGenericParams();
    }

    void addConstantSize(intptr_t value) {
      Fields.push_back(llvm::ConstantInt::get(IGM.SizeTy, value));
    }
    
    void addKind() {
      addConstantSize(asImpl().getKind());
    }
    
    void addName() {
      NominalTypeDecl *ntd = asImpl().getTarget();
      auto name = LinkEntity::forTypeMangling(
                                    ntd->getDeclaredType()->getCanonicalType());
      llvm::SmallString<32> mangling;
      name.mangle(mangling);
      Fields.push_back(IGM.getAddrOfGlobalString(mangling));
    }
    
    void addGenericParams() {
      NominalTypeDecl *ntd = asImpl().getTarget();
      if (!ntd->getGenericParams()) {
        // If there are no generic parameters, there is no generic parameter
        // vector.
        addConstantSize(0);
        addConstantSize(0);
        return;
      }
      
      // uintptr_t GenericParameterVectorOffset;
      addConstantSize(asImpl().getGenericParamsOffset());

      // The archetype order here needs to be consistent with
      // MetadataLayout::addGenericFields.
      
      // Note that we intentionally don't forward the generic arguments.
      
      // Add all the primary archetypes.
      // TODO: only the *primary* archetypes.
      // TODO: not archetypes from outer contexts.
      auto allArchetypes = ntd->getGenericParams()->getAllArchetypes();
      
      // uintptr_t NumGenericParameters;
      addConstantSize(allArchetypes.size());
      
      // GenericParameter Parameters[NumGenericParameters];
      // struct GenericParameter {
      for (auto archetype : allArchetypes) {
        //   uintptr_t NumWitnessTables;
        // Count the protocol conformances that require witness tables.
        unsigned count = std::count_if(archetype->getConformsTo().begin(),
                                       archetype->getConformsTo().end(),
               [](ProtocolDecl *p) { return requiresProtocolWitnessTable(p); });
        addConstantSize(count);
      }
      // };
    }
    
    llvm::Constant *emit() {
      asImpl().layout();
      auto init = llvm::ConstantStruct::getAnon(Fields);
      
      auto var = cast<llvm::GlobalVariable>(
                      IGM.getAddrOfNominalTypeDescriptor(asImpl().getTarget(),
                                                         init->getType()));
      var->setConstant(true);
      var->setInitializer(init);
      return var;
    }
    
    // Derived class must provide:
    //   NominalTypeDecl *getTarget();
    //   unsigned getKind();
    //   unsigned getGenericParamsOffset();
    //   void addKindDependentFields();
  };
  
  class StructNominalTypeDescriptorBuilder
    : public NominalTypeDescriptorBuilderBase<StructNominalTypeDescriptorBuilder>
  {
    using super
      = NominalTypeDescriptorBuilderBase<StructNominalTypeDescriptorBuilder>;
    
    // Offsets of key fields in the metadata records.
    unsigned FieldVectorOffset, GenericParamsOffset;
    
    StructDecl *Target;
    
  public:
    StructNominalTypeDescriptorBuilder(IRGenModule &IGM,
                                       StructDecl *s)
      : super(IGM), Target(s)
    {
      // Scan the metadata layout for the struct to find the key offsets to
      // put in our descriptor.
      struct ScanForDescriptorOffsets
        : StructMetadataScanner<ScanForDescriptorOffsets>
      {
        ScanForDescriptorOffsets(IRGenModule &IGM, StructDecl *Target)
          : StructMetadataScanner(IGM, Target) {}
        
        unsigned AddressPoint = ~0U, FieldVectorOffset = ~0U,
                 GenericParamsOffset = ~0U;
        
        void noteAddressPoint() { AddressPoint = NextIndex; }
        void noteStartOfFieldOffsets() { FieldVectorOffset = NextIndex; }
        void addGenericFields(const GenericParamList &g) {
          GenericParamsOffset = NextIndex;
          StructMetadataScanner::addGenericFields(g);
        }
      };
      
      ScanForDescriptorOffsets scanner(IGM, Target);
      scanner.layout();
      assert(scanner.AddressPoint != ~0U
             && scanner.FieldVectorOffset != ~0U
             && "did not find required fields in struct metadata?!");
      assert(scanner.FieldVectorOffset >= scanner.AddressPoint
             && "found field offset vector after address point?!");
      assert(scanner.GenericParamsOffset >= scanner.AddressPoint
             && "found generic param vector after address point?!");
      FieldVectorOffset = scanner.FieldVectorOffset - scanner.AddressPoint;
      GenericParamsOffset = scanner.GenericParamsOffset == ~0U
        ? 0
        : scanner.GenericParamsOffset - scanner.AddressPoint;
    }
    
    StructDecl *getTarget() { return Target; }
    
    unsigned getKind() {
      return unsigned(NominalTypeKind::Struct);
    }
    
    unsigned getGenericParamsOffset() {
      return GenericParamsOffset;
    }
    
    void addKindDependentFields() {
      // Build the field name list.
      llvm::SmallString<64> fieldNames;
      unsigned numFields = 0;
      
      for (auto prop : Target->getStoredProperties()) {
        fieldNames.append(prop->getName().str());
        fieldNames.push_back('\0');
        ++numFields;
      }
      // The final null terminator is provided by getAddrOfGlobalString.
      
      addConstantSize(numFields);
      addConstantSize(FieldVectorOffset);
      Fields.push_back(IGM.getAddrOfGlobalString(fieldNames));
    }
  };
  
  class ClassNominalTypeDescriptorBuilder
    : public NominalTypeDescriptorBuilderBase<ClassNominalTypeDescriptorBuilder>
  {
    using super
      = NominalTypeDescriptorBuilderBase<ClassNominalTypeDescriptorBuilder>;
    
    // Offsets of key fields in the metadata records.
    unsigned FieldVectorOffset, GenericParamsOffset;
    
    ClassDecl *Target;
    
  public:
    ClassNominalTypeDescriptorBuilder(IRGenModule &IGM,
                                       ClassDecl *c)
      : super(IGM), Target(c)
    {
      // Scan the metadata layout for the class to find the key offsets to
      // put in our descriptor.
      struct ScanForDescriptorOffsets
        : ClassMetadataScanner<ScanForDescriptorOffsets>
      {
        ScanForDescriptorOffsets(IRGenModule &IGM, ClassDecl *Target)
          : ClassMetadataScanner(IGM, Target) {}
        
        unsigned AddressPoint = ~0U, FieldVectorOffset = ~0U,
                 GenericParamsOffset = ~0U;
        
        void noteAddressPoint() { AddressPoint = NextIndex; }
        void noteStartOfFieldOffsets(ClassDecl *c) {
          if (c == TargetClass) {
            FieldVectorOffset = NextIndex;
          }
        }
        void addGenericFields(const GenericParamList &g, ClassDecl *c) {
          if (c == TargetClass) {
            GenericParamsOffset = NextIndex;
          }
          ClassMetadataScanner::addGenericFields(g, c);
        }
      };
      
      ScanForDescriptorOffsets scanner(IGM, Target);
      scanner.layout();
      assert(scanner.AddressPoint != ~0U
             && "did not find fields in Class metadata?!");
      assert(scanner.FieldVectorOffset >= scanner.AddressPoint
             && "found field offset vector after address point?!");
      assert(scanner.GenericParamsOffset >= scanner.AddressPoint
             && "found generic param vector after address point?!");
      FieldVectorOffset = scanner.FieldVectorOffset == ~0U
        ? 0 : scanner.FieldVectorOffset - scanner.AddressPoint;
      GenericParamsOffset = scanner.GenericParamsOffset == ~0U
        ? 0 : scanner.GenericParamsOffset - scanner.AddressPoint;
    }
    
    ClassDecl *getTarget() { return Target; }
    
    unsigned getKind() {
      return unsigned(NominalTypeKind::Class);
    }
    
    unsigned getGenericParamsOffset() {
      return GenericParamsOffset;
    }
    
    void addKindDependentFields() {
      // Build the field name list.
      llvm::SmallString<64> fieldNames;
      unsigned numFields = 0;
      
      for (auto prop : Target->getStoredProperties()) {
        fieldNames.append(prop->getName().str());
        fieldNames.push_back('\0');
        ++numFields;
      }
      // The final null terminator is provided by getAddrOfGlobalString.
      
      addConstantSize(numFields);
      addConstantSize(FieldVectorOffset);
      Fields.push_back(IGM.getAddrOfGlobalString(fieldNames));
    }
  };
  
  class EnumNominalTypeDescriptorBuilder
    : public NominalTypeDescriptorBuilderBase<EnumNominalTypeDescriptorBuilder>
  {
    using super
      = NominalTypeDescriptorBuilderBase<EnumNominalTypeDescriptorBuilder>;
    
    // Offsets of key fields in the metadata records.
    unsigned GenericParamsOffset;
    
    EnumDecl *Target;
    
  public:
    EnumNominalTypeDescriptorBuilder(IRGenModule &IGM, EnumDecl *c)
      : super(IGM), Target(c)
    {
      // Scan the metadata layout for the class to find the key offsets to
      // put in our descriptor.
      struct ScanForDescriptorOffsets
        : EnumMetadataScanner<ScanForDescriptorOffsets>
      {
        ScanForDescriptorOffsets(IRGenModule &IGM, EnumDecl *Target)
          : EnumMetadataScanner(IGM, Target) {}
        
        unsigned AddressPoint = ~0U, FieldVectorOffset = ~0U,
                 GenericParamsOffset = ~0U;
        
        void noteAddressPoint() { AddressPoint = NextIndex; }
        void addGenericFields(const GenericParamList &g) {
          GenericParamsOffset = NextIndex;
        }
      };
      
      ScanForDescriptorOffsets scanner(IGM, Target);
      scanner.layout();
      assert(scanner.AddressPoint != ~0U
             && "did not find fields in Enum metadata?!");
      assert(scanner.GenericParamsOffset >= scanner.AddressPoint
             && "found generic param vector after address point?!");
      GenericParamsOffset = scanner.GenericParamsOffset == ~0U
        ? 0 : scanner.GenericParamsOffset - scanner.AddressPoint;
    }
    
    EnumDecl *getTarget() { return Target; }
    
    unsigned getKind() {
      return unsigned(NominalTypeKind::Enum);
    }
    
    unsigned getGenericParamsOffset() {
      return GenericParamsOffset;
    }
    
    void addKindDependentFields() {
      // FIXME: Populate.
      addConstantSize(0);
      addConstantSize(0);
      addConstantSize(0);
    }
  };
}


/*****************************************************************************/
/** Metadata Emission ********************************************************/
/*****************************************************************************/

namespace {
  /// An adapter class which turns a metadata layout class into a
  /// generic metadata layout class.
  template <class Impl, class Base>
  class GenericMetadataBuilderBase : public Base {
    typedef Base super;

    /// The generics clause for the type we're emitting.
    const GenericParamList &ClassGenerics;
    
    /// The number of generic witnesses in the type we're emitting.
    /// This is not really something we need to track.
    unsigned NumGenericWitnesses = 0;

    struct FillOp {
      unsigned FromIndex;
      unsigned ToIndex;

      FillOp() = default;
      FillOp(unsigned from, unsigned to) : FromIndex(from), ToIndex(to) {}
    };

    SmallVector<FillOp, 8> FillOps;

    enum { TemplateHeaderFieldCount = 5 };

  protected:
    /// The index of the address point in the type we're emitting.
    unsigned AddressPoint = 0;
    
    IRGenModule &IGM = super::IGM;
    using super::Fields;
    using super::asImpl;
    
    /// Set to true if the metadata record for the generic type has fields
    /// outside of the generic parameter vector.
    bool HasDependentMetadata = false;
    
    /// Set to true if the value witness table for the generic type is dependent
    /// on its generic parameters. If true, the value witness will be
    /// tail-emplaced inside the metadata pattern and initialized by the fill
    /// function. Implies HasDependentMetadata.
    bool HasDependentVWT = false;
    
    /// The index of the tail-allocated dependent VWT, if any.
    unsigned DependentVWTPoint = 0;

    template <class... T>
    GenericMetadataBuilderBase(IRGenModule &IGM, 
                               const GenericParamList &generics,
                               T &&...args)
      : super(IGM, std::forward<T>(args)...), ClassGenerics(generics) {}

    /// Emit the fill function for the template.
    llvm::Function *emitFillFunction() {
      // void (*FillFunction)(void*, const void*)
      llvm::Type *argTys[] = {IGM.Int8PtrTy, IGM.Int8PtrTy};
      auto ty = llvm::FunctionType::get(IGM.VoidTy, argTys, /*isVarArg*/ false);
      llvm::Function *f = llvm::Function::Create(ty,
                                           llvm::GlobalValue::InternalLinkage,
                                           "fill_generic_metadata",
                                           &IGM.Module);
      
      IRGenFunction IGF(IGM, f);
      if (IGM.DebugInfo)
        IGM.DebugInfo->emitArtificialFunction(IGF, f);
      
      // Execute the fill ops. Cast the parameters to word pointers because the
      // fill indexes are word-indexed.
      Explosion params = IGF.collectParameters(ResilienceExpansion::Minimal);
      llvm::Value *fullMeta = params.claimNext();
      llvm::Value *args = params.claimNext();
      
      Address fullMetaWords(IGF.Builder.CreateBitCast(fullMeta,
                                                   IGM.SizeTy->getPointerTo()),
                            Alignment(IGM.getPointerAlignment()));
      Address argWords(IGF.Builder.CreateBitCast(args,
                                                 IGM.SizeTy->getPointerTo()),
                       Alignment(IGM.getPointerAlignment()));
      
      for (auto &fillOp : FillOps) {
        auto dest = IGF.Builder.CreateConstArrayGEP(fullMetaWords,
                                                    fillOp.ToIndex,
                                                    IGM.getPointerSize());
        auto src = IGF.Builder.CreateConstArrayGEP(argWords,
                                                   fillOp.FromIndex,
                                                   IGM.getPointerSize());
        IGF.Builder.CreateStore(IGF.Builder.CreateLoad(src), dest);
      }
      
      // Derive the metadata value.
      auto addressPointAddr = IGF.Builder.CreateConstArrayGEP(fullMetaWords,
                                                          AddressPoint,
                                                          IGM.getPointerSize());
      llvm::Value *metadataValue
        = IGF.Builder.CreateBitCast(addressPointAddr.getAddress(),
                                    IGF.IGM.TypeMetadataPtrTy);
      
      // Initialize the instantiated dependent value witness table, if we have
      // one.
      llvm::Value *vwtableValue;
      if (HasDependentVWT) {
        assert(AddressPoint >= 1 && "did not set valid address point!");
        assert(DependentVWTPoint != 0 && "did not set dependent VWT point!");
        
        // Fill in the pointer from the metadata to the VWT. The VWT pointer
        // always immediately precedes the address point.
        auto vwtAddr = IGF.Builder.CreateConstArrayGEP(fullMetaWords,
                                                       DependentVWTPoint,
                                                       IGM.getPointerSize());
        auto vwtAddrVal = IGF.Builder.CreatePtrToInt(vwtAddr.getAddress(),
                                                     IGM.SizeTy);
        auto vwtRefAddr = IGF.Builder.CreateConstArrayGEP(fullMetaWords,
                                                          AddressPoint - 1,
                                                          IGM.getPointerSize());
        IGF.Builder.CreateStore(vwtAddrVal, vwtRefAddr);

        vwtableValue = IGF.Builder.CreateBitCast(vwtAddr.getAddress(),
                                                 IGF.IGM.WitnessTablePtrTy);
        
        HasDependentMetadata = true;
      }

      if (HasDependentMetadata) {
        asImpl().emitInitializeMetadata(IGF, metadataValue, vwtableValue);
      }
      
      // The metadata is now complete.
      IGF.Builder.CreateRetVoid();
      
      return f;
    }
    
  public:
    void layout() {
      // Leave room for the header.
      Fields.append(TemplateHeaderFieldCount, nullptr);

      // Lay out the template data.
      super::layout();
      
      // If we have a dependent value witness table, emit its template.
      if (HasDependentVWT) {
        // Note the dependent VWT offset.
        DependentVWTPoint = getNextIndex();
        asImpl().addDependentValueWitnessTablePattern();
      }

      // Fill in the header:
      unsigned Field = 0;

      //   void (*FillFunction)(void *, const void*);
      Fields[Field++] = emitFillFunction();
      
      //   uint32_t MetadataSize;
      // We compute this assuming that every entry in the metadata table
      // is a pointer in size.
      Size size = this->getNextIndex() * IGM.getPointerSize();
      Fields[Field++] = llvm::ConstantInt::get(IGM.Int32Ty, size.getValue());
      
      //   uint16_t NumArguments;
      // TODO: ultimately, this should be the number of actual template
      // arguments, not the number of witness tables required.
      Fields[Field++]
        = llvm::ConstantInt::get(IGM.Int16Ty, NumGenericWitnesses);

      //   uint16_t AddressPoint;
      assert(AddressPoint != 0 && "address point not noted!");
      Size addressPoint = AddressPoint * IGM.getPointerSize();
      Fields[Field++]
        = llvm::ConstantInt::get(IGM.Int16Ty, addressPoint.getValue());

      //   void *PrivateData[8];
      Fields[Field++] = getPrivateDataInit();

      assert(TemplateHeaderFieldCount == Field);
    }

    /// Write down the index of the address point.
    void noteAddressPoint() {
      AddressPoint = getNextIndex();
      super::noteAddressPoint();
    }

    /// Ignore the preallocated header.
    unsigned getNextIndex() const {
      return super::getNextIndex() - TemplateHeaderFieldCount;
    }

    template <class... T>
    void addGenericArgument(ArchetypeType *type, T &&...args) {
      FillOps.push_back(FillOp(NumGenericWitnesses++, getNextIndex()));
      super::addGenericArgument(type, std::forward<T>(args)...);
    }

    template <class... T>
    void addGenericWitnessTable(ArchetypeType *type, ProtocolDecl *protocol,
                                T &&...args) {
      FillOps.push_back(FillOp(NumGenericWitnesses++, getNextIndex()));
      super::addGenericWitnessTable(type, protocol, std::forward<T>(args)...);
    }

  private:
    static llvm::Constant *makeArray(llvm::Type *eltTy,
                                     ArrayRef<llvm::Constant*> elts) {
      auto arrayTy = llvm::ArrayType::get(eltTy, elts.size());
      return llvm::ConstantArray::get(arrayTy, elts);
    }

    /// Produce the initializer for the private-data field of the
    /// template header.
    llvm::Constant *getPrivateDataInit() {
      // Spec'ed to be 8 pointers wide.  An arbitrary choice; should
      // work out an ideal size with the runtime folks.
      auto null = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
      
      llvm::Constant *privateData[8] = {
        null, null, null, null, null, null, null, null
      };
      return makeArray(IGM.Int8PtrTy, privateData);
    }
  };
}

// Classes

namespace {
  /// An adapter for laying out class metadata.
  template <class Impl>
  class ClassMetadataBuilderBase : public ClassMetadataLayout<Impl> {
    typedef ClassMetadataLayout<Impl> super;

  protected:
    using super::IGM;
    using super::TargetClass;
    SmallVector<llvm::Constant *, 8> Fields;
    const StructLayout &Layout;    

    /// A mapping from functions to their final overriders.
    llvm::DenseMap<FuncDecl*,FuncDecl*> FinalOverriders;

    ClassMetadataBuilderBase(IRGenModule &IGM, ClassDecl *theClass,
                             const StructLayout &layout)
      : super(IGM, theClass), Layout(layout) {

      computeFinalOverriders();
    }

    unsigned getNextIndex() const { return Fields.size(); }

    /// Compute a map of all the final overriders for the class.
    void computeFinalOverriders() {
      // Walk up the whole class hierarchy.
      ClassDecl *cls = TargetClass;
      do {
        // Make sure that each function has its final overrider set.
        for (auto member : cls->getMembers()) {
          auto fn = dyn_cast<FuncDecl>(member);
          if (!fn) continue;

          // Check whether we already have an entry for this function.
          auto &finalOverrider = FinalOverriders[fn];

          // If not, the function is its own final overrider.
          if (!finalOverrider) finalOverrider = fn;

          // If the function directly overrides something, update the
          // overridden function's entry.
          if (auto overridden = fn->getOverriddenDecl())
            FinalOverriders.insert(std::make_pair(overridden, finalOverrider));

        }

        
      } while (cls->hasSuperclass() &&
               (cls = cls->getSuperclass()->getClassOrBoundGenericClass()));
    }

  public:
    /// The 'metadata flags' field in a class is actually a pointer to
    /// the metaclass object for the class.
    ///
    /// NONAPPLE: This is only really required for ObjC interop; maybe
    /// suppress this for classes that don't need to be exposed to
    /// ObjC, e.g. for non-Apple platforms?
    void addMetadataFlags() {
      static_assert(unsigned(MetadataKind::Class) == 0,
                    "class metadata kind is non-zero?");

      // Get the metaclass pointer as an intptr_t.
      auto metaclass = IGM.getAddrOfMetaclassObject(TargetClass,
                                                    NotForDefinition);
      auto flags = llvm::ConstantExpr::getPtrToInt(metaclass, IGM.IntPtrTy);
      Fields.push_back(flags);
    }

    /// The runtime provides a value witness table for Builtin.ObjectPointer.
    void addValueWitnessTable() {
      ClassDecl *cls = TargetClass;
      
      auto type = cls->isObjC()
        ? CanType(this->IGM.Context.TheObjCPointerType)
        : CanType(this->IGM.Context.TheObjectPointerType);
      auto wtable = this->IGM.getAddrOfValueWitnessTable(type);
      Fields.push_back(wtable);
    }

    void addDestructorFunction() {
      auto expansion = ResilienceExpansion::Minimal;
      auto dtorRef = SILDeclRef(TargetClass->getDestructor(),
                                SILDeclRef::Kind::Deallocator);
      Fields.push_back(IGM.getAddrOfSILFunction(dtorRef, expansion,
                                                NotForDefinition));
    }
    
    void addNominalTypeDescriptor() {
      Fields.push_back(
                   ClassNominalTypeDescriptorBuilder(IGM, TargetClass).emit());
    }

    void addParentMetadataRef(ClassDecl *forClass) {
      // FIXME: this is wrong for multiple levels of generics; we need
      // to apply substitutions through.
      Type parentType =
        forClass->getDeclContext()->getDeclaredTypeInContext();
      addReferenceToType(parentType->getCanonicalType());
    }

    void addSuperClass() {
      // If this is a root class, use SwiftObject as our formal parent.
      if (!TargetClass->hasSuperclass()) {
        // This is only required for ObjC interoperation.
        if (!IGM.ObjCInterop) {
          Fields.push_back(llvm::ConstantPointerNull::get(IGM.TypeMetadataPtrTy));
          return;
        }

        // We have to do getAddrOfObjCClass ourselves here because
        // getSwiftRootClass needs to be ObjC-mangled but isn't
        // actually imported from a clang module.
        Fields.push_back(IGM.getAddrOfObjCClass(IGM.getSwiftRootClass(),
                                                NotForDefinition));
        return;
      }

      addReferenceToType(TargetClass->getSuperclass()->getCanonicalType());
    }
    
    void addReferenceToType(CanType type) {
      if (llvm::Constant *metadata
            = tryEmitConstantHeapMetadataRef(IGM, type)) {
        Fields.push_back(metadata);
      } else {
        // Leave a null pointer placeholder to be filled at runtime
        Fields.push_back(llvm::ConstantPointerNull::get(IGM.TypeMetadataPtrTy));
      }
    }

    void addInstanceSize() {
      if (llvm::Constant *size
            = tryEmitClassConstantFragileInstanceSize(IGM, TargetClass)) {
        Fields.push_back(size);
      } else {
        // Leave a zero placeholder to be filled at runtime
        Fields.push_back(llvm::ConstantInt::get(IGM.SizeTy, 0));
      }
    }
    
    void addInstanceAlignMask() {
      if (llvm::Constant *align
            = tryEmitClassConstantFragileInstanceAlignMask(IGM, TargetClass)) {
        Fields.push_back(align);
      } else {
        // Leave a zero placeholder to be filled at runtime
        Fields.push_back(llvm::ConstantInt::get(IGM.SizeTy, 0));
      }
    }
    
    void addClassCacheData() {
      // We initially fill in these fields with addresses taken from
      // the ObjC runtime.
      Fields.push_back(IGM.getObjCEmptyCachePtr());
      Fields.push_back(IGM.getObjCEmptyVTablePtr());
    }

    void addClassDataPointer() {
      // Derive the RO-data.
      llvm::Constant *data = emitClassPrivateData(IGM, TargetClass);

      // We always set the low bit to indicate this is a Swift class.
      data = llvm::ConstantExpr::getPtrToInt(data, IGM.IntPtrTy);
      data = llvm::ConstantExpr::getAdd(data,
                                    llvm::ConstantInt::get(IGM.IntPtrTy, 1));

      Fields.push_back(data);
    }

    void addFieldOffset(VarDecl *var) {
      // Use a fixed offset if we have one.
      if (auto offset = tryEmitClassConstantFragileFieldOffset(IGM, TargetClass,
                                                               var))
        Fields.push_back(offset);
      // Otherwise, leave a placeholder for the runtime to populate at runtime.
      else
        Fields.push_back(llvm::ConstantInt::get(IGM.IntPtrTy, 0));
    }

    void addMethod(FunctionRef fn) {
      // If this function is associated with the target class, go
      // ahead and emit the witness offset variable.
      if (fn.getDecl()->getDeclContext() == TargetClass) {
        Address offsetVar = IGM.getAddrOfWitnessTableOffset(fn, ForDefinition);
        auto global = cast<llvm::GlobalVariable>(offsetVar.getAddress());

        auto offset = Fields.size() * IGM.getPointerSize();
        auto offsetV = llvm::ConstantInt::get(IGM.SizeTy, offset.getValue());
        global->setInitializer(offsetV);
      }

      // Find the final overrider, which we should already have computed.
      auto it = FinalOverriders.find(fn.getDecl());
      assert(it != FinalOverriders.end());
      FuncDecl *finalOverrider = it->second;

      fn = FunctionRef(finalOverrider, fn.getExplosionLevel(),
                       fn.getUncurryLevel());

      // Add the appropriate method to the module.
      Fields.push_back(IGM.getAddrOfFunction(fn, ExtraData::None,
                                             NotForDefinition));
    }

    void addGenericArgument(ArchetypeType *archetype, ClassDecl *forClass) {
      Fields.push_back(llvm::Constant::getNullValue(IGM.TypeMetadataPtrTy));
    }

    void addGenericWitnessTable(ArchetypeType *archetype,
                                ProtocolDecl *protocol, ClassDecl *forClass) {
      Fields.push_back(llvm::Constant::getNullValue(IGM.WitnessTablePtrTy));
    }

    llvm::Constant *getInit() {
      return llvm::ConstantStruct::getAnon(Fields);
    }
  };

  class ClassMetadataBuilder :
    public ClassMetadataBuilderBase<ClassMetadataBuilder> {
  public:
    ClassMetadataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                         const StructLayout &layout)
      : ClassMetadataBuilderBase(IGM, theClass, layout) {}

    llvm::Constant *getInit() {
      if (Fields.size() == NumHeapMetadataFields) {
        return llvm::ConstantStruct::get(IGM.FullHeapMetadataStructTy, Fields);
      } else {
        return llvm::ConstantStruct::getAnon(Fields);
      }
    }
  };
  
  Address emitAddressOfSuperclassRefInClassMetadata(IRGenFunction &IGF,
                                                    ClassDecl *theClass,
                                                    llvm::Value *metadata) {
    struct GetOffsetToSuperclassRef
      : ClassMetadataScanner<GetOffsetToSuperclassRef>
    {
    public:
      GetOffsetToSuperclassRef(IRGenModule &IGM, ClassDecl *target)
        : ClassMetadataScanner(IGM, target) {}
      
      unsigned Result = ~0U;
      
      void noteAddressPoint() {
        assert(Result == ~0U && "found superclass before address point?!");
        NextIndex = 0;
      }
      
      void addSuperClass() {
        Result = NextIndex++;
      }
    };
    
    GetOffsetToSuperclassRef scanner(IGF.IGM, theClass);
    scanner.layout();
    assert(scanner.Result != ~0U && "did not find superclass?!");
    
    Address addr(metadata, IGF.IGM.getPointerAlignment());
    addr = IGF.Builder.CreateBitCast(addr,
                                     IGF.IGM.TypeMetadataPtrTy->getPointerTo());
    return IGF.Builder.CreateConstArrayGEP(addr, scanner.Result,
                                           IGF.IGM.getPointerSize());
  }
  
  Address emitAddressOfFieldOffsetVectorInClassMetadata(IRGenFunction &IGF,
                                                        ClassDecl *theClass,
                                                        llvm::Value *metadata) {
    struct GetOffsetToFieldOffsetVector
      : ClassMetadataScanner<GetOffsetToFieldOffsetVector>
    {
    public:
      GetOffsetToFieldOffsetVector(IRGenModule &IGM, ClassDecl *target)
        : ClassMetadataScanner(IGM, target) {}
      
      unsigned Result = ~0U;
      
      void noteAddressPoint() {
        assert(Result == ~0U && "found field offsets before address point?!");
        NextIndex = 0;
      }
      
      void noteStartOfFieldOffsets(ClassDecl *whichClass) {
        if (whichClass == TargetClass)
          Result = NextIndex;
      }
    };

    GetOffsetToFieldOffsetVector scanner(IGF.IGM, theClass);
    scanner.layout();
    assert(scanner.Result != ~0U && "did not find field offset vector?!");
    
    Address addr(metadata, IGF.IGM.getPointerAlignment());
    addr = IGF.Builder.CreateBitCast(addr,
                                     IGF.IGM.SizeTy->getPointerTo());
    return IGF.Builder.CreateConstArrayGEP(addr, scanner.Result,
                                           IGF.IGM.getPointerSize());
  }

  /// A builder for metadata templates.
  class GenericClassMetadataBuilder :
    public GenericMetadataBuilderBase<GenericClassMetadataBuilder,
                      ClassMetadataBuilderBase<GenericClassMetadataBuilder>>
  {
    typedef GenericMetadataBuilderBase super;

    bool HasDependentSuperclass = false;
    bool HasDependentFieldOffsetVector = false;
    
    std::vector<std::tuple<ClassDecl*, int, int>>
      AncestorFieldOffsetVectors;
    
    std::vector<int> AncestorFillOps;
  public:
    GenericClassMetadataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                                const StructLayout &layout,
                                const GenericParamList &classGenerics)
      : super(IGM, classGenerics, theClass, layout)
    {
      // We need special initialization of metadata objects to trick the ObjC
      // runtime into initializing them.
      HasDependentMetadata = true;
      
      // If the superclass is generic, we'll need to initialize the superclass
      // reference at runtime.
      if (theClass->hasSuperclass() &&
          theClass->getSuperclass()->is<BoundGenericClassType>()) {
        HasDependentSuperclass = true;
      }
    }
                        
    void addDependentValueWitnessTablePattern() {
      llvm_unreachable("classes should never have dependent vwtables");
    }
                        
    void noteStartOfFieldOffsets(ClassDecl *whichClass) {
      HasDependentMetadata = true;

      if (whichClass == TargetClass) {
        // If the metadata contains a field offset vector for the class itself,
        // then we need to initialize it at runtime.
        HasDependentFieldOffsetVector = true;
        return;
      }
      
      // If we have a field offset vector for an ancestor class, we will copy
      // it from our superclass metadata at instantiation time.
      AncestorFieldOffsetVectors.emplace_back(whichClass, getNextIndex(), ~0U);
    }
    
    void noteEndOfFieldOffsets(ClassDecl *whichClass) {
      if (whichClass == TargetClass)
        return;
      
      // Mark the end of the ancestor field offset vector.
      assert(!AncestorFieldOffsetVectors.empty()
             && "no start of ancestor field offsets?!");
      assert(std::get<0>(AncestorFieldOffsetVectors.back()) == whichClass
             && "mismatched start of ancestor field offsets?!");
      std::get<2>(AncestorFieldOffsetVectors.back()) = getNextIndex();
    }
    
    // Suppress GenericMetadataBuilderBase's default behavior of introducing
    // fill ops for generic arguments unless they belong directly to the target
    // class and not its ancestors.

    void addGenericArgument(ArchetypeType *type, ClassDecl *forClass) {
      if (forClass == TargetClass) {
        // Introduce the fill op.
        GenericMetadataBuilderBase::addGenericArgument(type, forClass);
      } else {
        // Lay out the field, but don't provide the fill op, which we'll get
        // from the superclass.
        HasDependentMetadata = true;
        AncestorFillOps.push_back(getNextIndex());
        ClassMetadataBuilderBase::addGenericArgument(type, forClass);
      }
    }
    
    void addGenericWitnessTable(ArchetypeType *type, ProtocolDecl *protocol,
                                ClassDecl *forClass) {
      if (forClass == TargetClass) {
        // Introduce the fill op.
        GenericMetadataBuilderBase::addGenericWitnessTable(type, protocol,
                                                           forClass);
      } else {
        // Lay out the field, but don't provide the fill op, which we'll get
        // from the superclass.

        HasDependentMetadata = true;
        AncestorFillOps.push_back(getNextIndex());
        ClassMetadataBuilderBase::addGenericWitnessTable(type, protocol,
                                                         forClass);
      }
    }

    void emitInitializeMetadata(IRGenFunction &IGF,
                                llvm::Value *metadata,
                                llvm::Value *vwtable) {
      emitPolymorphicParametersForGenericValueWitness(IGF,
                                                      TargetClass,
                                                      metadata);
      
      assert(!HasDependentVWT && "class should never have dependent VWT");
      
      // Get the superclass metadata.
      llvm::Value *superMetadata;
      if (TargetClass->hasSuperclass()) {
        superMetadata = IGF.emitTypeMetadataRef(
                            TargetClass->getSuperclass()->getCanonicalType());
      } else {
        assert(!HasDependentSuperclass
               && "dependent superclass without superclass?!");
        superMetadata
          = llvm::ConstantPointerNull::get(IGF.IGM.TypeMetadataPtrTy);
      }
      
      // If the superclass is generic, populate the superclass field.
      if (HasDependentSuperclass) {
        Address superField
          = emitAddressOfSuperclassRefInClassMetadata(IGF,TargetClass,metadata);
        IGF.Builder.CreateStore(superMetadata, superField);
      }
      
      // If we have any ancestor generic parameters or field offset vectors,
      // copy them from the superclass metadata.
      if (!AncestorFieldOffsetVectors.empty() || !AncestorFillOps.empty()) {
        Address superBase(superMetadata, IGF.IGM.getPointerAlignment());
        Address selfBase(metadata, IGF.IGM.getPointerAlignment());
        superBase = IGF.Builder.CreateBitCast(superBase,
                                              IGF.IGM.SizeTy->getPointerTo());
        selfBase = IGF.Builder.CreateBitCast(selfBase,
                                             IGF.IGM.SizeTy->getPointerTo());
        
        for (int ancestorOp : AncestorFillOps) {
          ancestorOp -= (int)AddressPoint;
          Address superOp = IGF.Builder.CreateConstArrayGEP(superBase,
                                         ancestorOp, IGF.IGM.getPointerSize());
          Address selfOp = IGF.Builder.CreateConstArrayGEP(selfBase,
                                         ancestorOp, IGF.IGM.getPointerSize());
          IGF.Builder.CreateStore(IGF.Builder.CreateLoad(superOp), selfOp);
        }
        
        for (auto &ancestorFields : AncestorFieldOffsetVectors) {
          ClassDecl *ancestor;
          unsigned startIndex, endIndex;
          std::tie(ancestor, startIndex, endIndex) = ancestorFields;
          if (startIndex == endIndex)
            continue;
          assert(startIndex <= endIndex);
          unsigned size = endIndex - startIndex;
          startIndex -= (int)AddressPoint;
          
          Address superVec = IGF.Builder.CreateConstArrayGEP(superBase,
                                         startIndex, IGF.IGM.getPointerSize());
          Address selfVec = IGF.Builder.CreateConstArrayGEP(selfBase,
                                         startIndex, IGF.IGM.getPointerSize());
          
          IGF.Builder.CreateMemCpy(selfVec.getAddress(),
                                   superVec.getAddress(),
                                   IGF.IGM.getPointerSize().getValue() * size,
                                   IGF.IGM.getPointerAlignment().getValue());
        }
      }
      
      // If the field layout is dependent, ask the runtime to populate the
      // offset vector.
      if (HasDependentFieldOffsetVector) {
        llvm::Value *fieldVector
          = emitAddressOfFieldOffsetVectorInClassMetadata(IGF,
                                                          TargetClass, metadata)
              .getAddress();
        
        // Collect the stored properties of the type.
        llvm::SmallVector<VarDecl*, 4> storedProperties;
        for (auto prop : TargetClass->getStoredProperties()) {
          storedProperties.push_back(prop);
        }
        // Fill out an array with the field type metadata records.
        Address fields = IGF.createAlloca(
                         llvm::ArrayType::get(IGF.IGM.TypeMetadataPtrTy,
                                              storedProperties.size()),
                         IGF.IGM.getPointerAlignment(), "classFields");
        fields = IGF.Builder.CreateBitCast(fields,
                                     IGF.IGM.TypeMetadataPtrTy->getPointerTo());
        unsigned index = 0;
        for (auto prop : storedProperties) {
          llvm::Value *metadata = IGF.emitTypeMetadataRef(
                                           prop->getType()->getCanonicalType());
          Address field = IGF.Builder.CreateConstArrayGEP(fields, index,
                                                      IGF.IGM.getPointerSize());
          IGF.Builder.CreateStore(metadata, field);
          ++index;
        }
        
        // Ask the runtime to lay out the class.
        auto numFields = llvm::ConstantInt::get(IGF.IGM.SizeTy,
                                                storedProperties.size());
        IGF.Builder.CreateCall5(IGF.IGM.getInitClassMetadataUniversalFn(),
                                metadata, superMetadata, numFields,
                                fields.getAddress(), fieldVector);
      }
      
      // FIXME: Crudely invoke an ObjC class method on the class to force the
      // ObjC runtime to do minimal initialization of the class.
      // We should really register the class pair with the runtime through the
      // approved channels.
      llvm::Value *msgSend = IGF.IGM.getObjCMsgSendFn();
      llvm::Type *classFArgs[] = {
        IGF.IGM.ObjCPtrTy,
        IGF.IGM.ObjCSELTy,
      };
      auto classFTy = llvm::FunctionType::get(IGF.IGM.ObjCClassPtrTy,
                                              classFArgs, /*isVarArg*/ false);
      msgSend = IGF.Builder.CreateBitCast(msgSend, classFTy->getPointerTo());
      auto classPtr = IGF.Builder.CreateBitCast(metadata, IGF.IGM.ObjCPtrTy);
      llvm::Value *classSel = IGF.emitObjCSelectorRefLoad("class");
      IGF.Builder.CreateCall2(msgSend, classPtr, classSel);
    }
    
  };
}

/// Emit the type metadata or metadata template for a class.
void irgen::emitClassMetadata(IRGenModule &IGM, ClassDecl *classDecl,
                              const StructLayout &layout) {
  // TODO: classes nested within generic types
  llvm::Constant *init;
  bool isPattern;
  if (auto *generics = classDecl->getGenericParamsOfContext()) {
    GenericClassMetadataBuilder builder(IGM, classDecl, layout, *generics);
    builder.layout();
    init = builder.getInit();
    isPattern = true;
  } else {
    ClassMetadataBuilder builder(IGM, classDecl, layout);
    builder.layout();
    init = builder.getInit();
    isPattern = false;
  }

  // For now, all type metadata is directly stored.
  bool isIndirect = false;

  CanType declaredType = classDecl->getDeclaredType()->getCanonicalType();
  auto var = cast<llvm::GlobalVariable>(
                     IGM.getAddrOfTypeMetadata(declaredType,
                                               isIndirect, isPattern,
                                               init->getType()));
  var->setInitializer(init);

  // TODO: the metadata global can actually be constant in a very
  // special case: it's not a pattern, ObjC interoperation isn't
  // required, there are no class fields, and there is nothing that
  // needs to be runtime-adjusted.
  var->setConstant(false);

  // Add non-generic classes to the ObjC class list.
  if (IGM.ObjCInterop && !isPattern && !isIndirect) {
    // We can't just use 'var' here because it's unadjusted.  Instead
    // of re-implementing the adjustment logic, just pull the metadata
    // pointer again.
    auto metadata =
      IGM.getAddrOfTypeMetadata(declaredType, isIndirect, isPattern);
    IGM.addObjCClass(metadata);
  }
}

namespace {
  /// A visitor for checking whether two types are compatible.
  ///
  /// It's guaranteed that 'override' is subtype-related to a
  /// substitution of 'overridden'; this is because dependent
  /// overrides are not allowed by the language.
  class IsIncompatibleOverride :
      public CanTypeVisitor<IsIncompatibleOverride, bool, CanType> {

    IRGenModule &IGM;
    ResilienceExpansion ExplosionLevel;
    bool AsExplosion;

  public:
    IsIncompatibleOverride(IRGenModule &IGM, ResilienceExpansion explosionLevel,
                           bool asExplosion)
      : IGM(IGM), ExplosionLevel(explosionLevel), AsExplosion(asExplosion) {}

    bool visit(CanType overridden, CanType override) {
      if (override == overridden) return false;

      return CanTypeVisitor::visit(overridden, override);
    }

    /// Differences in class types must be subtyping related.
    bool visitClassType(CanClassType overridden, CanType override) {
      assert(override->getClassOrBoundGenericClass());
      return false;
    }

    /// Differences in bound generic class types must be subtyping related.
    bool visitBoundGenericType(CanBoundGenericType overridden, CanType override) {
      if (isa<ClassDecl>(overridden->getDecl())) {
        assert(override->getClassOrBoundGenericClass());
        return false;
      }
      return visitType(overridden, override);
    }

    bool visitTupleType(CanTupleType overridden, CanType overrideTy) {
      CanTupleType override = cast<TupleType>(overrideTy);
      assert(overridden->getNumElements() == override->getNumElements());
      for (unsigned i = 0, e = overridden->getNumElements(); i != e; ++i) {
        if (visit(overridden.getElementType(i), override.getElementType(i)))
          return true;
      }
      return false;
    }

    /// Any other difference (unless we add implicit
    /// covariance/contravariance to generic types?) must be a
    /// substitution difference.
    bool visitType(CanType overridden, CanType override) {
      if (AsExplosion)
        return differsByAbstractionInExplosion(IGM, overridden,
                                               override, ExplosionLevel);
      return differsByAbstractionInMemory(IGM, overridden, override);
    }
  };
}

static bool isIncompatibleOverrideArgument(IRGenModule &IGM,
                                           CanType overrideTy,
                                           CanType overriddenTy,
                                           ResilienceExpansion explosionLevel) {
  return IsIncompatibleOverride(IGM, explosionLevel, /*as explosion*/ true)
    .visit(overriddenTy, overrideTy);  
}

static bool isIncompatibleOverrideResult(IRGenModule &IGM,
                                         CanType overrideTy,
                                         CanType overriddenTy,
                                         ResilienceExpansion explosionLevel) {
  // Fast path.
  if (overrideTy == overriddenTy) return false;

  bool asExplosion;

  auto requiresIndirectResult = [&](CanType type) {
    return IGM.requiresIndirectResult(SILType::getPrimitiveObjectType(type),
                                      explosionLevel);
  };

  // If the overridden type isn't returned indirectly, the overriding
  // type won't be, either, and we need to check as an explosion.
  if (!requiresIndirectResult(overriddenTy)) {
    assert(!requiresIndirectResult(overrideTy));
    asExplosion = true;

  // Otherwise, if the overriding type isn't returned indirectly,
  // there's an abstration mismatch and the types are incompatible.
  } else if (!requiresIndirectResult(overrideTy)) {
    return true;

  // Otherwise, both are returning indirectly and we need to check as
  // memory.
  } else {
    asExplosion = false;
  }

  return IsIncompatibleOverride(IGM, explosionLevel, asExplosion)
    .visit(overriddenTy, overrideTy);
}

/// Is the given method called in the same way that the overridden
/// method is?
static bool isCompatibleOverride(IRGenModule &IGM, FuncDecl *override,
                                 FuncDecl *overridden,
                                 ResilienceExpansion explosionLevel,
                                 unsigned uncurryLevel) {
  CanType overrideTy = override->getType()->getCanonicalType();
  CanType overriddenTy = overridden->getType()->getCanonicalType();

  // Check arguments for compatibility.
  for (++uncurryLevel; uncurryLevel; --uncurryLevel) {
    // Fast path.
    if (overrideTy == overriddenTy) return true;

    // Note that we're intentionally ignoring any differences in
    // polymorphism --- at the first level that's because that should
    // all be encapsulated in the self argument, and at the later
    // levels because that shouldn't be a legal override.
    auto overrideFnTy = cast<AnyFunctionType>(overrideTy);
    auto overriddenFnTy = cast<AnyFunctionType>(overriddenTy);

    if (isIncompatibleOverrideArgument(IGM,
                                       CanType(overrideFnTy->getInput()),
                                       CanType(overriddenFnTy->getInput()),
                                       explosionLevel))
      return false;

    overrideTy = CanType(overrideFnTy->getResult());
    overriddenTy = CanType(overriddenFnTy->getResult());
  }

  return isIncompatibleOverrideResult(IGM, overrideTy, overriddenTy,
                                      explosionLevel);
}

/// Does the given method require an override entry in the class v-table?
bool irgen::doesMethodRequireOverrideEntry(IRGenModule &IGM, FuncDecl *fn,
                                           ResilienceExpansion explosionLevel,
                                           unsigned uncurryLevel) {
  // Check each of the overridden declarations in turn.
  FuncDecl *overridden = fn->getOverriddenDecl();
  do {
    assert(overridden);
    
    // ObjC methods never get vtable entries, so overrides always need a new
    // entry.
    if (!hasKnownVTableEntry(IGM, overridden))
      return true;

    // If we ever find something we compatibly override, we're done.
    if (isCompatibleOverride(IGM, fn, overridden,
                             explosionLevel, uncurryLevel))
      return false;

  } while ((overridden = overridden->getOverriddenDecl()));

  // Otherwise, we need a new entry.
  return true;
}

/// Emit a load from the given metadata at a constant index.
static llvm::Value *emitLoadFromMetadataAtIndex(IRGenFunction &IGF,
                                                llvm::Value *metadata,
                                                int index,
                                                llvm::PointerType *objectTy) {
  // Require the metadata to be some type that we recognize as a
  // metadata pointer.
  assert(metadata->getType() == IGF.IGM.TypeMetadataPtrTy);

  // We require objectType to be a pointer type so that the GEP will
  // scale by the right amount.  We could load an arbitrary type using
  // some extra bitcasting.

  // Cast to T*.
  auto objectPtrTy = objectTy->getPointerTo();
  metadata = IGF.Builder.CreateBitCast(metadata, objectPtrTy);

  auto indexV = llvm::ConstantInt::getSigned(IGF.IGM.SizeTy, index);

  // GEP to the slot.
  Address slot(IGF.Builder.CreateInBoundsGEP(metadata, indexV),
               IGF.IGM.getPointerAlignment());

  // Load.
  auto result = IGF.Builder.CreateLoad(slot);
  return result;
}

/// Given a type metadata pointer, load its value witness table.
llvm::Value *
IRGenFunction::emitValueWitnessTableRefForMetadata(llvm::Value *metadata) {
  return emitLoadFromMetadataAtIndex(*this, metadata, -1,
                                     IGM.WitnessTablePtrTy);
}

/// Load the metadata reference at the given index.
static llvm::Value *emitLoadOfMetadataRefAtIndex(IRGenFunction &IGF,
                                                 llvm::Value *metadata,
                                                 int index) {
  return emitLoadFromMetadataAtIndex(IGF, metadata, index,
                                     IGF.IGM.TypeMetadataPtrTy);
}

/// Load the protocol witness table reference at the given index.
static llvm::Value *emitLoadOfWitnessTableRefAtIndex(IRGenFunction &IGF,
                                                     llvm::Value *metadata,
                                                     int index) {
  return emitLoadFromMetadataAtIndex(IGF, metadata, index,
                                     IGF.IGM.WitnessTablePtrTy);
}

namespace {
  /// A CRTP helper for classes which are simply searching for a
  /// specific index within the metadata.
  ///
  /// The pattern is that subclasses should override an 'add' method
  /// from the appropriate layout class and ensure that they call
  /// setTargetIndex() when the appropriate location is reached.  The
  /// subclass user then just calls getTargetIndex(), which performs
  /// the layout and returns the found index.
  ///
  /// \tparam Base the base class, which should generally be a CRTP
  ///   class template applied to the most-derived class
  template <class Base> class MetadataSearcher : public Base {
    static const unsigned InvalidIndex = ~0U;
    unsigned TargetIndex = InvalidIndex;
    unsigned AddressPoint = InvalidIndex;

  protected:
    void setTargetIndex() {
      assert(TargetIndex == InvalidIndex && "setting twice");
      TargetIndex = this->NextIndex;
    }

  public:
    template <class... T> MetadataSearcher(T &&...args)
      : Base(std::forward<T>(args)...) {}

    void noteAddressPoint() { AddressPoint = this->NextIndex; }

    int getTargetIndex() {
      assert(TargetIndex == InvalidIndex && "computing twice");
      this->layout();
      assert(TargetIndex != InvalidIndex && "target not found!");
      assert(AddressPoint != InvalidIndex && "address point not set");
      return (int) TargetIndex - (int) AddressPoint;
    }
  };

  /// A class for finding the 'parent' index in a class metadata object.
  class FindClassParentIndex :
      public MetadataSearcher<ClassMetadataScanner<FindClassParentIndex>> {
    typedef MetadataSearcher super;
  public:
    FindClassParentIndex(IRGenModule &IGM, ClassDecl *theClass)
      : super(IGM, theClass) {}

    void addParentMetadataRef(ClassDecl *forClass) {
      if (forClass == TargetClass) setTargetIndex();
      NextIndex++;
    }
  };
}

/// Given a reference to some metadata, derive a reference to the
/// type's parent type.
llvm::Value *irgen::emitParentMetadataRef(IRGenFunction &IGF,
                                          NominalTypeDecl *decl,
                                          llvm::Value *metadata) {
  assert(decl->getDeclContext()->isTypeContext());

  switch (decl->getKind()) {
#define NOMINAL_TYPE_DECL(id, parent)
#define DECL(id, parent) \
  case DeclKind::id:
#include "swift/AST/DeclNodes.def"
    llvm_unreachable("not a nominal type");

  case DeclKind::Protocol:
    llvm_unreachable("protocols never have parent types!");

  case DeclKind::Class: {
    int index =
      FindClassParentIndex(IGF.IGM, cast<ClassDecl>(decl)).getTargetIndex();
    return emitLoadOfMetadataRefAtIndex(IGF, metadata, index);
  }

  case DeclKind::Enum:
  case DeclKind::Struct:
    // In both of these cases, 'Parent' is always the third field.
    return emitLoadOfMetadataRefAtIndex(IGF, metadata, 2);
  }
  llvm_unreachable("bad decl kind!");
}

namespace {
  /// A class for finding a type argument in a class metadata object.
  class FindClassArgumentIndex :
      public MetadataSearcher<ClassMetadataScanner<FindClassArgumentIndex>> {
    typedef MetadataSearcher super;

    ArchetypeType *TargetArchetype;

  public:
    FindClassArgumentIndex(IRGenModule &IGM, ClassDecl *theClass,
                           ArchetypeType *targetArchetype)
      : super(IGM, theClass), TargetArchetype(targetArchetype) {}

    void addGenericArgument(ArchetypeType *argument, ClassDecl *forClass) {
      if (forClass == TargetClass && argument == TargetArchetype)
        setTargetIndex();
      NextIndex++;
    }
  };

  /// A class for finding a type argument in a value type metadata object.
  template<template <typename> class METADATA_SCANNER>
  class FindValueTypeArgumentIndex :
      public MetadataSearcher<METADATA_SCANNER<
               FindValueTypeArgumentIndex<METADATA_SCANNER>>>
  {
    using super = MetadataSearcher<METADATA_SCANNER<
              FindValueTypeArgumentIndex<METADATA_SCANNER>>>;
    
    using super::setTargetIndex;
    using super::NextIndex;
    using super::Target;

    ArchetypeType *TargetArchetype;

  public:
    FindValueTypeArgumentIndex(IRGenModule &IGM, decltype(Target) decl,
                               ArchetypeType *targetArchetype)
      : super(IGM, decl), TargetArchetype(targetArchetype) {}

    void addGenericArgument(ArchetypeType *argument) {
      if (argument == TargetArchetype)
        setTargetIndex();
      NextIndex++;
    }
  };
  
  using FindStructArgumentIndex
    = FindValueTypeArgumentIndex<StructMetadataScanner>;
  using FindEnumArgumentIndex
    = FindValueTypeArgumentIndex<EnumMetadataScanner>;
}

/// Given a reference to nominal type metadata of the given type,
/// derive a reference to the nth argument metadata.  The type must
/// have generic arguments.
llvm::Value *irgen::emitArgumentMetadataRef(IRGenFunction &IGF,
                                            NominalTypeDecl *decl,
                                            unsigned argumentIndex,
                                            llvm::Value *metadata) {
  assert(decl->getGenericParams() != nullptr);
  auto targetArchetype =
    decl->getGenericParams()->getAllArchetypes()[argumentIndex];

  switch (decl->getKind()) {
#define NOMINAL_TYPE_DECL(id, parent)
#define DECL(id, parent) \
  case DeclKind::id:
#include "swift/AST/DeclNodes.def"
    llvm_unreachable("not a nominal type");

  case DeclKind::Protocol:
    llvm_unreachable("protocols are never generic!");

  case DeclKind::Class: {
    int index =
      FindClassArgumentIndex(IGF.IGM, cast<ClassDecl>(decl), targetArchetype)
        .getTargetIndex();
    return emitLoadOfMetadataRefAtIndex(IGF, metadata, index);
  }

  case DeclKind::Struct: {
    int index =
      FindStructArgumentIndex(IGF.IGM, cast<StructDecl>(decl), targetArchetype)
        .getTargetIndex();
    return emitLoadOfMetadataRefAtIndex(IGF, metadata, index);
  }

  case DeclKind::Enum: {
    int index =
      FindEnumArgumentIndex(IGF.IGM, cast<EnumDecl>(decl), targetArchetype)
        .getTargetIndex();
    return emitLoadOfMetadataRefAtIndex(IGF, metadata, index);
  }
  }
  llvm_unreachable("bad decl kind!");
}

namespace {
  /// A class for finding a protocol witness table for a type argument
  /// in a class metadata object.
  class FindClassWitnessTableIndex :
      public MetadataSearcher<ClassMetadataScanner<FindClassWitnessTableIndex>> {
    typedef MetadataSearcher super;

    ArchetypeType *TargetArchetype;
    ProtocolDecl *TargetProtocol;

  public:
    FindClassWitnessTableIndex(IRGenModule &IGM, ClassDecl *theClass,
                               ArchetypeType *targetArchetype,
                               ProtocolDecl *targetProtocol)
      : super(IGM, theClass), TargetArchetype(targetArchetype),
        TargetProtocol(targetProtocol) {}

    void addGenericWitnessTable(ArchetypeType *argument,
                                ProtocolDecl *protocol,
                                ClassDecl *forClass) {
      if (forClass == TargetClass &&
          argument == TargetArchetype &&
          protocol == TargetProtocol)
        setTargetIndex();
      NextIndex++;
    }
  };

  /// A class for finding a protocol witness table for a type argument
  /// in a value type metadata object.
  template<template <typename> class METADATA_SCANNER>
  class FindValueTypeWitnessTableIndex :
      public MetadataSearcher<METADATA_SCANNER<
               FindValueTypeWitnessTableIndex<METADATA_SCANNER>>>
  {
    using super
      = MetadataSearcher<METADATA_SCANNER<
          FindValueTypeWitnessTableIndex<METADATA_SCANNER>>>;
    
    using super::setTargetIndex;
    using super::NextIndex;
    using super::Target;

    ArchetypeType *TargetArchetype;
    ProtocolDecl *TargetProtocol;

  public:
    FindValueTypeWitnessTableIndex(IRGenModule &IGM, decltype(Target) decl,
                                   ArchetypeType *targetArchetype,
                                   ProtocolDecl *targetProtocol)
      : super(IGM, decl),
        TargetArchetype(targetArchetype),
        TargetProtocol(targetProtocol)
    {}

    void addGenericWitnessTable(ArchetypeType *argument,
                                ProtocolDecl *protocol) {
      if (argument == TargetArchetype && protocol == TargetProtocol)
        setTargetIndex();
      NextIndex++;
    }
  };
  
  using FindStructWitnessTableIndex
    = FindValueTypeWitnessTableIndex<StructMetadataScanner>;
  using FindEnumWitnessTableIndex
    = FindValueTypeWitnessTableIndex<EnumMetadataScanner>;
}

/// Given a reference to nominal type metadata of the given type,
/// derive a reference to a protocol witness table for the nth
/// argument metadata.  The type must have generic arguments.
llvm::Value *irgen::emitArgumentWitnessTableRef(IRGenFunction &IGF,
                                                NominalTypeDecl *decl,
                                                unsigned argumentIndex,
                                                ProtocolDecl *targetProtocol,
                                                llvm::Value *metadata) {
  assert(decl->getGenericParams() != nullptr);
  auto targetArchetype =
    decl->getGenericParams()->getAllArchetypes()[argumentIndex];

  switch (decl->getKind()) {
#define NOMINAL_TYPE_DECL(id, parent)
#define DECL(id, parent) \
  case DeclKind::id:
#include "swift/AST/DeclNodes.def"
    llvm_unreachable("not a nominal type");

  case DeclKind::Protocol:
    llvm_unreachable("protocols are never generic!");

  case DeclKind::Class: {
    int index =
      FindClassWitnessTableIndex(IGF.IGM, cast<ClassDecl>(decl),
                                 targetArchetype, targetProtocol)
        .getTargetIndex();
    return emitLoadOfWitnessTableRefAtIndex(IGF, metadata, index);
  }

  case DeclKind::Enum: {
    int index =
      FindEnumWitnessTableIndex(IGF.IGM, cast<EnumDecl>(decl),
                                 targetArchetype, targetProtocol)
        .getTargetIndex();
    return emitLoadOfWitnessTableRefAtIndex(IGF, metadata, index);
  }
      
  case DeclKind::Struct: {
    int index =
      FindStructWitnessTableIndex(IGF.IGM, cast<StructDecl>(decl),
                                  targetArchetype, targetProtocol)
        .getTargetIndex();
    return emitLoadOfWitnessTableRefAtIndex(IGF, metadata, index);
  }
  }
  llvm_unreachable("bad decl kind!");
}

/// Given a reference to class metadata of the given type,
/// derive a reference to the field offset for a stored property.
/// The type must have dependent generic layout.
llvm::Value *irgen::emitClassFieldOffset(IRGenFunction &IGF,
                                         ClassDecl *theClass,
                                         VarDecl *field,
                                         llvm::Value *metadata) {
  /// A class for finding a field offset in a class metadata object.
  class FindClassFieldOffset :
      public MetadataSearcher<ClassMetadataScanner<FindClassFieldOffset>> {
    typedef MetadataSearcher super;

    VarDecl *TargetField;
  public:
    FindClassFieldOffset(IRGenModule &IGM, ClassDecl *theClass,
                         VarDecl *targetField)
      : super(IGM, theClass), TargetField(targetField) {}

    void addFieldOffset(VarDecl *field) {
      if (field == TargetField)
        setTargetIndex();
      NextIndex++;
    }
  };

  int index = FindClassFieldOffset(IGF.IGM, theClass, field).getTargetIndex();
  llvm::Value *val = emitLoadOfWitnessTableRefAtIndex(IGF, metadata, index);
  return IGF.Builder.CreatePtrToInt(val, IGF.IGM.SizeTy);
}

/// Given a reference to class metadata of the given type,
/// load the fragile instance size and alignment of the class.
std::pair<llvm::Value *, llvm::Value *>
irgen::emitClassFragileInstanceSizeAndAlignMask(IRGenFunction &IGF,
                                                ClassDecl *theClass,
                                                llvm::Value *metadata) {
  class FindClassSize :
    public ClassMetadataScanner<FindClassSize> {
  public:
    FindClassSize(IRGenModule &IGM, ClassDecl *theClass)
      : ClassMetadataScanner(IGM, theClass) {}

    unsigned InstanceSize = ~0U, InstanceAlignMask = ~0U;
        
    void noteAddressPoint() {
      assert(InstanceSize == ~0U && InstanceAlignMask == ~0U
             && "found size or alignment before address point?!");
      NextIndex = 0;
    }
        
    void addInstanceSize() {
      InstanceSize = NextIndex++;
    }
      
    void addInstanceAlignMask() {
      InstanceAlignMask = NextIndex++;
    }
  };
  
  // If the class has fragile fixed layout, return the constant size and
  // alignment.
  if (llvm::Constant *size
        = tryEmitClassConstantFragileInstanceSize(IGF.IGM, theClass)) {
    llvm::Constant *alignMask
      = tryEmitClassConstantFragileInstanceAlignMask(IGF.IGM, theClass);
    assert(alignMask && "static size without static align");
    return {size, alignMask};
  }
  
  // Otherwise, load from the metadata.
  FindClassSize scanner(IGF.IGM, theClass);
  scanner.layout();
  assert(scanner.InstanceSize != ~0U
         && scanner.InstanceAlignMask != ~0U
         && "didn't find size or alignment in metadata?!");
  llvm::Value *size = emitLoadOfWitnessTableRefAtIndex(IGF, metadata,
                                                       scanner.InstanceSize);
  size = IGF.Builder.CreatePtrToInt(size, IGF.IGM.SizeTy);
  llvm::Value *alignMask = emitLoadOfWitnessTableRefAtIndex(IGF, metadata,
                                                     scanner.InstanceAlignMask);
  alignMask = IGF.Builder.CreatePtrToInt(alignMask, IGF.IGM.SizeTy);
  
  return {size, alignMask};
}

/// Given a pointer to a heap object (i.e. definitely not a tagged
/// pointer), load its heap metadata pointer.
static llvm::Value *emitLoadOfHeapMetadataRef(IRGenFunction &IGF,
                                              llvm::Value *object,
                                              bool suppressCast) {
  // Drill into the object pointer.  Rather than bitcasting, we make
  // an effort to do something that should explode if we get something
  // mistyped.
  llvm::StructType *structTy =
    cast<llvm::StructType>(
      cast<llvm::PointerType>(object->getType())->getElementType());

  llvm::Value *slot;

  // We need a bitcast if we're dealing with an opaque class.
  if (structTy->isOpaque()) {
    auto metadataPtrPtrTy = IGF.IGM.TypeMetadataPtrTy->getPointerTo();
    slot = IGF.Builder.CreateBitCast(object, metadataPtrPtrTy);

  // Otherwise, make a GEP.
  } else {
    auto zero = llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0);

    SmallVector<llvm::Value*, 4> indexes;
    indexes.push_back(zero);
    do {
      indexes.push_back(zero);

      // Keep drilling down to the first element type.
      auto eltTy = structTy->getElementType(0);
      assert(isa<llvm::StructType>(eltTy) || eltTy == IGF.IGM.TypeMetadataPtrTy);
      structTy = dyn_cast<llvm::StructType>(eltTy);
    } while (structTy != nullptr);

    slot = IGF.Builder.CreateInBoundsGEP(object, indexes);

    if (!suppressCast) {
      slot = IGF.Builder.CreateBitCast(slot,
                                  IGF.IGM.TypeMetadataPtrTy->getPointerTo());
    }
  }

  auto metadata = IGF.Builder.CreateLoad(Address(slot,
                                             IGF.IGM.getPointerAlignment()));
  metadata->setName(llvm::Twine(object->getName()) + ".metadata");
  return metadata;
}

static bool isKnownNotTaggedPointer(IRGenModule &IGM, ClassDecl *theClass) {
  // For now, assume any class type defined in Clang might be tagged.
  return hasKnownSwiftMetadata(IGM, theClass);
}

/// Given an object of class type, produce the heap metadata reference
/// as a %type*.
llvm::Value *irgen::emitHeapMetadataRefForHeapObject(IRGenFunction &IGF,
                                                     llvm::Value *object,
                                                     CanType objectType,
                                                     bool suppressCast) {
  ClassDecl *theClass = objectType->getClassOrBoundGenericClass();
  if (isKnownNotTaggedPointer(IGF.IGM, theClass))
    return emitLoadOfHeapMetadataRef(IGF, object, suppressCast);

  // OK, ask the runtime for the class pointer of this
  // potentially-ObjC object.
  object = IGF.Builder.CreateBitCast(object, IGF.IGM.ObjCPtrTy);
  auto metadata = IGF.Builder.CreateCall(IGF.IGM.getGetObjectClassFn(),
                                         object,
                                         object->getName() + ".class");
  metadata->setCallingConv(IGF.IGM.RuntimeCC);
  metadata->setDoesNotThrow();
  metadata->setDoesNotAccessMemory();
  return metadata;
}

llvm::Value *irgen::emitHeapMetadataRefForHeapObject(IRGenFunction &IGF,
                                                     llvm::Value *object,
                                                     SILType objectType,
                                                     bool suppressCast) {
  return emitHeapMetadataRefForHeapObject(IGF, object,
                                          objectType.getSwiftRValueType(),
                                          suppressCast);
}

/// Given an opaque class instance pointer, produce the type metadata reference
/// as a %type*.
llvm::Value *irgen::emitTypeMetadataRefForOpaqueHeapObject(IRGenFunction &IGF,
                                                           llvm::Value *object)
{
  object = IGF.Builder.CreateBitCast(object, IGF.IGM.ObjCPtrTy);
  auto metadata = IGF.Builder.CreateCall(IGF.IGM.getGetObjectTypeFn(),
                                         object,
                                         object->getName() + ".metatype");
  metadata->setCallingConv(IGF.IGM.RuntimeCC);
  metadata->setDoesNotThrow();
  metadata->setDoesNotAccessMemory();
  return metadata;
}

/// Given an object of class type, produce the type metadata reference
/// as a %type*.
llvm::Value *irgen::emitTypeMetadataRefForHeapObject(IRGenFunction &IGF,
                                                     llvm::Value *object,
                                                     SILType objectType,
                                                     bool suppressCast) {
  // If it is known to have swift metadata, just load.
  ClassDecl *theClass = objectType.getClassOrBoundGenericClass();
  if (hasKnownSwiftMetadata(IGF.IGM, theClass)) {
    assert(isKnownNotTaggedPointer(IGF.IGM, theClass));
    return emitLoadOfHeapMetadataRef(IGF, object, suppressCast);
  }

  // Okay, ask the runtime for the type metadata of this
  // potentially-ObjC object.
  return emitTypeMetadataRefForOpaqueHeapObject(IGF, object);
}

/// Given a class metatype, produce the necessary heap metadata
/// reference.  This is generally the metatype pointer, but may
/// instead be a reference type.
llvm::Value *irgen::emitClassHeapMetadataRefForMetatype(IRGenFunction &IGF,
                                                        llvm::Value *metatype,
                                                        CanType type) {
  // If the type is known to have Swift metadata, this is trivial.
  if (hasKnownSwiftMetadata(IGF.IGM, type->getClassOrBoundGenericClass()))
    return metatype;

  // Otherwise, we inline a little operation here.

  // Load the metatype kind.
  auto metatypeKindAddr =
    Address(IGF.Builder.CreateStructGEP(metatype, 0),
            IGF.IGM.getPointerAlignment());
  auto metatypeKind =
    IGF.Builder.CreateLoad(metatypeKindAddr, metatype->getName() + ".kind");

  // Compare it with the class wrapper kind.
  auto classWrapperKind =
    llvm::ConstantInt::get(IGF.IGM.MetadataKindTy,
                           unsigned(MetadataKind::ObjCClassWrapper));
  auto isObjCClassWrapper =
    IGF.Builder.CreateICmpEQ(metatypeKind, classWrapperKind,
                             "isObjCClassWrapper");

  // Branch based on that.
  llvm::BasicBlock *contBB = IGF.createBasicBlock("metadataForClass.cont");
  llvm::BasicBlock *wrapBB = IGF.createBasicBlock("isWrapper");
  IGF.Builder.CreateCondBr(isObjCClassWrapper, wrapBB, contBB);
  llvm::BasicBlock *origBB = IGF.Builder.GetInsertBlock();

  // If it's a wrapper, load from the 'Class' field, which is at index 1.
  // TODO: if we guaranteed that this load couldn't crash, we could use
  // a select here instead, which might be profitable.
  IGF.Builder.emitBlock(wrapBB);
  auto classFromWrapper = 
    emitLoadFromMetadataAtIndex(IGF, metatype, 1, IGF.IGM.TypeMetadataPtrTy);
  IGF.Builder.CreateBr(contBB);

  // Continuation block.
  IGF.Builder.emitBlock(contBB);
  auto phi = IGF.Builder.CreatePHI(IGF.IGM.TypeMetadataPtrTy, 2,
                                   metatype->getName() + ".class");
  phi->addIncoming(metatype, origBB);
  phi->addIncoming(classFromWrapper, wrapBB);

  return phi;
}

namespace {
  /// A class for finding a protocol witness table for a type argument
  /// in a class metadata object.
  class FindClassMethodIndex :
      public MetadataSearcher<ClassMetadataScanner<FindClassMethodIndex>> {
    typedef MetadataSearcher super;

    FunctionRef TargetMethod;

  public:
    FindClassMethodIndex(IRGenModule &IGM, FunctionRef target)
      : super(IGM, cast<ClassDecl>(target.getDecl()->getDeclContext())),
        TargetMethod(target) {}

    void addMethod(FunctionRef fn) {
      if (TargetMethod == fn)
        setTargetIndex();
      NextIndex++;
    }
  };
}

/// Provide the abstract parameters for virtual calls to the given method.
AbstractCallee irgen::getAbstractVirtualCallee(IRGenFunction &IGF,
                                               FuncDecl *method) {
  // TODO: maybe use better versions in the v-table sometimes?
  ResilienceExpansion bestExplosion = ResilienceExpansion::Minimal;
  unsigned naturalUncurry = method->getNaturalArgumentCount() - 1;

  return AbstractCallee(AbstractCC::Method, bestExplosion,
                        naturalUncurry, naturalUncurry, ExtraData::None);
}

/// Find the function which will actually appear in the virtual table.
static FuncDecl *findOverriddenFunction(IRGenModule &IGM,
                                        FuncDecl *method,
                                        ResilienceExpansion explosionLevel,
                                        unsigned uncurryLevel) {
  // 'method' is the most final method in the hierarchy which we
  // haven't yet found a compatible override for.  'cur' is the method
  // we're currently looking at.  Compatibility is transitive,
  // so we can forget our original method and just keep going up.

  FuncDecl *cur = method;
  while ((cur = cur->getOverriddenDecl())) {
    if (!hasKnownVTableEntry(IGM, cur))
      break;
    if (isCompatibleOverride(IGM, method, cur, explosionLevel,
                             uncurryLevel))
      method = cur;
  }
  return method;
}

/// Load the correct virtual function for the given class method.
llvm::Value *irgen::emitVirtualMethodValue(IRGenFunction &IGF,
                                           llvm::Value *base,
                                           SILType baseType,
                                           SILDeclRef method,
                                           CanSILFunctionType methodType,
                                           ResilienceExpansion maxExplosion) {
  // TODO: maybe use better versions in the v-table sometimes?
  ResilienceExpansion bestExplosion = ResilienceExpansion::Minimal;

  // FIXME: Support property accessors.
  FuncDecl *methodDecl = cast<FuncDecl>(method.getDecl());

  // Find the function that's actually got an entry in the metadata.
  FuncDecl *overridden =
    findOverriddenFunction(IGF.IGM, methodDecl,
                           bestExplosion, method.uncurryLevel);

  // Find the metadata.
  llvm::Value *metadata;
  if (methodDecl->isStatic()) {
    metadata = base;
  } else {
    metadata = emitHeapMetadataRefForHeapObject(IGF, base, baseType,
                                                /*suppress cast*/ true);
  }

  // Use the type of the method we were type-checked against, not the
  // type of the overridden method.
  llvm::AttributeSet attrs;
  auto fnTy = IGF.IGM.getFunctionType(methodType, bestExplosion,
                                      ExtraData::None, attrs)->getPointerTo();

  FunctionRef fnRef(overridden, bestExplosion, method.uncurryLevel);
  auto index = FindClassMethodIndex(IGF.IGM, fnRef).getTargetIndex();

  return emitLoadFromMetadataAtIndex(IGF, metadata, index, fnTy);
}

// Structs

namespace {
  /// An adapter for laying out struct metadata.
  template <class Impl>
  class StructMetadataBuilderBase : public StructMetadataLayout<Impl> {
    typedef StructMetadataLayout<Impl> super;

  protected:
    using super::IGM;
    using super::Target;

    SmallVector<llvm::Constant *, 8> Fields;

    StructMetadataBuilderBase(IRGenModule &IGM, StructDecl *theStruct)
      : super(IGM, theStruct) {}

    unsigned getNextIndex() const { return Fields.size(); }

  public:
    void addMetadataFlags() {
      Fields.push_back(getMetadataKind(IGM, MetadataKind::Struct));
    }

    void addNominalTypeDescriptor() {
      // FIXME!
      Fields.push_back(StructNominalTypeDescriptorBuilder(IGM, Target).emit());
    }

    void addParentMetadataRef() {
      // FIXME!
      Fields.push_back(llvm::ConstantPointerNull::get(IGM.TypeMetadataPtrTy));
    }
    
    void addFieldOffset(VarDecl *var) {
      assert(!var->isComputed()
             && "storing field offset for computed property?!");
      SILType structType =
        SILType::getPrimitiveAddressType(
                       Target->getDeclaredTypeInContext()->getCanonicalType());

      llvm::Constant *offset =
        emitPhysicalStructMemberFixedOffset(IGM, structType, var);
      // If we have a fixed offset, add it. Otherwise, leave zero as a
      // placeholder.
      if (offset)
        Fields.push_back(offset);
      else
        Fields.push_back(llvm::ConstantInt::get(IGM.SizeTy, 0));
    }

    void addGenericArgument(ArchetypeType *type) {
      Fields.push_back(llvm::Constant::getNullValue(IGM.TypeMetadataPtrTy));
    }

    void addGenericWitnessTable(ArchetypeType *type, ProtocolDecl *protocol) {
      Fields.push_back(llvm::Constant::getNullValue(IGM.WitnessTablePtrTy));
    }

    llvm::Constant *getInit() {
      if (Fields.size() == NumHeapMetadataFields) {
        return llvm::ConstantStruct::get(this->IGM.FullHeapMetadataStructTy,
                                         Fields);
      } else {
        return llvm::ConstantStruct::getAnon(Fields);
      }
    }
  };

  class StructMetadataBuilder :
    public StructMetadataBuilderBase<StructMetadataBuilder> {
  public:
    StructMetadataBuilder(IRGenModule &IGM, StructDecl *theStruct)
      : StructMetadataBuilderBase(IGM, theStruct) {}

    void addValueWitnessTable() {
      auto type = this->Target->getDeclaredType()->getCanonicalType();
      Fields.push_back(emitValueWitnessTable(IGM, type));
    }

    llvm::Constant *getInit() {
      return llvm::ConstantStruct::getAnon(Fields);
    }
  };
  
  /// Emit a value witness table for a fixed-layout generic type, or a null
  /// placeholder if the value witness table is dependent on generic parameters.
  /// Returns true if the value witness table is dependent.
  static bool addValueWitnessTableSlotForGenericValueType(
                                     IRGenModule &IGM, NominalTypeDecl *decl,
                                     SmallVectorImpl<llvm::Constant*> &Fields) {
    CanType unboundType
      = decl->getDeclaredTypeOfContext()->getCanonicalType();
    
    bool dependent = hasDependentValueWitnessTable(IGM, unboundType);
    
    if (dependent)
      Fields.push_back(llvm::ConstantPointerNull::get(IGM.Int8PtrTy));
    else
      Fields.push_back(emitValueWitnessTable(IGM, unboundType));
    
    return dependent;
  }
  
  /// A builder for metadata templates.
  class GenericStructMetadataBuilder :
    public GenericMetadataBuilderBase<GenericStructMetadataBuilder,
                      StructMetadataBuilderBase<GenericStructMetadataBuilder>> {

    typedef GenericMetadataBuilderBase super;
                        
  public:
    GenericStructMetadataBuilder(IRGenModule &IGM, StructDecl *theStruct,
                                const GenericParamList &structGenerics)
      : super(IGM, structGenerics, theStruct) {}

    void addValueWitnessTable() {
      HasDependentVWT
        = addValueWitnessTableSlotForGenericValueType(IGM, Target, Fields);
    }
                        
    void addDependentValueWitnessTablePattern() {
      emitDependentValueWitnessTablePattern(IGM,
                Target->getDeclaredTypeOfContext()->getCanonicalType(), Fields);
    }
                        
    void emitInitializeMetadata(IRGenFunction &IGF,
                                llvm::Value *metadata,
                                llvm::Value *vwtable) {
      emitPolymorphicParametersForGenericValueWitness(IGF, Target, metadata);
      IGM.getTypeInfoForLowered(CanType(Target->getDeclaredTypeInContext()))
        .initializeMetadata(IGF, metadata, vwtable,
                            Target->getDeclaredTypeInContext()
                              ->getCanonicalType());
    }
  };
}

/// Emit the type metadata or metadata template for a struct.
void irgen::emitStructMetadata(IRGenModule &IGM, StructDecl *structDecl) {
  // TODO: structs nested within generic types
  llvm::Constant *init;
  bool isPattern;
  if (auto *generics = structDecl->getGenericParamsOfContext()) {
    GenericStructMetadataBuilder builder(IGM, structDecl, *generics);
    builder.layout();
    init = builder.getInit();
    isPattern = true;
  } else {
    StructMetadataBuilder builder(IGM, structDecl);
    builder.layout();
    init = builder.getInit();
    isPattern = false;
  }

  // For now, all type metadata is directly stored.
  bool isIndirect = false;

  CanType declaredType = structDecl->getDeclaredType()->getCanonicalType();
  auto var = cast<llvm::GlobalVariable>(
                     IGM.getAddrOfTypeMetadata(declaredType,
                                               isIndirect, isPattern,
                                               init->getType()));
  var->setConstant(!isPattern);
  var->setInitializer(init);
}

// Enums

namespace {

template<class Impl>
class EnumMetadataBuilderBase : public EnumMetadataLayout<Impl> {
  using super = EnumMetadataLayout<Impl>;

protected:
  using super::IGM;
  using super::Target;
  SmallVector<llvm::Constant *, 8> Fields;

  unsigned getNextIndex() const { return Fields.size(); }

public:
  EnumMetadataBuilderBase(IRGenModule &IGM, EnumDecl *theEnum)
    : super(IGM, theEnum) {}
  
  void addMetadataFlags() {
    Fields.push_back(getMetadataKind(IGM, MetadataKind::Enum));
  }
  
  void addNominalTypeDescriptor() {
    // FIXME!
    Fields.push_back(EnumNominalTypeDescriptorBuilder(IGM, Target).emit());
  }
  
  void addParentMetadataRef() {
    // FIXME!
    Fields.push_back(llvm::ConstantPointerNull::get(IGM.TypeMetadataPtrTy));
  }
  
  void addGenericArgument(ArchetypeType *type) {
    Fields.push_back(llvm::Constant::getNullValue(IGM.TypeMetadataPtrTy));
  }
  
  void addGenericWitnessTable(ArchetypeType *type, ProtocolDecl *protocol) {
    Fields.push_back(llvm::Constant::getNullValue(IGM.WitnessTablePtrTy));
  }
  
  llvm::Constant *getInit() {
    return llvm::ConstantStruct::getAnon(Fields);
  }
};
  
class EnumMetadataBuilder
  : public EnumMetadataBuilderBase<EnumMetadataBuilder>
{
public:
  EnumMetadataBuilder(IRGenModule &IGM, EnumDecl *theEnum)
    : EnumMetadataBuilderBase(IGM, theEnum) {}
  
  void addValueWitnessTable() {
    auto type = Target->getDeclaredType()->getCanonicalType();
    Fields.push_back(emitValueWitnessTable(IGM, type));
  }
  
  llvm::Constant *getInit() {
    return llvm::ConstantStruct::getAnon(Fields);
  }
};
  
class GenericEnumMetadataBuilder
  : public GenericMetadataBuilderBase<GenericEnumMetadataBuilder,
                        EnumMetadataBuilderBase<GenericEnumMetadataBuilder>>
{
public:
  GenericEnumMetadataBuilder(IRGenModule &IGM, EnumDecl *theEnum,
                              const GenericParamList &enumGenerics)
    : GenericMetadataBuilderBase(IGM, enumGenerics, theEnum) {}
  
  void addValueWitnessTable() {
    HasDependentVWT
      = addValueWitnessTableSlotForGenericValueType(IGM, Target, Fields);
  }
  
  void addDependentValueWitnessTablePattern() {
    emitDependentValueWitnessTablePattern(IGM,
                Target->getDeclaredTypeOfContext()->getCanonicalType(), Fields);
  }
  
  void emitInitializeMetadata(IRGenFunction &IGF,
                              llvm::Value *metadata,
                              llvm::Value *vwtable) {
    emitPolymorphicParametersForGenericValueWitness(IGF, Target, metadata);
    IGM.getTypeInfoForLowered(CanType(Target->getDeclaredTypeInContext()))
      .initializeMetadata(IGF, metadata, vwtable,
                          Target->getDeclaredTypeInContext()
                            ->getCanonicalType());
  }
};
  
}

void irgen::emitEnumMetadata(IRGenModule &IGM, EnumDecl *theEnum) {
  // TODO: enums nested inside generic types
  llvm::Constant *init;
  
  bool isPattern;
  if (auto *generics = theEnum->getGenericParamsOfContext()) {
    GenericEnumMetadataBuilder builder(IGM, theEnum, *generics);
    builder.layout();
    init = builder.getInit();
    isPattern = true;
  } else {
    EnumMetadataBuilder builder(IGM, theEnum);
    builder.layout();
    init = builder.getInit();
    isPattern = false;
  }
  
  // For now, all type metadata is directly stored.
  bool isIndirect = false;
  
  CanType declaredType = theEnum->getDeclaredType()->getCanonicalType();
  auto var = cast<llvm::GlobalVariable>(
                              IGM.getAddrOfTypeMetadata(declaredType,
                                                        isIndirect, isPattern,
                                                        init->getType()));
  var->setConstant(!isPattern);
  var->setInitializer(init);
}

llvm::Value *IRGenFunction::emitObjCSelectorRefLoad(StringRef selector) {
  llvm::Constant *loadSelRef = IGM.getAddrOfObjCSelectorRef(selector);
  llvm::Value *loadSel =
    Builder.CreateLoad(Address(loadSelRef, IGM.getPointerAlignment()));

  // When generating JIT'd code, we need to call sel_registerName() to force
  // the runtime to unique the selector. For non-JIT'd code, the linker will
  // do it for us.
  if (IGM.Opts.UseJIT) {
    loadSel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), loadSel);
  }

  return loadSel;
}

// Protocols

namespace {
  class ProtocolDescriptorBuilder {
    IRGenModule &IGM;
    ProtocolDecl *Protocol;

    SmallVector<llvm::Constant*, 8> Fields;

  public:
    ProtocolDescriptorBuilder(IRGenModule &IGM, ProtocolDecl *protocol)
      : IGM(IGM), Protocol(protocol) {}

    void layout() {
      addObjCCompatibilityIsa();
      addName();
      addInherited();
      addObjCCompatibilityTables();
      addSize();
      addFlags();
    }

    llvm::Constant *null() {
      return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    }
    
    void addObjCCompatibilityIsa() {
      // The ObjC runtime will drop a reference to its magic Protocol class
      // here.
      Fields.push_back(null());
    }
    
    void addName() {
      auto name = LinkEntity::forTypeMangling(
                              Protocol->getDeclaredType()->getCanonicalType());
      llvm::SmallString<32> mangling;
      name.mangle(mangling);
      Fields.push_back(IGM.getAddrOfGlobalString(mangling));
    }
    
    void addInherited() {
      // If there are no inherited protocols, produce null.
      auto inherited = Protocol->getProtocols();
      if (inherited.empty()) {
        Fields.push_back(null());
        return;
      }
      
      // Otherwise, collect references to all of the inherited protocol
      // descriptors.
      SmallVector<llvm::Constant*, 4> inheritedDescriptors;
      inheritedDescriptors.push_back(IGM.getSize(Size(inherited.size())));
      
      for (ProtocolDecl *p : inherited) {
        auto descriptor = IGM.getAddrOfProtocolDescriptor(p, NotForDefinition);
        inheritedDescriptors.push_back(descriptor);
      }
      
      auto inheritedInit = llvm::ConstantStruct::getAnon(inheritedDescriptors);
      auto inheritedVar = new llvm::GlobalVariable(IGM.Module,
                                           inheritedInit->getType(),
                                           /*isConstant*/ true,
                                           llvm::GlobalValue::InternalLinkage,
                                           inheritedInit);
      
      llvm::Constant *inheritedVarPtr
        = llvm::ConstantExpr::getBitCast(inheritedVar, IGM.Int8PtrTy);
      Fields.push_back(inheritedVarPtr);
    }
    
    void addObjCCompatibilityTables() {
      // Required instance methods
      Fields.push_back(null());
      // Required class methods
      Fields.push_back(null());
      // Optional instance methods
      Fields.push_back(null());
      // Optional class methods
      Fields.push_back(null());
      // Properties
      Fields.push_back(null());
    }
    
    void addSize() {
      // The number of fields so far in words, plus 4 bytes for size and
      // 4 bytes for flags.
      unsigned sz = (Fields.size() * IGM.getPointerSize()).getValue() + 4 + 4;
      Fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty, sz));
    }
    
    void addFlags() {
      // enum : uint32_t {
      //   IsSwift           = 1U <<  0U,
      unsigned flags = 1;
      
      //   ClassConstraint   = 1U <<  1U,
      // Set if the protocol is *not* class constrained.
      if (!Protocol->requiresClass())
        flags |= (1U << 1U);
      
      //   NeedsWitnessTable = 1U <<  2U,
      if (requiresProtocolWitnessTable(Protocol))
        flags |= (1U << 2U);

      // };
      
      Fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty, flags));
    }

    void addValueWitnessTable() {
      // Build a fresh value witness table.  FIXME: this is actually
      // unnecessary --- every existential type will have the exact
      // same value witness table.
      CanType type = CanType(Protocol->getDeclaredType());
      Fields.push_back(emitValueWitnessTable(IGM, type));
    }

    llvm::Constant *getInit() {
      return llvm::ConstantStruct::get(IGM.ProtocolDescriptorStructTy,
                                       Fields);
    }
  };
} // end anonymous namespace

/// Emit global structures associated with the given protocol. This comprises
/// the protocol descriptor, and for ObjC interop, references to the descriptor
/// that the ObjC runtime uses for uniquing.
void IRGenModule::emitProtocolDecl(ProtocolDecl *protocol) {
  // If the protocol is Objective-C-compatible, go through the path that
  // produces an ObjC-compatible protocol_t.
  if (protocol->isObjC()) {
    getObjCProtocolGlobalVars(protocol);
    return;
  }
  
  ProtocolDescriptorBuilder builder(*this, protocol);
  builder.layout();
  auto init = builder.getInit();

  auto var = cast<llvm::GlobalVariable>(
                       getAddrOfProtocolDescriptor(protocol, ForDefinition));
  var->setConstant(true);
  var->setInitializer(init);
}

/// \brief Load a reference to the protocol descriptor for the given protocol.
///
/// For Swift protocols, this is a constant reference to the protocol descriptor
/// symbol.
/// For ObjC protocols, descriptors are uniqued at runtime by the ObjC runtime.
/// We need to load the unique reference from a global variable fixed up at
/// startup.
llvm::Value *irgen::emitProtocolDescriptorRef(IRGenFunction &IGF,
                                              ProtocolDecl *protocol) {
  if (!protocol->isObjC())
    return IGF.IGM.getAddrOfProtocolDescriptor(protocol, NotForDefinition);
  
  auto refVar = IGF.IGM.getAddrOfObjCProtocolRef(protocol, NotForDefinition);
  llvm::Value *val
    = IGF.Builder.CreateLoad(refVar, IGF.IGM.getPointerAlignment());
  val = IGF.Builder.CreateBitCast(val,
                          IGF.IGM.ProtocolDescriptorStructTy->getPointerTo());
  return val;
}
