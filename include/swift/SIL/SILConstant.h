//===--- SILConstant.h - Defines the SILConstant struct ---------*- C++ -*-===//
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
// This file defines the SILConstant struct, which is used to identify a SIL
// global identifier that can be used as the operand of a FunctionRefInst
// instruction or that can have a SIL Function associated with it.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILCONSTANT_H
#define SWIFT_SIL_SILCONSTANT_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/PrettyStackTrace.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {
  class ValueDecl;
  class CapturingExpr;
  class ASTContext;
  class ClassDecl;

/// SILConstant - A key for referencing an entity that can be the subject of a
/// SIL FunctionRefInst or the name of a SILFunction body. This can currently
/// be either a reference to a ValueDecl for functions, methods, constructors,
/// and other named entities, or a reference to a CapturingExpr (that is, a
/// FuncExpr or ClosureExpr) for an anonymous function. In addition to the AST
/// reference, there is also an identifier for distinguishing definitions with
/// multiple associated entry points, such as a curried function.
struct SILConstant {
  typedef llvm::PointerUnion<ValueDecl*, CapturingExpr*> Loc;
  
  /// Represents the "kind" of the SILConstant. For some Swift decls there
  /// are multiple SIL entry points, and the kind is used to distinguish them.
  enum class Kind : unsigned {
    /// Func - this constant references the FuncDecl or CapturingExpr in loc
    /// directly.
    Func,
    
    /// Getter - this constant references the getter for the ValueDecl in loc.
    Getter,
    /// Setter - this constant references the setter for the ValueDecl in loc.
    Setter,
    
    /// Allocator - this constant references the allocating constructor
    /// entry point of a class ConstructorDecl or the constructor of a value
    /// ConstructorDecl.
    Allocator,
    /// Initializer - this constant references the initializing constructor
    /// entry point of the class ConstructorDecl in loc.
    Initializer,
    
    /// OneOfElement - this constant references the injection function for
    /// a OneOfElementDecl.
    OneOfElement,
    
    /// Destroyer - this constant references the destroying destructor for the
    /// ClassDecl in loc.
    Destroyer,
    
    /// GlobalAccessor - this constant references the lazy-initializing
    /// accessor for the global VarDecl in loc.
    GlobalAccessor,

    /// References the generator for a default argument of a function.
    DefaultArgGenerator
  };
  
  /// The ValueDecl or CapturingExpr represented by this SILConstant.
  Loc loc;
  /// The Kind of this SILConstant.
  Kind kind : 4;
  /// The uncurry level of this SILConstant.
  unsigned uncurryLevel : 16;
  /// True if this references an ObjC-visible method.
  unsigned isObjC : 1;
  /// The default argument index for a default argument getter.
  unsigned defaultArgIndex : 10;
  
  /// A magic value for SILConstant constructors to ask for the natural uncurry
  /// level of the constant.
  enum : unsigned { ConstructAtNaturalUncurryLevel = ~0U };
  
  /// Produces a null SILConstant.
  SILConstant() : loc(), kind(Kind::Func), uncurryLevel(0), isObjC(0),
                  defaultArgIndex(0) {}
  
  /// Produces a SILConstant of the given kind for the given decl.
  explicit SILConstant(ValueDecl *decl, Kind kind,
                       unsigned uncurryLevel = ConstructAtNaturalUncurryLevel,
                       bool isObjC = false);
  
  /// Produces the 'natural' SILConstant for the given ValueDecl or
  /// CapturingExpr:
  /// - If 'loc' is a func or closure, this returns a Func SILConstant.
  /// - If 'loc' is a getter or setter FuncDecl, this returns the Getter or
  ///   Setter SILConstant for the property VarDecl.
  /// - If 'loc' is a ConstructorDecl, this returns the Allocator SILConstant
  ///   for the constructor.
  /// - If 'loc' is a OneOfElementDecl, this returns the OneOfElement
  ///   SILConstant for the oneof element.
  /// - If 'loc' is a DestructorDecl, this returns the Destructor SILConstant
  ///   for the containing ClassDecl.
  /// - If 'loc' is a global VarDecl, this returns its GlobalAccessor
  ///   SILConstant.
  /// If the uncurry level is unspecified or specified as NaturalUncurryLevel,
  /// then the SILConstant for the natural uncurry level of the definition is
  /// used.
  explicit SILConstant(Loc loc,
                       unsigned uncurryLevel = ConstructAtNaturalUncurryLevel,
                       bool isObjC = false);

