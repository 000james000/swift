//===--- Types.h - Swift Language Type ASTs ---------------------*- C++ -*-===//
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
// This file defines the TypeBase class and subclasses.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TYPES_H
#define SWIFT_TYPES_H

#include "swift/AST/DeclContext.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/Identifier.h"
#include "swift/Basic/Optional.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ErrorHandling.h"

namespace llvm {
  struct fltSemantics;
}
namespace swift {
  enum class AllocationArena;
  class ArchetypeType;
  class ASTContext;
  class ClassDecl;
  class ExprHandle;
  class GenericParamList;
  class Identifier;
  class TypeAliasDecl;
  class TypeDecl;
  class NominalTypeDecl;
  class OneOfDecl;
  class OneOfElementDecl;
  class StructDecl;
  class ProtocolDecl;
  class TypeVariableType;
  class ValueDecl;
  class Module;
  class ProtocolConformance;
  class Substitution;

  enum class TypeKind {
#define TYPE(id, parent) id,
#define TYPE_RANGE(Id, FirstId, LastId) \
  First_##Id##Type = FirstId, Last_##Id##Type = LastId,
#include "swift/AST/TypeNodes.def"
  };
  

/// TypeBase - Base class for all types in Swift.
class alignas(8) TypeBase {
  // alignas(8) because we need three tag bits on Type.
  
  friend class ASTContext;
  TypeBase(const TypeBase&) = delete;
  void operator=(const TypeBase&) = delete;
  
  /// CanonicalType - This field is always set to the ASTContext for canonical
  /// types, and is otherwise lazily populated by ASTContext when the canonical
  /// form of a non-canonical type is requested.
  llvm::PointerUnion<TypeBase *, ASTContext*> CanonicalType;

  /// Kind - The discriminator that indicates what subclass of type this is.
  const TypeKind Kind;

  struct TypeBaseBits {
    /// Unresolved - Whether this type is unresolved.
    unsigned Unresolved : 1;

    /// \brief Whether this type has a type variable somewhere in it.
    unsigned HasTypeVariable : 1;

    /// \brief Whether this type has been validated.
    ///
    /// 0 -> not validated
    /// 1 -> invalid
    /// 2 -> valid
    unsigned Validated : 2;
  };
  static const unsigned NumTypeBaseBits = 4;
  
  union {
    TypeBaseBits TypeBase;
  } TypeBits;
  
protected:
  TypeBase(TypeKind kind, ASTContext *CanTypeCtx, bool Unresolved,
           bool HasTypeVariable)
    : CanonicalType((TypeBase*)nullptr), Kind(kind) {
    // If this type is canonical, switch the CanonicalType union to ASTContext.
    if (CanTypeCtx)
      CanonicalType = CanTypeCtx;
    
    setUnresolved(Unresolved);
    setHasTypeVariable(HasTypeVariable);
    TypeBits.TypeBase.Validated = 0;
  }

  /// \brief Mark this type as unresolved.
  void setUnresolved(bool D = true) { TypeBits.TypeBase.Unresolved = D; }

  /// \brief Mark this type as having a type variable.
  void setHasTypeVariable(bool TV = true) {
    TypeBits.TypeBase.HasTypeVariable = TV;
  }

public:
  /// getKind - Return what kind of type this is.
  TypeKind getKind() const { return Kind; }

  /// isCanonical - Return true if this is a canonical type.
  bool isCanonical() const { return CanonicalType.is<ASTContext*>(); }
  
  /// hasCanonicalTypeComputed - Return true if we've already computed a
  /// canonical version of this type.
  bool hasCanonicalTypeComputed() const { return !CanonicalType.isNull(); }
  
  /// getCanonicalType - Return the canonical version of this type, which has
  /// sugar from all levels stripped off.
  CanType getCanonicalType();
  
  /// getASTContext - Return the ASTContext that this type belongs to.
  ASTContext &getASTContext() {
    // If this type is canonical, it has the ASTContext in it.
    if (CanonicalType.is<ASTContext*>())
      return *CanonicalType.get<ASTContext*>();
    // If not, canonicalize it to get the Context.
    return *getCanonicalType()->CanonicalType.get<ASTContext*>();
  }
  
  /// isEqual - Return true if these two types are equal, ignoring sugar.
  bool isEqual(Type Other);
  
  /// isSpelledLike - Return true if these two types share a sugared spelling,
  /// for instance, two IdentifierTypes with the same spelling that map to the
  /// same named type.
  bool isSpelledLike(Type Other);
  
  /// getDesugaredType - If this type is a sugared type, remove all levels of
  /// sugar until we get down to a non-sugar type.
  TypeBase *getDesugaredType();
  
  /// If this type is a (potentially sugared) type of the specified kind, remove
  /// the minimal amount of sugar required to get a pointer to the type.
  template <typename T>
  T *getAs() {
    return dyn_cast<T>(getDesugaredType());
  }

  template <typename T>
  bool is() {
    return isa<T>(getDesugaredType());
  }
  
  template <typename T>
  T *castTo() {
    return cast<T>(getDesugaredType());
  }

  
  /// getString - Return the name of the type as a string, for use in
  /// diagnostics only.
  std::string getString() const;

  /// isMaterializable - Is this type 'materializable' according to
  /// the rules of the language?  Basically, does it not contain any
  /// l-value types?
  bool isMaterializable();

  /// hasReferenceSemantics() - Do objects of this type have reference
  /// semantics?
  bool hasReferenceSemantics();
  
  /// hasOwnership() - Are variables of this type permitted to have
  /// ownership attributes?
  bool hasOwnership();
  
  /// isUnresolvedType() - Determines whether this type is an unresolved
  /// type, meaning that part of the type depends on the context in which
  /// the type occurs.
  bool isUnresolvedType() const;

  /// \brief Determine whether this type involves a type variable.
  bool hasTypeVariable() const;

  /// \brief Compute and return the set of type variables that occur within this
  /// type.
  ///
  /// \param typeVariables This vector is populated with the set of
  /// type variables referenced by this type.
  void getTypeVariables(SmallVectorImpl<TypeVariableType *> &typeVariables);

  /// isExistentialType - Determines whether this type is an existential type,
  /// whose real (runtime) type is unknown but which is known to conform to
  /// some set of protocols. Protocol and protocol-conformance types are
  /// existential types.
  bool isExistentialType();
  
  /// Determines whether this type is an existential type with a class protocol
  /// bound.
  bool isClassExistentialType();

  /// isExistentialType - Determines whether this type is an existential type,
  /// whose real (runtime) type is unknown but which is known to conform to
  /// some set of protocols. Protocol and protocol-conformance types are
  /// existential types.
  ///
  /// \param Protocols If the type is an existential type, this vector is
  /// populated with the set of protocols
  bool isExistentialType(SmallVectorImpl<ProtocolDecl *> &Protocols);

  /// \brief Determine whether the given type is "specialized", meaning that
  /// it involves generic types for which generic arguments have been provided.
  /// For example, the types Vector<Int> and Vector<Int>.Element are both
  /// specialized, but the type Vector is not.
  bool isSpecialized();

  /// \brief Determine whether the given type is "generic", meaning that
  /// it involves generic types for which generic arguments have not been
  /// provided.
  /// For example, the type Vector and Vector<Int>.InnerGeneric are both
  /// unspecialized generic, but the type Vector<Int> is not.
  bool isUnspecializedGeneric();

  /// \brief Whether this type has been validated yet.
  bool wasValidated() const { return TypeBits.TypeBase.Validated != 0; }

  /// \brief Whether this type is valid.
  bool isValid() const {
    assert(wasValidated() && "Type not yet validated");
    return TypeBits.TypeBase.Validated == 2;
  }

  /// \brief Mark this type as having been validated already.
  void setValidated(bool valid) { TypeBits.TypeBase.Validated = 1 + valid; }

