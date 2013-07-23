//===--- TypeLowering.h - Convert Swift Types to SILTypes -------*- C++ -*-===//
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

#ifndef SIL_TypeLowering_h
#define SIL_TypeLowering_h

#include "swift/Basic/Optional.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILConstant.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"

namespace swift {
  class ValueDecl;
  class SILModule;
namespace Lowering {

/// Given a function type or polymorphic function type, returns the same type
/// with [thin] and calling convention attributes added.
/// FIXME: The thinness of func decls should be checked by the Swift
/// typechecker.
Type getThinFunctionType(Type t, AbstractCC cc);
Type getThinFunctionType(Type t);

/// Given a function type or polymorphic function type, returns the same type
/// with the [thin] attribute removed and a calling convention attribute added.
/// FIXME: The thinness of func decls should be checked by the Swift
/// typechecker.
Type getThickFunctionType(Type t, AbstractCC cc);
Type getThickFunctionType(Type t);

/// CaptureKind - Different ways in which a function can capture context.
enum class CaptureKind {
  /// A local value captured as a mutable box.
  Box,
  /// A local value captured by value.
  Constant,
  /// A byref argument captured by address.
  Byref,
  /// A getter-only property.
  Getter,
  /// A settable property.
  GetterSetter
};
  
/// getDeclCaptureKind - Return the CaptureKind to use when capturing a decl.
CaptureKind getDeclCaptureKind(ValueDecl *capture);
  
/// ReferenceTypeElement - a path to a reference type element within a loadable
/// aggregate type at an arbitrary depth.
struct ReferenceTypePath {
  /// A component of the reference type path, comprising the index of an
  /// element and its type.
  class Component {
  public:
    enum class Kind : unsigned { StructField, TupleElement };
  
  private:
    llvm::PointerIntPair<TypeBase*, 1, Kind> typeAndKind;
    union {
      VarDecl *structField;
      unsigned tupleElement;
    };

  public:
    Component() = default;
    
    Component(CanType fieldType, VarDecl *structField)
      : typeAndKind(fieldType.getPointer(), Kind::StructField),
        structField(structField)
    {}
    
    Component(CanType eltType, unsigned tupleElement)
      : typeAndKind(eltType.getPointer(), Kind::TupleElement),
        tupleElement(tupleElement)
    {}
    
    static Component forStructField(CanType fieldType, VarDecl *structField) {
      return {fieldType, structField};
    }
    static Component forTupleElement(CanType eltType, unsigned tupleElement) {
      return {eltType, tupleElement};
    }
    
    CanType getType() const {
      return CanType(typeAndKind.getPointer());
    }
    
    void setType(CanType t) {
      typeAndKind.setPointer(t.getPointer());
    }
    
    Kind getKind() const { return typeAndKind.getInt(); }
    
    VarDecl *getStructField() const {
      assert(getKind() == Kind::StructField && "not a struct field");
      return structField;
    }
    unsigned getTupleElement() const {
      assert(getKind() == Kind::TupleElement && "not a tuple element");
      return tupleElement;
    }
  };
  
  
  /// path - The index chain leading to the reference type element. For
  /// example, {0} refers to element zero, {0, 1} refers to element
  /// one of element zero, etc. An empty index list {} refers to the value
  /// itself, for reference types.
  llvm::SmallVector<Component, 4> path;
};

/// TypeLoweringInfo - Extended type information used by SILGen.
class TypeLoweringInfo {
  friend class TypeConverter;
  friend class LoadableTypeLoweringInfoVisitor;

  /// referenceTypeElements - For a loadable type, this contains element index
  /// paths to every element inside the aggregate that must be retained and
  /// released.
  llvm::SmallVector<ReferenceTypePath, 4> referenceTypeElements;
  
  /// loweredTypeAndIsAddressOnly - The SIL type of values with this Swift type,
  /// and whether it is an address-only type.
  llvm::PointerIntPair<SILType, 1, bool> LoweredTypeAndIsAddressOnly;
  
  TypeLoweringInfo() : LoweredTypeAndIsAddressOnly(SILType(), false) {}

public:
  TypeLoweringInfo(const TypeLoweringInfo &) = delete;
  TypeLoweringInfo &operator=(const TypeLoweringInfo &) = delete;
  TypeLoweringInfo(TypeLoweringInfo &&) = default;
  TypeLoweringInfo &operator=(TypeLoweringInfo &&) = default;
  
  /// isAddressOnly - Returns true if the type is an address-only type. A type
  /// is address-only if it is a resilient value type, or if it is a fragile
  /// value type with a resilient member. In either case, the full layout of
  /// values of the type is unavailable to the compiler.
  bool isAddressOnly() const {
    return LoweredTypeAndIsAddressOnly.getInt();
  }
  /// isLoadable - Returns true if the type is loadable, in other words, its
  /// full layout is available to the compiler. This is the inverse of
  /// isAddressOnly.
  bool isLoadable() const {
    return !isAddressOnly();
  }
  
  /// isTrivial - Returns true if the type is trivial, meaning it is a loadable
  /// value type with no reference type members that require releasing.
  bool isTrivial() const {
    return isLoadable() && referenceTypeElements.empty();
  }
  
  /// getReferenceTypeElements - For a nontrivial loadable value type, returns
  /// an array of ReferenceTypePaths addressing the reference type elements.
  llvm::ArrayRef<ReferenceTypePath> getReferenceTypeElements() const {
    return referenceTypeElements;
  }
  
