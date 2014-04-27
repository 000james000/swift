//===--- GenClass.cpp - Swift IR Generation For 'class' Types -----------===//
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
//  This file implements IR generation for class types.
//
//===----------------------------------------------------------------------===//

#include "GenClass.h"

#include "swift/ABI/Class.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeMemberVisitor.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/CallSite.h"

#include "Explosion.h"
#include "GenFunc.h"
#include "GenMeta.h"
#include "GenObjC.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "GenHeap.h"
#include "HeapTypeInfo.h"


using namespace swift;
using namespace irgen;

static ClassDecl *getRootClass(ClassDecl *theClass) {
  while (theClass->hasSuperclass()) {
    theClass = theClass->getSuperclass()->getClassOrBoundGenericClass();
    assert(theClass && "base type of class not a class?");
  }
  return theClass;
}

/// What reference counting mechanism does a class have?
ReferenceCounting irgen::getReferenceCountingForClass(IRGenModule &IGM,
                                                      ClassDecl *theClass) {
  // If the root class is implemented in swift, then we have a swift
  // refcount; otherwise, we have an ObjC refcount.
  if (hasKnownSwiftImplementation(IGM, getRootClass(theClass)))
    return ReferenceCounting::Native;
  return ReferenceCounting::ObjC;
}

/// What isa encoding mechanism does a type have?
IsaEncoding irgen::getIsaEncodingForType(IRGenModule &IGM,
                                         CanType type) {
  if (auto theClass = type->getClassOrBoundGenericClass()) {
    // We can access the isas of pure Swift classes directly.
    if (hasKnownSwiftImplementation(IGM, getRootClass(theClass)))
      return IsaEncoding::Pointer;
    // For ObjC or mixed classes, we need to use object_getClass.
    return IsaEncoding::ObjC;
  }
  // Non-class heap objects should be pure Swift, so we can access their isas
  // directly.
  return IsaEncoding::Pointer;
}

/// Different policies for accessing a physical field.
enum class FieldAccess : uint8_t {
  /// Instance variable offsets are constant.
  ConstantDirect,

  /// Instance variable offsets must be loaded from "direct offset"
  /// global variables.
  NonConstantDirect,

  /// Instance variable offsets are kept in fields in metadata, but
  /// the offsets of those fields within the metadata are constant.
  ConstantIndirect,

  /// Instance variable offsets are kept in fields in metadata, and
  /// the offsets of those fields within the metadata must be loaded
  /// from "indirect offset" global variables.
  NonConstantIndirect
};

namespace {
  class FieldEntry {
    llvm::PointerIntPair<VarDecl*, 2, FieldAccess> VarAndAccess;
  public:
    FieldEntry(VarDecl *var, FieldAccess access)
      : VarAndAccess(var, access) {}

    VarDecl *getVar() const {
      return VarAndAccess.getPointer();
    }
    FieldAccess getAccess() const {
      return VarAndAccess.getInt();
    }
  };

  /// Layout information for class types.
  class ClassTypeInfo : public HeapTypeInfo<ClassTypeInfo> {
    ClassDecl *TheClass;
    mutable StructLayout *Layout;
    /// Lazily-initialized array of all fragile stored properties in the class
    /// (including superclass stored properties).
    mutable ArrayRef<VarDecl*> AllStoredProperties;
    /// Lazily-initialized array of all fragile stored properties inherited from
    /// superclasses.
    mutable ArrayRef<VarDecl*> InheritedStoredProperties;

    /// Can we use swift reference-counting, or do we have to use
    /// objc_retain/release?
    const ReferenceCounting Refcount;
    
    void generateLayout(IRGenModule &IGM) const;

  public:
    ClassTypeInfo(llvm::PointerType *irType, Size size,
                  llvm::BitVector spareBits, Alignment align,
                  ClassDecl *D, ReferenceCounting refcount)
      : HeapTypeInfo(irType, size, std::move(spareBits), align), TheClass(D),
        Layout(nullptr), Refcount(refcount) {}

    ReferenceCounting getReferenceCounting() const {
      return Refcount;
    }

    ~ClassTypeInfo() {
      delete Layout;
    }

    ClassDecl *getClass() const { return TheClass; }

    const StructLayout &getLayout(IRGenModule &IGM) const;
    ArrayRef<VarDecl*> getAllStoredProperties(IRGenModule &IGM) const;
    ArrayRef<VarDecl*> getInheritedStoredProperties(IRGenModule &IGM) const;

    Alignment getHeapAlignment(IRGenModule &IGM) const {
      return getLayout(IGM).getAlignment();
    }
    ArrayRef<ElementLayout> getElements(IRGenModule &IGM) const {
      return getLayout(IGM).getElements();
    }
  };

  /// A class for computing properties of the instance-variable layout
  /// of a class.  TODO: cache the results!
  class LayoutClass {
    IRGenModule &IGM;

    ClassDecl *Root;
    SmallVector<FieldEntry, 8> Fields;

    bool IsMetadataResilient = false;
    bool IsObjectResilient = false;
    bool IsObjectGenericallyArranged = false;

    ResilienceScope Resilience;

  public:
    LayoutClass(IRGenModule &IGM, ResilienceScope resilience,
                ClassDecl *theClass, SILType type)
        : IGM(IGM), Resilience(resilience) {
      layout(theClass, type);
    }

    /// The root class for purposes of metaclass objects.
    ClassDecl *getRootClassForMetaclass() const {
      // If the formal root class is imported from Objective-C, then
      // we should use that.  For a class that's really implemented in
      // Objective-C, this is obviously right.  For a class that's
      // really implemented in Swift, but that we're importing via an
      // Objective-C interface, this would be wrong --- except such a
      // class can never be a formal root class, because a Swift class
      // without a formal superclass will actually be parented by
      // SwiftObject (or maybe eventually something else like it),
      // which will be visible in the Objective-C type system.
      if (Root->hasClangNode()) return Root;

      return IGM.getSwiftRootClass();
    }

    const FieldEntry &getFieldEntry(VarDecl *field) const {
      for (auto &entry : Fields)
        if (entry.getVar() == field)
          return entry;
      llvm_unreachable("no entry for field!");
    }

  private:
    void layout(ClassDecl *theClass, SILType type) {
      // First, collect information about the superclass.
      if (theClass->hasSuperclass()) {
        SILType superclassType = type.getSuperclass(nullptr);
        auto superclass = superclassType.getClassOrBoundGenericClass();
        assert(superclass);
        layout(superclass, superclassType);
      } else {
        Root = theClass;
      }

      // If the class is resilient, then it may have fields we can't
      // see, and all subsequent fields are *at least* resilient ---
      // and if the class is generic, then it may have
      // dependently-sized fields, and we'll be in the worst case.
      bool isClassResilient = IGM.isResilient(theClass, Resilience);
      if (isClassResilient) {
        IsMetadataResilient = true;
        IsObjectResilient = true;
      }

      // Okay, make entries for all the physical fields we know about.
      for (auto member : theClass->getMembers()) {
        auto var = dyn_cast<VarDecl>(member);
        if (!var) continue;

        // Skip properties that we have to access logically.
        assert(isClassResilient || !IGM.isResilient(var, Resilience));
        if (!var->hasStorage())
          continue;

        // Adjust based on the type of this field.
        // FIXME: this algorithm is assuming that fields are laid out
        // in declaration order.
        adjustAccessAfterField(var, type);

        Fields.push_back(FieldEntry(var, getCurFieldAccess()));
      }
    }

