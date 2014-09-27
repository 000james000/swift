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

#include "swift/AST/ArchetypeBuilder.h"
#include "swift/Basic/Optional.h"
#include "swift/SIL/AbstractionPattern.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILDeclRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"

namespace swift {
  class ValueDecl;
  class SILBuilder;
  class SILLocation;
  class SILModule;

namespace Lowering {

/// The default convention for handling the callee object on thick
/// callees.
const ParameterConvention DefaultThickCalleeConvention =
  ParameterConvention::Direct_Owned;

/// Given an AST function type, return a type that is identical except
/// for using the given ExtInfo.
CanAnyFunctionType adjustFunctionType(CanAnyFunctionType type,
                                      AnyFunctionType::ExtInfo extInfo);

/// Change the given function type's representation and CC.
inline CanAnyFunctionType adjustFunctionType(CanAnyFunctionType t,
                                          AbstractCC cc,
                                          AnyFunctionType::Representation rep) {
  auto extInfo = t->getExtInfo().withRepresentation(rep).withCallingConv(cc);
  return adjustFunctionType(t, extInfo);
}
/// Change the given function type's representation.
inline CanAnyFunctionType adjustFunctionType(CanAnyFunctionType t,
                                          AnyFunctionType::Representation rep) {
  auto extInfo = t->getExtInfo().withRepresentation(rep);
  return adjustFunctionType(t, extInfo);  
}
  
/// Given a SIL function type, return a type that is identical except
/// for using the given ExtInfo.
CanSILFunctionType adjustFunctionType(CanSILFunctionType type,
                                      SILFunctionType::ExtInfo extInfo,
                                      ParameterConvention calleeConv);
inline CanSILFunctionType adjustFunctionType(CanSILFunctionType type,
                                      SILFunctionType::ExtInfo extInfo) {
  return adjustFunctionType(type, extInfo, type->getCalleeConvention());
}
inline CanSILFunctionType adjustFunctionType(CanSILFunctionType t,
                                         SILFunctionType::Representation rep) {
  if (t->getRepresentation() == rep) return t;
  auto extInfo = t->getExtInfo().withRepresentation(rep);
  
  return adjustFunctionType(t, extInfo,
                    extInfo.hasContext() ? DefaultThickCalleeConvention
                                         : ParameterConvention::Direct_Unowned);
}
  
/// Different ways in which a function can capture context.
enum class CaptureKind {
  /// No context arguments are necessary.
  None,
  /// A local value captured as a mutable box.
  Box,
  // A local value captures as a constant.
  Constant,
  /// A local function captured by value.
  LocalFunction,
  /// A getter-only property.
  Getter,
  /// A settable property.
  GetterSetter
};
  
/// Return the CaptureKind to use when capturing a decl.
CaptureKind getDeclCaptureKind(CaptureInfo::LocalCaptureTy capture);
  
/// Flag used to place context-dependent TypeLowerings in their own arena which
/// can be disposed when a generic context is exited.
enum IsDependent_t : unsigned {
  IsNotDependent = false,
  IsDependent = true
};
  
/// Extended type information used by SIL.
class TypeLowering {
public:
  enum IsTrivial_t : bool { IsNotTrivial, IsTrivial };
  enum IsAddressOnly_t : bool { IsNotAddressOnly, IsAddressOnly };

private:
  /// The SIL type of values with this Swift type.
  SILType LoweredType;

  enum : unsigned {
    IsTrivialFlag     = 0x1,
    IsAddressOnlyFlag = 0x2,
  };
  unsigned Flags;

protected:  
  TypeLowering(SILType type, IsTrivial_t isTrivial,
               IsAddressOnly_t isAddressOnly)
    : LoweredType(type), Flags((isTrivial ? IsTrivialFlag : 0U) | 
                               (isAddressOnly ? IsAddressOnlyFlag : 0U)) {}

public:
  TypeLowering(const TypeLowering &) = delete;
  TypeLowering &operator=(const TypeLowering &) = delete;

  virtual ~TypeLowering() {}

  /// \brief Are r-values of this type passed as arguments indirectly?
  bool isPassedIndirectly() const {
    return isAddressOnly();
  }