  /// \brief If this is a class type or a bound generic class type, returns the
  /// (possibly generic) class.
  ClassDecl *getClassOrBoundGenericClass();
  
  /// \brief Determine whether this type may have a superclass, which holds for
  /// classes, bound generic classes, and archetypes that are only instantiable
  /// with a class type.
  bool mayHaveSuperclass();

  /// \brief If this is a nominal type or a bound generic nominal type,
  /// returns the (possibly generic) nominal type declaration.
  NominalTypeDecl *getNominalOrBoundGenericNominal();

  /// \brief If this is a nominal type, bound generic nominal type, or
  /// unbound generic nominal type, return the (possibly generic) nominal type
  /// declaration.
  NominalTypeDecl *getAnyNominal();

  /// getUnlabeledType - Retrieve a version of this type with all labels
  /// removed at every level. For example, given a tuple type 
  /// \code
  /// (p : (x : int, y : int))
  /// \endcode
  /// the result would be the (parenthesized) type ((int, int)).
  Type getUnlabeledType(ASTContext &Context);

  /// \brief Retrieve the type without any default arguments.
  Type getWithoutDefaultArgs(ASTContext &Context);

  /// getRValueType - For an lvalue type, retrieves the underlying object type.
  /// Otherwise, returns the type itself.
  Type getRValueType();
  
  /// isSettableLValue - Returns true if the type is a settable lvalue, or
  /// false if the type is an rvalue or non-settable lvalue.
  bool isSettableLValue();

  void dump() const;
  void print(raw_ostream &OS) const;
  
private:
  // Make vanilla new/delete illegal for Types.
  void *operator new(size_t Bytes) throw() = delete;
  void operator delete(void *Data) throw() = delete;
  void *operator new(size_t Bytes, void *Mem) throw() = delete;
public:
  // Only allow allocation of Types using the allocator in ASTContext
  // or by doing a placement new.
  void *operator new(size_t bytes, ASTContext &ctx, AllocationArena arena,
                     unsigned alignment = 8);
};

/// ErrorType - This represents a type that was erroneously constructed.  This
/// is produced when parsing types and when name binding type aliases, and is
/// installed in declaration that use these erroneous types.  All uses of a
/// declaration of invalid type should be ignored and not re-diagnosed.
class ErrorType : public TypeBase {
  friend class ASTContext;
  // The Error type is always canonical.
  ErrorType(ASTContext &C) 
    : TypeBase(TypeKind::Error, &C, /*Unresolved=*/false,
               /*HasTypeVariable=*/false) { }
public:
  static Type get(ASTContext &C);
  
  void print(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Error;
  }
};

/// BuiltinType - An abstract class for all the builtin types.
class BuiltinType : public TypeBase {
protected:
  BuiltinType(TypeKind kind, ASTContext &canTypeCtx)
  : TypeBase(kind, &canTypeCtx, /*unresolved*/ false,
             /*HasTypeVariable=*/false) {}
public:
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_BuiltinType &&
           T->getKind() <= TypeKind::Last_BuiltinType;
  }
};

/// BuiltinRawPointerType - The builtin raw (and dangling) pointer type.  This
/// pointer is completely unmanaged and is equivalent to i8* in LLVM IR.
class BuiltinRawPointerType : public BuiltinType {
  friend class ASTContext;
  BuiltinRawPointerType(ASTContext &C)
    : BuiltinType(TypeKind::BuiltinRawPointer, C) {}
public:
  void print(raw_ostream &OS) const;
  
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinRawPointer;
  }
};

/// BuiltinOpaquePointerType - The builtin opaque pointer type.  This
/// pointer is completely unmanaged and is equivalent to %swift.opaque* in LLVM
/// IR. This is distinct from RawPointer to provide a thin layer of type
/// checking against using arbitrary raw pointers as generic parameters.
class BuiltinOpaquePointerType : public BuiltinType {
  friend class ASTContext;
  BuiltinOpaquePointerType(ASTContext &C)
    : BuiltinType(TypeKind::BuiltinOpaquePointer, C) {}
public:
  void print(raw_ostream &OS) const;
  
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinOpaquePointer;
  }
};

/// BuiltinObjectPointerType - The builtin opaque object-pointer type.
/// Useful for keeping an object alive when it is otherwise being
/// manipulated via an unsafe pointer type.
class BuiltinObjectPointerType : public BuiltinType {
  friend class ASTContext;
  BuiltinObjectPointerType(ASTContext &C)
    : BuiltinType(TypeKind::BuiltinObjectPointer, C) {}
public:
  void print(raw_ostream &OS) const;

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinObjectPointer;
  }
};

/// BuiltinObjCPointerType - The builtin opaque Objective-C pointer type.
/// Useful for pushing an Objective-C type through swift.
class BuiltinObjCPointerType : public BuiltinType {
  friend class ASTContext;
  BuiltinObjCPointerType(ASTContext &C)
    : BuiltinType(TypeKind::BuiltinObjCPointer, C) {}
public:
  void print(raw_ostream &OS) const;

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinObjCPointer;
  }
};

/// \brief A builtin vector type.
class BuiltinVectorType : public BuiltinType, public llvm::FoldingSetNode {
  Type elementType;
  unsigned numElements;

  friend class ASTContext;

  BuiltinVectorType(ASTContext &context, Type elementType, unsigned numElements)
    : BuiltinType(TypeKind::BuiltinVector, context),
      elementType(elementType), numElements(numElements) { }

public:
  static BuiltinVectorType *get(ASTContext &context, Type elementType,
                                unsigned numElements);

  void print(raw_ostream &OS) const;

  /// \brief Retrieve the type of this vector's elements.
  Type getElementType() const { return elementType; }

  /// \brief Retrieve the number of elements in this vector.
  unsigned getNumElements() const { return numElements; }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getElementType(), getNumElements());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, Type elementType,
                      unsigned numElements) {
    ID.AddPointer(elementType.getPointer());
    ID.AddInteger(numElements);
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinVector;
  }
};


/// BuiltinIntegerType - The builtin integer types.  These directly correspond
/// to LLVM IR integer types.  They lack signedness and have an arbitrary
/// bitwidth.
class BuiltinIntegerType : public BuiltinType {
  friend class ASTContext;
  unsigned BitWidth;
  BuiltinIntegerType(unsigned BitWidth, ASTContext &C)
    : BuiltinType(TypeKind::BuiltinInteger, C), BitWidth(BitWidth) {}
public:
  
  static BuiltinIntegerType *get(unsigned BitWidth, ASTContext &C);
  
  /// getBitWidth - Return the bitwidth of the integer.
  unsigned getBitWidth() const {
    return BitWidth;
  }

  void print(raw_ostream &OS) const;

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinInteger;
  }
};
  
class BuiltinFloatType : public BuiltinType {
  friend class ASTContext;
public:
  enum FPKind {
    IEEE16, IEEE32, IEEE64, IEEE80, IEEE128, /// IEEE floating point types.
    PPC128   /// PowerPC "double double" type.
  };
private:
  FPKind Kind;
  
  BuiltinFloatType(FPKind Kind, ASTContext &C)
    : BuiltinType(TypeKind::BuiltinFloat, C), Kind(Kind) {}
public:
  
  /// getFPKind - Get the 
  FPKind getFPKind() const {
    return Kind;
  }

  const llvm::fltSemantics &getAPFloatSemantics() const;

  unsigned getBitWidth() const {
    switch (Kind) {
    case IEEE16: return 16;
    case IEEE32: return 32;
    case IEEE64: return 64;
    case IEEE80: return 80;
    case IEEE128:
    case PPC128: return 128;
    }
  }
  
  void print(raw_ostream &OS) const;

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BuiltinFloat;
  }
};
  