    FieldAccess getCurFieldAccess() const {
      if (IsObjectGenericallyArranged) {
        if (IsMetadataResilient) {
          return FieldAccess::NonConstantIndirect;
        } else {
          return FieldAccess::ConstantIndirect;
        }
      } else {
        if (IsObjectResilient) {
          return FieldAccess::NonConstantDirect;
        } else {
          return FieldAccess::ConstantDirect;
        }
      }
    }

    void adjustAccessAfterField(VarDecl *var, SILType classType) {
      if (!var->hasStorage()) return;

      SILType fieldType = classType.getFieldType(var, *IGM.SILMod);
      switch (IGM.classifyTypeSize(fieldType, ResilienceScope::Local)) {
      case ObjectSize::Fixed:
        return;
      case ObjectSize::Resilient:
        IsObjectResilient = true;
        return;
      case ObjectSize::Dependent:
        IsObjectResilient = IsObjectGenericallyArranged = true;
        return;
      }
      llvm_unreachable("bad ObjectSize value");
    }
  };
}  // end anonymous namespace.

/// Return the lowered type for the class's 'self' type within its context.
static SILType getSelfType(ClassDecl *base) {
  auto loweredTy = base->getDeclaredTypeInContext()->getCanonicalType();
  return SILType::getPrimitiveObjectType(loweredTy);
}

/// Return the type info for the class's 'self' type within its context.
static const ClassTypeInfo &getSelfTypeInfo(IRGenModule &IGM, ClassDecl *base) {
  return IGM.getTypeInfo(getSelfType(base)).as<ClassTypeInfo>();
}

/// Return the index of the given field within the class.
static unsigned getFieldIndex(IRGenModule &IGM,
                              ClassDecl *base, VarDecl *target) {
  // FIXME: This is algorithmically terrible.
  auto &ti = getSelfTypeInfo(IGM, base);
  
  auto props = ti.getAllStoredProperties(IGM);
  auto found = std::find(props.begin(), props.end(), target);
  assert(found != props.end() && "didn't find field in type?!");
  return found - props.begin();
}

namespace {
  class ClassLayoutBuilder : public StructLayoutBuilder {
    SmallVector<ElementLayout, 8> Elements;
    SmallVector<VarDecl*, 8> AllStoredProperties;
    unsigned NumInherited = 0;
  public:
    ClassLayoutBuilder(IRGenModule &IGM, ClassDecl *theClass)
      : StructLayoutBuilder(IGM)
    {
      // Start by adding a heap header.
      addHeapHeader();

      // Next, add the fields for the given class.
      addFieldsForClass(theClass, getSelfType(theClass));
      
      // Add these fields to the builder.
      addFields(Elements, LayoutStrategy::Universal);
    }

    /// Return the element layouts.
    ArrayRef<ElementLayout> getElements() const {
      return Elements;
    }
    
    /// Return the full list of stored properties.
    ArrayRef<VarDecl *> getAllStoredProperties() const {
      return AllStoredProperties;
    }

    /// Return the inherited stored property count.
    unsigned getNumInherited() const {
      return NumInherited;
    }
  private:
    void addFieldsForClass(ClassDecl *theClass,
                           SILType classType) {
      if (theClass->hasSuperclass()) {
        // TODO: apply substitutions when computing base-class layouts!
        SILType superclassType = classType.getSuperclass(nullptr);
        auto superclass = superclassType.getClassOrBoundGenericClass();
        assert(superclass);

        // Recur.
        addFieldsForClass(superclass, superclassType);
        // Count the fields we got from the superclass.
        NumInherited = Elements.size();
      }

      // Collect fields from this class and add them to the layout as a chunk.
      addDirectFieldsFromClass(theClass, classType);
    }

    void addDirectFieldsFromClass(ClassDecl *theClass,
                                  SILType classType) {
      for (VarDecl *var : theClass->getStoredProperties()) {
        SILType type = classType.getFieldType(var, *IGM.SILMod);
        auto &eltType = IGM.getTypeInfo(type);

        // FIXME: Type-parameter-dependent field layout isn't fully
        // implemented yet.
        if (!eltType.isFixedSize() && !IGM.Opts.EnableDynamicValueTypeLayout) {
          IGM.fatal_unimplemented(var->getLoc(), "non-fixed class layout");
        }
        
        Elements.push_back(ElementLayout::getIncomplete(eltType));
        AllStoredProperties.push_back(var);
      }
    }
  };
}

void ClassTypeInfo::generateLayout(IRGenModule &IGM) const {
  assert(!Layout && AllStoredProperties.empty() && "already generated layout");

  // Add the heap header.
  ClassLayoutBuilder builder(IGM, getClass());
  
  // Set the body of the class type.
  auto classPtrTy = cast<llvm::PointerType>(getStorageType());
  auto classTy = cast<llvm::StructType>(classPtrTy->getElementType());
  builder.setAsBodyOfStruct(classTy);
  
  // Record the layout.
  Layout = new StructLayout(builder, classTy, builder.getElements());
  AllStoredProperties
    = IGM.Context.AllocateCopy(builder.getAllStoredProperties());
  InheritedStoredProperties
    = AllStoredProperties.slice(0, builder.getNumInherited());
}

const StructLayout &ClassTypeInfo::getLayout(IRGenModule &IGM) const {
  // Return the cached layout if available.
  if (Layout) return *Layout;

  generateLayout(IGM);
  return *Layout;
}

ArrayRef<VarDecl*>
ClassTypeInfo::getAllStoredProperties(IRGenModule &IGM) const {
  // Return the cached layout if available.
  if (Layout)
    return AllStoredProperties;
  
  generateLayout(IGM);
  return AllStoredProperties;
}

ArrayRef<VarDecl*>
ClassTypeInfo::getInheritedStoredProperties(IRGenModule &IGM) const {
  // Return the cached layout if available.
  if (Layout)
    return InheritedStoredProperties;
  
  generateLayout(IGM);
  return InheritedStoredProperties;
}

/// Cast the base to i8*, apply the given inbounds offset (in bytes,
/// as a size_t), and cast to a pointer to the given type.
llvm::Value *IRGenFunction::emitByteOffsetGEP(llvm::Value *base,
                                              llvm::Value *offset,
                                              llvm::Type *objectType,
                                              const llvm::Twine &name) {
  assert(offset->getType() == IGM.SizeTy);
  auto addr = Builder.CreateBitCast(base, IGM.Int8PtrTy);
  addr = Builder.CreateInBoundsGEP(addr, offset);
  return Builder.CreateBitCast(addr, objectType->getPointerTo(), name);
}

/// Cast the base to i8*, apply the given inbounds offset (in bytes,
/// as a size_t), and create an address in the given type.
Address IRGenFunction::emitByteOffsetGEP(llvm::Value *base,
                                         llvm::Value *offset,
                                         const TypeInfo &type,
                                         const llvm::Twine &name) {
  auto addr = emitByteOffsetGEP(base, offset, type.getStorageType(), name);
  return type.getAddressForPointer(addr);
}

/// Emit a field l-value by applying the given offset to the given base.
static OwnedAddress emitAddressAtOffset(IRGenFunction &IGF,
                                        SILType baseType,
                                        llvm::Value *base,
                                        llvm::Value *offset,
                                        VarDecl *field) {
  auto &fieldTI =
    IGF.getTypeInfo(baseType.getFieldType(field, *IGF.IGM.SILMod));
  auto addr = IGF.emitByteOffsetGEP(base, offset, fieldTI,
                              base->getName() + "." + field->getName().str());
  return OwnedAddress(addr, base);
}