  /// \brief Are r-values of this type returned indirectly?
  bool isReturnedIndirectly() const {
    return isAddressOnly();
  }

  /// isAddressOnly - Returns true if the type is an address-only type. A type
  /// is address-only if it is a resilient value type, or if it is a fragile
  /// value type with a resilient member. In either case, the full layout of
  /// values of the type is unavailable to the compiler.
  bool isAddressOnly() const {
    return Flags & IsAddressOnlyFlag;
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
    return Flags & IsTrivialFlag;
  }

  /// getLoweredType - Get the type used to represent values of the Swift type
  /// in SIL.
  SILType getLoweredType() const {
    return LoweredType;
  }

  /// Return the semantic type.
  ///
  /// The semantic type is what a type pretends to be during
  /// type-checking: that is, the type that getTypeOfRValue would
  /// return on a variable of this type.
  SILType getSemanticType() const {
    // If you change this, change getSemanticTypeLowering() too.
    auto storageType = getLoweredType().getSwiftRValueType();
    if (auto refType = dyn_cast<ReferenceStorageType>(storageType))
      return SILType::getPrimitiveType(refType.getReferentType(),
                                       SILValueCategory::Object);
    return getLoweredType();
  }
  
  /// Return the lowering for the semantic type.
  inline const TypeLowering &getSemanticTypeLowering(TypeConverter &TC) const;

  /// Produce a exact copy of the value in the given address as a
  /// scalar.  The caller is responsible for destroying this value,
  /// e.g. by releasing it.
  ///
  /// This type must be loadable.
  virtual SILValue emitLoadOfCopy(SILBuilder &B,
                                  SILLocation loc,
                                  SILValue addr,
                                  IsTake_t isTake) const = 0;

  /// Store an exact copy of the given value in the destination
  /// address, taking ownership of it.
  ///
  /// This type must be loadable.
  ///
  /// Note that, for an assignment, this produces lowered code: that
  /// is, for non-POD types, an explicit load and release.  Therefore,
  /// it is generally not correct to call this during SIL-gen.
  virtual void emitStoreOfCopy(SILBuilder &B,
                               SILLocation loc,
                               SILValue value,
                               SILValue addr,
                               IsInitialization_t isInit) const = 0;

  /// Put an exact copy of the value in the source address in the
  /// destination address.
  virtual void emitCopyInto(SILBuilder &B,
                            SILLocation loc,
                            SILValue src,
                            SILValue dest,
                            IsTake_t isTake,
                            IsInitialization_t isInit) const = 0;

  /// Given an address, emit operations to destroy it.
  ///
  /// This produces canonicalized SIL.
  virtual void emitDestroyAddress(SILBuilder &B, SILLocation loc,
                                  SILValue value) const = 0;

  /// Given a +1 r-value which are are claiming ownership of, destroy it.
  ///
  /// Note that an r-value might be an address.
  virtual void emitDestroyRValue(SILBuilder &B, SILLocation loc,
                                 SILValue value) const = 0;

  enum class LoweringStyle {
    Shallow,
    Deep,
    DeepNoEnum
  };

  /// Emit a lowered 'release_value' operation.
  ///
  /// This type must be loadable.
  virtual void emitLoweredReleaseValue(SILBuilder &B, SILLocation loc,
                                       SILValue value,
                                       LoweringStyle loweringStyle) const = 0;

  void emitLoweredDestroyChildValue(SILBuilder &B, SILLocation loc,
                                    SILValue value,
                                    LoweringStyle loweringStyle) const {
    if (loweringStyle != LoweringStyle::Shallow)
      return emitLoweredReleaseValue(B, loc, value, loweringStyle);
    return emitReleaseValue(B, loc, value);
  }

  /// Emit a lowered 'release_value' operation.
  ///
  /// This type must be loadable.
  void emitLoweredReleaseValueShallow(SILBuilder &B, SILLocation loc,
                                      SILValue value) const {
    emitLoweredReleaseValue(B, loc, value, LoweringStyle::Shallow);
  }