  /// Produce a SIL constant for a default argument generator.
  static SILConstant getDefaultArgGenerator(Loc loc, unsigned defaultArgIndex);

  bool isNull() const { return loc.isNull(); }
  
  bool hasDecl() const { return loc.is<ValueDecl*>(); }
  bool hasExpr() const { return loc.is<CapturingExpr*>(); }
  
  ValueDecl *getDecl() const { return loc.get<ValueDecl*>(); }
  CapturingExpr *getExpr() const { return loc.get<CapturingExpr*>(); }
  
  /// True if the SILConstant references a function.
  bool isFunc() const {
    return kind == Kind::Func;
  }
  /// True if the SILConstant references a property accessor.
  bool isProperty() const {
    return kind == Kind::Getter || kind == Kind::Setter;
  }
  /// True if the SILConstant references a constructor entry point.
  bool isConstructor() const {
    return kind == Kind::Allocator || kind == Kind::Initializer;
  }
  /// True if the SILConstant references a oneof entry point.
  bool isOneOfElement() const {
    return kind == Kind::OneOfElement;
  }
  /// True if the SILConstant references a global variable accessor.
  bool isGlobal() const {
    return kind == Kind::GlobalAccessor;
  }
  
  bool operator==(SILConstant rhs) const {
    return loc.getOpaqueValue() == rhs.loc.getOpaqueValue()
      && kind == rhs.kind
      && uncurryLevel == rhs.uncurryLevel
      && isObjC == rhs.isObjC
      && defaultArgIndex == rhs.defaultArgIndex;
  }
  bool operator!=(SILConstant rhs) const {
    return loc.getOpaqueValue() != rhs.loc.getOpaqueValue()
      || kind != rhs.kind
      || uncurryLevel != rhs.uncurryLevel
      || isObjC != rhs.isObjC
      || defaultArgIndex != rhs.defaultArgIndex;
  }
  
  void print(llvm::raw_ostream &os) const;
  void dump() const;
  
  // Returns the SILConstant for an entity at a shallower uncurry level.
  SILConstant atUncurryLevel(unsigned level) const {
    assert(level <= uncurryLevel && "can't safely go to deeper uncurry level");
    return SILConstant(loc.getOpaqueValue(), kind, level, isObjC,
                       defaultArgIndex);
  }
  
  // Returns the ObjC (or native) entry point corresponding to the same
  // constant.
  SILConstant asObjC(bool objc = true) const {
    return SILConstant(loc.getOpaqueValue(), kind, uncurryLevel, objc,
                       defaultArgIndex);
  }
  
  /// Produces a SILConstant from an opaque value.
  explicit SILConstant(void *opaqueLoc,
                       Kind kind,
                       unsigned uncurryLevel,
                       bool isObjC,
                       unsigned defaultArgIndex)
    : loc(Loc::getFromOpaqueValue(opaqueLoc)),
      kind(kind), uncurryLevel(uncurryLevel), isObjC(isObjC),
      defaultArgIndex(defaultArgIndex)
  {}
};

} // end swift namespace

namespace llvm {

// DenseMap key support for SILConstant.
template<> struct DenseMapInfo<swift::SILConstant> {
  using SILConstant = swift::SILConstant;
  using Kind = SILConstant::Kind;
  using Loc = SILConstant::Loc;
  using PointerInfo = DenseMapInfo<void*>;
  using UnsignedInfo = DenseMapInfo<unsigned>;

  static SILConstant getEmptyKey() {
    return SILConstant(PointerInfo::getEmptyKey(), Kind::Func, 0, false, 0);
  }
  static swift::SILConstant getTombstoneKey() {
    return SILConstant(PointerInfo::getEmptyKey(), Kind::Func, 0, false, 0);
  }
  static unsigned getHashValue(swift::SILConstant Val) {
    unsigned h1 = PointerInfo::getHashValue(Val.loc.getOpaqueValue());
    unsigned h2 = UnsignedInfo::getHashValue(unsigned(Val.kind));
    unsigned h3 = (Val.kind == Kind::DefaultArgGenerator)
                    ? UnsignedInfo::getHashValue(Val.defaultArgIndex)
                    : UnsignedInfo::getHashValue(Val.uncurryLevel);
    unsigned h4 = UnsignedInfo::getHashValue(Val.isObjC);
    return h1 ^ (h2 << 4) ^ (h3 << 9) ^ (h4 << 7);
  }
  static bool isEqual(swift::SILConstant const &LHS,
                      swift::SILConstant const &RHS) {
    return LHS == RHS;
  }
};

} // end llvm namespace

#endif