llvm::Constant *irgen::tryEmitClassConstantFragileFieldOffset(IRGenModule &IGM,
                                                            ClassDecl *theClass,
                                                            VarDecl *field) {
  assert(field->hasStorage());
  // FIXME: This field index computation is an ugly hack.
  auto &ti = getSelfTypeInfo(IGM, theClass);

  unsigned fieldIndex = getFieldIndex(IGM, theClass, field);
  auto &element = ti.getElements(IGM)[fieldIndex];
  if (element.getKind() == ElementLayout::Kind::Fixed)
    return IGM.getSize(element.getByteOffset());
  return nullptr;
}

OwnedAddress irgen::projectPhysicalClassMemberAddress(IRGenFunction &IGF,
                                                      llvm::Value *base,
                                                      SILType baseType,
                                                      VarDecl *field) {
  auto &baseClassTI = IGF.getTypeInfo(baseType).as<ClassTypeInfo>();
  ClassDecl *baseClass = baseType.getClassOrBoundGenericClass();
  
  // TODO: Lay out the class based on the substituted baseType rather than
  // the generic type. Doing this requires that we also handle
  // specialized layout in ClassTypeInfo.
  LayoutClass layout(IGF.IGM, ResilienceScope::Local, baseClass,
                     getSelfType(baseClass) /* TODO: should be baseType */);
  
  auto &entry = layout.getFieldEntry(field);
  switch (entry.getAccess()) {
  case FieldAccess::ConstantDirect: {
    // FIXME: This field index computation is an ugly hack.
    unsigned fieldIndex = getFieldIndex(IGF.IGM, baseClass, field);

    Address baseAddr(base, baseClassTI.getHeapAlignment(IGF.IGM));
    auto &element = baseClassTI.getElements(IGF.IGM)[fieldIndex];
    Address memberAddr = element.project(IGF, baseAddr, Nothing);
    return OwnedAddress(memberAddr, base);
  }
    
  case FieldAccess::NonConstantDirect: {
    Address offsetA = IGF.IGM.getAddrOfFieldOffset(field, /*indirect*/ false,
                                                   NotForDefinition);
    auto offset = IGF.Builder.CreateLoad(offsetA, "offset");
    return emitAddressAtOffset(IGF, baseType, base, offset, field);
  }
    
  case FieldAccess::ConstantIndirect: {
    auto metadata = emitHeapMetadataRefForHeapObject(IGF, base, baseType);
    auto offset = emitClassFieldOffset(IGF, baseClass, field, metadata);
    return emitAddressAtOffset(IGF, baseType, base, offset, field);
  }
    
  case FieldAccess::NonConstantIndirect: {
    auto metadata = emitHeapMetadataRefForHeapObject(IGF, base, baseType);
    Address indirectOffsetA =
      IGF.IGM.getAddrOfFieldOffset(field, /*indirect*/ true,
                                   NotForDefinition);
    auto indirectOffset =
      IGF.Builder.CreateLoad(indirectOffsetA, "indirect-offset");
    auto offsetA =
      IGF.emitByteOffsetGEP(metadata, indirectOffset, IGF.IGM.SizeTy);
    auto offset =
      IGF.Builder.CreateLoad(Address(offsetA, IGF.IGM.getPointerAlignment()));
    return emitAddressAtOffset(IGF, baseType, base, offset, field);
  }
  }
  llvm_unreachable("bad field-access strategy");
}

/// Emit a checked unconditional downcast.
llvm::Value *IRGenFunction::emitDowncast(llvm::Value *from, SILType toType,
                                         CheckedCastMode mode) {
  // Emit the value we're casting from.
  if (from->getType() != IGM.Int8PtrTy)
    from = Builder.CreateBitCast(from, IGM.Int8PtrTy);
  
  // Emit a reference to the metadata.
  bool isConcreteClass = toType.is<ClassType>();
  llvm::Value *metadataRef;
  llvm::Constant *castFn;
  if (isConcreteClass) {
    // If the dest type is a concrete class, get the full class metadata
    // and call dynamicCastClass directly.
    metadataRef
      = IGM.getAddrOfTypeMetadata(toType.getSwiftRValueType(), false, false);
    switch (mode) {
    case CheckedCastMode::Unconditional:
      castFn = IGM.getDynamicCastClassUnconditionalFn();
      break;
    case CheckedCastMode::Conditional:
      castFn = IGM.getDynamicCastClassFn();
      break;
    }
  } else {
    // Otherwise, get the type metadata, which may be local, and go through
    // the more general dynamicCast entry point.
    metadataRef = emitTypeMetadataRef(toType);
    switch (mode) {
    case CheckedCastMode::Unconditional:
      castFn = IGM.getDynamicCastUnconditionalFn();
      break;
    case CheckedCastMode::Conditional:
      castFn = IGM.getDynamicCastFn();
      break;
    }
  }
  
  if (metadataRef->getType() != IGM.Int8PtrTy)
    metadataRef = Builder.CreateBitCast(metadataRef, IGM.Int8PtrTy);
  
  // Call the (unconditional) dynamic cast.
  auto call
    = Builder.CreateCall2(castFn, from, metadataRef);
  // FIXME: Eventually, we may want to throw.
  call->setDoesNotThrow();
  
  llvm::Type *subTy = getTypeInfo(toType).StorageType;
  return Builder.CreateBitCast(call, subTy);
}

/// Emit an allocation of a class.
llvm::Value *irgen::emitClassAllocation(IRGenFunction &IGF, SILType selfType,
                                        bool objc) {
  auto &classTI = IGF.getTypeInfo(selfType).as<ClassTypeInfo>();
  llvm::Value *metadata = emitClassHeapMetadataRef(IGF, selfType);

  // If we need to use Objective-C allocation, do so.
  // If the root class isn't known to use the Swift allocator, we need
  // to call [self alloc].
  if (objc) {
    return emitObjCAllocObjectCall(IGF, metadata, selfType.getSwiftRValueType());
  }

  // FIXME: Long-term, we clearly need a specialized runtime entry point.
  llvm::Value *size, *alignMask;
  std::tie(size, alignMask)
    = emitClassFragileInstanceSizeAndAlignMask(IGF,
                                   selfType.getClassOrBoundGenericClass(),
                                   metadata);
  
  llvm::Value *val = IGF.emitAllocObjectCall(metadata, size, alignMask,
                                             "reference.new");
  auto &layout = classTI.getLayout(IGF.IGM);
  llvm::Type *destType = layout.getType()->getPointerTo();
  return IGF.Builder.CreateBitCast(val, destType);
}

llvm::Value *irgen::emitClassAllocationDynamic(IRGenFunction &IGF, 
                                               llvm::Value *metadata,
                                               SILType selfType,
                                               bool objc) {
  // If we need to use Objective-C allocation, do so.
  if (objc) {
    return emitObjCAllocObjectCall(IGF, metadata, 
                                   selfType.getSwiftRValueType());
  }

  // Otherwise, allocate using Swift's routines.
  llvm::Value *size, *alignMask;
  std::tie(size, alignMask)
    = emitClassResilientInstanceSizeAndAlignMask(IGF,
                                   selfType.getClassOrBoundGenericClass(),
                                   metadata);
  
  llvm::Value *val = IGF.emitAllocObjectCall(metadata, size, alignMask,
                                             "reference.new");
  auto &classTI = IGF.getTypeInfo(selfType).as<ClassTypeInfo>();
  auto &layout = classTI.getLayout(IGF.IGM);
  llvm::Type *destType = layout.getType()->getPointerTo();
  return IGF.Builder.CreateBitCast(val, destType);
}