  /// Emit a lowered 'release_value' operation.
  ///
  /// This type must be loadable.
  void emitLoweredReleaseValueDeep(SILBuilder &B, SILLocation loc,
                                   SILValue value) const {
    emitLoweredReleaseValue(B, loc, value, LoweringStyle::Deep);
  }

  /// Emit a lowered 'release_value' operation.
  ///
  /// This type must be loadable.
  void emitLoweredReleaseValueDeepNoEnum(SILBuilder &B, SILLocation loc,
                                         SILValue value) const {
    emitLoweredReleaseValue(B, loc, value, LoweringStyle::DeepNoEnum);
  }

  /// Given a primitively loaded value of this type (which must be
  /// loadable), -1 it.
  ///
  /// This should be used when dropping a value which has been copied
  /// from place to place with exactly the same semantics.  For
  /// example, it performs an unowned_release on a value of [unknown]
  /// type.  It is therefore not necessarily the right thing to do on
  /// a semantic load.
  virtual void emitReleaseValue(SILBuilder &B, SILLocation loc,
                                SILValue value) const = 0;

  /// Emit a lowered 'retain_value' operation.
  ///
  /// This type must be loadable.
  virtual void emitLoweredRetainValue(SILBuilder &B, SILLocation loc,
                                    SILValue value,
                                    LoweringStyle style) const = 0;

  /// Emit a lowered 'retain_value' operation.
  ///
  /// This type must be loadable.
  void emitLoweredRetainValueShallow(SILBuilder &B, SILLocation loc,
                                   SILValue value) const {
    emitLoweredRetainValue(B, loc, value, LoweringStyle::Shallow);
  }

  /// Emit a lowered 'retain_value' operation.
  ///
  /// This type must be loadable.
  void emitLoweredRetainValueDeep(SILBuilder &B, SILLocation loc,
                                SILValue value) const {
    emitLoweredRetainValue(B, loc, value, LoweringStyle::Deep);
  }

  /// Emit a lowered 'retain_value' operation.
  ///
  /// This type must be loadable.
  void emitLoweredRetainValueDeepNoEnum(SILBuilder &B, SILLocation loc,
                                      SILValue value) const {
    emitLoweredRetainValue(B, loc, value, LoweringStyle::DeepNoEnum);
  }

  /// Given a primitively loaded value of this type (which must be
  /// loadable), +1 it.
  ///
  /// This should be used for duplicating a value from place to place
  /// with exactly the same semantics.  For example, it performs an
  /// unowned_retain on a value of [unknown] type.  It is therefore
  /// not necessarily the right thing to do on a semantic load.
  virtual void emitRetainValue(SILBuilder &B, SILLocation loc,
                             SILValue value) const = 0;

  void emitLoweredCopyChildValue(SILBuilder &B, SILLocation loc,
                                 SILValue value,
                                 LoweringStyle style) const {
    if (style != LoweringStyle::Shallow) {
      emitLoweredRetainValue(B, loc, value, style);
    } else {
      emitRetainValue(B, loc, value);
    }
  }

  /// Allocate a new TypeLowering using the TypeConverter's allocator.
  void *operator new(size_t size, TypeConverter &tc,
                     IsDependent_t dependent);
  void *operator new[](size_t size, TypeConverter &tc,
                       IsDependent_t dependent);

  // Forbid 'new FooTypeLowering' and try to forbid 'delete tl'.
  // The latter is made challenging because the existence of the
  // virtual destructor requires an accessible 'operator delete'.
  void *operator new(size_t) = delete;

protected:
  void operator delete(void*) {}
};

/// Type and lowering information about a constant function.
SIL_FUNCTION_TYPE_IGNORE_DEPRECATED_BEGIN
struct SILConstantInfo {
  /// The formal type of the constant, still curried.  For a normal
  /// function, this is just its declared type; for a getter or
  /// setter, computing this can be more involved.
  CanAnyFunctionType FormalType SIL_FUNCTION_TYPE_DEPRECATED;
  CanAnyFunctionType FormalInterfaceType;

  /// The uncurried and bridged type of the constant.
  CanAnyFunctionType LoweredType SIL_FUNCTION_TYPE_DEPRECATED;
  CanAnyFunctionType LoweredInterfaceType;

