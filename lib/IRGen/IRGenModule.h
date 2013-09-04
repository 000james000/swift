//===--- IRGenModule.h - Swift Global IR Generation Module ------*- C++ -*-===//
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
// This file defines the interface used 
// the AST into LLVM IR.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_IRGENMODULE_H
#define SWIFT_IRGEN_IRGENMODULE_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/ValueHandle.h"
#include "llvm/IR/CallingConv.h"
#include "IRGen.h"
#include "SwiftTargetInfo.h"
#include "ValueWitness.h"

namespace llvm {
  class Constant;
  class DataLayout;
  class Function;
  class FunctionType;
  class GlobalVariable;
  class IntegerType;
  class LLVMContext;
  class Module;
  class PointerType;
  class StructType;
  class StringRef;
  class Type;
  class AttributeSet;
}

namespace swift {
  class ASTContext;
  class BraceStmt;
  class CanType;
  class CapturingExpr;
  class ClassDecl;
  class ConstructorDecl;
  class Decl;
  class DestructorDecl;
  class ExtensionDecl;
  class FuncDecl;
  class SILFunction;
  class UnionElementDecl;
  class UnionDecl;
  class ProtocolCompositionType;
  class ProtocolDecl;
  struct SILDeclRef;
  class SILModule;
  class SILType;
  class SourceLoc;
  class StructDecl;
  class TranslationUnit;
  class Type;
  class TypeAliasDecl;
  class TypeDecl;
  class ValueDecl;
  class VarDecl;

  enum class AbstractCC : unsigned char;
  
namespace Mangle {
  enum class ExplosionKind : unsigned;
}

namespace irgen {
  class Address;
  class CodeRef;
  class ExplosionSchema;
  class FormalType;
  class FunctionRef;
  class IRGenDebugInfo;
  class LinkEntity;
  class Options;
  class ProtocolInfo;
  class TypeConverter;
  class TypeInfo;
  enum class ValueWitness : unsigned;

/// IRGenModule - Primary class for emitting IR for global declarations.
/// 
class IRGenModule {
public:
  ASTContext &Context;
  Options &Opts;
  llvm::Module &Module;
  llvm::LLVMContext &LLVMContext;
  const llvm::DataLayout &DataLayout;
  SILModule *SILMod;
  /// Order dependency -- TargetInfo must be initialized after Opts.
  SwiftTargetInfo TargetInfo;
  /// Holds lexical scope info, etc. Is a nullptr if we compile without -g.
  IRGenDebugInfo *DebugInfo;

  /// Does the current target require Objective-C interoperation?
  static const bool ObjCInterop = true;

  llvm::Type *VoidTy;                  /// void (usually {})
  llvm::IntegerType *Int1Ty;           /// i1
  llvm::IntegerType *Int8Ty;           /// i8
  llvm::IntegerType *Int16Ty;          /// i16
  llvm::IntegerType *Int32Ty;          /// i32
  llvm::IntegerType *Int64Ty;          /// i64
  union {
    llvm::IntegerType *SizeTy;         /// usually i32 or i64
    llvm::IntegerType *IntPtrTy;
    llvm::IntegerType *MetadataKindTy;
  };
  union {
    llvm::PointerType *Int8PtrTy;      /// i8*
    llvm::PointerType *WitnessTableTy;
    llvm::PointerType *ObjCSELTy;
  };
  union {
    llvm::PointerType *Int8PtrPtrTy;   /// i8**
    llvm::PointerType *WitnessTablePtrTy;
  };
  llvm::StructType *RefCountedStructTy;/// %swift.refcounted = type { ... }
  llvm::PointerType *RefCountedPtrTy;  /// %swift.refcounted*
  llvm::PointerType *WeakReferencePtrTy;/// %swift.weak_reference*
  llvm::Constant *RefCountedNull;      /// %swift.refcounted* null
  llvm::StructType *FunctionPairTy;    /// { i8*, %swift.refcounted* }
  llvm::FunctionType *DeallocatingDtorTy; /// void (%swift.refcounted*)
  llvm::StructType *TypeMetadataStructTy; /// %swift.type = type { ... }
  llvm::PointerType *TypeMetadataPtrTy;/// %swift.type*
  llvm::PointerType *TupleTypeMetadataPtrTy; /// %swift.tuple_type*
  llvm::StructType *FullHeapMetadataStructTy; /// %swift.full_heapmetadata = type { ... }
  llvm::PointerType *FullHeapMetadataPtrTy;/// %swift.full_heapmetadata*
  llvm::StructType *TypeMetadataPatternStructTy;/// %swift.type_pattern = type { ... }
  llvm::PointerType *TypeMetadataPatternPtrTy;/// %swift.type_pattern*
  llvm::StructType *FullTypeMetadataStructTy; /// %swift.full_type = type { ... }
  llvm::PointerType *FullTypeMetadataPtrTy;/// %swift.full_type*
  union {
    llvm::PointerType *ObjCPtrTy;        /// %objc_object*
    llvm::PointerType *UnknownRefCountedPtrTy;
  };
  llvm::PointerType *OpaquePtrTy;      /// %swift.opaque*
  llvm::StructType *ObjCClassStructTy; /// %objc_class
  llvm::PointerType *ObjCClassPtrTy;   /// %objc_class*
  llvm::StructType *ObjCSuperStructTy; /// %objc_super
  llvm::PointerType *ObjCSuperPtrTy;   /// %objc_super*
  llvm::CallingConv::ID RuntimeCC;     /// lightweight calling convention