/// UnstructuredUnresolvedType - This is a expression type whose actual kind
/// is specified by context which hasn't been provided yet, and which
/// has no known structure. For example, a tuple element ".foo" will have
/// an unstructured unresolved type. however, a tuple "(x, .foo)" would have
/// an unresolved type that is not an UnstructuredUnresolvedType, because it is
/// known to be a tuple type and have a first element of the type of x.
class UnstructuredUnresolvedType : public TypeBase {
  friend class ASTContext;
  // The Unresolved type is always canonical.
  UnstructuredUnresolvedType(ASTContext &C) 
    : TypeBase(TypeKind::UnstructuredUnresolved, &C, /*Unresolved=*/true,
               /*HasTypeVariable=*/false) {}
public:
  static Type get(ASTContext &C);

  void print(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::UnstructuredUnresolved;
  }
};

/// NameAliasType - An alias type is a name for another type, just like a
/// typedef in C.
class NameAliasType : public TypeBase {
  friend class TypeAliasDecl;
  // NameAliasType are never canonical.
  NameAliasType(TypeAliasDecl *d) 
    : TypeBase(TypeKind::NameAlias, nullptr, /*Unresolved=*/false,
               /*HasTypeVariable=*/false),
      TheDecl(d) {}
  TypeAliasDecl *const TheDecl;

public:
  TypeAliasDecl *getDecl() const { return TheDecl; }
   
  /// getDesugaredType - If this type is a sugared type, remove all levels of
  /// sugar until we get down to a non-sugar type.
  TypeBase *getDesugaredType();

  void print(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::NameAlias;
  }
};

/// IdentifierType - A use of a type through a (possibly dotted) name, like
/// "foo" or "a.b.c".  These are never canonical and never uniqued, as they
/// carry location info for each identifier.
class IdentifierType : public TypeBase {
public:
  class Component {
  public:
    SourceLoc Loc;
    Identifier Id;
    ArrayRef<TypeLoc> GenericArgs;
    
    /// Value is the decl or module that this refers to.
    ///
    /// Before name binding, each component has its value set to a DeclContext
    /// for the root lookup, giving a context for that lookup.
    ///
    /// After name binding, the value is set to the decl being referenced, and
    /// the last entry in the component list is known to be a Type.
    llvm::PointerUnion4<DeclContext*, ValueDecl*, Type, Module*> Value;
    
    Component(SourceLoc Loc, Identifier Id, ArrayRef<TypeLoc> GenericArgs,
              DeclContext *Ctx) :
      Loc(Loc), Id(Id), GenericArgs(GenericArgs), Value(Ctx) {}
    
    /// isBound - Return true if this Component has been namebound already.
    bool isBound() const { return !Value.is<DeclContext*>(); }
    
    void setValue(ValueDecl *VD) { Value = VD; }
    void setValue(Type T) { Value = T; }
    void setValue(Module *M) { Value = M; }
  };
  
private:
  // IdentifierType are never canonical.
  IdentifierType(MutableArrayRef<Component> Components)
    : TypeBase(TypeKind::Identifier, nullptr, /*Unresolved=*/false,
               /*HasTypeVariable=*/false),
      Components(Components) {}
public:
  
  /// The components that make this up.
  const MutableArrayRef<Component> Components;

  
  /// getNew - Return a new IdentifierType with the specified Component
  /// information.
  static IdentifierType *getNew(ASTContext &C,
                                MutableArrayRef<Component> Components);

  /// isMapped - Determine whether name binding has resolved the identifiers
  /// to an actual type.
  bool isMapped() const {
    return Components.back().Value.is<Type>();
  }
  
  /// getMappedType - After name binding is complete, this indicates what type
  /// this refers to (without removing any other sugar).
  Type getMappedType() {
    assert(isMapped() && "Name binding hasn't resolved this to a type yet");
    return Components.back().Value.get<Type>();
  }
  
  /// getDesugaredType - If this type is a sugared type, remove all levels of
  /// sugar until we get down to a non-sugar type.
  TypeBase *getDesugaredType();

  void print(raw_ostream &OS) const;
  static void printComponents(raw_ostream &OS, ArrayRef<Component> Components);
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Identifier;
  }
};

/// ParenType - A paren type is a type that's been written in parentheses.
class ParenType : public TypeBase {
  Type UnderlyingType;

  friend class ASTContext;
  ParenType(Type UnderlyingType, bool HasTypeVariable)
    : TypeBase(TypeKind::Paren, nullptr, UnderlyingType->isUnresolvedType(),
               HasTypeVariable),
      UnderlyingType(UnderlyingType) {}
public:
  Type getUnderlyingType() const { return UnderlyingType; }

  static ParenType *get(ASTContext &C, Type underlying);
   
  /// getDesugaredType - If this type is a sugared type, remove all levels of
  /// sugar until we get down to a non-sugar type.
  TypeBase *getDesugaredType();

  void print(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Paren;
  }
};

/// TupleTypeElt - This represents a single element of a tuple.
class TupleTypeElt {
  /// Name - An optional name for the field.
  Identifier Name;

  /// Ty - This is the type of the field, which is mandatory.
  Type Ty;

  /// Init - This is a default value for the tuple element, used if an explicit
  /// value is not specified.
  ExprHandle *Init;

  /// VarargBaseTy - This is the base type of the field, ignoring the "..."
  /// specifier, if one is present.
  Type VarargBaseTy;

public:
  TupleTypeElt() = default;
  /*implicit*/ TupleTypeElt(Type ty,
                            Identifier name = Identifier(),
                            ExprHandle *init = nullptr,
                            Type VarargBaseTy = Type())
    : Name(name), Ty(ty), Init(init), VarargBaseTy(VarargBaseTy) { }
  /*implicit*/ TupleTypeElt(TypeBase *Ty)
    : Name(Identifier()), Ty(Ty), Init(nullptr),VarargBaseTy(Type()) { }

  bool hasName() const { return !Name.empty(); }
  Identifier getName() const { return Name; }

  Type getType() const { return Ty; }
  bool isVararg() const { return !VarargBaseTy.isNull(); }
  Type getVarargBaseTy() const { return VarargBaseTy; }

  /// \brief Retrieve a copy of this tuple type element with the type replaced.
  TupleTypeElt getWithType(Type T) const {
    TupleTypeElt Result(*this);
    Result.Ty = T;
    return Result;
  }
  
  bool hasInit() const { return Init != nullptr; }
  ExprHandle *getInit() const { return Init; }
  void setInit(ExprHandle *E) { Init = E; }
};
  
/// TupleType - A tuple is a parenthesized list of types where each name has an
/// optional name.
///
class TupleType : public TypeBase, public llvm::FoldingSetNode {
  const ArrayRef<TupleTypeElt> Fields;
  
public:
  /// get - Return the uniqued tuple type with the specified elements.
  /// Returns a ParenType instead if there is exactly one element which
  /// is unlabeled and not varargs, so it doesn't accidentally construct
  /// a tuple which is impossible to write.
  static Type get(ArrayRef<TupleTypeElt> Fields, ASTContext &C);

  /// getEmpty - Return the empty tuple type '()'.
  static Type getEmpty(ASTContext &C);

  /// getFields - Return the fields of this tuple.
  ArrayRef<TupleTypeElt> getFields() const { return Fields; }
  
  /// getElementType - Return the type of the specified field.
  Type getElementType(unsigned FieldNo) const {
    return Fields[FieldNo].getType();
  }
  
  /// getNamedElementId - If this tuple has a field with the specified name,
  /// return the field index, otherwise return -1.
  int getNamedElementId(Identifier I) const;
  
  /// hasAnyDefaultValues - Return true if any of our elements has a default
  /// value.
  bool hasAnyDefaultValues() const;
  
  /// getFieldForScalarInit - If a tuple of this type can be initialized with a
  /// scalar, return the field number that the scalar is assigned to.  If not,
  /// return -1.
  int getFieldForScalarInit() const;
  
  
  /// updateInitializedElementType - This methods updates the element type and
  /// initializer for a non-canonical TupleType that has an initializer for the
  /// specified element.  This should only be used by TypeChecker.
  void updateInitializedElementType(unsigned EltNo, Type NewTy);
  