  /// The SIL function type of the constant.
  CanSILFunctionType SILFnType;
  
  /// The context generic parameters used by the constant.
  /// This will be the innermost generic parameter list that applies to the
  /// constant, which may be the generic parameter list of an enclosing context.
  GenericParamList *ContextGenericParams;
  
  /// The generic parameter list of the function.
  /// If the function does not have any generic parameters of its own, this
  /// will be null.
  GenericParamList *InnerGenericParams;

  SILType getSILType() const {
    return SILType::getPrimitiveObjectType(SILFnType);
  }

  friend bool operator==(SILConstantInfo lhs, SILConstantInfo rhs) {
    return lhs.FormalType == rhs.FormalType &&
           lhs.FormalInterfaceType == rhs.FormalInterfaceType &&
           lhs.LoweredType == rhs.LoweredType &&
           lhs.LoweredInterfaceType == rhs.LoweredInterfaceType &&
           lhs.SILFnType == rhs.SILFnType &&
           lhs.ContextGenericParams == rhs.ContextGenericParams &&
           lhs.InnerGenericParams == rhs.InnerGenericParams;
  }
  friend bool operator!=(SILConstantInfo lhs, SILConstantInfo rhs) {
    return !(lhs == rhs);
  }
};
SIL_FUNCTION_TYPE_IGNORE_DEPRECATED_END
  
/// TypeConverter - helper class for creating and managing TypeLowerings.
class TypeConverter {
  friend class TypeLowering;

  llvm::BumpPtrAllocator IndependentBPA;
  /// BumpPtrAllocator for types dependent on contextual generic parameters,
  /// which is reset when the generic context is popped.
  llvm::BumpPtrAllocator DependentBPA;

  enum : unsigned {
    /// There is a unique entry with this uncurry level in the
    /// type-lowering map for every TLI we create.  The map has the
    /// responsibility to call the destructor for these entries.
    UniqueLoweringEntry = ~0U
  };

  struct TypeKey {
    /// An unsubstituted version of a type, dictating its abstraction patterns.
    CanType OrigType;

    /// The substituted version of the type, dictating the types that
    /// should be used in the lowered type.
    CanType SubstType;

    /// The uncurrying level of the type.
    unsigned UncurryLevel;
    
    IsDependent_t isDependent() const {
      if (SubstType->isDependentType())
        return IsDependent;
      if (OrigType->isDependentType())
        return IsDependent;
      return IsNotDependent;
    }
    
    friend bool operator==(const TypeKey &lhs, const TypeKey &rhs) {
      return lhs.OrigType == rhs.OrigType
          && lhs.SubstType == rhs.SubstType
          && lhs.UncurryLevel == rhs.UncurryLevel;
    }
    friend bool operator!=(const TypeKey &lhs, const TypeKey &rhs) {
      return !(lhs == rhs);
    }
  };
  friend struct llvm::DenseMapInfo<TypeKey>;
  
  TypeKey getTypeKey(AbstractionPattern origTy, CanType substTy,
                     unsigned uncurryLevel) {
    return {origTy.getAsType(), substTy, uncurryLevel};
  }
  
  /// Find an cached TypeLowering by TypeKey, or return null if one doesn't
  /// exist.
  const TypeLowering *find(TypeKey k);
  /// Insert a mapping into the cache.
  void insert(TypeKey k, const TypeLowering *tl);
  
  /// Mapping for types independent on contextual generic parameters, which is
  /// cleared when the generic context is popped.
  llvm::DenseMap<TypeKey, const TypeLowering *> IndependentTypes;
  /// Mapping for types dependent on contextual generic parameters, which is
  /// cleared when the generic context is popped.
  llvm::DenseMap<TypeKey, const TypeLowering *> DependentTypes;
  
  llvm::DenseMap<SILDeclRef, SILConstantInfo> ConstantTypes;
  
  /// The set of recursive types we've already diagnosed.
  llvm::DenseSet<NominalTypeDecl *> RecursiveNominalTypes;
  
  /// ArchetypeBuilder used for lowering types in generic function contexts.
  Optional<ArchetypeBuilder> GenericArchetypes;
  