  Size getPointerSize() const { return PtrSize; }
  Alignment getPointerAlignment() const {
    // We always use the pointer's width as its swift ABI alignment.
    return Alignment(PtrSize.getValue());
  }

  Size getWeakReferenceSize() const { return PtrSize; }
  Alignment getWeakReferenceAlignment() const { return getPointerAlignment(); }

  llvm::Type *getFixedBufferTy();
  llvm::Type *getValueWitnessTy(ValueWitness index);

  void unimplemented(SourceLoc, StringRef Message);
  void error(SourceLoc loc, const Twine &message);

private:
  Size PtrSize;
  llvm::Type *FixedBufferTy;          /// [N x i8], where N == 3 * sizeof(void*)

  llvm::Type *ValueWitnessTys[MaxNumValueWitnesses];
  
  llvm::DenseMap<llvm::Type *, llvm::BitVector> SpareBitsForTypes;

//--- Types -----------------------------------------------------------------
public:
  const ProtocolInfo &getProtocolInfo(ProtocolDecl *D);
  const TypeInfo &getTypeInfo(CanType T);
  const TypeInfo &getTypeInfo(Type T);
  const TypeInfo &getTypeInfo(SILType T);
  const TypeInfo &getWitnessTablePtrTypeInfo();
  const TypeInfo &getTypeMetadataPtrTypeInfo();
  llvm::Type *getStorageType(CanType T);
  llvm::PointerType *getStoragePointerType(CanType T);
  llvm::StructType *createNominalType(TypeDecl *D);
  llvm::StructType *createNominalType(ProtocolCompositionType *T);
  void getSchema(CanType T, ExplosionSchema &schema);
  ExplosionSchema getSchema(CanType T, Mangle::ExplosionKind kind);
  unsigned getExplosionSize(CanType T, Mangle::ExplosionKind kind);
  llvm::PointerType *isSingleIndirectValue(CanType T, Mangle::ExplosionKind kind);
  llvm::PointerType *requiresIndirectResult(CanType T, Mangle::ExplosionKind kind);
  bool hasTrivialMetatype(CanType type);
  bool isPOD(CanType type, ResilienceScope scope);
  ObjectSize classifyTypeSize(CanType type, ResilienceScope scope);

  bool isResilient(Decl *decl, ResilienceScope scope);

  llvm::BitVector getSpareBitsForType(llvm::Type *scalarTy);
  
private:
  TypeConverter &Types;
  friend class TypeConverter;

//--- Globals ---------------------------------------------------------------
public:
  llvm::Constant *getAddrOfGlobalString(StringRef string);
  llvm::Constant *getAddrOfObjCSelectorRef(StringRef selector);
  llvm::Constant *getAddrOfObjCMethodName(StringRef methodName);
  void addUsedGlobal(llvm::GlobalValue *global);
  void addObjCClass(llvm::Constant *addr);

private:
  llvm::DenseMap<LinkEntity, llvm::GlobalVariable*> GlobalVars;
  llvm::DenseMap<LinkEntity, llvm::Function*> GlobalFuncs;
  llvm::StringMap<llvm::Constant*> GlobalStrings;
  llvm::StringMap<llvm::Constant*> ObjCSelectorRefs;
  llvm::StringMap<llvm::Constant*> ObjCMethodNames;

  /// LLVMUsed - List of global values which are required to be
  /// present in the object file; bitcast to i8*. This is used for
  /// forcing visibility of symbols which may otherwise be optimized
  /// out.
  SmallVector<llvm::WeakVH, 4> LLVMUsed;

  /// ObjCClasses - List of Objective-C classes, bitcast to i8*.
  SmallVector<llvm::WeakVH, 4> ObjCClasses;
  /// ObjCCategories - List of Objective-C categories, bitcast to i8*.
  SmallVector<llvm::WeakVH, 4> ObjCCategories;
  /// ObjCCategoryDecls - List of ExtensionDecls corresponding to the generated
  /// categories.
  SmallVector<ExtensionDecl*, 4> ObjCCategoryDecls;