void irgen::emitClassDeallocation(IRGenFunction &IGF, SILType selfType,
                                  llvm::Value *selfValue) {
  auto *theClass = selfType.getClassOrBoundGenericClass();

  // Determine the size of the object we're deallocating.
  // FIXME: We should get this value dynamically!
  auto &info = IGF.IGM.getTypeInfo(selfType).as<ClassTypeInfo>();
  auto &layout = info.getLayout(IGF.IGM);
  // FIXME: Dynamic-layout deallocation size.
  llvm::Value *size, *alignMask;
  if (layout.isFixedLayout()) {
    size = info.getLayout(IGF.IGM).emitSize(IGF.IGM);
  } else {
    llvm::Value *metadata = emitTypeMetadataRefForHeapObject(IGF, selfValue, 
                                                             selfType);
    std::tie(size, alignMask)
      = emitClassFragileInstanceSizeAndAlignMask(IGF, theClass, metadata);
  }

  selfValue = IGF.Builder.CreateBitCast(selfValue, IGF.IGM.RefCountedPtrTy);
  emitDeallocateHeapObject(IGF, selfValue, size);
}

llvm::Constant *irgen::tryEmitClassConstantFragileInstanceSize(
                                                        IRGenModule &IGM,
                                                        ClassDecl *Class) {
  auto &classTI = getSelfTypeInfo(IGM, Class);

  auto &layout = classTI.getLayout(IGM);
  if (layout.isFixedLayout())
    return layout.emitSize(IGM);
  
  return nullptr;
}

llvm::Constant *irgen::tryEmitClassConstantFragileInstanceAlignMask(
                                                             IRGenModule &IGM,
                                                             ClassDecl *Class) {
  auto &classTI = getSelfTypeInfo(IGM, Class);
  
  auto &layout = classTI.getLayout(IGM);
  if (layout.isFixedLayout())
    return layout.emitAlignMask(IGM);
  
  return nullptr;
}

/// emitClassDecl - Emit all the declarations associated with this class type.
void IRGenModule::emitClassDecl(ClassDecl *D) {
  PrettyStackTraceDecl prettyStackTrace("emitting class metadata for", D);

  auto &classTI = Types.getTypeInfo(D).as<ClassTypeInfo>();
  auto &layout = classTI.getLayout(*this);

  // Emit the class metadata.
  emitClassMetadata(*this, D, layout);
  
  // FIXME: This is mostly copy-paste from emitExtension;
  // figure out how to refactor! 
  for (Decl *member : D->getMembers()) {
    switch (member->getKind()) {
    case DeclKind::Import:
    case DeclKind::TopLevelCode:
    case DeclKind::Protocol:
    case DeclKind::EnumElement:
    case DeclKind::Extension:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
    case DeclKind::EnumCase:
    case DeclKind::Param:
      llvm_unreachable("decl not allowed in class!");
        
    // We can have meaningful initializers for variables, but
    // we can't handle them yet.  For the moment, just ignore them.
    case DeclKind::PatternBinding:
      continue;

    case DeclKind::Subscript:
      // Getter/setter will be handled separately.
      continue;
        
    case DeclKind::IfConfig:
      // Any active IfConfig block members are handled separately.
      continue;

    case DeclKind::TypeAlias:
    case DeclKind::AssociatedType:
    case DeclKind::GenericTypeParam:
      continue;
    case DeclKind::Enum:
      emitEnumDecl(cast<EnumDecl>(member));
      continue;
    case DeclKind::Struct:
      emitStructDecl(cast<StructDecl>(member));
      continue;
    case DeclKind::Class:
      emitClassDecl(cast<ClassDecl>(member));
      continue;
    case DeclKind::Var:
      if (!cast<VarDecl>(member)->hasStorage())
        // Getter/setter will be handled separately.
        continue;
      // FIXME: Will need an implementation here for resilience
      continue;
    case DeclKind::Func:
      emitLocalDecls(cast<FuncDecl>(member));
      continue;
    case DeclKind::Constructor:
      emitLocalDecls(cast<ConstructorDecl>(member));
      continue;
    case DeclKind::Destructor:
      emitLocalDecls(cast<DestructorDecl>(member));
      continue;
    }
    llvm_unreachable("bad extension member kind");
  }
}

namespace {
  enum ForMetaClass_t : bool {
    ForClass = false,
    ForMetaClass = true
  };

  typedef std::pair<ClassDecl*, Module*> CategoryNameKey;
  /// Used to provide unique names to ObjC categories generated by Swift
  /// extensions. The first category for a class in a module gets the module's
  /// name as its key, e.g., NSObject (MySwiftModule). Another extension of the
  /// same class in the same module gets a category name with a number appended,
  /// e.g., NSObject (MySwiftModule1).
  llvm::DenseMap<CategoryNameKey, unsigned> CategoryCounts;

  /// A class for building ObjC class data (in Objective-C terms, class_ro_t),
  /// category data (category_t), or protocol data (protocol_t).
  class ClassDataBuilder : public ClassMemberVisitor<ClassDataBuilder> {
    IRGenModule &IGM;
    PointerUnion<ClassDecl *, ProtocolDecl *> TheEntity;
    ExtensionDecl *TheExtension;
    const LayoutClass *Layout;
    const StructLayout *FieldLayout;
    
    ClassDecl *getClass() const {
      return TheEntity.get<ClassDecl*>();
    }
    ProtocolDecl *getProtocol() const {
      return TheEntity.get<ProtocolDecl*>();
    }
    
    bool isBuildingClass() const {
      return TheEntity.is<ClassDecl*>() && !TheExtension;
    }
    bool isBuildingCategory() const {
      return TheEntity.is<ClassDecl*>() && TheExtension;
    }
    bool isBuildingProtocol() const {
      return TheEntity.is<ProtocolDecl*>();
    }

    bool HasNonTrivialDestructor = false;
    bool HasNonTrivialConstructor = false;
    llvm::SmallString<16> CategoryName;
    SmallVector<llvm::Constant*, 8> Ivars;
    SmallVector<llvm::Constant*, 16> InstanceMethods;
    SmallVector<llvm::Constant*, 16> ClassMethods;
    SmallVector<llvm::Constant*, 16> OptInstanceMethods;
    SmallVector<llvm::Constant*, 16> OptClassMethods;
    SmallVector<llvm::Constant*, 4> Protocols;
    SmallVector<llvm::Constant*, 8> Properties;
    SmallVector<llvm::Constant*, 16> MethodTypesExt;
    SmallVector<llvm::Constant*, 16> OptMethodTypesExt;
    
    llvm::Constant *Name = nullptr;
    /// Index of the first non-inherited field in the layout.
    unsigned FirstFieldIndex;
    unsigned NextFieldIndex;
  public:
    ClassDataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                     const LayoutClass &layout,
                     const StructLayout &fieldLayout,
                     unsigned firstField)
        : IGM(IGM), TheEntity(theClass), TheExtension(nullptr),
          Layout(&layout), FieldLayout(&fieldLayout),
          FirstFieldIndex(firstField),
          NextFieldIndex(firstField)
    {
      visitConformances(theClass->getProtocols());
      visitMembers(theClass);

      if (Lowering::usesObjCAllocator(theClass)) {
        addIVarInitializer(); 
        addIVarDestroyer(); 
      }
    }
    
    ClassDataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                     ExtensionDecl *theExtension)
      : IGM(IGM), TheEntity(theClass), TheExtension(theExtension),
        Layout(nullptr), FieldLayout(nullptr)
    {
      buildCategoryName(CategoryName);

      visitConformances(theExtension->getProtocols());

      for (Decl *member : TheExtension->getMembers())
        visit(member);
      
      // ObjC protocol conformances may need to pull method descriptors for
      // definitions from other contexts into the category.
      for (unsigned i = 0, size = TheExtension->getProtocols().size();
           i < size; ++i)
        visitObjCConformance(TheExtension->getProtocols()[i],
                             TheExtension->getConformances()[i]);
    }
    
    ClassDataBuilder(IRGenModule &IGM, ProtocolDecl *theProtocol)
      : IGM(IGM), TheEntity(theProtocol), TheExtension(nullptr)
    {
      visitConformances(theProtocol->getProtocols());

      for (Decl *member : theProtocol->getMembers())
        visit(member);
    }
    
    void visitConformances(ArrayRef<ProtocolDecl*> allProtocols) {
      // Gather protocol records for all of the formal ObjC protocol
      // conformances.
      for (ProtocolDecl *p : allProtocols) {
        if (!p->isObjC())
          continue;
        // Don't emit the magic AnyObject conformance.
        if (p == IGM.Context.getProtocol(KnownProtocolKind::AnyObject))
          continue;
        Protocols.push_back(buildProtocolRef(p));
      }
    }
    
    void visitObjCConformance(ProtocolDecl *protocol,
                              ProtocolConformance *conformance) {
      assert(TheExtension &&
             "should only consider objc conformances for extensions");
      if (protocol->isObjC()) {
        conformance->forEachValueWitness(nullptr,
                                         [&](ValueDecl *req,
                                             ConcreteDeclRef witness) {
          // Missing optional requirement.
          if (!witness)
            return;

          ValueDecl *vd = witness.getDecl();
          if (vd->getDeclContext() != TheExtension && !vd->isObjC())
            visit(vd);
        });
      }
      
      for (auto &inherited : conformance->getInheritedConformances())
        visitObjCConformance(inherited.first, inherited.second);
    }

    /// Build the metaclass stub object.
    void buildMetaclassStub() {
      assert(Layout && "can't build a metaclass from a category");
      // The isa is the metaclass pointer for the root class.
      auto rootClass = Layout->getRootClassForMetaclass();
      auto rootPtr = IGM.getAddrOfMetaclassObject(rootClass, NotForDefinition);

      // The superclass of the metaclass is the metaclass of the
      // superclass.  Note that for metaclass stubs, we can always
      // ignore parent contexts and generic arguments.
      //
      // If this class has no formal superclass, then its actual
      // superclass is SwiftObject, i.e. the root class.
      llvm::Constant *superPtr;
      if (getClass()->hasSuperclass()) {
        auto base = getClass()->getSuperclass()->getClassOrBoundGenericClass();
        superPtr = IGM.getAddrOfMetaclassObject(base, NotForDefinition);
      } else {
        superPtr = rootPtr;
      }

      auto dataPtr = emitROData(ForMetaClass);
      dataPtr = llvm::ConstantExpr::getPtrToInt(dataPtr, IGM.IntPtrTy);

      llvm::Constant *fields[] = {
        rootPtr,
        superPtr,
        IGM.getObjCEmptyCachePtr(),
        IGM.getObjCEmptyVTablePtr(),
        dataPtr
      };
      auto init = llvm::ConstantStruct::get(IGM.ObjCClassStructTy,
                                            makeArrayRef(fields));
      auto metaclass =
        cast<llvm::GlobalVariable>(
                     IGM.getAddrOfMetaclassObject(getClass(), ForDefinition));
      metaclass->setInitializer(init);
    }
    
  private:
    void buildCategoryName(SmallVectorImpl<char> &s) {
      llvm::raw_svector_ostream os(s);
      // Find the module the extension is declared in.
      Module *TheModule = TheExtension->getParentModule();

      os << TheModule->Name;
      
      unsigned categoryCount = CategoryCounts[{getClass(), TheModule}]++;
      if (categoryCount > 0)
        os << categoryCount;
        
      os.flush();
    }
    
  public:
    llvm::Constant *emitCategory() {
      assert(TheExtension && "can't emit category data for a class");
      SmallVector<llvm::Constant*, 11> fields;
      // struct category_t {
      //   char const *name;
      fields.push_back(IGM.getAddrOfGlobalString(CategoryName));
      //   const class_t *theClass;
      if (getClass()->hasClangNode())
        fields.push_back(IGM.getAddrOfObjCClass(getClass(), NotForDefinition));
      else {
        auto type = getSelfType(getClass()).getSwiftRValueType();
        llvm::Constant *metadata = tryEmitConstantHeapMetadataRef(IGM, type);
        assert(metadata &&
               "extended objc class doesn't have constant metadata?");
        fields.push_back(metadata);
      }
      //   const method_list_t *instanceMethods;
      fields.push_back(buildInstanceMethodList());
      //   const method_list_t *classMethods;
      fields.push_back(buildClassMethodList());
      //   const protocol_list_t *baseProtocols;
      fields.push_back(buildProtocolList());
      //   const property_list_t *properties;
      fields.push_back(buildPropertyList());
      // };
      
      return buildGlobalVariable(fields, "_CATEGORY_");
    }
    
    llvm::Constant *emitProtocol() {
      SmallVector<llvm::Constant*, 11> fields;
      llvm::SmallString<64> nameBuffer;

      assert(isBuildingProtocol() && "not emitting a protocol");
      
      // struct protocol_t {
      //   Class super;
      fields.push_back(null());
      //   char const *name;
      fields.push_back(IGM.getAddrOfGlobalString(getEntityName(nameBuffer)));
      //   const protocol_list_t *baseProtocols;
      fields.push_back(buildProtocolList());
      //   const method_list_t *requiredInstanceMethods;
      fields.push_back(buildInstanceMethodList());
      //   const method_list_t *requiredClassMethods;
      fields.push_back(buildClassMethodList());
      //   const method_list_t *optionalInstanceMethods;
      fields.push_back(buildOptInstanceMethodList());
      //   const method_list_t *optionalClassMethods;
      fields.push_back(buildOptClassMethodList());
      //   const property_list_t *properties;
      fields.push_back(buildPropertyList());
      //   uint32_t size;
      unsigned size = IGM.getPointerSize().getValue() * fields.size() +
                      IGM.getPointerSize().getValue(); // This is for extendedMethodTypes
      size += 8; // 'size' and 'flags' fields that haven't been added yet.
      fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty, size));
      //   uint32_t flags;
      //   1 = Swift
      unsigned swiftFlag = getProtocol()->hasClangNode() ? 0 : 1;
      fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty, swiftFlag));
      
      // const char ** extendedMethodTypes;
      fields.push_back(buildOptExtendedMethodTypes());
      
      // };
      
      return buildGlobalVariable(fields, "_PROTOCOL_");
    }

    llvm::Constant *emitROData(ForMetaClass_t forMeta) {
      assert(Layout && FieldLayout && "can't emit rodata for a category");
      SmallVector<llvm::Constant*, 11> fields;
      // struct _class_ro_t {
      //   uint32_t flags;
      fields.push_back(buildFlags(forMeta));

      //   uint32_t instanceStart;
      //   uint32_t instanceSize;
      // The runtime requires that the ivar offsets be initialized to
      // a valid layout of the ivars of this class, bounded by these
      // two values.  If the instanceSize of the superclass equals the
      // stored instanceStart of the subclass, the ivar offsets
      // will not be changed.
      Size instanceStart = Size(0);
      Size instanceSize = Size(0);
      if (!forMeta) {
        instanceSize = FieldLayout->getSize();
        if (FieldLayout->getElements().empty()
            || FieldLayout->getElements().size() == FirstFieldIndex) {
          instanceStart = instanceSize;
        } else if (FieldLayout->getElements()[FirstFieldIndex].getKind()
                     == ElementLayout::Kind::Fixed) {
          // FIXME: assumes layout is always sequential!
          instanceStart = FieldLayout->getElements()[FirstFieldIndex].getByteOffset();
        } else {
          // FIXME: arrange to initialize this at runtime
        }
      }
      fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty,
                                              instanceStart.getValue()));
      fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty,
                                              instanceSize.getValue()));

      //   uint32_t reserved;  // only when building for 64bit targets
      if (IGM.getPointerAlignment().getValue() > 4) {
        assert(IGM.getPointerAlignment().getValue() == 8);
        fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty, 0));
      }

      //   const uint8_t *ivarLayout;
      // GC/ARC layout.  TODO.
      fields.push_back(null());

      //   const char *name;
      // It is correct to use the same name for both class and metaclass.
      fields.push_back(buildName());

      //   const method_list_t *baseMethods;
      fields.push_back(forMeta ? buildClassMethodList()
                               : buildInstanceMethodList());

      //   const protocol_list_t *baseProtocols;
      // Apparently, this list is the same in the class and the metaclass.
      fields.push_back(buildProtocolList());

      //   const ivar_list_t *ivars;
      fields.push_back(forMeta ? null() : buildIvarList());

      //   const uint8_t *weakIvarLayout;
      // More GC/ARC layout.  TODO.
      fields.push_back(null());

      //   const property_list_t *baseProperties;
      fields.push_back(forMeta ? null() : buildPropertyList());

      // };

      auto dataSuffix = forMeta ? "_METACLASS_DATA_" : "_DATA_";
      return buildGlobalVariable(fields, dataSuffix);
    }

  private:
    llvm::Constant *buildFlags(ForMetaClass_t forMeta) {
      ClassFlags flags = ClassFlags::CompiledByARC;

      // Mark metaclasses as appropriate.
      if (forMeta) {
        flags |= ClassFlags::Meta;

      // Non-metaclasses need us to record things whether primitive
      // construction/destructor is trivial.
      } else if (HasNonTrivialDestructor || HasNonTrivialConstructor) {
        flags |= ClassFlags::HasCXXStructors;
        if (!HasNonTrivialConstructor)
          flags |= ClassFlags::HasCXXDestructorOnly;
      }

      // FIXME: set ClassFlags::Hidden when appropriate
      return llvm::ConstantInt::get(IGM.Int32Ty, uint32_t(flags));
    }

    llvm::Constant *buildName() {
      if (Name) return Name;

      llvm::SmallString<64> buffer;
      Name = IGM.getAddrOfGlobalString(getClass()->getObjCRuntimeName(buffer));
      return Name;
    }

    llvm::Constant *null() {
      return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
    }

    /*** Methods ***********************************************************/

  public:
    /// Methods need to be collected into the appropriate methods list.
    void visitFuncDecl(FuncDecl *method) {
      if (!isBuildingProtocol() &&
          !requiresObjCMethodDescriptor(method)) return;
      
      // getters and setters funcdecls will be handled by their parent
      // var/subscript.
      if (method->isAccessor()) return;
      
      llvm::Constant *entry = emitObjCMethodDescriptor(IGM, method);
      if (!method->isStatic()) {
        if (method->getAttrs().isOptional()) {
          OptInstanceMethods.push_back(entry);
          if (isBuildingProtocol())
            OptMethodTypesExt.push_back(getMethodTypeExtendedEncoding(IGM, method));
        }
        else {
          InstanceMethods.push_back(entry);
          if (isBuildingProtocol())
            MethodTypesExt.push_back(getMethodTypeExtendedEncoding(IGM, method));
        }
      } else {
        if (method->getAttrs().isOptional()) {
          OptClassMethods.push_back(entry);
          if (isBuildingProtocol())
            OptMethodTypesExt.push_back(getMethodTypeExtendedEncoding(IGM, method));
        }
        else {
          ClassMethods.push_back(entry);
          if (isBuildingProtocol())
            MethodTypesExt.push_back(getMethodTypeExtendedEncoding(IGM, method));
        }
      }
    }

    /// Constructors need to be collected into the appropriate methods list.
    void visitConstructorDecl(ConstructorDecl *constructor) {
      if (!isBuildingProtocol() &&
          !requiresObjCMethodDescriptor(constructor)) return;
      llvm::Constant *entry = emitObjCMethodDescriptor(IGM, constructor);
      if (constructor->getAttrs().isOptional())
        OptInstanceMethods.push_back(entry);
      else
        InstanceMethods.push_back(entry);
    }

    /// Determine whether the given destructor has an Objective-C
    /// definition.
    bool hasObjCDeallocDefinition(DestructorDecl *destructor) {
      // If we have the destructor body, we know whether SILGen
      // generated a -dealloc body.
      if (auto braceStmt = destructor->getBody())
        return !braceStmt->getElements().empty();

      // We don't have a destructor body, so hunt for the SIL function
      // for it.
      SILDeclRef dtorRef(destructor, SILDeclRef::Kind::Deallocator,
                         ResilienceExpansion::Minimal,
                         SILDeclRef::ConstructAtNaturalUncurryLevel,
                         /*isForeign=*/true);
      llvm::SmallString<64> dtorNameBuffer;
      auto dtorName = dtorRef.mangle(dtorNameBuffer);
      if (auto silFn = IGM.SILMod->lookUpFunction(dtorName))
        return silFn->isDefinition();

      // The Objective-C thunk was never even declared, so it is not defined.
      return false;
    }

    /// Destructors need to be collected into the instance methods
    /// list 
    void visitDestructorDecl(DestructorDecl *destructor) {
      auto classDecl = cast<ClassDecl>(destructor->getDeclContext());
      if (Lowering::usesObjCAllocator(classDecl) &&
          hasObjCDeallocDefinition(destructor)) {
        llvm::Constant *entry = emitObjCMethodDescriptor(IGM, destructor);
        InstanceMethods.push_back(entry);
      }
    }

    void addIVarInitializer() {
      if (auto entry = emitObjCIVarInitDestroyDescriptor(IGM, getClass(),
                                                         false)) {
        InstanceMethods.push_back(*entry);

        HasNonTrivialConstructor = true;
      }
    }

    void addIVarDestroyer() {
      if (auto entry = emitObjCIVarInitDestroyDescriptor(IGM, getClass(),
                                                         true)) {
        InstanceMethods.push_back(*entry);

        HasNonTrivialDestructor = true;
      }
    }

  private:
    StringRef chooseNamePrefix(StringRef forClass,
                               StringRef forCategory,
                               StringRef forProtocol) {
      if (isBuildingCategory())
        return forCategory;
      if (isBuildingClass())
        return forClass;
      if (isBuildingProtocol())
        return forProtocol;
      
      llvm_unreachable("not a class, category, or protocol?!");
    }
    
    llvm::Constant *buildClassMethodList() {
      return buildMethodList(ClassMethods,
                             chooseNamePrefix("_CLASS_METHODS_",
                                              "_CATEGORY_CLASS_METHODS_",
                                              "_PROTOCOL_CLASS_METHODS_"));
    }

    llvm::Constant *buildInstanceMethodList() {
      return buildMethodList(InstanceMethods,
                             chooseNamePrefix("_INSTANCE_METHODS_",
                                              "_CATEGORY_INSTANCE_METHODS_",
                                              "_PROTOCOL_INSTANCE_METHODS_"));
    }

    llvm::Constant *buildOptClassMethodList() {
      return buildMethodList(OptClassMethods,
                             "_PROTOCOL_CLASS_METHODS_OPT_");
    }

    llvm::Constant *buildOptInstanceMethodList() {
      return buildMethodList(OptInstanceMethods,
                             "_PROTOCOL_INSTANCE_METHODS_OPT_");
    }

    llvm::Constant *buildOptExtendedMethodTypes() {
      MethodTypesExt.insert(MethodTypesExt.end(),
                            OptMethodTypesExt.begin(), OptMethodTypesExt.end());
      return buildMethodList(MethodTypesExt,
                             "_PROTOCOL_METHOD_TYPES_");
    }

    /// struct method_list_t {
    ///   uint32_t entsize; // runtime uses low bits for its own purposes
    ///   uint32_t count;
    ///   method_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildMethodList(ArrayRef<llvm::Constant*> methods,
                                    StringRef name) {
      return buildOptionalList(methods, 3 * IGM.getPointerSize(), name);
    }

    /*** Protocols *********************************************************/

    /// typedef uintptr_t protocol_ref_t;  // protocol_t*, but unremapped
    llvm::Constant *buildProtocolRef(ProtocolDecl *protocol) {
      assert(protocol->isObjC());
      return IGM.getAddrOfObjCProtocolRecord(protocol, NotForDefinition);
    }
    
    /// struct protocol_list_t {
    ///   uintptr_t count;
    ///   protocol_ref_t[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildProtocolList() {
      return buildOptionalList(Protocols, Size(0),
                               chooseNamePrefix("_PROTOCOLS_",
                                                "_CATEGORY_PROTOCOLS_",
                                                "_PROTOCOL_PROTOCOLS_"));
    }

    /*** Ivars *************************************************************/

  public:
    /// Variables might be stored or computed.
    void visitVarDecl(VarDecl *var) {
      if (var->hasStorage())
        visitStoredVar(var);
      else
        visitProperty(var);
    }

  private:
    /// Ivars need to be collected in the ivars list, and they also
    /// affect flags.
    void visitStoredVar(VarDecl *var) {
      // FIXME: how to handle ivar extensions in categories?
      if (!Layout && !FieldLayout)
        return;

      // For now, we never try to emit specialized versions of the
      // metadata statically, so compute the field layout using the
      // originally-declared type.
      SILType fieldType =
        IGM.getLoweredType(AbstractionPattern(var->getType()), var->getType());
      Ivars.push_back(buildIvar(var, fieldType));

      // Build property accessors for the ivar if necessary.
      visitProperty(var);
    }

    /// struct ivar_t {
    ///   uintptr_t *offset;
    ///   const char *name;
    ///   const char *type;
    ///   uint32_t alignment;
    ///   uint32_t size;
    /// };
    llvm::Constant *buildIvar(VarDecl *ivar, SILType loweredType) {
      assert(Layout && FieldLayout && "can't build ivar for category");
      // FIXME: this is not always the right thing to do!
      auto &elt = FieldLayout->getElements()[NextFieldIndex++];
      auto &ivarTI = IGM.getTypeInfo(loweredType);
      
      llvm::Constant *offsetPtr;
      if (elt.getKind() == ElementLayout::Kind::Fixed) {
        // Emit a field offset variable for the fixed field statically.
        auto offsetAddr = IGM.getAddrOfFieldOffset(ivar, /*indirect*/ false,
                                                   ForDefinition);
        auto offsetVar = cast<llvm::GlobalVariable>(offsetAddr.getAddress());
        offsetVar->setConstant(false);
        auto offsetVal =
          llvm::ConstantInt::get(IGM.IntPtrTy, elt.getByteOffset().getValue());
        offsetVar->setInitializer(offsetVal);
        
        offsetPtr = offsetVar;
      } else {
        // We need to set this up when the metadata is instantiated.
        // FIXME: set something up to fill at runtime
        offsetPtr
          = llvm::ConstantPointerNull::get(IGM.IntPtrTy->getPointerTo());
      }

      // TODO: clang puts this in __TEXT,__objc_methname,cstring_literals
      auto name = IGM.getAddrOfGlobalString(ivar->getName().str());

      // TODO: clang puts this in __TEXT,__objc_methtype,cstring_literals
      auto typeEncode = llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

      Size size;
      Alignment alignment;
      if (auto fixedTI = dyn_cast<FixedTypeInfo>(&ivarTI)) {
        size = fixedTI->getFixedSize();
        alignment = fixedTI->getFixedAlignment();
      } else {
        // FIXME: set something up to fill these in at runtime!
        size = Size(0);
        alignment = Alignment(0);
      }

      // If the size is larger than we can represent in 32-bits,
      // complain about the unimplementable ivar.
      if (uint32_t(size.getValue()) != size.getValue()) {
        IGM.error(ivar->getLoc(),
                  "ivar size (" + Twine(size.getValue()) +
                  " bytes) overflows Objective-C ivar layout");
        size = Size(0);
      }

      llvm::Constant *fields[] = {
        offsetPtr,
        name,
        typeEncode,
        llvm::ConstantInt::get(IGM.Int32Ty, size.getValue()),
        llvm::ConstantInt::get(IGM.Int32Ty, alignment.getValue())
      };
      return llvm::ConstantStruct::getAnon(IGM.getLLVMContext(), fields);
    }

    /// struct ivar_list_t {
    ///   uint32_t entsize;
    ///   uint32_t count;
    ///   ivar_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildIvarList() {
      Size eltSize = 3 * IGM.getPointerSize() + Size(8);
      return buildOptionalList(Ivars, eltSize, "_IVARS_");
    }

    /*** Properties ********************************************************/

    /// Properties need to be collected in the properties list.
    void visitProperty(VarDecl *var) {
      if (requiresObjCPropertyDescriptor(IGM, var)) {
        if (llvm::Constant *prop = buildProperty(var))
          Properties.push_back(prop);
        auto getter_setter = emitObjCPropertyMethodDescriptors(IGM, var);
        if (var->getAttrs().isOptional())
          OptInstanceMethods.push_back(getter_setter.first);
        else
          InstanceMethods.push_back(getter_setter.first);

        if (getter_setter.second) {
          if (var->getAttrs().isOptional())
            OptInstanceMethods.push_back(getter_setter.second);
          else
            InstanceMethods.push_back(getter_setter.second);
        }
      }
    }
    
    /// Build the property attribute string for a property decl.
    void buildPropertyAttributes(VarDecl *prop, SmallVectorImpl<char> &out,
                                 ClassDecl *theClass) {
      llvm::raw_svector_ostream outs(out);
      
      // Emit the type encoding.
      // FIXME: Only correct for class types.
      outs << "T@";
      // FIXME: Assume 'NSObject' really means 'id'.
      if (theClass->getName() != prop->getASTContext().getIdentifier("NSObject"))
        outs << '"' << theClass->getName().str() << '"';
      
      // FIXME: Emit attributes for (nonatomic, strong) if the property has a
      // setter, or (nonatomic, readonly) if the property has only a getter.
      // Are these attributes always appropriate?
      outs << (prop->isSettable(prop->getDeclContext())
        ? ",&,N" // strong, nonatomic
        : ",R,N"); // readonly, nonatomic
      
      // Emit the selector name for the getter. Clang only appears to emit the
      // setter name if the property has an explicit setter= attribute.
      outs << ",V" << prop->getName();
      
      outs.flush();
    }

    /// struct property_t {
    ///   const char *name;
    ///   const char *attributes;
    /// };
    llvm::Constant *buildProperty(VarDecl *prop) {
      // FIXME: For now we only emit properties of ObjC class type.
      ClassDecl *theClass
        = IGM.SILMod->Types.getLoweredBridgedType(prop->getType(),
                                                  AbstractCC::ObjCMethod)
          ->getClassOrBoundGenericClass();
      if (!theClass)
        return nullptr;
      if (!theClass->isObjC())
        return nullptr;

      llvm::SmallString<16> propertyAttributes;
      buildPropertyAttributes(prop, propertyAttributes, theClass);
      
      llvm::Constant *fields[] = {
        IGM.getAddrOfGlobalString(prop->getName().str()),
        IGM.getAddrOfGlobalString(propertyAttributes)
      };
      return llvm::ConstantStruct::getAnon(IGM.getLLVMContext(), fields);
    }

    /// struct property_list_t {
    ///   uint32_t entsize;
    ///   uint32_t count;
    ///   property_t list[count];
    /// };
    ///
    /// This method does not return a value of a predictable type.
    llvm::Constant *buildPropertyList() {
      Size eltSize = 2 * IGM.getPointerSize();
      return buildOptionalList(Properties, eltSize,
                               chooseNamePrefix("_PROPERTIES_",
                                                "_CATEGORY_PROPERTIES_",
                                                "_PROTOCOL_PROPERTIES_"));
    }

    /*** General ***********************************************************/

    /// Build a list structure from the given array of objects.
    /// If the array is empty, use null.  The assumption is that every
    /// initializer has the same size.
    ///
    /// \param optionalEltSize - if non-zero, a size which needs
    ///   to be placed in the list header
    llvm::Constant *buildOptionalList(ArrayRef<llvm::Constant*> objects,
                                      Size optionalEltSize,
                                      StringRef nameBase) {
      if (objects.empty())
        return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);

      SmallVector<llvm::Constant*, 3> fields;

      // FIXME. _PROTOCOL_METHOD_TYPES_ does not have the first two entries.
      // May want to pull this into its own routine for performance; if needed.
      if (!nameBase.equals("_PROTOCOL_METHOD_TYPES_")) {
        // In all of the foo_list_t structs, either:
        //   - there's a 32-bit entry size and a 32-bit count or
        //   - there's no entry size and a uintptr_t count.
        if (!optionalEltSize.isZero()) {
          fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty,
                                                  optionalEltSize.getValue()));
          fields.push_back(llvm::ConstantInt::get(IGM.Int32Ty, objects.size()));
        } else {
          fields.push_back(llvm::ConstantInt::get(IGM.IntPtrTy, objects.size()));
        }
      }

      auto arrayTy =
        llvm::ArrayType::get(objects[0]->getType(), objects.size());
      fields.push_back(llvm::ConstantArray::get(arrayTy, objects));

      return buildGlobalVariable(fields, nameBase);
    }
    
    /// Get the name of the class or protocol to mangle into the ObjC symbol
    /// name.
    StringRef getEntityName(llvm::SmallVectorImpl<char> &buffer) const {
      if (auto theClass = TheEntity.dyn_cast<ClassDecl*>()) {
        return theClass->getObjCRuntimeName(buffer);
      }
      
      if (auto theProtocol = TheEntity.dyn_cast<ProtocolDecl*>()) {
        return theProtocol->getObjCRuntimeName(buffer);
      }
      
      llvm_unreachable("not a class or protocol?!");
    }

    /// Build a private global variable as a structure containing the
    /// given fields.
    llvm::Constant *buildGlobalVariable(ArrayRef<llvm::Constant*> fields,
                                        StringRef nameBase) {
      llvm::SmallString<64> nameBuffer;
      auto init = llvm::ConstantStruct::getAnon(IGM.getLLVMContext(), fields);
      auto var = new llvm::GlobalVariable(IGM.Module, init->getType(),
                                        /*constant*/ true,
                                        llvm::GlobalVariable::PrivateLinkage,
                                        init,
                                        Twine(nameBase) 
                                          + getEntityName(nameBuffer)
                                          + (TheExtension
                                             ? Twine("_$_") + CategoryName.str()
                                             : Twine()));
      var->setAlignment(IGM.getPointerAlignment().getValue());
      var->setSection("__DATA, __objc_const");
      return var;
    }

  public:
    /// Member types don't get any representation.
    /// Maybe this should change for reflection purposes?
    void visitTypeDecl(TypeDecl *type) {}

    /// Pattern-bindings don't require anything special as long as
    /// these initializations are performed in the constructor, not
    /// .cxx_construct.
    void visitPatternBindingDecl(PatternBindingDecl *binding) {}

    /// Subscripts should probably be collected in extended metadata.
    void visitSubscriptDecl(SubscriptDecl *subscript) {
      if (!requiresObjCSubscriptDescriptor(IGM, subscript)) return;
      auto getter_setter = emitObjCSubscriptMethodDescriptors(IGM, subscript);
      if (subscript->getAttrs().isOptional())
        OptInstanceMethods.push_back(getter_setter.first);
      else
        InstanceMethods.push_back(getter_setter.first);

      if (getter_setter.second) {
        if (subscript->getAttrs().isOptional())
          OptInstanceMethods.push_back(getter_setter.second);
        else
          InstanceMethods.push_back(getter_setter.second);
      }
    }
  };
}