  CanAnyFunctionType makeConstantType(SILDeclRef constant, bool addCaptures);
  CanAnyFunctionType makeConstantInterfaceType(SILDeclRef constant,
                                               bool addCaptures);
  
  /// Get the context parameters for a constant. Returns a pair of the innermost
  /// generic parameter list and the generic param list that directly applies
  /// to the constant, if any.
  std::pair<GenericParamList *, GenericParamList*>
  getConstantContextGenericParams(SILDeclRef constant, bool addCaptures);
  
  // Types converted during foreign bridging.
#define BRIDGING_KNOWN_TYPE(BridgedModule,BridgedType) \
  Optional<CanType> BridgedType##Ty;
#include "swift/SIL/BridgedTypes.def"

  CanType MostGeneralArchetype;
  
  const TypeLowering &getTypeLoweringForLoweredType(TypeKey key);
  const TypeLowering &getTypeLoweringForUncachedLoweredType(TypeKey key);
  const TypeLowering &getTypeLoweringForLoweredFunctionType(TypeKey key);
  const TypeLowering &getTypeLoweringForUncachedLoweredFunctionType(TypeKey key);

public:
  SILModule &M;
  ASTContext &Context;

  TypeConverter(SILModule &m);
  ~TypeConverter();
  TypeConverter(TypeConverter const &) = delete;
  TypeConverter &operator=(TypeConverter const &) = delete;

  /// Return a most-general-possible abstraction pattern.
  AbstractionPattern getMostGeneralAbstraction();

  /// Get the calling convention used by witnesses of a protocol.
  static AbstractCC getProtocolWitnessCC(ProtocolDecl *P) {
    // ObjC protocols use the objc method
    // convention.
    if (P->isObjC())
      return AbstractCC::ObjCMethod;
    
    // Native protocols use the witness calling convention.
    return AbstractCC::WitnessMethod;
  }
  
  /// Get the calling convention used to call a declaration.
  AbstractCC getAbstractCC(SILDeclRef c);
  
  /// True if a protocol uses witness tables for dynamic dispatch.
  static bool protocolRequiresWitnessTable(ProtocolDecl *P) {
    return !P->isObjC();
  }
  
  /// True if a type is passed indirectly at +0 when used as the "self"
  /// parameter of its own methods.
  ///
  /// TODO: We want this always to hold.
  static bool isIndirectPlusZeroSelfParameter(Type T) {
    // Calls through opaque protocols can be done with +0 rvalues.  This allows
    // us to avoid materializing copies of existentials.
    return !T->hasReferenceSemantics()
        && (T->isExistentialType() || T->is<ArchetypeType>());
  }
  
  static bool isIndirectPlusZeroSelfParameter(SILType T) {
    return isIndirectPlusZeroSelfParameter(T.getSwiftRValueType());
  }
  
  /// Is the given declaration resilient from the current context?
  bool isResilient(Decl *D) {
    // FIXME
    return false;
  }

  /// Is the given declaration resilient in any theoretical component,
  /// possibly one compiled in the past?  This can lead to some
  /// sub-optimal decisions sometimes, but it has the merit of being
  /// guaranteed stable.
  bool isAnywhereResilient(Decl *D) {
    // FIXME
    return false;
  }

  /// Lowers a Swift type to a SILType, and returns the SIL TypeLowering
  /// for that type.
  const TypeLowering &getTypeLowering(Type t, unsigned uncurryLevel = 0) {
    return getTypeLowering(AbstractionPattern(t), t, uncurryLevel);
  }

  /// Lowers a Swift type to a SILType according to the abstraction
  /// patterns of the given original type.
  const TypeLowering &getTypeLowering(AbstractionPattern origType,
                                      Type substType,
                                      unsigned uncurryLevel = 0);

  /// Returns the SIL TypeLowering for an already lowered SILType. If the
  /// SILType is an address, returns the TypeLowering for the pointed-to
  /// type.
  const TypeLowering &getTypeLowering(SILType t);
  
  // Returns the lowered SIL type for a Swift type.
  SILType getLoweredType(Type t, unsigned uncurryLevel = 0) {
    return getTypeLowering(t, uncurryLevel).getLoweredType();
  }