  void print(raw_ostream &OS) const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Tuple;
  }

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, Fields);
  }
  static void Profile(llvm::FoldingSetNodeID &ID, 
                      ArrayRef<TupleTypeElt> Fields);
  
private:
  TupleType(ArrayRef<TupleTypeElt> fields, ASTContext *CanCtx,
            bool hasTypeVariable);
};

/// UnboundGenericType - Represents a generic nominal type where the
/// type arguments have not yet been resolved.
class UnboundGenericType : public TypeBase, public llvm::FoldingSetNode {
  NominalTypeDecl *TheDecl;

  /// \brief The type of the parent, in which this type is nested.
  Type Parent;

private:
  UnboundGenericType(NominalTypeDecl *TheDecl, Type Parent, ASTContext &C,
                     bool hasTypeVariable)
    : TypeBase(TypeKind::UnboundGeneric,
               (!Parent || Parent->isCanonical())? &C : nullptr,
               /*Unresolved=*/false, hasTypeVariable),
      TheDecl(TheDecl), Parent(Parent) { }

public:
  static UnboundGenericType* get(NominalTypeDecl *TheDecl, Type Parent,
                                 ASTContext &C);

  /// \brief Returns the declaration that declares this type.
  NominalTypeDecl *getDecl() const { return TheDecl; }

  /// \brief Returns the type of the parent of this type. This will
  /// be null for top-level types or local types, and for non-generic types
  /// will simply be the same as the declared type of the declaration context
  /// of TheDecl. For types nested within generic types, however, this will
  /// involve \c BoundGenericType nodes that provide context for the nested
  /// type, e.g., the bound type Dictionary<String, Int>.Inner would be
  /// represented as an UnboundGenericType with Dictionary<String, Int> as its
  /// parent type.
  Type getParent() const { return Parent; }

  void print(raw_ostream &O) const;

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getDecl(), getParent());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, NominalTypeDecl *D,
                      Type Parent);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::UnboundGeneric;
  }
};

/// BoundGenericType - An abstract class for applying a generic
/// nominal type to the given type arguments.
class BoundGenericType : public TypeBase, public llvm::FoldingSetNode {
  NominalTypeDecl *TheDecl;

  /// \brief The type of the parent, in which this type is nested.
  Type Parent;
  
  ArrayRef<Type> GenericArgs;
  

protected:
  BoundGenericType(TypeKind theKind, NominalTypeDecl *theDecl, Type parent,
                   ArrayRef<Type> genericArgs, ASTContext *context,
                   bool hasTypeVariable);

public:
  static BoundGenericType* get(NominalTypeDecl *TheDecl, Type Parent,
                               ArrayRef<Type> GenericArgs);

  /// \brief Returns the declaration that declares this type.
  NominalTypeDecl *getDecl() const { return TheDecl; }

  /// \brief Returns the type of the parent of this type. This will
  /// be null for top-level types or local types, and for non-generic types
  /// will simply be the same as the declared type of the declaration context
  /// of TheDecl. For types nested within generic types, however, this will
  /// involve \c BoundGenericType nodes that provide context for the nested
  /// type, e.g., the bound type Dictionary<String, Int>.Inner<Int> would be
  /// represented as a BoundGenericType with Dictionary<String, Int> as its
  /// parent type.
  Type getParent() const { return Parent; }

  ArrayRef<Type> getGenericArgs() const { return GenericArgs; }

  /// \brief Determine whether this bound generic type has substitution
  /// information that provides protocol conformances.
  bool hasSubstitutions();

  /// \brief Retrieve the set of substitutions used to produce this bound
  /// generic type from the underlying generic type.
  ArrayRef<Substitution> getSubstitutions();

  /// \brief Set the substitution information for this bound generic type.
  ///
  /// \param Subs The set of substitutions, which must point into
  /// ASTContext-allocated memory.
  void setSubstitutions(ArrayRef<Substitution> Subs);

  void print(raw_ostream &O) const;

  void Profile(llvm::FoldingSetNodeID &ID) {
    bool hasTypeVariable = false;
    Profile(ID, TheDecl, Parent, GenericArgs, hasTypeVariable);
  }
  static void Profile(llvm::FoldingSetNodeID &ID, NominalTypeDecl *TheDecl,
                      Type Parent, ArrayRef<Type> GenericArgs,
                      bool &hasTypeVariable);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_BoundGenericType &&
           T->getKind() <= TypeKind::Last_BoundGenericType;
  }
};

/// BoundGenericClassType - A subclass of BoundGenericType for the case
/// when the nominal type is a generic class type.
class BoundGenericClassType : public BoundGenericType {
private:
  BoundGenericClassType(ClassDecl *theDecl, Type parent,
                        ArrayRef<Type> genericArgs, ASTContext *context,
                        bool hasTypeVariable)
    : BoundGenericType(TypeKind::BoundGenericClass,
                       reinterpret_cast<NominalTypeDecl*>(theDecl), parent,
                       genericArgs, context, hasTypeVariable) {}
  friend class BoundGenericType;

public:
  static BoundGenericClassType* get(ClassDecl *theDecl, Type parent,
                                    ArrayRef<Type> genericArgs) {
    return cast<BoundGenericClassType>(
             BoundGenericType::get(reinterpret_cast<NominalTypeDecl*>(theDecl),
                                   parent, genericArgs));
  }

  /// \brief Returns the declaration that declares this type.
  ClassDecl *getDecl() const {
    return reinterpret_cast<ClassDecl*>(BoundGenericType::getDecl());
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BoundGenericClass;
  }
};

/// BoundGenericOneOfType - A subclass of BoundGenericType for the case
/// when the nominal type is a generic oneof type.
class BoundGenericOneOfType : public BoundGenericType {
private:
  BoundGenericOneOfType(OneOfDecl *theDecl, Type parent,
                        ArrayRef<Type> genericArgs, ASTContext *context,
                        bool hasTypeVariable)
    : BoundGenericType(TypeKind::BoundGenericOneOf,
                       reinterpret_cast<NominalTypeDecl*>(theDecl), parent,
                       genericArgs, context, hasTypeVariable) {}
  friend class BoundGenericType;

public:
  static BoundGenericOneOfType* get(OneOfDecl *theDecl, Type parent,
                                    ArrayRef<Type> genericArgs) {
    return cast<BoundGenericOneOfType>(
             BoundGenericType::get(reinterpret_cast<NominalTypeDecl*>(theDecl),
                                   parent, genericArgs));
  }

  /// \brief Returns the declaration that declares this type.
  OneOfDecl *getDecl() const {
    return reinterpret_cast<OneOfDecl*>(BoundGenericType::getDecl());
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BoundGenericOneOf;
  }
};

/// BoundGenericStructType - A subclass of BoundGenericType for the case
/// when the nominal type is a generic struct type.
class BoundGenericStructType : public BoundGenericType {
private:
  BoundGenericStructType(StructDecl *theDecl, Type parent,
                         ArrayRef<Type> genericArgs, ASTContext *context,
                         bool hasTypeVariable)
    : BoundGenericType(TypeKind::BoundGenericStruct, 
                       reinterpret_cast<NominalTypeDecl*>(theDecl), parent,
                       genericArgs, context, hasTypeVariable) {}
  friend class BoundGenericType;

public:
  static BoundGenericStructType* get(StructDecl *theDecl, Type parent,
                                    ArrayRef<Type> genericArgs) {
    return cast<BoundGenericStructType>(
             BoundGenericType::get(reinterpret_cast<NominalTypeDecl*>(theDecl),
                                   parent, genericArgs));
  }