/// Emit the private data (RO-data) associated with a class.
llvm::Constant *irgen::emitClassPrivateData(IRGenModule &IGM,
                                            ClassDecl *cls) {
  assert(IGM.ObjCInterop && "emitting RO-data outside of interop mode");
  SILType selfType = getSelfType(cls);
  auto &classTI = IGM.getTypeInfo(selfType).as<ClassTypeInfo>();
  auto &fieldLayout = classTI.getLayout(IGM);
  LayoutClass layout(IGM, ResilienceScope::Universal, cls, selfType);
  ClassDataBuilder builder(IGM, cls, layout, fieldLayout,
                           classTI.getInheritedStoredProperties(IGM).size());

  // First, build the metaclass object.
  builder.buildMetaclassStub();

  // Then build the class RO-data.
  return builder.emitROData(ForClass);
}
  
/// Emit the metadata for an ObjC category.
llvm::Constant *irgen::emitCategoryData(IRGenModule &IGM,
                                        ExtensionDecl *ext) {
  assert(IGM.ObjCInterop && "emitting RO-data outside of interop mode");
  ClassDecl *cls = ext->getDeclaredTypeInContext()
    ->getClassOrBoundGenericClass();
  assert(cls && "generating category metadata for a non-class extension");
  
  ClassDataBuilder builder(IGM, cls, ext);
  
  return builder.emitCategory();
}
  