  void mangleGlobalInitializer(raw_ostream &buffer, TranslationUnit *D);
  void emitGlobalLists();

//--- Runtime ---------------------------------------------------------------
public:
  llvm::Constant *getEmptyTupleMetadata();
  llvm::Constant *getObjCEmptyCachePtr();
  llvm::Constant *getObjCEmptyVTablePtr();
  ClassDecl *getSwiftRootClass();

private:
  llvm::Constant *EmptyTupleMetadata = nullptr;
  llvm::Constant *ObjCEmptyCachePtr = nullptr;
  llvm::Constant *ObjCEmptyVTablePtr = nullptr;
  ClassDecl *SwiftRootClass = nullptr;

#define FUNCTION_ID(Id)             \
public:                             \
  llvm::Constant *get##Id##Fn();    \
private:                            \
  llvm::Constant *Id##Fn = nullptr;
#include "RuntimeFunctions.def"

//--- Generic ---------------------------------------------------------------
public:
  IRGenModule(ASTContext &Context, Options &Opts, llvm::Module &Module,
              const llvm::DataLayout &DataLayout,
              SILModule *SILMod);
  ~IRGenModule();

  llvm::LLVMContext &getLLVMContext() const { return LLVMContext; }

  void emitTranslationUnit(TranslationUnit *TU, unsigned StartElem);

  void emitProtocolDecl(ProtocolDecl *D);
  void emitUnionDecl(UnionDecl *D);
  void emitStructDecl(StructDecl *D);
  void emitClassDecl(ClassDecl *D);
  void emitExtension(ExtensionDecl *D);
  Address emitGlobalVariable(VarDecl *var, const TypeInfo &type);
  
  void emitSILFunction(SILFunction *f);
  
  /// Generate local decls in the given function body. This skips VarDecls and
  /// other locals that are consumed by SIL.
  void emitLocalDecls(BraceStmt *body);
  void emitLocalDecls(FuncDecl *fd);
  void emitLocalDecls(ConstructorDecl *cd);
  void emitLocalDecls(DestructorDecl *dd);

  llvm::FunctionType *getFunctionType(AbstractCC cc,
                                      CanType fnType, Mangle::ExplosionKind kind,
                                      unsigned uncurryLevel,
                                      ExtraData data,
                                      llvm::AttributeSet &attrs);
                                      
  llvm::FunctionType *getFunctionType(SILType type, ExplosionKind explosionKind,
                                      ExtraData extraData,
                                      llvm::AttributeSet &attrs);

  llvm::Constant *getSize(Size size);

  FormalType getTypeOfGetter(ValueDecl *D);
  FormalType getTypeOfSetter(ValueDecl *D);

  Address getAddrOfGlobalVariable(VarDecl *D);
  Address getAddrOfFieldOffset(VarDecl *D, bool isIndirect);
  llvm::Function *getAddrOfFunction(FunctionRef ref, ExtraData data);
  llvm::Function *getAddrOfInjectionFunction(UnionElementDecl *D);
  llvm::Function *getAddrOfGetter(ValueDecl *D, FormalType type,
                                  Mangle::ExplosionKind kind);
  llvm::Function *getAddrOfGetter(ValueDecl *D, Mangle::ExplosionKind kind);
  llvm::Function *getAddrOfSetter(ValueDecl *D, FormalType type,
                                  Mangle::ExplosionKind kind);
  llvm::Function *getAddrOfSetter(ValueDecl *D, Mangle::ExplosionKind kind);
  Address getAddrOfWitnessTableOffset(CodeRef code);
  Address getAddrOfWitnessTableOffset(VarDecl *field);
  llvm::Function *getAddrOfValueWitness(CanType concreteType,
                                        ValueWitness index);
  llvm::Constant *getAddrOfValueWitnessTable(CanType concreteType,
                                             llvm::Type *definitionType = nullptr);
  llvm::Function *getAddrOfConstructor(ConstructorDecl *D,
                                       ConstructorKind kind,
                                       Mangle::ExplosionKind explosionLevel);
  llvm::Function *getAddrOfDestructor(ClassDecl *D, DestructorKind kind);
  llvm::Constant *getAddrOfTypeMetadata(CanType concreteType,
                                        bool isIndirect, bool isPattern,
                                        llvm::Type *definitionType = nullptr);
  llvm::Constant *getAddrOfObjCClass(ClassDecl *D);
  llvm::Constant *getAddrOfObjCMetaclass(ClassDecl *D);
  llvm::Constant *getAddrOfSwiftMetaclassStub(ClassDecl *D);
  llvm::Constant *getAddrOfMetaclassObject(ClassDecl *D);
  llvm::Function *getAddrOfSILFunction(SILFunction *f,
                                       ExplosionKind level);
  llvm::Function *getAddrOfBridgeToBlockConverter(SILType blockType);

  StringRef mangleType(CanType type, SmallVectorImpl<char> &buffer);

//--- Global context emission --------------------------------------------------
public:
  void emitGlobalTopLevel(TranslationUnit *TU, unsigned StartElem);
private:
  void emitGlobalDecl(Decl *D);
  void emitExternalDefinition(Decl *D);
};

} // end namespace irgen
} // end namespace swift

#endif