  /// \brief Returns the declaration that declares this type.
  StructDecl *getDecl() const {
    return reinterpret_cast<StructDecl*>(BoundGenericType::getDecl());
  }

  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::BoundGenericStruct;
  }
};

/// NominalType - Represents a type with a name that is significant, such that
/// the name distinguishes it from other structurally-similar types that have
/// different names. Nominal types are always canonical.
class NominalType : public TypeBase {
  /// TheDecl - This is the TypeDecl which declares the given type. It
  /// specifies the name and other useful information about this type.
  NominalTypeDecl * const TheDecl;

  /// \brief The type of the parent, in which this type is nested.
  Type Parent;

protected:
  NominalType(TypeKind K, ASTContext *C, NominalTypeDecl *TheDecl,
              Type Parent, bool HasTypeVariable)
    : TypeBase(K, (!Parent || Parent->isCanonical())? C : nullptr,
               /*Unresolved=*/false, HasTypeVariable),
      TheDecl(TheDecl), Parent(Parent) { }

public:
  static NominalType *get(NominalTypeDecl *D, Type Parent, ASTContext &C);

  /// \brief Returns the declaration that declares this type.
  NominalTypeDecl *getDecl() const { return TheDecl; }

  /// \brief Returns the type of the parent of this type. This will
  /// be null for top-level types or local types, and for non-generic types
  /// will simply be the same as the declared type of the declaration context
  /// of TheDecl. For types nested within generic types, however, this will
  /// involve \c BoundGenericType nodes that provide context for the nested
  /// type, e.g., the type Dictionary<String, Int>.ItemRange would be
  /// represented as a NominalType with Dictionary<String, Int> as its parent
  /// type.
  Type getParent() const { return Parent; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_NominalType &&
           T->getKind() <= TypeKind::Last_NominalType;
  }
};

/// OneOfType - This represents the type declared by a OneOfDecl.
class OneOfType : public NominalType, public llvm::FoldingSetNode {
public:
  /// getDecl() - Returns the decl which declares this type.
  OneOfDecl *getDecl() const {
    return reinterpret_cast<OneOfDecl *>(NominalType::getDecl());
  }

  /// \brief Retrieve the type when we're referencing the given oneof
  /// declaration in the parent type \c Parent.
  static OneOfType *get(OneOfDecl *D, Type Parent, ASTContext &C);

  void print(raw_ostream &O) const;

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getDecl(), getParent());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, OneOfDecl *D, Type Parent);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::OneOf;
  }

private:
  OneOfType(OneOfDecl *TheDecl, Type Parent, ASTContext &Ctx,
            bool HasTypeVariable);
};

/// StructType - This represents the type declared by a StructDecl.
class StructType : public NominalType, public llvm::FoldingSetNode {  
public:
  /// getDecl() - Returns the decl which declares this type.
  StructDecl *getDecl() const {
    return reinterpret_cast<StructDecl *>(NominalType::getDecl());
  }

  /// \brief Retrieve the type when we're referencing the given struct
  /// declaration in the parent type \c Parent.
  static StructType *get(StructDecl *D, Type Parent, ASTContext &C);

  void print(raw_ostream &O) const;

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getDecl(), getParent());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, StructDecl *D, Type Parent);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Struct;
  }
  
private:
  StructType(StructDecl *TheDecl, Type Parent, ASTContext &Ctx,
             bool HasTypeVariable);
};

/// ClassType - This represents the type declared by a ClassDecl.
class ClassType : public NominalType, public llvm::FoldingSetNode {  
public:
  /// getDecl() - Returns the decl which declares this type.
  ClassDecl *getDecl() const {
    return reinterpret_cast<ClassDecl *>(NominalType::getDecl());
  }

  /// \brief Retrieve the type when we're referencing the given class
  /// declaration in the parent type \c Parent.
  static ClassType *get(ClassDecl *D, Type Parent, ASTContext &C);

  void print(raw_ostream &O) const;

  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, getDecl(), getParent());
  }
  static void Profile(llvm::FoldingSetNodeID &ID, ClassDecl *D, Type Parent);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Class;
  }
  
private:
  ClassType(ClassDecl *TheDecl, Type Parent, ASTContext &Ctx,
            bool HasTypeVariable);
};

/// MetaTypeType - This is the type given to a metatype value.  When a type is
/// declared, a 'metatype' value is injected into the value namespace to
/// resolve references to the type.  An example:
///
///  struct x { ... }  // declares type 'x' and metatype 'x'.
///  x.a()             // use of the metatype value since its a value context.
class MetaTypeType : public TypeBase {
  Type InstanceType;
  
public:
  /// get - Return the MetaTypeType for the specified type declaration.
  static MetaTypeType *get(Type T, ASTContext &C);

  Type getInstanceType() const { return InstanceType; }

  void print(raw_ostream &O) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::MetaType;
  }
  
private:
  MetaTypeType(Type T, ASTContext *Ctx, bool HasTypeVariable);
  friend class TypeDecl;
};
  
/// ModuleType - This is the type given to a module value, e.g. the "Builtin" in
/// "Builtin.int".  This is typically given to a ModuleExpr, but can also exist
/// on ParenExpr, for example.
class ModuleType : public TypeBase {
  Module *const TheModule;
  
public:
  /// get - Return the ModuleType for the specified module.
  static ModuleType *get(Module *M);

  Module *getModule() const { return TheModule; }
  
  void print(raw_ostream &O) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return  T->getKind() == TypeKind::Module;
  }
  
private:
  ModuleType(Module *M, ASTContext &Ctx)
    : TypeBase(TypeKind::Module, &Ctx, // Always canonical
               /*Unresolved=*/false, /*HasTypeVariable=*/false),
      TheModule(M) {
  }
};
  
/// A high-level calling convention.
enum class AbstractCC : unsigned char {
  /// The C freestanding calling convention.
  C,
  
  /// The ObjC method calling convention.
  ObjCMethod,
  
  /// The calling convention used for calling a normal function.
  Freestanding,
  
  /// The calling convention used for calling an instance method.
  Method,
  
  Last_AbstractCC = Method,
};
  
/// AnyFunctionType - A function type has a single input and result, but
/// these types may be tuples, for example:
///   "(int) -> int" or "(a : int, b : int) -> (int, int)".
/// Note that the parser requires that the input to a function type be a Tuple
/// or ParenType, but ParenType desugars to its element, so the input to a
/// function may be an arbitrary type.
///
/// There are two kinds of function types:  monomorphic (FunctionType) and
/// polymorphic (PolymorphicFunctionType). Both type families additionally can
/// be 'thin', indicating that a function value has no capture context and can be
/// represented at the binary level as a single function pointer.
class AnyFunctionType : public TypeBase {
  const llvm::PointerIntPair<Type, 3, AbstractCC> InputAndCC;
  const llvm::PointerIntPair<Type, 1, bool> OutputAndIsThin;
protected:
  AnyFunctionType(TypeKind Kind, ASTContext *CanTypeContext,
                  Type Input, Type Output, bool isUnresolved,
                  bool HasTypeVariable, bool isThin, AbstractCC cc)
    : TypeBase(Kind, CanTypeContext, isUnresolved, HasTypeVariable),
      InputAndCC(Input, cc), OutputAndIsThin(Output, isThin) {
  }

public:
  Type getInput() const { return InputAndCC.getPointer(); }
  Type getResult() const { return OutputAndIsThin.getPointer(); }
  
  AbstractCC getAbstractCC() const { return InputAndCC.getInt(); }
  
  /// True if the function type is "thin", meaning values of the type can be
  /// represented as simple function pointers without context.
  bool isThin() const { return OutputAndIsThin.getInt(); }

  /// True if this type allows an implicit conversion from a function argument
  /// expression of type T to a function of type () -> T.
  bool isAutoClosure() const;
  
  /// True if this type is an Objective-C-compatible block type.
  bool isBlock() const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_AnyFunctionType &&
           T->getKind() <= TypeKind::Last_AnyFunctionType;
  }
};