/// Emit the metadata for an ObjC protocol.
llvm::Constant *irgen::emitObjCProtocolData(IRGenModule &IGM,
                                            ProtocolDecl *proto) {
  assert(proto->isObjC() && "not an objc protocol");
  ClassDataBuilder builder(IGM, proto);
  return builder.emitProtocol();
}

const TypeInfo *TypeConverter::convertClassType(ClassDecl *D) {
  llvm::StructType *ST = IGM.createNominalType(D);
  llvm::PointerType *irType = ST->getPointerTo();
  ReferenceCounting refcount = ::getReferenceCountingForClass(IGM, D);
  return new ClassTypeInfo(irType, IGM.getPointerSize(),
                           IGM.getHeapObjectSpareBits(),
                           IGM.getPointerAlignment(),
                           D, refcount);
}

/// Lazily declare the Swift root-class, SwiftObject.
ClassDecl *IRGenModule::getSwiftRootClass() {
  if (SwiftRootClass) return SwiftRootClass;

  auto name = Context.getIdentifier("SwiftObject");

  // Make a really fake-looking class.
  SwiftRootClass = new (Context) ClassDecl(SourceLoc(), name, SourceLoc(),
                                           MutableArrayRef<TypeLoc>(),
                                           /*generics*/ nullptr,
                                           Context.TheBuiltinModule);
  SwiftRootClass->computeType();
  SwiftRootClass->setIsObjC(true);
  SwiftRootClass->getMutableAttrs().add(ObjCAttr::createNullary(Context, name));
  SwiftRootClass->setImplicit();
  return SwiftRootClass;
}