  /// getLoweredType - Get the type used to represent values of the Swift type
  /// in SIL.
  SILType getLoweredType() const {
    return LoweredTypeAndIsAddressOnly.getPointer();
  }
  
  /// Allocate a new TypeLoweringInfo using the TypeConverter's allocator.
  void *operator new(size_t size, TypeConverter &tc);
  
  void *operator new(size_t) = delete;
  void operator delete(void*) = delete;
};
  
/// Argument order of uncurried functions.
enum class UncurryDirection {
  LeftToRight,
  RightToLeft
};

/// TypeConverter - helper class for creating and managing TypeLoweringInfos.
class TypeConverter {
  friend class TypeLoweringInfo;

  llvm::BumpPtrAllocator TypeLoweringInfoBPA;

  enum : unsigned {
    /// There is a unique entry with this uncurry level in the
    /// type-lowering map for every TLI we create.  The map has the
    /// responsibility to call the destructor for these entries.
    UniqueLoweringEntry = ~0U
  };
  
  using TypeKey = std::pair<TypeBase *, unsigned>;
  TypeKey getTypeKey(CanType t, unsigned uncurryLevel) {
    return {t.getPointer(), uncurryLevel};
  }
  
  llvm::DenseMap<TypeKey, const TypeLoweringInfo *> Types;
  llvm::DenseMap<SILConstant, SILType> constantTypes;
  
  Type makeConstantType(SILConstant constant);
  
  // Types converted during foreign bridging.
#define BRIDGE_TYPE(BridgedModule,BridgedType, NativeModule,NativeType) \
  Optional<CanType> BridgedType##Ty; \
  Optional<CanType> NativeType##Ty;
#include "swift/SIL/BridgedTypes.def"

  const TypeLoweringInfo &getTypeLoweringInfoForLoweredType(CanType type);
  const TypeLoweringInfo &getTypeLoweringInfoForUncachedLoweredType(CanType type);
  
public:
  SILModule &M;
  ASTContext &Context;

  TypeConverter(SILModule &m);
  ~TypeConverter();
  TypeConverter(TypeConverter const &) = delete;
  TypeConverter &operator=(TypeConverter const &) = delete;

  /// Lowers a Swift type to a SILType, and returns the SIL TypeLoweringInfo
  /// for that type.
  const TypeLoweringInfo &getTypeLoweringInfo(Type t,unsigned uncurryLevel = 0);
  
  /// Returns the SIL TypeLoweringInfo for an already lowered SILType. If the
  /// SILType is an address, returns the TypeLoweringInfo for the pointed-to
  /// type.
  const TypeLoweringInfo &getTypeLoweringInfo(SILType t);
  
  // Returns the lowered SIL type for a Swift type.
  SILType getLoweredType(Type t, unsigned uncurryLevel = 0) {
    return getTypeLoweringInfo(t, uncurryLevel).getLoweredType();
  }
  
  /// Returns the SIL type of a constant reference.
  SILType getConstantType(SILConstant constant);
  
  /// Get the empty tuple type as a SILType.
  SILType getEmptyTupleType() {
    return getLoweredType(TupleType::getEmpty(Context));
  }
  
  /// Get a function type curried with its capture context.
  Type getFunctionTypeWithCaptures(AnyFunctionType *funcType,
                                   ArrayRef<ValueDecl*> captures,
                                   DeclContext *parentContext);
  
  /// Returns the type of the "this" parameter to methods of a type.
  Type getMethodThisType(Type thisType) const;
  
  /// Returns the type of a property accessor, () -> T for a getter,
  /// or (value:T) -> () for a setter. 'kind' must be one of the Kind constants
  /// from SILConstant, SILConstant::Getter or SILConstant::Setter.
  Type getPropertyType(SILConstant::Kind kind, Type propType) const;
  
  /// Returns the type of a subscript property accessor, Index -> () -> T
  /// for a getter, or Index -> (value:T) -> () for a setter.
  /// 'kind' must be one of the Kind constants
  /// from SILConstant, SILConstant::Getter or SILConstant::Setter.
  Type getSubscriptPropertyType(SILConstant::Kind kind,
                                Type indexType,
                                Type elementType) const;

  /// Get the type of a method of function type M for a type:
  ///   This -> M for a concrete This,
  ///   <T,U,...> This -> M for an unbound generic This,
  ///   or the type M of the function itself if the context type is null.
  Type getMethodTypeInContext(Type /*nullable*/ contextType,
                              Type methodType,
                              GenericParamList *genericParams = nullptr) const;
  
  /// Convert a nested function type into an uncurried representation.
  CanAnyFunctionType getUncurriedFunctionType(CanAnyFunctionType t,
                                              unsigned uncurryLevel);
  
  /// Get the uncurried argument order for a calling convention.
  static UncurryDirection getUncurryDirection(AbstractCC cc);
  
  /// Map an AST-level type to the corresponding foreign representation type we
  /// implicitly convert to for a given calling convention.
  Type getLoweredBridgedType(Type t, AbstractCC cc);
  
  /// Known types for bridging.
#define BRIDGE_TYPE(BridgedModule,BridgedType, NativeModule,NativeType) \
  CanType get##BridgedType##Type(); \
  CanType get##NativeType##Type();
#include "swift/SIL/BridgedTypes.def"
};
  
} // namespace Lowering
} // namespace swift

#endif