/// FunctionType - A monomorphic function type.
///
/// If the AutoClosure bit is set to true, then the input type is known to be ()
/// and a value of this function type is only assignable (in source code) from
/// the destination type of the function.  Sema inserts an ImplicitClosure to
/// close over the value.  For example:
///   var x : [auto_closure] () -> int = 4
class FunctionType : public AnyFunctionType {
  bool AutoClosure : 1;
  
  /// True if this type represents an ObjC-compatible block value. This is a
  /// temporary hack to make simple demo-quality block interop easy.
  bool Block : 1;
public:
  /// 'Constructor' Factory Function
  static FunctionType *get(Type Input, Type Result, ASTContext &C) {
    return get(Input, Result, false, false, false, C);
  }
  static FunctionType *get(Type Input, Type Result, bool isAutoClosure,
                           ASTContext &C) {
    return get(Input, Result, isAutoClosure, false, false,
               AbstractCC::Freestanding, C);
  }
  static FunctionType *get(Type Input, Type Result,
                           bool isAutoClosure, bool isBlock,
                           ASTContext &C) {
    return get(Input, Result, isAutoClosure, isBlock, false,
               AbstractCC::Freestanding, C);
  }
  static FunctionType *get(Type Input, Type Result,
                           bool isAutoClosure, bool isBlock, bool isThin,
                           ASTContext &C) {
    return get(Input, Result, isAutoClosure, isBlock, isThin,
               AbstractCC::Freestanding, C);
  }
  static FunctionType *get(Type Input, Type Result,
                           bool isAutoClosure, bool isBlock, bool isThin,
                           AbstractCC cc, ASTContext &C);

  /// True if this type allows an implicit conversion from a function argument
  /// expression of type T to a function of type () -> T.
  bool isAutoClosure() const { return AutoClosure; }
  
  /// True if this type is an Objective-C-compatible block type.
  bool isBlock() const { return Block; }
  
  void print(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Function;
  }
  
private:
  FunctionType(Type Input, Type Result,
               bool isAutoClosure,
               bool isBlock,
               bool HasTypeVariable,
               bool isThin,
               AbstractCC cc);
};
  
/// PolymorphicFunctionType - A polymorphic function type.
class PolymorphicFunctionType : public AnyFunctionType {
  // TODO: storing a GenericParamList* here is really the wrong solution;
  // we should be able to store something readily canonicalizable.
  GenericParamList *Params;
public:
  /// 'Constructor' Factory Function
  static PolymorphicFunctionType *get(Type input, Type output,
                                      GenericParamList *params,
                                      ASTContext &C) {
    return get(input, output, params, false, AbstractCC::Freestanding, C);
  }

  static PolymorphicFunctionType *get(Type input, Type output,
                                      GenericParamList *params,
                                      bool isThin,
                                      ASTContext &C) {
    return get(input, output, params, isThin, AbstractCC::Freestanding, C);
  }
  
  static PolymorphicFunctionType *get(Type input, Type output,
                                      GenericParamList *params,
                                      bool isThin,
                                      AbstractCC cc,
                                      ASTContext &C);

  GenericParamList &getGenericParams() const { return *Params; }

  void print(raw_ostream &OS) const;
  void printGenericParams(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::PolymorphicFunction;
  }
  
private:
  PolymorphicFunctionType(Type input, Type output, GenericParamList *params,
                          bool isThin,
                          AbstractCC cc,
                          ASTContext &C);
};  
  
/// ArrayType - An array type has a base type and either an unspecified or a
/// constant size.  For example "int[]" and "int[4]".  Array types cannot have
/// size = 0.
class ArrayType : public TypeBase {
  const Type Base;
  
  /// Size - When this is zero it indicates an unsized array like "int[]".
  uint64_t Size;
  
public:
  /// 'Constructor' Factory Function.
  static ArrayType *get(Type BaseType, uint64_t Size, ASTContext &C);

  Type getBaseType() const { return Base; }
  uint64_t getSize() const { return Size; }
  
  void print(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Array;
  }
  
private:
  ArrayType(Type Base, uint64_t Size, bool HasTypeVariable);
};
  
/// ArraySliceType - An array slice type is the type T[], which is
/// always sugar for a library type.
class ArraySliceType : public TypeBase {
  // ArraySliceTypes are never canonical.
  ArraySliceType(Type base, bool hasTypeVariable)
    : TypeBase(TypeKind::ArraySlice, nullptr,
               /*Unresolved=*/base->isUnresolvedType(), hasTypeVariable),
      Base(base) {}

  Type Base;
  Type Impl;

public:
  static ArraySliceType *get(Type baseTy, ASTContext &C);

  bool hasImplementationType() const { return !Impl.isNull(); }
  void setImplementationType(Type ty) {
    assert(!hasImplementationType());
    Impl = ty;
  }
  Type getImplementationType() const {
    assert(hasImplementationType());
    return Impl;
  }

  Type getBaseType() const {
    return Base;
  }
   
  /// getDesugaredType - If this type is a sugared type, remove all levels of
  /// sugar until we get down to a non-sugar type.
  TypeBase *getDesugaredType();

  void print(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::ArraySlice;
  }
};

/// ProtocolType - A protocol type describes an abstract interface implemented
/// by another type.
class ProtocolType : public NominalType {
public:
  ProtocolDecl *getDecl() const {
    return reinterpret_cast<ProtocolDecl *>(NominalType::getDecl());
  }
  
  void print(raw_ostream &OS) const;
  
  /// True if only classes may conform to the protocol.
  bool requiresClass() const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Protocol;
  }

private:
  friend class ProtocolDecl;
  ProtocolType(ProtocolDecl *TheDecl, ASTContext &Ctx);
};

/// ProtocolCompositionType - A type that composes some number of protocols
/// together to represent types that conform to all of the named protocols.
///
/// \code
/// protocol P { /* ... */ }
/// protocol Q { /* ... */ }
/// var x : protocol<P, Q>
/// \endcode
///
/// Here, the type of x is a a composition of the protocols 'P' and 'Q'.
///
/// The canonical form of a protocol composition type is based on a sorted (by
/// module and name), minimized (based on redundancy due to protocol
/// inheritance) protocol list. If the sorted, minimized list is a single
/// protocol, then the canonical type is that protocol type. Otherwise, it is
/// a composition of the protocols in that list.
class ProtocolCompositionType : public TypeBase, public llvm::FoldingSetNode {
  ArrayRef<Type> Protocols;
  
public:
  /// \brief Retrieve an instance of a protocol composition type with the
  /// given set of protocols.
  static Type get(ASTContext &C, ArrayRef<Type> Protocols);
  
  /// \brief Retrieve the set of protocols composed to create this type.
  ArrayRef<Type> getProtocols() const { return Protocols; }
  
  void print(raw_ostream &OS) const;
  
  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, Protocols);
  }
  static void Profile(llvm::FoldingSetNodeID &ID, ArrayRef<Type> Protocols);

  /// True if one or more of the protocols is class.
  bool requiresClass() const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::ProtocolComposition;
  }
  
private:
  static ProtocolCompositionType *build(ASTContext &C,
                                        ArrayRef<Type> Protocols);

  ProtocolCompositionType(ASTContext *Ctx, ArrayRef<Type> Protocols)
    : TypeBase(TypeKind::ProtocolComposition, /*Context=*/Ctx,
               /*Unresolved=*/false, /*HasTypeVariable=*/false),
      Protocols(Protocols) { }
};