  // Returns the lowered SIL type for a Swift type.
  SILType getLoweredType(AbstractionPattern origType, Type substType,
                         unsigned uncurryLevel = 0) {
    return getTypeLowering(origType, substType, uncurryLevel).getLoweredType();
  }

  SILType getLoweredLoadableType(Type t, unsigned uncurryLevel = 0) {
    const TypeLowering &ti = getTypeLowering(t, uncurryLevel);
    assert(ti.isLoadable() && "unexpected address-only type");
    return ti.getLoweredType();
  }

  /// Return the SILFunctionType for a native function value of the
  /// given type.
  CanSILFunctionType getSILFunctionType(AbstractionPattern origType,
                                        CanAnyFunctionType substType,
                                        unsigned uncurryLevel);

  /// Returns the formal type, lowered AST type, and SILFunctionType
  /// for a constant reference.
  SILConstantInfo getConstantInfo(SILDeclRef constant);

  /// Returns the type of a local function without any additional captures.
  CanAnyFunctionType getConstantFormalTypeWithoutCaptures(SILDeclRef constant);
  
  /// Returns the SIL type of a constant reference.
  SILType getConstantType(SILDeclRef constant) {
    return getConstantInfo(constant).getSILType();
  }

  /// Returns the formal AST type of a constant reference.
  /// Parameters remain uncurried and unbridged.
  CanAnyFunctionType getConstantFormalType(SILDeclRef constant)
  SIL_FUNCTION_TYPE_DEPRECATED {
    return getConstantInfo(constant).FormalType;
  }

  /// Returns the lowered AST type of a constant reference.
  /// Parameters have been uncurried and bridged.
  CanAnyFunctionType getConstantLoweredType(SILDeclRef constant)
  SIL_FUNCTION_TYPE_DEPRECATED {
    return getConstantInfo(constant).LoweredType;
  }

  /// Returns the SILFunctionType for the given declaration.
  CanSILFunctionType getConstantFunctionType(SILDeclRef constant) {
    return getConstantInfo(constant).SILFnType;
  }

  /// Returns the SILFunctionType for the given declaration, applying
  /// substitutions to match the given type.
  ///
  ///
  /// \param substFormalType - a valid substitution of the
  ///   still-curried type of the function.
  CanSILFunctionType getConstantFunctionType(SILDeclRef constant,
                                   CanAnyFunctionType substFormalType,
                                   CanAnyFunctionType substFormalInterfaceType,
                                   AnyFunctionType::Representation rep);

  /// Substitute the given function type so that it implements the
  /// given substituted type.
  CanSILFunctionType substFunctionType(CanSILFunctionType origFnType,
                                 CanAnyFunctionType origLoweredType,
                                 CanAnyFunctionType substLoweredType,
                                 CanAnyFunctionType substLoweredInterfaceType);
  
  /// Get the empty tuple type as a SILType.
  SILType getEmptyTupleType() {
    return getLoweredType(TupleType::getEmpty(Context));
  }
  
  /// Get a function type curried with its capture context.
  CanAnyFunctionType getFunctionTypeWithCaptures(CanAnyFunctionType funcType,
                                 ArrayRef<CaptureInfo::LocalCaptureTy> captures,
                                                 DeclContext *parentContext);
  CanAnyFunctionType getFunctionInterfaceTypeWithCaptures(
                                                 CanAnyFunctionType funcType,
                                 ArrayRef<CaptureInfo::LocalCaptureTy> captures,
                                                 DeclContext *parentContext);
  
  /// Map an AST-level type to the corresponding foreign representation type we
  /// implicitly convert to for a given calling convention.
  Type getLoweredBridgedType(Type t, AbstractCC cc);

  /// Convert a nested function type into an uncurried AST representation.
  CanAnyFunctionType getLoweredASTFunctionType(CanAnyFunctionType t,
                                               unsigned uncurryLevel) {
    return getLoweredASTFunctionType(t, uncurryLevel, t->getExtInfo());
  }

  /// Convert a nested function type into an uncurried AST representation.
  CanAnyFunctionType getLoweredASTFunctionType(CanAnyFunctionType t,
                                               unsigned uncurryLevel,
                                               AnyFunctionType::ExtInfo info);