/// LValueType - An l-value is a handle to a physical object.  The
/// type of that object uniquely determines the type of an l-value
/// for it.
///
/// L-values are not fully first-class in Swift:
///
///  A type is said to "carry" an l-value if
///   - it is an l-value type or
///   - it is a tuple and at least one of its element types
///     carries an l-value.
///
/// The type of a function argument may carry an l-value.  This
/// is done by annotating the bound variable with the [byref]
/// attribute.
///
/// The type of a return value, local variable, or field may not
/// carry an l-value.
///
/// When inferring a value type from an expression whose type
/// carries an l-value, the carried l-value types are converted
/// to their object type.
class LValueType : public TypeBase {
public:
  class Qual {
  public:
    typedef unsigned opaque_type;

    enum QualBits : opaque_type {
      // The bits are chosen to make the subtype queries as efficient
      // as possible.  Basically, we want the subtypes to involve
      // fewer bits.

      /// An implicit lvalue is an lvalue that has not been explicitly written
      /// in the source as '&'.
      ///
      /// This qualifier is only used by the (constraint-based) type checker.
      Implicit = 0x1,
      
      /// A non-settable lvalue is an lvalue that cannot be assigned to because
      /// it is a property with a `get` but no `set` method, a property of a
      /// non-settable lvalue, or a property of an rvalue. Non-settable
      /// lvalues cannot be used as the destination of an assignment or as
      /// [byref] arguments.
      NonSettable = 0x2,
      
      /// The default for a [byref] type.
      DefaultForType = 0,

      /// The default for a variable reference.
      DefaultForVar = 0,
  
      /// The default for the base of a member access.
      DefaultForMemberAccess = 0
    };

  private:
    opaque_type Bits;

  public:
    Qual() : Bits(0) {}
    explicit Qual(unsigned bits) : Bits(bits) {}
    Qual(QualBits qual) : Bits(qual) {}

    /// Return an opaque representation of this qualifier set.
    /// The result is hashable by DenseMap.
    opaque_type getOpaqueData() const { return Bits; }

    bool isSettable() const { return !(*this & NonSettable); }
    bool isImplicit() const { return (*this & Implicit); }
    
    friend Qual operator|(QualBits l, QualBits r) {
      return Qual(opaque_type(l) | opaque_type(r));
    }

    /// Union two qualifier sets, given that they are compatible.
    friend Qual operator|(Qual l, Qual r) { return Qual(l.Bits | r.Bits); }

    /// Union a qualifier set into this qualifier set, given that
    /// they are compatible.
    Qual &operator|=(Qual r) { Bits |= r.Bits; return *this; }

    /// Intersect two qualifier sets, given that they are compatible.
    friend QualBits operator&(Qual l, Qual r) {
      // Use QualBits to allow a wider range of conversions to bool.
      return QualBits(l.Bits & r.Bits);
    }

    /// Intersect a qualifier set into this qualifier set.
    Qual &operator&=(Qual r) { Bits &= r.Bits; return *this; }

    /// \brief Remove qualifiers from a qualifier set.
    friend Qual operator-(Qual l, Qual r) {
      return Qual(l.Bits & ~r.Bits);
    }

    /// Invert a qualifier set.  The state of the resulting
    /// non-boolean qualifiers is non-determined, except that they are
    /// is compatible with anything.
    friend Qual operator~(Qual qs) { return Qual(~qs.Bits); }
    friend Qual operator~(QualBits qs) { return Qual(~opaque_type(qs)); }

    /// Are these qualifier sets equivalent?
    friend bool operator==(Qual l, Qual r) { return l.Bits == r.Bits; }
    friend bool operator!=(Qual l, Qual r) { return l.Bits != r.Bits; }

    /// Is one qualifier set 'QL' "smaller than" another set 'QR'?
    /// This corresponds to the subtype relation on lvalue types
    /// for a fixed type T;  that is,
    ///   'QL <= QR' iff 'T [byref(QL)]' <= 'T [byref(QR)]'.
    /// Recall that this means that the first is implicitly convertible
    /// to the latter without "coercion", for some sense of that.
    ///
    /// This is not a total order.
    ///
    /// Right now, the subtyping rules are as follows:
    ///   An l-value type is a subtype of another l-value of the
    ///   same object type except:
    ///   - an implicit l-value is not a subtype of an explicit one.
    ///   - a non-settable lvalue is not a subtype of a settable one.
    friend bool operator<=(Qual l, Qual r) {
      // Right now, all our qualifiers are boolean and independent,
      // and we've set it up so that 1 bits correspond to supertypes.
      // Therefore this is just the set-algebraic 'is subset of'
      // operation and can be performed by intersecting the sets and
      // testing for identity with the left.
      return (l & r) == l;
    }
    friend bool operator<(Qual l, Qual r) { return l != r && l <= r; }
    friend bool operator>(Qual l, Qual r) { return r < l; }
    friend bool operator>=(Qual l, Qual r) { return r <= l; }
  };

private:
  Type ObjectTy;
  Qual Quals; // TODO: put these bits in TypeBase

  LValueType(Type objectTy, Qual quals, ASTContext *canonicalContext,
             bool hasTypeVariable)
    : TypeBase(TypeKind::LValue, canonicalContext, 
               objectTy->isUnresolvedType(), hasTypeVariable),
      ObjectTy(objectTy), Quals(quals) {}

public:
  static LValueType *get(Type type, Qual quals, ASTContext &C);

  Type getObjectType() const { return ObjectTy; }
  Qual getQualifiers() const { return Quals; }

  /// Is this lvalue settable?
  bool isSettable() const {
    return getQualifiers().isSettable();
  }

  /// For now, no l-values are ever materializable.  Maybe in the
  /// future we'll make heap l-values materializable.
  bool isMaterializable() const {
    return false;
  }

  void print(raw_ostream &OS) const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *type) {
    return type->getKind() == TypeKind::LValue;
  }
};

/// SubstitutableType - A reference to a type that can be substituted, i.e.,
/// an archetype or a generic parameter.
class SubstitutableType : public TypeBase {
  ArrayRef<ProtocolDecl *> ConformsTo;
  Type Superclass;

protected:
  SubstitutableType(TypeKind K, ASTContext *C, bool Unresolved,
                    ArrayRef<ProtocolDecl *> ConformsTo,
                    Type Superclass)
    : TypeBase(K, C, Unresolved, /*HasTypeVariable=*/false),
      ConformsTo(ConformsTo), Superclass(Superclass) { }

public:
  /// \brief Retrieve the name of this type.
  Identifier getName() const;

  /// \brief Retrieve the parent of this type, or null if this is a
  /// primary type.
  SubstitutableType *getParent() const;

  /// \brief Retrieve the archetype corresponding to this substitutable type.
  ArchetypeType *getArchetype();

  // FIXME: Temporary hack.
  bool isPrimary() const;
  unsigned getPrimaryIndex() const;

  /// getConformsTo - Retrieve the set of protocols to which this substitutable
  /// type shall conform.
  ArrayRef<ProtocolDecl *> getConformsTo() const { return ConformsTo; }
  
  /// requiresClass - True if the type can only be substituted with class types.
  /// This is true if the type conforms to one or more class protocols or has
  /// a superclass constraint.
  bool requiresClass() const;

  /// \brief Retrieve the superclass of this type, if such a requirement exists.
  Type getSuperclass() const { return Superclass; }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() >= TypeKind::First_SubstitutableType &&
           T->getKind() <= TypeKind::Last_SubstitutableType;
  }
};

/// ArchetypeType - An archetype is a type that is a stand-in used to describe
/// type parameters and associated types in generic definition and protocols.
/// Archetypes will be replaced with actual, concrete types at some later
/// point in time, whether it be at compile time due to a direct binding or
/// at run time due to the use of generic types.
class ArchetypeType : public SubstitutableType {
  ArchetypeType *Parent;
  Identifier Name;
  unsigned IndexIfPrimary;
  ArrayRef<std::pair<Identifier, ArchetypeType *>> NestedTypes;
  
public:
  /// getNew - Create a new archetype with the given name.
  ///
  /// The ConformsTo array will be copied into the ASTContext by this routine.
  static ArchetypeType *getNew(ASTContext &Ctx, ArchetypeType *Parent,
                               Identifier Name, ArrayRef<Type> ConformsTo,
                               Type Superclass,
                               Optional<unsigned> Index = Optional<unsigned>());