  /// Given a referenced value and the substituted formal type of a
  /// resulting l-value expression, produce the substituted formal
  /// type of the storage of the value.
  ///
  /// \return - always an address type
  SILType getSubstitutedStorageType(ValueDecl *value, Type lvalueType);

  /// Retrieve the set of archetypes open in the given context.
  GenericParamList *getEffectiveGenericParamsForContext(DeclContext *dc);

  /// Retrieve the set of generic parameters for the given context.
  CanGenericSignature getEffectiveGenericSignatureForContext(DeclContext *dc);

  /// Push a generic function context. See GenericContextScope for an RAII
  /// interface to this function.
  ///
  /// Types containing generic parameter references must be lowered in a generic
  /// context. There can be at most one level of generic context active at any
  /// point in time.
  void pushGenericContext(GenericSignature *sig);
  
  /// Pop a generic function context. See GenericContextScope for an RAII
  /// interface to this function. There must be an active generic context.
  void popGenericContext(GenericSignature *sig);
  
  /// Return the archetype builder for the current generic context. Fails if no
  /// generic context has been pushed.
  ArchetypeBuilder &getArchetypes() {
    return *GenericArchetypes;
  }
  
  // Map a type involving context archetypes out of its context into a
  // dependent type.
  CanType getInterfaceTypeOutOfContext(CanType contextTy,
                                    DeclContext *context) const;
  
  // Map a type involving context archetypes out of its context into a
  // dependent type.
  CanType getInterfaceTypeOutOfContext(CanType contextTy,
                                    GenericParamList *contextParams) const;

  /// Known types for bridging.
#define BRIDGING_KNOWN_TYPE(BridgedModule,BridgedType) \
  CanType get##BridgedType##Type();
#include "swift/SIL/BridgedTypes.def"

  /// Get the linkage for a protocol conformance's witness table.
  static SILLinkage getLinkageForProtocolConformance(
                                             const NormalProtocolConformance *C,
                                             ForDefinition_t definition);
  
private:
  Type getLoweredCBridgedType(Type t);
};

inline const TypeLowering &
TypeLowering::getSemanticTypeLowering(TypeConverter &TC) const {
  // If you change this, change getSemanticType() too.
  auto storageType = getLoweredType().getSwiftRValueType();
  if (auto refType = dyn_cast<ReferenceStorageType>(storageType))
    return TC.getTypeLowering(refType.getReferentType());
  return *this;
}

/// RAII interface to push a generic context.
class GenericContextScope {
  TypeConverter &TC;
  GenericSignature *Sig;
public:
  GenericContextScope(TypeConverter &TC,
                      GenericSignature *Sig)
    : TC(TC), Sig(Sig)
  {
    TC.pushGenericContext(Sig);
  }
  
  ~GenericContextScope() {
    TC.popGenericContext(Sig);
  }
  
private:
  GenericContextScope(const GenericContextScope&) = delete;
  GenericContextScope &operator=(const GenericContextScope&) = delete;
};
  
} // namespace Lowering
} // namespace swift

namespace llvm {
  template<> struct DenseMapInfo<swift::Lowering::TypeConverter::TypeKey> {
    typedef swift::Lowering::TypeConverter::TypeKey TypeKey;
    static TypeKey getEmptyKey() {
      return {DenseMapInfo<swift::CanType>::getEmptyKey(),
              swift::CanType(), 0};
    }
    static TypeKey getTombstoneKey() {
      return {DenseMapInfo<swift::CanType>::getTombstoneKey(),
              swift::CanType(), 0};
    }
    static unsigned getHashValue(TypeKey val) {
      auto hashOrig = DenseMapInfo<swift::CanType>::getHashValue(val.OrigType);
      auto hashSubst =
        DenseMapInfo<swift::CanType>::getHashValue(val.SubstType);
      return hash_combine(hashOrig, hashSubst, val.UncurryLevel);
    }
    static bool isEqual(TypeKey LHS, TypeKey RHS) {
      return LHS == RHS;
    }
  };
}

#endif