  /// getNew - Create a new archetype with the given name.
  ///
  /// The ConformsTo array will be minimized then copied into the ASTContext
  /// by this routine.
  static ArchetypeType *getNew(ASTContext &Ctx, ArchetypeType *Parent,
                          Identifier Name,
                          llvm::SmallVectorImpl<ProtocolDecl *> &ConformsTo,
                          Type Superclass,
                          Optional<unsigned> Index = Optional<unsigned>());

  void print(raw_ostream &OS) const;

  /// \brief Retrieve the name of this archetype.
  Identifier getName() const { return Name; }

  /// \brief Retrieve the fully-dotted name that should be used to display this
  /// archetype.
  std::string getFullName() const;

  /// \brief Retrieve the parent of this archetype, or null if this is a
  /// primary archetype.
  ArchetypeType *getParent() const { return Parent; }

  /// \brief Retrieve the nested type with the given name.
  ArchetypeType *getNestedType(Identifier Name) const;

  /// \brief Retrieve the nested types of this archetype.
  ArrayRef<std::pair<Identifier, ArchetypeType *>> getNestedTypes() const {
    return NestedTypes;
  }

  /// \brief Set the nested types to a copy of the given array of
  /// archetypes, which will first be sorted in place.
  void setNestedTypes(ASTContext &Ctx,
         MutableArrayRef<std::pair<Identifier, ArchetypeType *>> Nested);

  /// isPrimary - Determine whether this is the archetype for a 'primary'
  /// archetype, e.g., 
  bool isPrimary() const { return IndexIfPrimary > 0; }

  // getPrimaryIndex - For a primary archetype, return the zero-based index.
  unsigned getPrimaryIndex() const {
    assert(isPrimary() && "Non-primary archetype does not have index");
    return IndexIfPrimary - 1;
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Archetype;
  }
  
private:
  ArchetypeType(ASTContext &Ctx, ArchetypeType *Parent,
                Identifier Name, ArrayRef<ProtocolDecl *> ConformsTo,
                Type Superclass, Optional<unsigned> Index)
    : SubstitutableType(TypeKind::Archetype, &Ctx, /*Unresolved=*/false,
                        ConformsTo, Superclass),
      Parent(Parent), Name(Name), IndexIfPrimary(Index? *Index + 1 : 0) { }
};

/// SubstitutedType - A type that has been substituted for some other type,
/// which implies that the replacement type meets all of the requirements of
/// the original type.
class SubstitutedType : public TypeBase {
  // SubstitutedTypes are never canonical.
  explicit SubstitutedType(Type Original, Type Replacement,
                           bool HasTypeVariable)
    : TypeBase(TypeKind::Substituted, nullptr, Replacement->isUnresolvedType(),
               HasTypeVariable),
      Original(Original), Replacement(Replacement) {}
  
  Type Original;
  Type Replacement;
  
public:
  static SubstitutedType *get(Type Original, Type Replacement, ASTContext &C);
  
  /// \brief Retrieve the original type that is being replaced.
  Type getOriginal() const { return Original; }
  
  /// \brief Retrieve the replacement type.
  Type getReplacementType() const { return Replacement; }
  
  /// getDesugaredType - If this type is a sugared type, remove all levels of
  /// sugar until we get down to a non-sugar type.
  TypeBase *getDesugaredType();
  
  void print(raw_ostream &OS) const;
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::Substituted;
  }
};

/// \brief A type variable used during type checking.
class TypeVariableType : public TypeBase {
  TypeVariableType(ASTContext &C)
    : TypeBase(TypeKind::TypeVariable, &C, true, true) { }

  class Implementation;
  
public:
  /// \brief Create a new type variable whose implementation is constructed
  /// with the given arguments.
  template<typename ...Args>
  static TypeVariableType *getNew(ASTContext &C, Args &&...args);

  /// \brief Retrieve the implementation data corresponding to this type
  /// variable.
  ///
  /// The contents of the implementation data for this type are hidden in the
  /// details of the constraint solver used for type checking.
  Implementation &getImpl() {
    return *reinterpret_cast<Implementation *>(this + 1);
  }

  /// \brief Retrieve the implementation data corresponding to this type
  /// variable.
  ///
  /// The contents of the implementation data for this type are hidden in the
  /// details of the constraint solver used for type checking.
  const Implementation &getImpl() const {
    return *reinterpret_cast<const Implementation *>(this + 1);
  }

  /// \brief Access the implementation object for this type variable.
  Implementation *operator->() {
    return reinterpret_cast<Implementation *>(this + 1);
  }

  void print(raw_ostream &OS) const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const TypeBase *T) {
    return T->getKind() == TypeKind::TypeVariable;
  }
};

inline bool TypeBase::isUnresolvedType() const {
  return TypeBits.TypeBase.Unresolved;
}

inline bool TypeBase::hasTypeVariable() const {
  return TypeBits.TypeBase.HasTypeVariable;
}

inline bool TypeBase::isExistentialType() {
  CanType T = getCanonicalType();
  return T->getKind() == TypeKind::Protocol
         || T->getKind() == TypeKind::ProtocolComposition;
}
  
inline bool TypeBase::isClassExistentialType() {
  CanType T = getCanonicalType();
  if (auto *pt = dyn_cast<ProtocolType>(T))
    return pt->requiresClass();
  if (auto *pct = dyn_cast<ProtocolCompositionType>(T))
    return pct->requiresClass();
  return false;
}

inline Type TypeBase::getRValueType() {
  if (!is<LValueType>())
    return this;

  return castTo<LValueType>()->getObjectType();
}

inline bool TypeBase::isSettableLValue() {
  if (!is<LValueType>())
    return false;
  
  return castTo<LValueType>()->isSettable();
}

inline bool TypeBase::mayHaveSuperclass() {
  if (getClassOrBoundGenericClass())
    return true;

  auto archetype = getAs<ArchetypeType>();
  if (!archetype)
    return nullptr;

  return (bool)archetype->requiresClass();
}

inline Identifier SubstitutableType::getName() const {
  if (auto Archetype = dyn_cast<ArchetypeType>(this))
    return Archetype->getName();

  llvm_unreachable("Not a substitutable type");
}

inline SubstitutableType *SubstitutableType::getParent() const {
  if (auto Archetype = dyn_cast<ArchetypeType>(this))
    return Archetype->getParent();

  llvm_unreachable("Not a substitutable type");
}

inline ArchetypeType *SubstitutableType::getArchetype() {
  if (auto Archetype = dyn_cast<ArchetypeType>(this))
    return Archetype;

  llvm_unreachable("Not a substitutable type");
}

inline bool SubstitutableType::isPrimary() const {
  if (auto Archetype = dyn_cast<ArchetypeType>(this))
    return Archetype->isPrimary();

  llvm_unreachable("Not a substitutable type");
}

inline unsigned SubstitutableType::getPrimaryIndex() const {
  if (auto Archetype = dyn_cast<ArchetypeType>(this))
    return Archetype->getPrimaryIndex();
  llvm_unreachable("Not a substitutable type");
}

} // end namespace swift

namespace llvm {
  // ArchetypeType* is always at least eight-byte aligned; make the three tag
  // bits available through PointerLikeTypeTraits.
  template<>
  class PointerLikeTypeTraits<swift::ArchetypeType*> {
  public:
    static inline void *getAsVoidPointer(swift::ArchetypeType *I) {
      return (void*)I;
    }
    static inline swift::ArchetypeType *getFromVoidPointer(void *P) {
      return (swift::ArchetypeType*)P;
    }
    enum { NumLowBitsAvailable = 3 };
  };

} // end namespace llvm

#endif
