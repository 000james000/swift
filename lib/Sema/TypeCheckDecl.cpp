//===--- TypeCheckDecl.cpp - Type Checking for Declarations ---------------===//
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
// This file implements semantic analysis for declarations.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "ConstraintSystem.h"
#include "DerivedConformances.h"
#include "TypeChecker.h"
#include "GenericTypeResolver.h"
#include "MiscDiagnostics.h"
#include "swift/AST/ArchetypeBuilder.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Expr.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ReferencedNameTracker.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Parse/Lexer.h"
#include "swift/Strings.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "swift/AST/NameLookup.h"
using namespace swift;

namespace {

/// Used during enum raw value checking to identify duplicate raw values.
/// Character, string, float, and integer literals are all keyed by value.
/// Float and integer literals are additionally keyed by numeric equivalence.
struct RawValueKey {
  enum class Kind : uint8_t {
    String, UnicodeScalar, Float, Int, Tombstone, Empty
  } kind;
  
  struct IntValueTy {
    uint64_t v0;
    uint64_t v1;

    IntValueTy(const APInt &bits) {
      APInt bits128 = bits.sextOrTrunc(128);
      assert(bits128.getBitWidth() <= 128);
      const uint64_t *data = bits128.getRawData();
      v0 = data[0];
      v1 = data[1];
    }
  };

  struct FloatValueTy {
    uint64_t v0;
    uint64_t v1;
  };

  // FIXME: doesn't accommodate >64-bit or signed raw integer or float values.
  union {
    StringRef stringValue;
    uint32_t charValue;
    IntValueTy intValue;
    FloatValueTy floatValue;
  };
  
  explicit RawValueKey(LiteralExpr *expr) {
    switch (expr->getKind()) {
    case ExprKind::IntegerLiteral:
      kind = Kind::Int;
      intValue = IntValueTy(cast<IntegerLiteralExpr>(expr)->getValue());
      return;
    case ExprKind::FloatLiteral: {
      APFloat value = cast<FloatLiteralExpr>(expr)->getValue();
      llvm::APSInt asInt(127, /*isUnsigned=*/false);
      bool isExact = false;
      APFloat::opStatus status =
          value.convertToInteger(asInt, APFloat::rmTowardZero, &isExact);
      if (asInt.getBitWidth() <= 128 && status == APFloat::opOK && isExact) {
        kind = Kind::Int;
        intValue = IntValueTy(asInt);
        return;
      }
      APInt bits = value.bitcastToAPInt();
      const uint64_t *data = bits.getRawData();
      if (bits.getBitWidth() == 80) {
        kind = Kind::Float;
        floatValue = FloatValueTy{ data[0], data[1] };
      } else {
        assert(bits.getBitWidth() == 64);
        kind = Kind::Float;
        floatValue = FloatValueTy{ data[0], 0 };
      }
      return;
    }
    case ExprKind::CharacterLiteral:
      kind = Kind::UnicodeScalar;
      charValue = cast<CharacterLiteralExpr>(expr)->getValue();
      return;
    case ExprKind::StringLiteral:
      kind = Kind::String;
      stringValue = cast<StringLiteralExpr>(expr)->getValue();
      return;
    default:
      llvm_unreachable("not a valid literal expr for raw value");
    }
  }
  
  explicit RawValueKey(Kind k) : kind(k) {
    assert((k == Kind::Tombstone || k == Kind::Empty)
           && "this ctor is only for creating DenseMap special values");
  }
};
  
/// Used during enum raw value checking to identify the source of a raw value,
/// which may have been derived by auto-incrementing, for diagnostic purposes.
struct RawValueSource {
  /// The decl that has the raw value.
  EnumElementDecl *sourceElt;
  /// If the sourceDecl didn't explicitly name a raw value, this is the most
  /// recent preceding decl with an explicit raw value. This is used to
  /// diagnose 'autoincrementing from' messages.
  EnumElementDecl *lastExplicitValueElt;
};

} // end anonymous namespace

namespace llvm {

template<>
class DenseMapInfo<RawValueKey> {
public:
  static RawValueKey getEmptyKey() {
    return RawValueKey(RawValueKey::Kind::Empty);
  }
  static RawValueKey getTombstoneKey() {
    return RawValueKey(RawValueKey::Kind::Tombstone);
  }
  static unsigned getHashValue(RawValueKey k) {
    switch (k.kind) {
    case RawValueKey::Kind::Float:
      // Hash as bits. We want to treat distinct but IEEE-equal values as not
      // equal.
      return DenseMapInfo<uint64_t>::getHashValue(k.floatValue.v0) ^
             DenseMapInfo<uint64_t>::getHashValue(k.floatValue.v1);
    case RawValueKey::Kind::Int:
      return DenseMapInfo<uint64_t>::getHashValue(k.intValue.v0) &
             DenseMapInfo<uint64_t>::getHashValue(k.intValue.v1);
    case RawValueKey::Kind::UnicodeScalar:
      return DenseMapInfo<uint32_t>::getHashValue(k.charValue);
    case RawValueKey::Kind::String:
      return llvm::HashString(k.stringValue);
    case RawValueKey::Kind::Empty:
    case RawValueKey::Kind::Tombstone:
      return 0;
    }
  }
  static bool isEqual(RawValueKey a, RawValueKey b) {
    if (a.kind != b.kind)
      return false;
    switch (a.kind) {
    case RawValueKey::Kind::Float:
      // Hash as bits. We want to treat distinct but IEEE-equal values as not
      // equal.
      return a.floatValue.v0 == b.floatValue.v0 &&
             a.floatValue.v1 == b.floatValue.v1;
    case RawValueKey::Kind::Int:
      return a.intValue.v0 == b.intValue.v0 &&
             a.intValue.v1 == b.intValue.v1;
    case RawValueKey::Kind::UnicodeScalar:
      return a.charValue == b.charValue;
    case RawValueKey::Kind::String:
      return a.stringValue.equals(b.stringValue);
    case RawValueKey::Kind::Empty:
    case RawValueKey::Kind::Tombstone:
      return true;
    }
  }
};
  
} // end llvm namespace

/// Determine whether the given declaration can inherit a class.
static bool canInheritClass(Decl *decl) {
  // Classes can inherit from a class.
  if (isa<ClassDecl>(decl))
    return true;

  // Generic type parameters can inherit a class.
  if (isa<GenericTypeParamDecl>(decl))
    return true;

  // Associated types can inherit a class.
  if (isa<AssociatedTypeDecl>(decl))
    return true;

  return false;
}

/// Retrieve the declared type of a type declaration or extension.
static Type getDeclaredType(Decl *decl) {
  if (auto typeDecl = dyn_cast<TypeDecl>(decl))
    return typeDecl->getDeclaredType();
  return cast<ExtensionDecl>(decl)->getExtendedType();
}

// Add implicit conformances to the given declaration.
static void addImplicitConformances(
              TypeChecker &tc, Decl *decl,
              llvm::SmallSetVector<ProtocolDecl *, 4> &allProtocols) {
  if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
    SmallVector<ProtocolDecl *, 2> protocols;
    nominal->getImplicitProtocols(protocols);
    allProtocols.insert(protocols.begin(), protocols.end());
  }
}

/// Check that the declaration attributes are ok.
static void validateAttributes(TypeChecker &TC, Decl *VD);

/// Check the inheritance clause of a type declaration or extension thereof.
///
/// This routine validates all of the types in the parsed inheritance clause,
/// recording the superclass (if any and if allowed) as well as the protocols
/// to which this type declaration conforms.
void TypeChecker::checkInheritanceClause(Decl *decl, DeclContext *DC,
                                         GenericTypeResolver *resolver) {
  TypeResolutionOptions options = TR_InheritanceClause;
  if (!DC) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
      DC = nominal;
      options |= TR_NominalInheritanceClause;
    } else {
      DC = decl->getDeclContext();
    }
  }

  // Establish a default generic type resolver.
  PartialGenericTypeToArchetypeResolver defaultResolver(*this);
  if (!resolver)
    resolver = &defaultResolver;

  MutableArrayRef<TypeLoc> inheritedClause;

  // If we already checked the inheritance clause, don't do so again.
  if (auto type = dyn_cast<TypeDecl>(decl)) {
    if (type->checkedInheritanceClause())
      return;

    // This breaks infinite recursion, which will be diagnosed separately.
    type->setCheckedInheritanceClause();
    inheritedClause = type->getInherited();
  } else {
    auto ext = cast<ExtensionDecl>(decl);

    validateExtension(ext);

    if (ext->checkedInheritanceClause())
      return;

    // This breaks infinite recursion, which will be diagnosed separately.
    ext->setCheckedInheritanceClause();
    inheritedClause = ext->getInherited();
  }

  // Check all of the types listed in the inheritance clause.
  Type superclassTy;
  SourceRange superclassRange;
  llvm::SmallSetVector<ProtocolDecl *, 4> allProtocols;
  llvm::SmallDenseMap<CanType, SourceRange> inheritedTypes;
  addImplicitConformances(*this, decl, allProtocols);
  for (unsigned i = 0, n = inheritedClause.size(); i != n; ++i) {
    auto &inherited = inheritedClause[i];

    // Validate the type.
    if (validateType(inherited, DC, options, resolver)) {
      inherited.setInvalidType(Context);
      continue;
    }

    auto inheritedTy = inherited.getType();

    // If this is an error type, ignore it.
    if (inheritedTy->is<ErrorType>())
      continue;

    // Retrieve the interface type for this inherited type.
    if (DC->isGenericContext() && DC->isTypeContext()) {
      inheritedTy = getInterfaceTypeFromInternalType(DC, inheritedTy);
    }

    // Check whether we inherited from the same type twice.
    CanType inheritedCanTy = inheritedTy->getCanonicalType();
    auto knownType = inheritedTypes.find(inheritedCanTy);
    if (knownType != inheritedTypes.end()) {
      SourceLoc afterPriorLoc
        = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                     inheritedClause[i-1].getSourceRange().End);
      SourceLoc afterMyEndLoc
        = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                     inherited.getSourceRange().End);

      diagnose(inherited.getSourceRange().Start,
               diag::duplicate_inheritance, inheritedTy)
        .fixItRemoveChars(afterPriorLoc, afterMyEndLoc)
        .highlight(knownType->second);
      inherited.setInvalidType(Context);
      continue;
    }
    inheritedTypes[inheritedCanTy] = inherited.getSourceRange();

    // If this is a protocol or protocol composition type, record the
    // protocols.
    if (inheritedTy->isExistentialType()) {
      SmallVector<ProtocolDecl *, 4> protocols;
      inheritedTy->isExistentialType(protocols);

      allProtocols.insert(protocols.begin(), protocols.end());
      continue;
    }
    
    // If this is an enum inheritance clause, check for a raw type.
    if (isa<EnumDecl>(decl)) {
      // Check if we already had a raw type.
      if (superclassTy) {
        diagnose(inherited.getSourceRange().Start,
                 diag::multiple_enum_raw_types, superclassTy, inheritedTy)
          .highlight(superclassRange);
        inherited.setInvalidType(Context);
        continue;
      }
      
      // If this is not the first entry in the inheritance clause, complain.
      if (i > 0) {
        SourceLoc afterPriorLoc
          = Lexer::getLocForEndOfToken(
              Context.SourceMgr,
              inheritedClause[i-1].getSourceRange().End);
        SourceLoc afterMyEndLoc
          = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                       inherited.getSourceRange().End);

        diagnose(inherited.getSourceRange().Start,
                 diag::raw_type_not_first, inheritedTy)
          .fixItRemoveChars(afterPriorLoc, afterMyEndLoc)
          .fixItInsert(inheritedClause[0].getSourceRange().Start,
                       inheritedTy.getString() + ", ");

        // Fall through to record the raw type.
      }

      // Record the raw type.
      superclassTy = inheritedTy;
      superclassRange = inherited.getSourceRange();
      
      // Add the RawRepresentable conformance implied by the raw type.
      allProtocols.insert(getProtocol(decl->getLoc(),
                                      KnownProtocolKind::RawRepresentable));
      continue;
    }

    // If this is a class type, it may be the superclass.
    if (inheritedTy->getClassOrBoundGenericClass()) {
      // First, check if we already had a superclass.
      if (superclassTy) {
        // FIXME: Check for shadowed protocol names, i.e., NSObject?

        // Complain about multiple inheritance.
        // Don't emit a Fix-It here. The user has to think harder about this.
        diagnose(inherited.getSourceRange().Start,
                 diag::multiple_inheritance, superclassTy, inheritedTy)
          .highlight(superclassRange);
        inherited.setInvalidType(Context);
        continue;
      }

      // If the declaration we're looking at doesn't allow a superclass,
      // complain.
      if (!canInheritClass(decl)) {
        diagnose(decl->getLoc(),
                 isa<ExtensionDecl>(decl)
                   ? diag::extension_class_inheritance
                   : diag::non_class_inheritance,
                 getDeclaredType(decl), inheritedTy)
          .highlight(inherited.getSourceRange());
        inherited.setInvalidType(Context);
        continue;
      }

      // If this is not the first entry in the inheritance clause, complain.
      if (i > 0) {
        SourceLoc afterPriorLoc
          = Lexer::getLocForEndOfToken(
              Context.SourceMgr,
              inheritedClause[i-1].getSourceRange().End);
        SourceLoc afterMyEndLoc
          = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                       inherited.getSourceRange().End);

        diagnose(inherited.getSourceRange().Start,
                 diag::superclass_not_first, inheritedTy)
          .fixItRemoveChars(afterPriorLoc, afterMyEndLoc)
          .fixItInsert(inheritedClause[0].getSourceRange().Start,
                       inheritedTy.getString() + ", ");

        // Fall through to record the superclass.
      }

      // Record the superclass.
      superclassTy = inheritedTy;
      superclassRange = inherited.getSourceRange();
      continue;
    }

    // We can't inherit from a non-class, non-protocol type.
    diagnose(decl->getLoc(),
             canInheritClass(decl)
               ? diag::inheritance_from_non_protocol_or_class
               : diag::inheritance_from_non_protocol,
             inheritedTy);
    // FIXME: Note pointing to the declaration 'inheritedTy' references?
    inherited.setInvalidType(Context);
  }

  // Record the protocols to which this declaration conforms along with the
  // superclass.
  auto allProtocolsCopy = Context.AllocateCopy(allProtocols);
  if (auto ext = dyn_cast<ExtensionDecl>(decl)) {
    assert(!superclassTy && "Extensions can't add superclasses");
    ext->setProtocols(allProtocolsCopy);
    return;
  }

  auto typeDecl = cast<TypeDecl>(decl);

  // FIXME: If we already set the protocols, bail out. We'd rather not have
  // to check this.
  if (typeDecl->isProtocolsValid())
    return;

  typeDecl->setProtocols(allProtocolsCopy);
  if (superclassTy) {
    if (auto classDecl = dyn_cast<ClassDecl>(decl)) {
      classDecl->setSuperclass(superclassTy);
      resolveImplicitConstructors(superclassTy->getClassOrBoundGenericClass());
    } else if (auto enumDecl = dyn_cast<EnumDecl>(decl)) {
      enumDecl->setRawType(superclassTy);
    } else {
      cast<AbstractTypeParamDecl>(decl)->setSuperclass(superclassTy);
    }
  }

  // For protocol decls, fill in null conformances.
  // FIXME: This shouldn't really be necessary, but for now the conformances
  // array is supposed to have a 1-to-1 mapping with the protocols array.
  if (auto proto = dyn_cast<ProtocolDecl>(decl)) {
    auto nulls = Context.Allocate<ProtocolConformance *>(allProtocols.size());
    proto->setConformances(nulls);
  }
}

/// Retrieve the set of protocols the given protocol inherits.
static ArrayRef<ProtocolDecl *>
getInheritedForCycleCheck(TypeChecker &tc,
                          ProtocolDecl *proto,
                          ProtocolDecl **scratch) {
  return tc.getDirectConformsTo(proto);
}

/// Retrieve the superclass of the given class.
static ArrayRef<ClassDecl *> getInheritedForCycleCheck(TypeChecker &tc,
                                                       ClassDecl *classDecl,
                                                       ClassDecl **scratch) {
  tc.checkInheritanceClause(classDecl);

  if (classDecl->hasSuperclass()) {
    *scratch = classDecl->getSuperclass()->getClassOrBoundGenericClass();
    return *scratch;
  }
  return { };
}

/// Retrieve the raw type of the given enum.
static ArrayRef<EnumDecl *> getInheritedForCycleCheck(TypeChecker &tc,
                                                      EnumDecl *enumDecl,
                                                      EnumDecl **scratch) {
  tc.checkInheritanceClause(enumDecl);
  
  if (enumDecl->hasRawType()) {
    *scratch = enumDecl->getRawType()->getEnumOrBoundGenericEnum();
    return *scratch ? ArrayRef<EnumDecl*>(*scratch) : ArrayRef<EnumDecl*>{};
  }
  return { };
}

// Break the inheritance cycle for a protocol by removing all inherited
// protocols.
//
// FIXME: Just remove the problematic inheritance?
static void breakInheritanceCycle(ProtocolDecl *proto) {
  proto->setProtocols({ });
  proto->setConformances({ });
}

/// Break the inheritance cycle for a class by removing its superclass.
static void breakInheritanceCycle(ClassDecl *classDecl) {
  classDecl->setSuperclass(Type());
}

/// Break the inheritance cycle for an enum by removing its raw type.
static void breakInheritanceCycle(EnumDecl *enumDecl) {
  enumDecl->setRawType(Type());
}

/// Check for circular inheritance.
template<typename T>
static void checkCircularity(TypeChecker &tc, T *decl,
                             Diag<StringRef> circularDiag,
                             Diag<Identifier> declHereDiag,
                             SmallVectorImpl<T *> &path) {
  switch (decl->getCircularityCheck()) {
  case CircularityCheck::Checked:
    return;

  case CircularityCheck::Checking: {
    // We're already checking this protocol, which means we have a cycle.
    
    // The type directly references itself.
    if (path.size() == 1) {
      tc.diagnose((*path.begin())->getLoc(),
                  circularDiag,
                  (*path.begin())->getName().str());
      
      decl->setInvalid();
      decl->overwriteType(ErrorType::get(tc.Context));
      breakInheritanceCycle(decl);
      break;
    }

    // Find the beginning of the cycle within the full path.
    auto cycleStart = path.end()-2;
    while (*cycleStart != decl) {
      assert(cycleStart != path.begin() && "Missing cycle start?");
      --cycleStart;
    }

    // Form the textual path illustrating the cycle.
    llvm::SmallString<128> pathStr;
    for (auto i = cycleStart, iEnd = path.end(); i != iEnd; ++i) {
      if (!pathStr.empty())
        pathStr += " -> ";
      pathStr += ("'" + (*i)->getName().str() + "'").str();
    }
    pathStr += (" -> '" + decl->getName().str() + "'").str();

    // Diagnose the cycle.
    tc.diagnose(decl->getLoc(), circularDiag, pathStr);
    for (auto i = cycleStart + 1, iEnd = path.end(); i != iEnd; ++i) {
      tc.diagnose(*i, declHereDiag, (*i)->getName());
    }

    // Set this declaration as invalid, then break the cycle somehow.
    decl->setInvalid();
    decl->overwriteType(ErrorType::get(tc.Context));
    breakInheritanceCycle(decl);
    break;
  }

  case CircularityCheck::Unchecked: {
    // Walk to the inherited class or protocols.
    path.push_back(decl);
    decl->setCircularityCheck(CircularityCheck::Checking);
    T *scratch = nullptr;
    for (auto inherited : getInheritedForCycleCheck(tc, decl, &scratch)) {
      checkCircularity(tc, inherited, circularDiag, declHereDiag, path);
    }
    decl->setCircularityCheck(CircularityCheck::Checked);
    path.pop_back();
    break;
  }
  }
}

/// Set each bound variable in the pattern to have an error type.
static void setBoundVarsTypeError(Pattern *pattern, ASTContext &ctx) {
  pattern->forEachVariable([&](VarDecl *var) {
    // Don't change the type of a variable that we've been able to
    // compute a type for.
    if (var->hasType()) {
      if (var->getType()->is<ErrorType>())
        var->setInvalid();
    } else {
      var->setType(ErrorType::get(ctx));
      var->setInvalid();
    }
  });
}

/// Create a fresh archetype builder.
ArchetypeBuilder TypeChecker::createArchetypeBuilder(Module *mod) {
  return ArchetypeBuilder(
           *mod, Diags, this,
           [=](ProtocolDecl *protocol) -> ArrayRef<ProtocolDecl *> {
             return getDirectConformsTo(protocol);
           },
           [=](AbstractTypeParamDecl *assocType) -> 
                 std::pair<Type, ArrayRef<ProtocolDecl *>> {
             checkInheritanceClause(assocType);
             return std::make_pair(assocType->getSuperclass(),
                                   assocType->getProtocols());
           },
           [=](Module &M, Type T, ProtocolDecl *Protocol)
           -> ProtocolConformance* {
             ProtocolConformance *c;
             if (conformsToProtocol(T, Protocol, &M, /*expression=*/false, &c))
               return c;
             return nullptr;
           });
}

static void revertDependentTypeLoc(TypeLoc &tl) {
  // If there's no type representation, there's nothing to revert.
  if (!tl.getTypeRepr())
    return;

  // Don't revert an error type; we've already complained.
  if (tl.wasValidated() && tl.isError())
    return;

  // Make sure we validate the type again.
  tl.setType(Type(), /*validated=*/false);

  // Walker that reverts dependent identifier types.
  class RevertWalker : public ASTWalker {
  public:
    // Skip expressions.
    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      return { false, expr };
    }

    // Skip statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }

    // Skip patterns
    std::pair<bool, Pattern*> walkToPatternPre(Pattern *pattern) override {
      return { false, pattern };
    }

    bool walkToTypeReprPost(TypeRepr *repr) override {
      auto identType = dyn_cast<IdentTypeRepr>(repr);
      if (!identType)
        return true;

      for (auto &comp : identType->getComponentRange()) {
        // If it's not a bound type, we're done.
        if (!comp->isBoundType())
          return true;

        // If the bound type isn't dependent, there's nothing to do.
        auto type = comp->getBoundType();
        if (!type->isDependentType())
          return true;

        // Turn a generic parameter type back into a reference to the
        // generic parameter itself.
        if (auto genericParamType
            = dyn_cast<GenericTypeParamType>(type.getPointer())) {
          assert(genericParamType->getDecl() && "Missing type parameter decl");
          comp->setValue(genericParamType->getDecl());
        } else {
          comp->revert();
        }
      }

      return true;
    }
  };

  if (tl.isNull())
    return;

  tl.getTypeRepr()->walk(RevertWalker());
}

static void revertDependentPattern(Pattern *pattern) {
  // Clear out the pattern's type.
  if (pattern->hasType()) {
    // If the type of the pattern was in error, we're done.
    if (pattern->getType()->is<ErrorType>())
      return;

    pattern->overwriteType(Type());
  }

  switch (pattern->getKind()) {
#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) case PatternKind::Id:
#include "swift/AST/PatternNodes.def"
    // Do nothing for refutable patterns.
    break;

  case PatternKind::Any:
    // Do nothing;
    break;

  case PatternKind::Named: {
    // Clear out the type of the variable.
    auto named = cast<NamedPattern>(pattern);
    if (named->getDecl()->hasType() &&
        !named->getDecl()->isInvalid())
      named->getDecl()->overwriteType(Type());
    break;
  }

  case PatternKind::Paren:
    // Recurse into parentheses patterns.
    revertDependentPattern(cast<ParenPattern>(pattern)->getSubPattern());
    break;
      
  case PatternKind::Var:
    // Recurse into var patterns.
    revertDependentPattern(cast<VarPattern>(pattern)->getSubPattern());
    break;

  case PatternKind::Tuple: {
    // Recurse into tuple elements.
    auto tuple = cast<TuplePattern>(pattern);
    for (auto &field : tuple->getFields()) {
      revertDependentPattern(field.getPattern());
    }
    break;
  }

  case PatternKind::Typed: {
    // Revert the type annotation.
    auto typed = cast<TypedPattern>(pattern);
    revertDependentTypeLoc(typed->getTypeLoc());

    // Revert the subpattern.
    revertDependentPattern(typed->getSubPattern());
    break;
  }
  }
}

/// Add the generic parameters and requirements from the parent context to the
/// archetype builder.
static void addContextParamsAndRequirements(ArchetypeBuilder &builder,
                                            DeclContext *dc) {
  auto type = dc->getDeclaredTypeOfContext();
  if (!type || type->is<ErrorType>())
    return;

  auto nominal = type->getAnyNominal();
  assert(nominal && "Parent context is not a nominal type?");

  if (auto sig = nominal->getGenericSignature()) {
    // Add generic signature from this context.
    builder.addGenericSignature(sig, true);
  } else if (auto parentDC = dc->getParent()) {
    // Recurse into parent context.
    addContextParamsAndRequirements(builder, parentDC);
  }
}

/// Check the given generic parameter list, introduce the generic parameters
/// and requirements into the archetype builder, but don't assign archetypes
/// yet.
static void checkGenericParamList(ArchetypeBuilder &builder,
                                  GenericParamList *genericParams,
                                  TypeChecker &TC, DeclContext *DC) {
  assert(genericParams && "Missing generic parameters");
  unsigned Depth = genericParams->getDepth();

  // Add outer parameters.
  addContextParamsAndRequirements(builder, DC);

  // Assign archetypes to each of the generic parameters.
  for (auto GP : *genericParams) {
    // Set the depth of this type parameter.
    GP->setDepth(Depth);

    // Check the constraints on the type parameter.
    TC.checkInheritanceClause(GP, DC);

    // Add the generic parameter to the builder.
    builder.addGenericParameter(GP);

    // Infer requirements from the "inherited" types.
    for (auto &inherited : GP->getInherited()) {
      builder.inferRequirements(inherited);
    }
  }

  // Add the requirements clause to the builder, validating the types in
  // the requirements clause along the way.
  for (auto &Req : genericParams->getRequirements()) {
    if (Req.isInvalid())
      continue;

    switch (Req.getKind()) {
    case RequirementKind::Conformance: {
      // Validate the types.
      if (TC.validateType(Req.getSubjectLoc(), DC)) {
        Req.setInvalid();
        continue;
      }

      if (TC.validateType(Req.getConstraintLoc(), DC, TR_InheritanceClause)) {
        Req.setInvalid();
        continue;
      }

      // FIXME: Feels too early to perform this check.
      if (!Req.getConstraint()->isExistentialType() &&
          !Req.getConstraint()->getClassOrBoundGenericClass()) {
        TC.diagnose(genericParams->getWhereLoc(),
                    diag::requires_conformance_nonprotocol,
                    Req.getSubjectLoc(), Req.getConstraintLoc());
        Req.getConstraintLoc().setInvalidType(TC.Context);
        Req.setInvalid();
        continue;
      }
      
      break;
    }

    case RequirementKind::SameType:
      if (TC.validateType(Req.getFirstTypeLoc(), DC)) {
        Req.setInvalid();
        continue;
      }

      if (TC.validateType(Req.getSecondTypeLoc(), DC)) {
        Req.setInvalid();
        continue;
      }
      
      break;

    case RequirementKind::WitnessMarker:
      llvm_unreachable("value witness markers in syntactic requirement?");
    }
    
    if (builder.addRequirement(Req))
      Req.setInvalid();
  }
}

/// Revert the dependent types within the given generic parameter list.
void TypeChecker::revertGenericParamList(GenericParamList *genericParams) {
  // Revert the inherited clause of the generic parameter list.
  for (auto param : *genericParams) {
    param->setCheckedInheritanceClause(false);
    for (auto &inherited : param->getInherited())
      revertDependentTypeLoc(inherited);
  }

  // Revert the requirements of the generic parameter list.
  for (auto &req : genericParams->getRequirements()) {
    if (req.isInvalid())
      continue;

    switch (req.getKind()) {
    case RequirementKind::Conformance: {
      revertDependentTypeLoc(req.getSubjectLoc());
      revertDependentTypeLoc(req.getConstraintLoc());
      break;
    }

    case RequirementKind::SameType:
      revertDependentTypeLoc(req.getFirstTypeLoc());
      revertDependentTypeLoc(req.getSecondTypeLoc());
      break;

    case RequirementKind::WitnessMarker:
      llvm_unreachable("value witness markers in syntactic requirement?");
    }
  }
}

/// Finalize the given generic parameter list, assigning archetypes to
/// the generic parameters.
static void finalizeGenericParamList(ArchetypeBuilder &builder,
                                     GenericParamList *genericParams,
                                     DeclContext *dc,
                                     TypeChecker &TC) {
  // Wire up the archetypes.
  for (auto GP : *genericParams) {
    GP->setArchetype(builder.getArchetype(GP));
    TC.checkInheritanceClause(GP);
  }
  genericParams->setAllArchetypes(
    TC.Context.AllocateCopy(builder.getAllArchetypes()));

  // Replace the generic parameters with their archetypes throughout the
  // types in the requirements.
  // FIXME: This should not be necessary at this level; it is a transitional
  // step.
  for (auto &Req : genericParams->getRequirements()) {
    if (Req.isInvalid())
      continue;

    switch (Req.getKind()) {
    case RequirementKind::Conformance: {
      revertDependentTypeLoc(Req.getSubjectLoc());
      if (TC.validateType(Req.getSubjectLoc(), dc)) {
        Req.setInvalid();
        continue;
      }

      revertDependentTypeLoc(Req.getConstraintLoc());
      if (TC.validateType(Req.getConstraintLoc(), dc, TR_InheritanceClause)) {
        Req.setInvalid();
        continue;
      }
      break;
    }

    case RequirementKind::SameType:
      revertDependentTypeLoc(Req.getFirstTypeLoc());
      if (TC.validateType(Req.getFirstTypeLoc(), dc)) {
        Req.setInvalid();
        continue;
      }

      revertDependentTypeLoc(Req.getSecondTypeLoc());
      if (TC.validateType(Req.getSecondTypeLoc(), dc)) {
        Req.setInvalid();
        continue;
      }
      break;

    case RequirementKind::WitnessMarker:
      llvm_unreachable("value witness markers in syntactic requirement?");
    }
  }
}

/// Expose TypeChecker's handling of GenericParamList to SIL parsing.
/// We pass in a vector of nested GenericParamLists and a vector of
/// ArchetypeBuilders with the innermost GenericParamList in the beginning
/// of the vector.
bool TypeChecker::handleSILGenericParams(
                    SmallVectorImpl<ArchetypeBuilder *> &builders,
                    SmallVectorImpl<GenericParamList *> &gps,
                    DeclContext *DC) {
  // We call checkGenericParamList on all lists, then call
  // finalizeGenericParamList on all lists. After finalizeGenericParamList, the
  // generic parameters will be assigned to archetypes. That will cause SameType
  // requirement to have Archetypes inside.

  // Since the innermost GenericParamList is in the beginning of the vector,
  // we process in reverse order to handle the outermost list first.
  GenericSignature *outerSignature = nullptr;
  for (unsigned i = 0, e = gps.size(); i < e; i++) {
    auto &builder = *builders.rbegin()[i];

    auto genericParams = gps.rbegin()[i];
    bool invalid = false;
    outerSignature = validateGenericSignature(genericParams, DC, outerSignature,
                                              nullptr, invalid);
    if (invalid)
      return true;

    revertGenericParamList(genericParams);
    checkGenericParamList(builder, genericParams, *this, DC);
    finalizeGenericParamList(builder, genericParams, DC, *this);
  }
  return false;
}

void TypeChecker::revertGenericFuncSignature(AbstractFunctionDecl *func) {
  // Revert the result type.
  if (auto fn = dyn_cast<FuncDecl>(func)) {
    if (!fn->getBodyResultTypeLoc().isNull()) {
      revertDependentTypeLoc(fn->getBodyResultTypeLoc());
    }
  }

  // Revert the body patterns.
  ArrayRef<Pattern *> bodyPatterns = func->getBodyParamPatterns();
  for (auto bodyPattern : bodyPatterns) {
    revertDependentPattern(bodyPattern);
  }

  // Revert the generic parameter list.
  if (func->getGenericParams())
    revertGenericParamList(func->getGenericParams());

  // Clear out the types.
  if (auto fn = dyn_cast<FuncDecl>(func))
    fn->revertType();
  else
    func->overwriteType(Type());
}

/// Check whether the given type representation will be
/// default-initializable.
static bool isDefaultInitializable(TypeRepr *typeRepr) {
  // Look through most attributes.
  if (auto attributed = dyn_cast<AttributedTypeRepr>(typeRepr)) {
    // Weak ownership implies optionality.
    if (attributed->getAttrs().getOwnership() == Ownership::Weak)
      return true;
    
    return isDefaultInitializable(attributed->getTypeRepr());
  }

  // Look through named types.
  if (auto named = dyn_cast<NamedTypeRepr>(typeRepr))
    return isDefaultInitializable(named->getTypeRepr());
  
  // Optional types are default-initializable.
  if (isa<OptionalTypeRepr>(typeRepr) ||
      isa<ImplicitlyUnwrappedOptionalTypeRepr>(typeRepr))
    return true;

  // Tuple types are default-initializable if all of their element
  // types are.
  if (auto tuple = dyn_cast<TupleTypeRepr>(typeRepr)) {
    // ... but not variadic ones.
    if (tuple->hasEllipsis())
      return false;

    for (auto elt : tuple->getElements()) {
      if (!isDefaultInitializable(elt))
        return false;
    }

    return true;
  }

  // Not default initializable.
  return false;
}

/// Determine whether the given pattern binding declaration either has
/// or will have a default initializer, without performing any type
/// checking on it.
static bool isDefaultInitializable(PatternBindingDecl *pbd) {
  // If it has an initializer, this is trivially true.
  if (pbd->hasInit())
    return true;

  // If it is NSManaged or is a lazy variable, it is trivially true.
  if (auto var = pbd->getSingleVar()) {
    if (var->getAttrs().hasAttribute<NSManagedAttr>() ||
        var->getAttrs().hasAttribute<LazyAttr>())
      return true;
  }

  // If the pattern is typed with optionals, it is true.
  if (auto typedPattern = dyn_cast<TypedPattern>(pbd->getPattern())) {
    if (auto typeRepr = typedPattern->getTypeLoc().getTypeRepr()) {
      return isDefaultInitializable(typeRepr);
    }
  }

  return false;
}

/// Build a default initializer for the given type.
static Expr *buildDefaultInitializer(TypeChecker &tc, Type type) {
  // Default-initialize optional types and weak values to 'nil'.
  if (type->getReferenceStorageReferent()->getAnyOptionalObjectType())
    return new (tc.Context) NilLiteralExpr(SourceLoc(), /*implicit=*/true);

  // Build tuple literals for tuple types.
  if (auto tupleType = type->getAs<TupleType>()) {
    SmallVector<Expr *, 2> inits;
    for (const auto &elt : tupleType->getFields()) {
      if (elt.isVararg())
        return nullptr;

      auto eltInit = buildDefaultInitializer(tc, elt.getType());
      if (!eltInit)
        return nullptr;

      inits.push_back(eltInit);
    }

    return TupleExpr::createImplicit(tc.Context, inits, { });
  }

  // We don't default-initialize anything else.
  return nullptr;
}

/// Check whether \c current is a declaration.
static void checkRedeclaration(TypeChecker &tc, ValueDecl *current) {
  // If we've already checked this declaration, don't do it again.
  if (current->alreadyCheckedRedeclaration())
    return;

  // Make sure we don't do this checking again.
  current->setCheckedRedeclaration(true);

  // Ignore invalid declarations.
  if (current->isInvalid())
    return;

  // If this declaration isn't from a source file, don't check it.
  // FIXME: Should restrict this to the source file we care about.
  DeclContext *currentDC = current->getDeclContext();
  SourceFile *currentFile = currentDC->getParentSourceFile();
  if (!currentFile || currentDC->isLocalContext())
    return;

  // Find other potential definitions.
  SmallVector<ValueDecl *, 4> otherDefinitionsVec;
  ArrayRef<ValueDecl *> otherDefinitions;
  if (currentDC->isTypeContext()) {
    // Look within a type context.
    if (auto nominal = currentDC->getDeclaredTypeOfContext()->getAnyNominal()) {
      otherDefinitions = nominal->lookupDirect(current->getBaseName());
    }
  } else {
    // Look within a module context.
    currentDC->getParentModule()->lookupValue({ }, current->getBaseName(),
                                              NLKind::QualifiedLookup,
                                              otherDefinitionsVec);
    otherDefinitions = otherDefinitionsVec;
  }

  // Compare this signature against the signature of other
  // declarations with the same name.
  OverloadSignature currentSig = current->getOverloadSignature();
  Module *currentModule = current->getModuleContext();
  for (auto other : otherDefinitions) {
    // Skip invalid declarations and ourselves.
    if (current == other || other->isInvalid())
      continue;

    // Skip declarations in other modules.
    if (currentModule != other->getModuleContext())
      continue;

    // Don't compare methods vs. non-methods (which only happens with
    // operators).
    if (currentDC->isTypeContext() != other->getDeclContext()->isTypeContext())
      continue;

    // Validate the declaration.
    tc.validateDecl(other);
    if (other->isInvalid())
      continue;

    // Skip declarations in other files.
    // In practice, this means we will warn on a private declaration that
    // shadows a non-private one, but only in the file where the shadowing
    // happens. We will warn on conflicting non-private declarations in both
    // files.
    if (tc.Context.LangOpts.UsePrivateDiscriminators)
      if (!other->isAccessibleFrom(currentDC))
        continue;

    // If there is a conflict, complain.
    if (conflicting(currentSig, other->getOverloadSignature())) {
      // If the two declarations occur in the same source file, make sure
      // we get the diagnostic ordering to be sensible.
      if (auto otherFile = other->getDeclContext()->getParentSourceFile()) {
        if (currentFile == otherFile &&
            current->getLoc().isValid() &&
            other->getLoc().isValid() &&
            tc.Context.SourceMgr.isBeforeInBuffer(current->getLoc(),
                                                  other->getLoc())) {
          std::swap(current, other);
        }
      }

      tc.diagnose(current, diag::invalid_redecl, current->getFullName());
      tc.diagnose(other, diag::invalid_redecl_prev, other->getFullName());

      current->setInvalid();
      if (current->hasType())
        current->overwriteType(ErrorType::get(tc.Context));
      break;
    }
  }
}

/// Does the context allow pattern bindings that don't bind any variables?
static bool contextAllowsPatternBindingWithoutVariables(DeclContext *dc) {
  
  // Property decls in type context must bind variables.
  if (dc->isTypeContext())
    return false;
  
  // Global variable decls must bind variables, except in scripts.
  if (dc->isModuleScopeContext()) {
    if (dc->getParentSourceFile()
        && dc->getParentSourceFile()->isScriptMode())
      return true;
    
    return false;
  }
  
  return true;
}

/// Validate the given pattern binding declaration.
static void validatePatternBindingDecl(TypeChecker &tc,
                                       PatternBindingDecl *binding,
                                       TypeResolutionOptions extraOptions = {}){
  // If the pattern already has a type, we're done.
  if (binding->getPattern()->hasType() || binding->isBeingTypeChecked())
    return;
  
  binding->setIsBeingTypeChecked();

  // Validate 'static'/'class' on properties in nominal type decls.
  auto StaticSpelling = binding->getStaticSpelling();
  if (StaticSpelling != StaticSpellingKind::None &&
      binding->getDeclContext()->isExtensionContext()) {
    if (Type T = binding->getDeclContext()->getDeclaredTypeInContext()) {
      if (auto NTD = T->getAnyNominal()) {
        if (!isa<ClassDecl>(NTD)) {
          if (StaticSpelling == StaticSpellingKind::KeywordClass) {
            tc.diagnose(binding, diag::class_var_not_in_class)
              .fixItReplace(binding->getStaticLoc(), "static");
            tc.diagnose(NTD, diag::extended_type_declared_here);
          }
        }
      }
    }
  }

  // Check the pattern. PBDs can never affect a function's signature, so pass
  // TR_InExpression.
  TypeResolutionOptions options = TR_InExpression;
  if (binding->getInit()) {
    // If we have an initializer, we can also have unknown types.
    options |= TR_AllowUnspecifiedTypes;
    options |= TR_AllowUnboundGenerics;
  }
  if (tc.typeCheckPattern(binding->getPattern(),
                          binding->getDeclContext(),
                          options)) {
    setBoundVarsTypeError(binding->getPattern(), tc.Context);
    binding->setInvalid();
    binding->getPattern()->setType(ErrorType::get(tc.Context));
    goto done;
  }

  // If the pattern didn't get a type, it's because we ran into some
  // unknown types along the way. We'll need to check the initializer.
  if (!binding->getPattern()->hasType()) {
    if (tc.typeCheckBinding(binding, extraOptions)) {
      setBoundVarsTypeError(binding->getPattern(), tc.Context);
      binding->setInvalid();
      binding->getPattern()->setType(ErrorType::get(tc.Context));
      goto done;
    }
  }
  
  // If the pattern binding appears in a type or library file context, then
  // it must bind at least one variable.
  if (!contextAllowsPatternBindingWithoutVariables(binding->getDeclContext())) {
    llvm::SmallVector<VarDecl*, 2> vars;
    binding->getPattern()->collectVariables(vars);
    if (vars.empty()) {
      // Selector for error message.
      enum : unsigned {
        Property,
        GlobalVariable,
      };
      tc.diagnose(binding->getPattern()->getLoc(),
                  diag::pattern_binds_no_variables,
                  binding->getDeclContext()->isTypeContext()
                                                   ? Property : GlobalVariable);
    }
  }

  // If we have any type-adjusting attributes, apply them here.
  if (binding->getPattern()->hasType())
    if (auto var = binding->getSingleVar())
      tc.checkTypeModifyingDeclAttributes(var);

  // If we're in a generic type context, provide interface types for all of
  // the variables.
  {
    auto dc = binding->getDeclContext();
    if (dc->isGenericContext() && dc->isTypeContext()) {
      binding->getPattern()->forEachVariable([&](VarDecl *var) {
        var->setInterfaceType(
          tc.getInterfaceTypeFromInternalType(dc, var->getType()));
      });
    }

    // For now, we only support static/class variables in specific contexts.
    if (binding->isStatic()) {
      // Selector for unimplemented_type_var message.
      enum : unsigned {
        Misc,
        GenericTypes,
        Classes,
      };
      auto unimplementedStatic = [&](unsigned diagSel) {
        auto staticLoc = binding->getStaticLoc();
        tc.diagnose(staticLoc, diag::unimplemented_type_var,
                    diagSel, binding->getStaticSpelling(),
                    diagSel == Classes)
          .highlight(staticLoc);
      };

      assert(dc->isTypeContext());
      // The parser only accepts 'type' variables in type contexts, so
      // we're either in a nominal type context or an extension.
      NominalTypeDecl *nominal;
      if (auto extension = dyn_cast<ExtensionDecl>(dc)) {
        nominal = extension->getExtendedType()->getAnyNominal();
        assert(nominal);
      } else {
        nominal = cast<NominalTypeDecl>(dc);
      }

      // Non-stored properties are fine.
      if (!binding->hasStorage()) {
        // do nothing

      // Stored type variables in a generic context need to logically
      // occur once per instantiation, which we don't yet handle.
      } else if (dc->isGenericContext()) {
        unimplementedStatic(GenericTypes);
      } else if (dc->isClassOrClassExtensionContext()) {
        auto staticSpelling = binding->getStaticSpelling();
        if (staticSpelling != StaticSpellingKind::KeywordStatic)
          unimplementedStatic(Classes);
      }
    }
  }
  
done:
  binding->setIsBeingTypeChecked(false);
}

void swift::makeFinal(ASTContext &ctx, ValueDecl *D) {
  if (D && !D->isFinal()) {
    D->getAttrs().add(new (ctx) FinalAttr(/*IsImplicit=*/true));
  }
}

void swift::makeDynamic(ASTContext &ctx, ValueDecl *D) {
  if (D && !D->isDynamic()) {
    D->getAttrs().add(new (ctx) DynamicAttr(/*IsImplicit=*/true));
  }
}

/// Configure the implicit 'self' parameter of a function, setting its type,
/// pattern, etc.
///
/// \param func The function whose 'self' is being configured.
/// \param outerGenericParams The generic parameters from the outer scope.
///
/// \returns the type of 'self'.
Type swift::configureImplicitSelf(TypeChecker &tc,
                                  AbstractFunctionDecl *func,
                                  GenericParamList *&outerGenericParams) {
  outerGenericParams = nullptr;

  auto selfDecl = func->getImplicitSelfDecl();

  // Validate the context.
  if (auto nominal = dyn_cast<NominalTypeDecl>(func->getDeclContext())) {
    tc.validateDecl(nominal);
  } else {
    tc.validateExtension(cast<ExtensionDecl>(func->getDeclContext()));
  }

  // Compute the type of self.
  Type selfTy = func->computeSelfType(&outerGenericParams);
  assert(selfDecl && selfTy && "Not a method");

  // 'self' is 'let' for reference types (i.e., classes) or when 'self' is
  // neither inout.
  selfDecl->setLet(!selfTy->is<InOutType>());
  selfDecl->setType(selfTy);

  auto bodyPattern = cast<TypedPattern>(func->getBodyParamPatterns()[0]);
  if (!bodyPattern->getTypeLoc().getTypeRepr())
    bodyPattern->getTypeLoc() = TypeLoc::withoutLoc(selfTy);

  return selfTy;
}

/// Compute the allocating and initializing constructor types for
/// the given constructor.
void swift::configureConstructorType(ConstructorDecl *ctor,
                                     GenericParamList *outerGenericParams,
                                     Type selfType,
                                     Type argType) {
  Type fnType;
  Type allocFnType;
  Type initFnType;
  Type resultType = selfType->getInOutObjectType();
  if (ctor->getFailability() != OTK_None) {
    resultType = OptionalType::get(ctor->getFailability(), resultType);
  }

  // Use the argument names in the argument type.
  argType = argType->getRelabeledType(ctor->getASTContext(), 
                                      ctor->getFullName().getArgumentNames());

  if (GenericParamList *innerGenericParams = ctor->getGenericParams()) {
    innerGenericParams->setOuterParameters(outerGenericParams);
    fnType = PolymorphicFunctionType::get(argType, resultType,
                                          innerGenericParams);
  } else {
    fnType = FunctionType::get(argType, resultType);
  }
  Type selfMetaType = MetatypeType::get(selfType->getInOutObjectType());
  if (outerGenericParams) {
    allocFnType = PolymorphicFunctionType::get(selfMetaType, fnType,
                                               outerGenericParams);
    initFnType = PolymorphicFunctionType::get(selfType, fnType,
                                              outerGenericParams);
  } else {
    allocFnType = FunctionType::get(selfMetaType, fnType);
    initFnType = FunctionType::get(selfType, fnType);
  }
  ctor->setType(allocFnType);
  ctor->setInitializerType(initFnType);
}

static void computeDefaultAccessibility(TypeChecker &TC, ExtensionDecl *ED) {
  if (ED->hasDefaultAccessibility())
    return;

  if (auto *AA = ED->getAttrs().getAttribute<AccessibilityAttr>()) {
    ED->setDefaultAccessibility(AA->getAccess());
    return;
  }

  TC.checkInheritanceClause(ED);
  if (auto nominal = ED->getExtendedType()->getAnyNominal()) {
    TC.validateDecl(nominal);
    ED->setDefaultAccessibility(std::min(nominal->getAccessibility(),
                                         Accessibility::Internal));
  } else {
    // Recover by assuming "internal", which is the most common thing anyway.
    ED->setDefaultAccessibility(Accessibility::Internal);
  }
}

void TypeChecker::computeAccessibility(ValueDecl *D) {
  if (D->hasAccessibility())
    return;

  // Check if the decl has an explicit accessibility attribute.
  if (auto *AA = D->getAttrs().getAttribute<AccessibilityAttr>()) {
    D->setAccessibility(AA->getAccess());

  } else if (auto fn = dyn_cast<FuncDecl>(D)) {
    // Special case for accessors, which inherit the access of their storage.
    // decl. A setter attribute can also override this.
    if (AbstractStorageDecl *storage = fn->getAccessorStorageDecl()) {
      if (storage->hasAccessibility()) {
        if (fn->getAccessorKind() == AccessorKind::IsSetter ||
            fn->getAccessorKind() == AccessorKind::IsMaterializeForSet)
          fn->setAccessibility(storage->getSetterAccessibility());
        else
          fn->setAccessibility(storage->getAccessibility());
      } else {
        computeAccessibility(storage);
      }
    }
  }

  if (!D->hasAccessibility()) {
    DeclContext *DC = D->getDeclContext();
    switch (DC->getContextKind()) {
    case DeclContextKind::SerializedLocal:
    case DeclContextKind::AbstractClosureExpr:
    case DeclContextKind::Initializer:
    case DeclContextKind::TopLevelCodeDecl:
    case DeclContextKind::AbstractFunctionDecl:
      D->setAccessibility(Accessibility::Private);
      break;
    case DeclContextKind::Module:
    case DeclContextKind::FileUnit:
      D->setAccessibility(Accessibility::Internal);
      break;
    case DeclContextKind::NominalTypeDecl: {
      auto nominal = cast<NominalTypeDecl>(DC);
      validateAccessibility(nominal);
      Accessibility access = nominal->getAccessibility();
      if (!isa<ProtocolDecl>(nominal))
        access = std::min(access, Accessibility::Internal);
      D->setAccessibility(access);
      break;
    }
    case DeclContextKind::ExtensionDecl: {
      auto extension = cast<ExtensionDecl>(DC);
      computeDefaultAccessibility(*this, extension);
      D->setAccessibility(extension->getDefaultAccessibility());
    }
    }
  }

  if (auto ASD = dyn_cast<AbstractStorageDecl>(D)) {
    if (auto *AA = D->getAttrs().getAttribute<SetterAccessibilityAttr>())
      ASD->setSetterAccessibility(AA->getAccess());
    else
      ASD->setSetterAccessibility(ASD->getAccessibility());

    if (auto getter = ASD->getGetter())
      computeAccessibility(getter);
    if (auto setter = ASD->getSetter())
      computeAccessibility(setter);
  }
}

namespace {

class TypeAccessibilityChecker : private TypeWalker {
  using TypeAccessibilityCacheMap =
    decltype(TypeChecker::TypeAccessibilityCache);
  TypeAccessibilityCacheMap &Cache;
  SmallVector<Accessibility, 8> AccessStack;

  explicit TypeAccessibilityChecker(TypeAccessibilityCacheMap &cache)
      : Cache(cache) {
    // Always have something on the stack.
    AccessStack.push_back(Accessibility::Private);
  }

  Action walkToTypePre(Type ty) override {
    // Assume failure until we post-visit this node.
    // This will be correct as long as we don't ever have self-referential
    // Types.
    auto cached = Cache.find(ty);
    if (cached != Cache.end()) {
      AccessStack.back() = std::min(AccessStack.back(), cached->second);
      return Action::SkipChildren;
    }

    Accessibility current;
    if (auto alias = dyn_cast<NameAliasType>(ty.getPointer()))
      current = alias->getDecl()->getAccessibility();
    else if (auto nominal = ty->getAnyNominal())
      current = nominal->getAccessibility();
    else
      current = Accessibility::Public;
    AccessStack.push_back(current);

    return Action::Continue;
  }

  Action walkToTypePost(Type ty) override {
    Accessibility last = AccessStack.pop_back_val();
    Cache[ty] = last;
    AccessStack.back() = std::min(AccessStack.back(), last);
    return Action::Continue;
  }

public:
  static Accessibility getAccessibility(Type ty,
                                        TypeAccessibilityCacheMap &cache) {
    ty.walk(TypeAccessibilityChecker(cache));
    return cache[ty];
  }
};

class TypeAccessibilityDiagnoser : private ASTWalker {
  const ComponentIdentTypeRepr *minAccessibilityType = nullptr;

  bool walkToTypeReprPre(TypeRepr *TR) override {
    auto CITR = dyn_cast<ComponentIdentTypeRepr>(TR);
    if (!CITR)
      return true;

    const ValueDecl *VD = getValueDecl(CITR);
    if (!VD)
      return true;

    if (minAccessibilityType) {
      const ValueDecl *minDecl = getValueDecl(minAccessibilityType);
      if (minDecl->getAccessibility() <= VD->getAccessibility())
        return true;
    }

    minAccessibilityType = CITR;
    return true;
  }

public:
  static const ValueDecl *getValueDecl(const ComponentIdentTypeRepr *TR) {
    if (const ValueDecl *VD = TR->getBoundDecl())
      return VD;
    if (Type ty = TR->getBoundType()) {
      if (auto alias = dyn_cast<NameAliasType>(ty.getPointer()))
        return alias->getDecl();
      return ty->getAnyNominal();
    }
    assert(TR->isBoundModule());
    return nullptr;
  }

  static const TypeRepr *findMinAccessibleType(TypeRepr *TR) {
    TypeAccessibilityDiagnoser diagnoser;
    TR->walk(diagnoser);
    return diagnoser.minAccessibilityType;
  }
};
} // end anonymous namespace

/// Checks if the accessibility of the type described by \p TL is at least
/// \p access. If it isn't, calls \p diagnose with a TypeRepr representing the
/// offending part of \p TL.
///
/// The TypeRepr passed to \p diagnose may be null, in which case a particular
/// part of the type that caused the problem could not be found.
static void checkTypeAccessibility(
    TypeChecker &TC, TypeLoc TL, Accessibility access,
    std::function<void(Accessibility, const TypeRepr *)> diagnose) {
  // Don't spend time checking private access; this is always valid.
  // This includes local declarations.
  if (access == Accessibility::Private || !TL.getType())
    return;

  Accessibility typeAccess =
    TypeAccessibilityChecker::getAccessibility(TL.getType(),
                                               TC.TypeAccessibilityCache);
  if (typeAccess >= access)
    return;

  const TypeRepr *complainRepr = nullptr;
  if (TypeRepr *TR = TL.getTypeRepr())
    complainRepr = TypeAccessibilityDiagnoser::findMinAccessibleType(TR);
  diagnose(typeAccess, complainRepr);
}

/// Highlights the given TypeRepr, and adds a note pointing to the type's
/// declaration if possible.
///
/// Just flushes \p diag as is if \p complainRepr is null.
static void highlightOffendingType(TypeChecker &TC, InFlightDiagnostic &diag,
                                   const TypeRepr *complainRepr) {
  if (!complainRepr) {
    diag.flush();
    return;
  }

  diag.highlight(complainRepr->getSourceRange());
  diag.flush();

  if (auto CITR = dyn_cast<ComponentIdentTypeRepr>(complainRepr)) {
    const ValueDecl *VD = TypeAccessibilityDiagnoser::getValueDecl(CITR);
    TC.diagnose(VD, diag::type_declared_here);
  }
}

static void checkGenericParamAccessibility(TypeChecker &TC,
                                           const GenericParamList *params,
                                           const ValueDecl *owner) {
  if (!params)
    return;

  // This must stay in sync with diag::generic_param_access.
  enum {
    AEK_Parameter = 0,
    AEK_Requirement
  } accessibilityErrorKind;
  Optional<Accessibility> minAccess;
  const TypeRepr *complainRepr = nullptr;

  for (auto param : *params) {
    if (param->getInherited().empty())
      continue;
    assert(param->getInherited().size() == 1);
    checkTypeAccessibility(TC, param->getInherited().front(),
                           owner->getAccessibility(),
                           [&](Accessibility typeAccess,
                               const TypeRepr *thisComplainRepr) {
      if (!minAccess || *minAccess > typeAccess) {
        minAccess = typeAccess;
        complainRepr = thisComplainRepr;
        accessibilityErrorKind = AEK_Parameter;
      }
    });
  }

  for (auto &requirement : params->getRequirements()) {
    auto callback = [&](Accessibility typeAccess,
                        const TypeRepr *thisComplainRepr) {
      if (!minAccess || *minAccess > typeAccess) {
        minAccess = typeAccess;
        complainRepr = thisComplainRepr;
        accessibilityErrorKind = AEK_Requirement;
      }
    };
    switch (requirement.getKind()) {
    case RequirementKind::Conformance:
      checkTypeAccessibility(TC, requirement.getSubjectLoc(),
                             owner->getAccessibility(), callback);
      checkTypeAccessibility(TC, requirement.getConstraintLoc(),
                             owner->getAccessibility(), callback);
      break;
    case RequirementKind::SameType:
      checkTypeAccessibility(TC, requirement.getFirstTypeLoc(),
                             owner->getAccessibility(), callback);
      checkTypeAccessibility(TC, requirement.getSecondTypeLoc(),
                             owner->getAccessibility(), callback);
      break;
    case RequirementKind::WitnessMarker:
      break;
    }
  }

  if (minAccess.hasValue()) {
    bool isExplicit = owner->getAttrs().hasAttribute<AccessibilityAttr>() ||
                      isa<ProtocolDecl>(owner->getDeclContext());
    auto diag = TC.diagnose(owner, diag::generic_param_access,
                            owner->getDescriptiveKind(), isExplicit,
                            owner->getAccessibility(), minAccess.getValue(),
                            accessibilityErrorKind);
    highlightOffendingType(TC, diag, complainRepr);
  }
}

/// Check temporary limitations on generic extension deserialization.
static bool checkGenericExtensionLimitations(TypeChecker &TC, const Decl *D) {
  // Don't allow public declarations within an extension of a generic type
  // that occurs in a different module from the generic type definition itself.
  // FIXME: Artificial limitation because we cannot deserialize such extensions
  // safely. The "Foundation" module carefully avoids the bugs here in a way
  // that is not easily checked or communicated to users, so give it a pass.
  auto DC = D->getDeclContext();
  if (isa<ExtensionDecl>(DC) && isa<ValueDecl>(D) &&
      cast<ValueDecl>(D)->getAccessibility() == Accessibility::Public &&
      DC->getDeclaredInterfaceType()->is<BoundGenericType>() &&
      DC->getParentModule()
        != DC->getDeclaredInterfaceType()->getAnyNominal()->getModuleContext()&&
      !(isa<FuncDecl>(D) && cast<FuncDecl>(D)->isAccessor()) &&
      DC->getParentModule()->Name.str() != FOUNDATION_MODULE_NAME) {
    TC.diagnose(D, diag::unsupported_generic_extension,
                DC->getDeclaredInterfaceType());
    return true;
  }

  return false;
}

/// Checks the given declaration's accessibility to make sure it is valid given
/// the way it is defined.
///
/// \p D must be a ValueDecl or a Decl that can appear in a type context.
static void checkAccessibility(TypeChecker &TC, const Decl *D) {
  if (D->isInvalid() || D->isImplicit())
    return;

  checkGenericExtensionLimitations(TC, D);

  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
    llvm_unreachable("cannot appear in a type context");

  case DeclKind::Param:
  case DeclKind::GenericTypeParam:
    llvm_unreachable("does not have accessibility");

  case DeclKind::IfConfig:
    // Does not have accessibility.
  case DeclKind::EnumCase:
    // Handled at the EnumElement level.
  case DeclKind::Var:
    // Handled at the PatternBindingDecl level.
  case DeclKind::Destructor:
    // Always correct.
    return;

  case DeclKind::PatternBinding: {
    auto PBD = cast<PatternBindingDecl>(D);
    bool isTypeContext = PBD->getDeclContext()->isTypeContext();

    llvm::DenseSet<const VarDecl *> seenVars;
    PBD->getPattern()->forEachNode([&](const Pattern *P) {
      if (auto *NP = dyn_cast<NamedPattern>(P)) {
        // Only check individual variables if we didn't check an enclosing
        // TypedPattern.
        const VarDecl *theVar = NP->getDecl();
        if (seenVars.count(theVar) || theVar->isInvalid())
          return;

        checkTypeAccessibility(TC, TypeLoc::withoutLoc(theVar->getType()),
                               theVar->getAccessibility(),
                               [&](Accessibility typeAccess,
                                   const TypeRepr *complainRepr) {
          bool isExplicit =
            theVar->getAttrs().hasAttribute<AccessibilityAttr>();
          auto diag = TC.diagnose(P->getLoc(),
                                  diag::pattern_type_access_inferred,
                                  theVar->isLet(),
                                  isTypeContext,
                                  isExplicit,
                                  theVar->getAccessibility(),
                                  typeAccess,
                                  theVar->getType());
        });
        return;
      }

      auto *TP = dyn_cast<TypedPattern>(P);
      if (!TP)
        return;

      // FIXME: We need an accessibility value to check against, so we pull
      // one out of some random VarDecl in the pattern. They're all going to
      // be the same, but still, ick.
      const VarDecl *anyVar = nullptr;
      TP->forEachVariable([&](VarDecl *V) {
        seenVars.insert(V);
        anyVar = V;
      });
      if (!anyVar)
        return;

      checkGenericExtensionLimitations(TC, anyVar);

      checkTypeAccessibility(TC, TP->getTypeLoc(), anyVar->getAccessibility(),
                             [&](Accessibility typeAccess,
                                 const TypeRepr *complainRepr) {
        bool isExplicit =
          anyVar->getAttrs().hasAttribute<AccessibilityAttr>() ||
          isa<ProtocolDecl>(anyVar->getDeclContext());
        auto diag = TC.diagnose(P->getLoc(), diag::pattern_type_access,
                                anyVar->isLet(),
                                isTypeContext,
                                isExplicit,
                                anyVar->getAccessibility(),
                                typeAccess);
        highlightOffendingType(TC, diag, complainRepr);
      });
    });
    return;
  }

  case DeclKind::TypeAlias: {
    auto TAD = cast<TypeAliasDecl>(D);

    checkTypeAccessibility(TC, TAD->getUnderlyingTypeLoc(),
                           TAD->getAccessibility(),
                           [&](Accessibility typeAccess,
                               const TypeRepr *complainRepr) {
      bool isExplicit = TAD->getAttrs().hasAttribute<AccessibilityAttr>();
      auto diag = TC.diagnose(TAD, diag::type_alias_underlying_type_access,
                              isExplicit, TAD->getAccessibility(),
                              typeAccess);
      highlightOffendingType(TC, diag, complainRepr);
    });

    return;
  }

  case DeclKind::AssociatedType: {
    auto assocType = cast<AssociatedTypeDecl>(D);

    // This must stay in sync with diag::associated_type_access.
    enum {
      AEK_DefaultDefinition = 0,
      AEK_Requirement
    } accessibilityErrorKind;
    Optional<Accessibility> minAccess;
    const TypeRepr *complainRepr = nullptr;

    std::for_each(assocType->getInherited().begin(),
                  assocType->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccessibility(TC, requirement,
                             assocType->getAccessibility(),
                             [&](Accessibility typeAccess,
                                 const TypeRepr *thisComplainRepr) {
        if (!minAccess || *minAccess > typeAccess) {
          minAccess = typeAccess;
          complainRepr = thisComplainRepr;
          accessibilityErrorKind = AEK_Requirement;
        }
      });
    });
    checkTypeAccessibility(TC, assocType->getDefaultDefinitionLoc(),
                           assocType->getAccessibility(),
                           [&](Accessibility typeAccess,
                               const TypeRepr *thisComplainRepr) {
      if (!minAccess || *minAccess > typeAccess) {
        minAccess = typeAccess;
        complainRepr = thisComplainRepr;
        accessibilityErrorKind = AEK_DefaultDefinition;
      }
    });

    if (minAccess) {
      auto diag = TC.diagnose(assocType, diag::associated_type_access,
                              assocType->getAccessibility(),
                              *minAccess, accessibilityErrorKind);
      highlightOffendingType(TC, diag, complainRepr);
    }
    return;
  }

  case DeclKind::Enum: {
    auto ED = cast<EnumDecl>(D);

    checkGenericParamAccessibility(TC, ED->getGenericParams(), ED);

    if (ED->hasRawType()) {
      Type rawType = ED->getRawType();
      auto rawTypeLocIter = std::find_if(ED->getInherited().begin(),
                                         ED->getInherited().end(),
                                         [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        return inherited.getType().getPointer() == rawType.getPointer();
      });
      if (rawTypeLocIter == ED->getInherited().end())
        return;
      checkTypeAccessibility(TC, *rawTypeLocIter, ED->getAccessibility(),
                             [&](Accessibility typeAccess,
                                 const TypeRepr *complainRepr) {
        bool isExplicit = ED->getAttrs().hasAttribute<AccessibilityAttr>();
        auto diag = TC.diagnose(ED, diag::enum_raw_type_access,
                                isExplicit, ED->getAccessibility(),
                                typeAccess);
        highlightOffendingType(TC, diag, complainRepr);
      });
    }

    return;
  }

  case DeclKind::Struct: {
    auto SD = cast<StructDecl>(D);
    checkGenericParamAccessibility(TC, SD->getGenericParams(), SD);
    return;
  }

  case DeclKind::Class: {
    auto CD = cast<ClassDecl>(D);

    checkGenericParamAccessibility(TC, CD->getGenericParams(), CD);

    if (CD->hasSuperclass()) {
      Type superclass = CD->getSuperclass();
      auto superclassLocIter = std::find_if(CD->getInherited().begin(),
                                            CD->getInherited().end(),
                                            [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        return inherited.getType().getPointer() == superclass.getPointer();
      });
      if (superclassLocIter == CD->getInherited().end())
        return;
      checkTypeAccessibility(TC, *superclassLocIter, CD->getAccessibility(),
                             [&](Accessibility typeAccess,
                                 const TypeRepr *complainRepr) {
        bool isExplicit = CD->getAttrs().hasAttribute<AccessibilityAttr>();
        auto diag = TC.diagnose(CD, diag::class_super_access,
                                isExplicit, CD->getAccessibility(),
                                typeAccess);
        highlightOffendingType(TC, diag, complainRepr);
      });
    }

    return;
  }

  case DeclKind::Protocol: {
    auto proto = cast<ProtocolDecl>(D);

    Optional<Accessibility> minAccess;
    const TypeRepr *complainRepr = nullptr;

    std::for_each(proto->getInherited().begin(),
                  proto->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccessibility(TC, requirement, proto->getAccessibility(),
                             [&](Accessibility typeAccess,
                                 const TypeRepr *thisComplainRepr) {
        if (!minAccess || *minAccess > typeAccess) {
          minAccess = typeAccess;
          complainRepr = thisComplainRepr;
        }
      });
    });

    if (minAccess) {
      bool isExplicit = proto->getAttrs().hasAttribute<AccessibilityAttr>();
      auto diag = TC.diagnose(proto, diag::protocol_refine_access,
                              isExplicit,
                              proto->getAccessibility(),
                              *minAccess);
      highlightOffendingType(TC, diag, complainRepr);
    }
    return;
  }

  case DeclKind::Subscript: {
    auto SD = cast<SubscriptDecl>(D);

    Optional<Accessibility> minAccess;
    const TypeRepr *complainRepr = nullptr;
    bool problemIsElement = false;
    SD->getIndices()->forEachNode([&](const Pattern *P) {
      auto *TP = dyn_cast<TypedPattern>(P);
      if (!TP)
        return;

      checkTypeAccessibility(TC, TP->getTypeLoc(), SD->getAccessibility(),
                             [&](Accessibility typeAccess,
                                 const TypeRepr *thisComplainRepr) {
        if (!minAccess || *minAccess > typeAccess) {
          minAccess = typeAccess;
          complainRepr = thisComplainRepr;
        }
      });
    });

    checkTypeAccessibility(TC, SD->getElementTypeLoc(),
                           SD->getAccessibility(),
                           [&](Accessibility typeAccess,
                               const TypeRepr *thisComplainRepr) {
      if (!minAccess || *minAccess > typeAccess) {
        minAccess = typeAccess;
        complainRepr = thisComplainRepr;
        problemIsElement = true;
      }
    });

    if (minAccess) {
      bool isExplicit = SD->getAttrs().hasAttribute<AccessibilityAttr>() ||
                        isa<ProtocolDecl>(SD->getDeclContext());
      auto diag = TC.diagnose(SD, diag::subscript_type_access,
                              isExplicit,
                              SD->getAccessibility(),
                              *minAccess,
                              problemIsElement);
      highlightOffendingType(TC, diag, complainRepr);
    }
    return;
  }

  case DeclKind::Func:
    if (cast<FuncDecl>(D)->isAccessor())
      return;
    SWIFT_FALLTHROUGH;
  case DeclKind::Constructor: {
    auto fn = cast<AbstractFunctionDecl>(D);
    bool isTypeContext = fn->getDeclContext()->isTypeContext();

    checkGenericParamAccessibility(TC, fn->getGenericParams(), fn);

    // This must stay in sync with diag::associated_type_access.
    enum {
      FK_Function = 0,
      FK_Method,
      FK_Initializer
    };

    Optional<Accessibility> minAccess;
    const TypeRepr *complainRepr = nullptr;
    bool problemIsResult = false;
    std::for_each(fn->getBodyParamPatterns().begin() + isTypeContext,
                  fn->getBodyParamPatterns().end(),
                  [&](const Pattern *paramList) {
      paramList->forEachNode([&](const Pattern *P) {
        auto *TP = dyn_cast<TypedPattern>(P);
        if (!TP)
          return;

        checkTypeAccessibility(TC, TP->getTypeLoc(), fn->getAccessibility(),
                               [&](Accessibility typeAccess,
                                   const TypeRepr *thisComplainRepr) {
          if (!minAccess || *minAccess > typeAccess) {
            minAccess = typeAccess;
            complainRepr = thisComplainRepr;
          }
        });
      });
    });

    if (auto FD = dyn_cast<FuncDecl>(fn)) {
      checkTypeAccessibility(TC, FD->getBodyResultTypeLoc(),
                             fn->getAccessibility(),
                             [&](Accessibility typeAccess,
                                 const TypeRepr *thisComplainRepr) {
        if (!minAccess || *minAccess > typeAccess) {
          minAccess = typeAccess;
          complainRepr = thisComplainRepr;
          problemIsResult = true;
        }
      });
    }

    if (minAccess) {
      bool isExplicit = fn->getAttrs().hasAttribute<AccessibilityAttr>() ||
                        isa<ProtocolDecl>(D->getDeclContext());
      auto diag = TC.diagnose(fn, diag::function_type_access,
                              isExplicit,
                              fn->getAccessibility(),
                              *minAccess,
                              isa<ConstructorDecl>(fn) ? FK_Initializer :
                                isTypeContext ? FK_Method : FK_Function,
                              problemIsResult);
      highlightOffendingType(TC, diag, complainRepr);
    }
    return;
  }

  case DeclKind::EnumElement: {
    auto EED = cast<EnumElementDecl>(D);

    if (!EED->hasArgumentType())
      return;
    checkTypeAccessibility(TC, EED->getArgumentTypeLoc(),
                           EED->getAccessibility(),
                           [&](Accessibility typeAccess,
                               const TypeRepr *complainRepr) {
      auto diag = TC.diagnose(EED, diag::enum_case_access,
                              EED->getAccessibility(), typeAccess);
      highlightOffendingType(TC, diag, complainRepr);
    });

    return;
  }
  }
}

/// Returns true if \p VD should be exposed to Objective-C iff it is
/// representable in Objective-C.
static bool isImplicitlyObjC(const ValueDecl *VD, bool allowImplicit = false) {
  if (VD->isInvalid())
    return false;
  if (!allowImplicit && VD->isImplicit())
    return false;

  // If this declaration overrides an @objc declaration, it is
  // implicitly @objc.
  if (auto overridden = VD->getOverriddenDecl()) {
    if (overridden->isObjC())
      return true;
  }

  if (VD->getAccessibility() == Accessibility::Private)
    return false;

  Type contextTy = VD->getDeclContext()->getDeclaredTypeInContext();
  auto classContext = contextTy->getClassOrBoundGenericClass();
  if (!classContext)
    return false;
  return classContext->isObjC();
}

/// If we need to infer 'dynamic', do so now.
///
/// This occurs when
/// - it is implied by an attribute like @NSManaged
/// - we need to dynamically dispatch to a method in an extension.
///
/// FIXME: The latter reason is a hack. We should figure out how to safely
/// put extension methods into the class vtable.
static void inferDynamic(ASTContext &ctx, ValueDecl *D) {
  // If we can't infer dynamic here, don't.
  if (!DeclAttribute::canAttributeAppearOnDecl(DAK_Dynamic, D))
    return;

  // Only 'objc' declarations use 'dynamic'.
  if (!D->isObjC() || D->hasClangNode())
    return;

  // Only introduce 'dynamic' on declarations...
  if (isa<ExtensionDecl>(D->getDeclContext())) {
    // ...in extensions that don't override other declarations.
    if (D->getOverriddenDecl())
      return;
  } else {
    // ...and in classes on decls marked @NSManaged.
    if (!D->getAttrs().hasAttribute<NSManagedAttr>())
      return;
  }

  // The presence of 'dynamic' or 'final' blocks the inference of 'dynamic'.
  if (D->isDynamic() || D->isFinal())
    return;

  // Add the 'dynamic' attribute.
  D->getAttrs().add(new (ctx) DynamicAttr(/*isImplicit=*/true));
}

/// Check runtime functions responsible for implicit bridging of Objective-C
/// types.
static void checkObjCBridgingFunctions(TypeChecker &TC,
                                       Module *mod,
                                       StringRef bridgedTypeName,
                                       StringRef forwardConversion,
                                       StringRef reverseConversion) {
  assert(mod);
  Module::AccessPathTy unscopedAccess = {};
  SmallVector<ValueDecl *, 4> results;
  
  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(bridgedTypeName),
                   NLKind::QualifiedLookup, results);
  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(forwardConversion),
                   NLKind::QualifiedLookup, results);
  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(reverseConversion),
                   NLKind::QualifiedLookup, results);
  
  for (auto D : results)
    TC.validateDecl(D);
}

static void checkBridgedFunctions(TypeChecker &TC) {
  if (TC.HasCheckedBridgeFunctions)
    return;
  
  TC.HasCheckedBridgeFunctions = true;
  
  #define BRIDGE_TYPE(BRIDGED_MOD, BRIDGED_TYPE, _, NATIVE_TYPE, OPT) \
  Identifier ID_##BRIDGED_MOD = TC.Context.getIdentifier(#BRIDGED_MOD);\
  if (Module *module = TC.Context.getLoadedModule(ID_##BRIDGED_MOD)) {\
    checkObjCBridgingFunctions(TC, module, #BRIDGED_TYPE, \
    "_convert" #BRIDGED_TYPE "To" #NATIVE_TYPE, \
    "_convert" #NATIVE_TYPE "To" #BRIDGED_TYPE); \
  }
  #include "swift/SIL/BridgedTypes.def"
  
  if (Module *module = TC.Context.getLoadedModule(ID_Foundation)) {
    checkObjCBridgingFunctions(TC, module, "NSArray",
                               "_convertNSArrayToArray",
                               "_convertArrayToNSArray");
    checkObjCBridgingFunctions(TC, module, "NSDictionary",
                               "_convertNSDictionaryToDictionary",
                               "_convertDictionaryToNSDictionary");
    checkObjCBridgingFunctions(TC, module, "NSSet",
                               "_convertNSSetToSet",
                               "_convertSetToNSSet");
  }
}

/// Mark the given declarations as being Objective-C compatible (or
/// not) as appropriate.
void swift::markAsObjC(TypeChecker &TC, ValueDecl *D, bool isObjC) {
  D->setIsObjC(isObjC);
  
  if (isObjC) {
    // Make sure we have the appropriate bridging operations.
    checkBridgedFunctions(TC);

    // Record the name of this Objective-C method in its class.
    if (auto classDecl
          = D->getDeclContext()->isClassOrClassExtensionContext()) {
      if (auto method = dyn_cast<AbstractFunctionDecl>(D)) {
        // If we are overriding another method, make sure the
        // selectors line up.
        if (auto baseMethod = method->getOverriddenDecl()) {
          ObjCSelector baseSelector = baseMethod->getObjCSelector();
          if (baseSelector != method->getObjCSelector()) {
            // The selectors differ. If the method's selector was
            // explicitly specified, this is an error. Otherwise, we
            // inherit the selector.
            if (auto attr = method->getAttrs().getAttribute<ObjCAttr>()) {
              if (attr->hasName() && !attr->isNameImplicit()) {
                llvm::SmallString<64> baseScratch;
                TC.diagnose(attr->AtLoc,
                            diag::objc_override_method_selector_mismatch,
                            *attr->getName(), baseSelector)
                  .fixItReplaceChars(attr->getNameLocs().front(),
                                     attr->getRParenLoc(),
                                     baseSelector.getString(baseScratch));
                TC.diagnose(baseMethod, diag::overridden_here);
              }

              // Override the name on the attribute.
              const_cast<ObjCAttr *>(attr)->setName(baseSelector,
                                                    /*implicit=*/true);
            } else {
              method->getAttrs().add(ObjCAttr::create(TC.Context,
                                                      baseSelector,
                                                      true));
            }
          }
        }

        classDecl->recordObjCMethod(method);

        // Swift does not permit class methods named "load".
        if (!method->isInstanceMember()) {
          auto selector = method->getObjCSelector();
          if (selector.getNumArgs() == 0 &&
              selector.getSelectorPieces()[0] == TC.Context.Id_load) {
            auto diagInfo = getObjCMethodDiagInfo(method);
            TC.diagnose(method, diag::objc_class_method_load,
                        diagInfo.first, diagInfo.second);
          }
        }
      } else if (auto var = dyn_cast<VarDecl>(D)) {
        // If we are overriding a property, make sure that the
        // Objective-C names of the properties match.
        if (auto baseVar = var->getOverriddenDecl()) {
          if (var->getObjCPropertyName() != baseVar->getObjCPropertyName()) {
            Identifier baseName = baseVar->getObjCPropertyName();
            ObjCSelector baseSelector(TC.Context, 0, baseName);

            // If not, see whether we can implicitly adjust.
            if (auto attr = var->getAttrs().getAttribute<ObjCAttr>()) {
              if (attr->hasName() && !attr->isNameImplicit()) {
                TC.diagnose(attr->AtLoc,
                            diag::objc_override_property_name_mismatch,
                            attr->getName()->getSelectorPieces()[0],
                            baseName)
                  .fixItReplaceChars(attr->getNameLocs().front(),
                                     attr->getRParenLoc(),
                                     baseName.str());
                TC.diagnose(baseVar, diag::overridden_here);
              }

              // Override the name on the attribute.
              const_cast<ObjCAttr *>(attr)->setName(baseSelector,
                                                    /*implicit=*/true);
            } else {
              var->getAttrs().add(ObjCAttr::create(TC.Context,
                                                   baseSelector,
                                                   true));
            }
          }
        }
      }
    }

    return;
  } 

  // FIXME: For now, only @objc declarations can be dynamic.
  if (auto attr = D->getAttrs().getAttribute<DynamicAttr>(D))
    attr->setInvalid();
}

/// Given the raw value literal expression for an enum case, produces the
/// auto-incremented raw value for the subsequent case, or returns null if
/// the value is not auto-incrementable.
static LiteralExpr *getAutoIncrementedLiteralExpr(TypeChecker &TC,
                                                  Type rawTy,
                                                  EnumElementDecl *forElt,
                                                  LiteralExpr *prevValue) {
  // If there was no previous value, start from zero.
  if (!prevValue) {
    // The raw type must be integer literal convertible for this to work.
    ProtocolDecl *ilcProto =
      TC.getProtocol(forElt->getLoc(),
                     KnownProtocolKind::IntegerLiteralConvertible);
    if (!TC.conformsToProtocol(rawTy, ilcProto, forElt->getDeclContext(),
                               false)) {
      TC.diagnose(forElt->getLoc(),
                  diag::enum_non_integer_convertible_raw_type_no_value);
      return nullptr;
    }
    
    return new (TC.Context) IntegerLiteralExpr("0", SourceLoc(),
                                               /*Implicit=*/true);
  }
  
  if (auto intLit = dyn_cast<IntegerLiteralExpr>(prevValue)) {
    APInt nextVal = intLit->getValue() + 1;
    bool negative = nextVal.slt(0);
    if (negative)
      nextVal = -nextVal;
    
    llvm::SmallString<10> nextValStr;
    nextVal.toStringSigned(nextValStr);
    auto expr = new (TC.Context)
      IntegerLiteralExpr(TC.Context.AllocateCopy(StringRef(nextValStr)),
                         SourceLoc(), /*Implicit=*/true);
    if (negative)
      expr->setNegative(SourceLoc());
    
    return expr;
  }
  
  TC.diagnose(forElt->getLoc(),
              diag::enum_non_integer_raw_value_auto_increment);
  return nullptr;
}

static void checkEnumRawValues(TypeChecker &TC, EnumDecl *ED) {
  Type rawTy = ED->getRawType();

  if (!rawTy) {
    // @objc enums must have a raw type.
    if (ED->isObjC())
      TC.diagnose(ED->getNameLoc(), diag::objc_enum_no_raw_type);
    return;
  }

  rawTy = ArchetypeBuilder::mapTypeIntoContext(ED, rawTy);

  if (ED->isObjC()) {
    // @objc enums must have a raw type that's an ObjC-representable
    // integer type.
    if (!TC.isCIntegerType(ED, rawTy)) {
      TC.diagnose(ED->getInherited().front().getSourceRange().Start,
                  diag::objc_enum_raw_type_not_integer,
                  rawTy);
      ED->getInherited().front().setInvalidType(TC.Context);
      return;
    }
  } else {
    // Swift enums require that the raw type is convertible from one of the
    // primitive literal protocols.
    static auto literalProtocolKinds = {
      KnownProtocolKind::IntegerLiteralConvertible,
      KnownProtocolKind::FloatLiteralConvertible,
      KnownProtocolKind::UnicodeScalarLiteralConvertible,
      KnownProtocolKind::ExtendedGraphemeClusterLiteralConvertible,
      KnownProtocolKind::StringLiteralConvertible
    };
    bool literalConvertible = std::any_of(literalProtocolKinds.begin(),
                                          literalProtocolKinds.end(),
                                          [&](KnownProtocolKind protoKind) {
      ProtocolDecl *proto = TC.getProtocol(ED->getLoc(), protoKind);
      return TC.conformsToProtocol(rawTy, proto, ED->getDeclContext(),
                                   /*inExpression*/false);
    });

    if (!literalConvertible) {
      TC.diagnose(ED->getInherited().front().getSourceRange().Start,
                  diag::raw_type_not_literal_convertible,
                  rawTy);
      ED->getInherited().front().setInvalidType(TC.Context);
      return;
    }
  }

  // We need at least one case to have a raw value.
  if (ED->getAllElements().empty()) {
    TC.diagnose(ED->getInherited().front().getSourceRange().Start,
                diag::empty_enum_raw_type);
    return;
  }

  // Check the raw values of the cases.
  LiteralExpr *prevValue = nullptr;
  EnumElementDecl *lastExplicitValueElt = nullptr;

  // Keep a map we can use to check for duplicate case values.
  llvm::SmallDenseMap<RawValueKey, RawValueSource, 8> uniqueRawValues;

  for (auto elt : ED->getAllElements()) {
    if (elt->isInvalid())
      continue;

    // We don't yet support raw values on payload cases.
    if (elt->hasArgumentType()) {
      TC.diagnose(elt->getLoc(),
                  diag::enum_with_raw_type_case_with_argument);
      TC.diagnose(ED->getInherited().front().getSourceRange().Start,
                  diag::enum_raw_type_here, rawTy);
      continue;
    }
    
    // Check the raw value expr, if we have one.
    if (auto *rawValue = elt->getRawValueExpr()) {
      Expr *typeCheckedExpr = rawValue;
      if (!TC.typeCheckExpression(typeCheckedExpr, ED, rawTy,
                                  /*contextualType=*/Type(),
                                  /*discarded=*/false)) {
        elt->setTypeCheckedRawValueExpr(typeCheckedExpr);
      }
      lastExplicitValueElt = elt;
    } else {
      // If the enum element has no explicit raw value, try to
      // autoincrement from the previous value, or start from zero if this
      // is the first element.
      auto nextValue = getAutoIncrementedLiteralExpr(TC, rawTy, elt, prevValue);
      if (!nextValue) {
        break;
      }
      elt->setRawValueExpr(nextValue);
      Expr *typeChecked = nextValue;
      if (!TC.typeCheckExpression(typeChecked, ED, rawTy, Type(), false))
        elt->setTypeCheckedRawValueExpr(typeChecked);
    }
    prevValue = elt->getRawValueExpr();
    assert(prevValue && "continued without setting raw value of enum case");
    
    // Check that the raw value is unique.
    RawValueKey key(elt->getRawValueExpr());
    RawValueSource source{elt, lastExplicitValueElt};

    auto insertIterPair = uniqueRawValues.insert({key, source});
    if (insertIterPair.second)
      continue;

    // Diagnose the duplicate value.
    SourceLoc diagLoc = elt->getRawValueExpr()->isImplicit()
        ? elt->getLoc() : elt->getRawValueExpr()->getLoc();
    TC.diagnose(diagLoc, diag::enum_raw_value_not_unique);
    assert(lastExplicitValueElt &&
           "should not be able to have non-unique raw values when "
           "relying on autoincrement");
    if (lastExplicitValueElt != elt)
      TC.diagnose(lastExplicitValueElt->getRawValueExpr()->getLoc(),
                  diag::enum_raw_value_incrementing_from_here);

    RawValueSource prevSource = insertIterPair.first->second;
    auto foundElt = prevSource.sourceElt;
    diagLoc = foundElt->getRawValueExpr()->isImplicit()
        ? foundElt->getLoc() : foundElt->getRawValueExpr()->getLoc();
    TC.diagnose(diagLoc, diag::enum_raw_value_used_here);
    if (foundElt != prevSource.lastExplicitValueElt) {
      if (prevSource.lastExplicitValueElt)
        TC.diagnose(prevSource.lastExplicitValueElt
                      ->getRawValueExpr()->getLoc(),
                    diag::enum_raw_value_incrementing_from_here);
      else
        TC.diagnose(ED->getAllElements().front()->getLoc(),
                    diag::enum_raw_value_incrementing_from_zero);
    }
  }
}

namespace {
class DeclChecker : public DeclVisitor<DeclChecker> {
public:
  TypeChecker &TC;

  // For library-style parsing, we need to make two passes over the global
  // scope.  These booleans indicate whether this is currently the first or
  // second pass over the global scope (or neither, if we're in a context where
  // we only visit each decl once).
  unsigned IsFirstPass : 1;
  unsigned IsSecondPass : 1;

  DeclChecker(TypeChecker &TC, bool IsFirstPass, bool IsSecondPass)
      : TC(TC), IsFirstPass(IsFirstPass), IsSecondPass(IsSecondPass) {}

  void visit(Decl *decl) {
    
    DeclVisitor<DeclChecker>::visit(decl);

    if (auto valueDecl = dyn_cast<ValueDecl>(decl)) {
      checkRedeclaration(TC, valueDecl);
    }
  }

  //===--------------------------------------------------------------------===//
  // Helper Functions.
  //===--------------------------------------------------------------------===//

  static bool isPrivateConformer(const ExtensionDecl *ED) {
    return ED->getDefaultAccessibility() == Accessibility::Private;
  }

  static bool isPrivateConformer(const NominalTypeDecl *NTD) {
    return NTD->getAccessibility() == Accessibility::Private;
  }

  template<typename NominalOrExtensionDecl>
  void checkExplicitConformance(NominalOrExtensionDecl *D, Type T) {
    SmallVector<ProtocolConformance *, 4> conformances;
    ReferencedNameTracker *tracker = nullptr;
    if (SourceFile *SF = D->getParentSourceFile())
      tracker = SF->getReferencedNameTracker();

    // Don't force delayed protocols to be created if they haven't already been
    // resolved.
    for (auto proto : D->getProtocols(false)) {
      ProtocolConformance *conformance = nullptr;
      // FIXME: Better location info
      (void)TC.conformsToProtocol(T, proto, D, /*expr=*/false, &conformance,
                                  D->getStartLoc(), D);
      conformances.push_back(conformance);

      if (tracker)
        tracker->addUsedNominal(proto, !isPrivateConformer(D));
    }

    D->setConformances(D->getASTContext().AllocateCopy(conformances));
  }

  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//
  
  void visitImportDecl(ImportDecl *ID) {
    TC.checkDeclAttributesEarly(ID);
    TC.checkDeclAttributes(ID);
  }

  void visitOperatorDecl(OperatorDecl *OD) {
    TC.checkDeclAttributesEarly(OD);
    TC.checkDeclAttributes(OD);
  }

  void visitBoundVariable(VarDecl *VD) {
    if (!VD->getType()->isMaterializable()) {
      TC.diagnose(VD->getStartLoc(), diag::var_type_not_materializable,
                  VD->getType());
      VD->overwriteType(ErrorType::get(TC.Context));
      VD->setInvalid();
    }

    TC.validateDecl(VD);

    // WARNING: Anything you put in this function will only be run when the
    // VarDecl is fully type-checked within its own file. It will NOT be run
    // when the VarDecl is merely used from another file.

    // Reject cases where this is a variable that has storage but it isn't
    // allowed.
    if (VD->hasStorage()) {
      // In a protocol context, variables written as "var x : Int" are errors
      // and recovered by building a computed property with just a getter.
      // Diagnose this and create the getter decl now.
      if (isa<ProtocolDecl>(VD->getDeclContext())) {
        if (VD->isLet())
          TC.diagnose(VD->getLoc(),
                      diag::protocol_property_must_be_computed_var);
        else
          TC.diagnose(VD->getLoc(), diag::protocol_property_must_be_computed);

        convertStoredVarInProtocolToComputed(VD, TC);
      } else if (isa<EnumDecl>(VD->getDeclContext()) &&
                 !VD->isStatic()) {
        // Enums can only have computed properties.
        TC.diagnose(VD->getLoc(), diag::enum_stored_property);
        VD->setInvalid();
        VD->overwriteType(ErrorType::get(TC.Context));
      } else if (isa<ExtensionDecl>(VD->getDeclContext()) &&
                 !VD->isStatic()) {
        TC.diagnose(VD->getLoc(), diag::extension_stored_property);
        VD->setInvalid();
        VD->overwriteType(ErrorType::get(TC.Context));
      } // Objective-C compatible class types with static stored properties
        // can be accessed as Objective-C class methods but need accessors
        // to do so.
        else if (VD->isObjC() && VD->isStatic() && VD->hasStorage())
          if (!VD->hasAccessorFunctions())
            addTrivialAccessorsToStorage(VD, TC);
    }

    // Synthesize accessors for lazy, all checking already been performed.
    if (VD->getAttrs().hasAttribute<LazyAttr>() && !VD->isStatic() &&
        !VD->getGetter()->hasBody())
      TC.completeLazyVarImplementation(VD);

    // If this is a willSet/didSet property, synthesize the getter and setter
    // decl.
    if (VD->hasObservers() && !VD->getGetter()->getBody())
      synthesizeObservingAccessors(VD, TC);

    // If this is a get+mutableAddress property, synthesize the setter body.
    if (VD->getStorageKind() == VarDecl::ComputedWithMutableAddress &&
        !VD->getSetter()->getBody()) {
      synthesizeSetterForMutableAddressedStorage(VD, TC);
    }

    // Synthesize materializeForSet in non-protocol contexts.
    if (auto materializeForSet = VD->getMaterializeForSetFunc()) {
      Type containerTy = VD->getDeclContext()->getDeclaredTypeOfContext();
      if (!containerTy || !containerTy->is<ProtocolType>()) {
        synthesizeMaterializeForSet(materializeForSet, VD, TC);
        TC.typeCheckDecl(materializeForSet, true);
        TC.typeCheckDecl(materializeForSet, false);
      }
    }

    TC.checkDeclAttributes(VD);
  }


  void visitBoundVars(Pattern *P) {
    P->forEachVariable([&] (VarDecl *VD) { this->visitBoundVariable(VD); });
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    validatePatternBindingDecl(TC, PBD);
    if (PBD->isInvalid())
      return;
    
    if (!IsFirstPass) {
      if (PBD->getInit() && !PBD->wasInitChecked()) {
        if (TC.typeCheckBinding(PBD)) {
          PBD->setInvalid();
          if (!PBD->getPattern()->hasType()) {
            PBD->getPattern()->setType(ErrorType::get(TC.Context));
            setBoundVarsTypeError(PBD->getPattern(), TC.Context);
            return;
          }
        }
      }
    }

    TC.checkDeclAttributesEarly(PBD);

    if (!IsSecondPass) {
      // Type check each VarDecl in that his PatternBinding handles.
      visitBoundVars(PBD->getPattern());

      // If we have a type but no initializer, check whether the type is
      // default-initializable. If so, do it.
      if (PBD->getPattern()->hasType() && !PBD->hasInit() &&
          PBD->hasStorage() && !PBD->getPattern()->getType()->is<ErrorType>()) {

        // If we have a type-adjusting attribute (like ownership), apply it now.
        if (auto var = PBD->getSingleVar())
          TC.checkTypeModifyingDeclAttributes(var);

        // Decide whether we should suppress default initialization.
        bool suppressDefaultInit = false;
        PBD->getPattern()->forEachVariable([&](VarDecl *var) {
          // @NSManaged properties never get default initialized, nor do
          // debugger variables and immutable properties.
          if (var->getAttrs().hasAttribute<NSManagedAttr>() ||
              var->isDebuggerVar() ||
              var->isLet())
            suppressDefaultInit = true;
        });

        if (!suppressDefaultInit) {
          auto type = PBD->getPattern()->getType();
          if (auto defaultInit = buildDefaultInitializer(TC, type)) {
            // If we got a default initializer, install it and re-type-check it
            // to make sure it is properly coerced to the pattern type.
            PBD->setInit(defaultInit, /*checked=*/false);
            TC.typeCheckBinding(PBD);
          }
        }
      }
    }

    bool isInSILMode = false;
    if (auto sourceFile = PBD->getDeclContext()->getParentSourceFile())
      isInSILMode = sourceFile->Kind == SourceFileKind::SIL;
    bool isTypeContext = PBD->getDeclContext()->isTypeContext();

    // If this is a declaration without an initializer, reject code if
    // uninitialized vars are not allowed.
    if (!PBD->hasInit() && !isInSILMode) {
      PBD->getPattern()->forEachVariable([&](VarDecl *var) {
        // If the variable has no storage, it never needs an initializer.
        if (!var->hasStorage())
          return;

        auto *varDC = var->getDeclContext();

        // Non-member observing properties need an initializer.
        if (var->getStorageKind() == VarDecl::StoredWithObservers &&
            !isTypeContext) {
          TC.diagnose(var->getLoc(), diag::observingprop_requires_initializer);
          PBD->setInvalid();
          var->setInvalid();
          if (!var->hasType())
            var->setType(ErrorType::get(TC.Context));
          return;
        }

        // Static/class declarations require an initializer unless in a
        // protocol.
        if (var->isStatic() && !isa<ProtocolDecl>(varDC)) {
          TC.diagnose(var->getLoc(), diag::static_requires_initializer,
                      var->getCorrectStaticSpelling());
          PBD->setInvalid();
          var->setInvalid();
          if (!var->hasType())
            var->setType(ErrorType::get(TC.Context));
          return;
        }

        // Global variables require an initializer (except in top level code).
        if (varDC->isModuleScopeContext() &&
            !varDC->getParentSourceFile()->isScriptMode()) {
          TC.diagnose(var->getLoc(),
                      diag::global_requires_initializer, var->isLet());
          PBD->setInvalid();
          var->setInvalid();
          if (!var->hasType())
            var->setType(ErrorType::get(TC.Context));
          return;
        }
      });
    }

    if (!IsFirstPass)
      checkAccessibility(TC, PBD);

    TC.checkDeclAttributes(PBD);
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    if (IsSecondPass) {
      checkAccessibility(TC, SD);
      return;
    }

    if (SD->hasType())
      return;

    assert(SD->getDeclContext()->isTypeContext() &&
           "Decl parsing must prevent subscripts outside of types!");

    TC.checkDeclAttributesEarly(SD);
    TC.computeAccessibility(SD);

    auto dc = SD->getDeclContext();
    bool isInvalid = TC.validateType(SD->getElementTypeLoc(), dc);
    isInvalid |= TC.typeCheckPattern(SD->getIndices(), dc, None);

    if (isInvalid) {
      SD->overwriteType(ErrorType::get(TC.Context));
      SD->setInvalid();
    } else {
      // Hack to deal with types already getting set during type validation
      // above.
      if (SD->hasType())
        return;

      // Relabel the indices according to the subscript name.
      auto indicesType = SD->getIndices()->getType();
      SD->setType(FunctionType::get(indicesType, SD->getElementType()));

      // If we're in a generic context, set the interface type.
      if (dc->isGenericContext()) {
        auto indicesTy = TC.getInterfaceTypeFromInternalType(
                           dc, indicesType);
        auto elementTy = TC.getInterfaceTypeFromInternalType(
                           dc, SD->getElementType());
        SD->setInterfaceType(FunctionType::get(indicesTy, elementTy));
      }
    }

    validateAttributes(TC, SD);

    if (!checkOverrides(TC, SD)) {
      // If a subscript has an override attribute but does not override
      // anything, complain.
      if (auto *OA = SD->getAttrs().getAttribute<OverrideAttr>()) {
        if (!SD->getOverriddenDecl()) {
          TC.diagnose(SD, diag::subscript_does_not_override)
              .highlight(OA->getLocation());
          OA->setInvalid();
        }
      }
    }

    // Member subscripts need some special validation logic.
    if (auto contextType = dc->getDeclaredTypeInContext()) {
      // If this is a class member, mark it final if the class is final.
      if (auto cls = contextType->getClassOrBoundGenericClass()) {
        if (cls->isFinal() && !SD->isFinal()) {
          makeFinal(TC.Context, SD);
        }
      }

      // A subscript is ObjC-compatible if it's explicitly @objc, or a
      // member of an ObjC-compatible class or protocol.
      ProtocolDecl *protocolContext = dyn_cast<ProtocolDecl>(dc);
      ObjCReason reason = ObjCReason::DontDiagnose;
      if (SD->getAttrs().hasAttribute<ObjCAttr>())
        reason = ObjCReason::ExplicitlyObjC;
      else if (SD->getAttrs().hasAttribute<DynamicAttr>())
        reason = ObjCReason::ExplicitlyDynamic;
      else if (protocolContext && protocolContext->isObjC())
        reason = ObjCReason::MemberOfObjCProtocol;
      bool isObjC = (reason != ObjCReason::DontDiagnose) ||
                    isImplicitlyObjC(SD);
      if (isObjC && !TC.isRepresentableInObjC(SD, reason))
        isObjC = false;
      
      markAsObjC(TC, SD, isObjC);
    }

    // If this variable is marked final and has a getter or setter, mark the
    // getter and setter as final as well.
    if (SD->isFinal()) {
      makeFinal(TC.Context, SD->getGetter());
      makeFinal(TC.Context, SD->getSetter());
      makeFinal(TC.Context, SD->getMaterializeForSetFunc());
    }

    if (SD->hasAccessorFunctions()) {
      maybeAddMaterializeForSet(SD, TC);
    }

    // Make sure the getter and setter have valid types, since they will be
    // used by SILGen for any accesses to this subscript.
    if (auto getter = SD->getGetter())
      TC.validateDecl(getter);
    if (auto setter = SD->getSetter())
      TC.validateDecl(setter);

    // If this is a get+mutableAddress property, synthesize the setter body.
    if (SD->getStorageKind() == SubscriptDecl::ComputedWithMutableAddress &&
        !SD->getSetter()->getBody()) {
      synthesizeSetterForMutableAddressedStorage(SD, TC);
    }

    inferDynamic(TC.Context, SD);

    // Synthesize materializeForSet in non-protocol contexts.
    if (auto materializeForSet = SD->getMaterializeForSetFunc()) {
      Type containerTy = SD->getDeclContext()->getDeclaredTypeOfContext();
      if (!containerTy || !containerTy->is<ProtocolType>()) {
        synthesizeMaterializeForSet(materializeForSet, SD, TC);
        TC.typeCheckDecl(materializeForSet, true);
        TC.typeCheckDecl(materializeForSet, false);
      }
    }

    TC.checkDeclAttributes(SD);
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    if (TAD->isBeingTypeChecked()) {
      
      if (!TAD->hasUnderlyingType()) {
        TAD->setInvalid();
        TAD->overwriteType(ErrorType::get(TC.Context));
        TAD->getUnderlyingTypeLoc().setType(ErrorType::get(TC.Context));
        
        TC.diagnose(TAD->getLoc(), diag::circular_type_alias, TAD->getName());
      }
      return;
    }
    
    TAD->setIsBeingTypeChecked();
    
    TC.checkDeclAttributesEarly(TAD);
    TC.computeAccessibility(TAD);
    if (!IsSecondPass) {
      TypeResolutionOptions options;
      if (!TAD->getDeclContext()->isTypeContext())
        options |= TR_GlobalTypeAlias;
      if (TAD->getAccessibility() == Accessibility::Private)
        options |= TR_KnownNonCascadingDependency;
      
      if (TC.validateType(TAD->getUnderlyingTypeLoc(), TAD->getDeclContext(),
                          options)) {
        TAD->setInvalid();
        TAD->overwriteType(ErrorType::get(TC.Context));
        TAD->getUnderlyingTypeLoc().setType(ErrorType::get(TC.Context));
      } else if (TAD->getDeclContext()->isGenericContext()) {
        TAD->setInterfaceType(
          TC.getInterfaceTypeFromInternalType(TAD->getDeclContext(),
                                              TAD->getType()));
      }

      // We create TypeAliasTypes with invalid underlying types, so we
      // need to propagate recursive properties now.
      if (TAD->hasUnderlyingType())
        TAD->getAliasType()->setRecursiveProperties(
                         TAD->getUnderlyingType()->getRecursiveProperties());

      if (!isa<ProtocolDecl>(TAD->getDeclContext()))
        TC.checkInheritanceClause(TAD);
    }

    if (IsSecondPass)
      checkAccessibility(TC, TAD);

    TC.checkDeclAttributes(TAD);
    
    TAD->setIsBeingTypeChecked(false);
  }
  
  void visitAssociatedTypeDecl(AssociatedTypeDecl *assocType) {
    TC.checkDeclAttributesEarly(assocType);
    if (!assocType->hasAccessibility())
      assocType->setAccessibility(assocType->getProtocol()->getAccessibility());

    // Check the default definition, if there is one.
    TypeLoc &defaultDefinition = assocType->getDefaultDefinitionLoc();
    if (!defaultDefinition.isNull() &&
        TC.validateType(defaultDefinition, assocType->getDeclContext())) {
      defaultDefinition.setInvalidType(TC.Context);
    }
    TC.checkDeclAttributes(assocType);
  }

  bool checkUnsupportedNestedGeneric(NominalTypeDecl *NTD) {
    // We don't support nested types in generics yet.
    if (NTD->isGenericContext()) {
      auto DC = NTD->getDeclContext();
      if (DC->isTypeContext()) {
        if (NTD->getGenericParams())
          TC.diagnose(NTD->getLoc(), diag::unsupported_generic_nested_in_type,
                NTD->getName(),
                DC->getDeclaredTypeOfContext());
        else
          TC.diagnose(NTD->getLoc(),
                      diag::unsupported_type_nested_in_generic_type,
                      NTD->getName(),
                      DC->getDeclaredTypeOfContext());
        return true;
      } else if (DC->isLocalContext()) {
        // A local generic context is a generic function.
        if (auto AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
          TC.diagnose(NTD->getLoc(),
                      diag::unsupported_type_nested_in_generic_function,
                      NTD->getName(),
                      AFD->getName());
          return true;
        }
      }
    }
    return false;
  }

  void visitEnumDecl(EnumDecl *ED) {
    // This enum declaration is technically a parse error, so do not type
    // check.
    if (isa<ProtocolDecl>(ED->getParent()))
      return;

    TC.checkDeclAttributesEarly(ED);
    TC.computeAccessibility(ED);

    if (!IsSecondPass) {
      checkUnsupportedNestedGeneric(ED);

      TC.validateDecl(ED);

      TC.ValidatedTypes.remove(ED);

      {
        // Check for circular inheritance of the raw type.
        SmallVector<EnumDecl *, 8> path;
        checkCircularity(TC, ED, diag::circular_enum_inheritance,
                         diag::enum_here, path);
      }
      {
        // Check for duplicate enum members.
        llvm::DenseMap<Identifier, EnumElementDecl *> Elements;
        for (auto *EED : ED->getAllElements()) {
          auto Res = Elements.insert({ EED->getName(), EED });
          if (!Res.second) {
            EED->overwriteType(ErrorType::get(TC.Context));
            EED->setInvalid();
            if (auto *RawValueExpr = EED->getRawValueExpr())
              RawValueExpr->setType(ErrorType::get(TC.Context));

            auto PreviousEED = Res.first->second;
            TC.diagnose(EED->getLoc(), diag::duplicate_enum_element);
            TC.diagnose(PreviousEED->getLoc(),
                        diag::previous_decldef, true, EED->getName());
          }
        }
      }
    }

    if (!IsFirstPass) {
      checkAccessibility(TC, ED);

      if (ED->hasRawType() && !ED->isObjC()) {
        // ObjC enums have already had their raw values checked, but pure Swift
        // enums haven't.
        checkEnumRawValues(TC, ED);
      }

      checkExplicitConformance(ED, ED->getDeclaredTypeInContext());
    }
    
    for (Decl *member : ED->getMembers())
      visit(member);
    for (Decl *global : ED->getDerivedGlobalDecls())
      visit(global);
    

    TC.checkDeclAttributes(ED);
  }

  void visitStructDecl(StructDecl *SD) {
    // This struct declaration is technically a parse error, so do not type
    // check.
    if (isa<ProtocolDecl>(SD->getParent()))
      return;

    TC.checkDeclAttributesEarly(SD);
    TC.computeAccessibility(SD);

    if (!IsSecondPass) {
      checkUnsupportedNestedGeneric(SD);

      TC.validateDecl(SD);
      TC.ValidatedTypes.remove(SD);

      SmallVector<Decl*, 2> NewDecls;
      TC.addImplicitConstructors(SD, NewDecls);
    }

    if (!IsFirstPass) {
      checkAccessibility(TC, SD);
    }
    
    // Visit each of the members.
    for (Decl *Member : SD->getMembers())
      visit(Member);
    for (Decl *global : SD->getDerivedGlobalDecls())
      visit(global);

    if (!(IsFirstPass || SD->isInvalid())) {
      checkExplicitConformance(SD, SD->getDeclaredTypeInContext());
    }
    TC.checkDeclAttributes(SD);
  }

  /// Check whether the given propertes can be @NSManaged in this class.
  static bool propertiesCanBeNSManaged(ClassDecl *classDecl,
                                       ArrayRef<VarDecl *> vars) {
    // Check whether we have an Objective-C-defined class in our
    // inheritance chain.
    while (classDecl) {
      // If we found an Objective-C-defined class, continue checking.
      if (classDecl->hasClangNode())
        break;

      // If we ran out of superclasses, we're done.
      if (!classDecl->hasSuperclass())
        return false;

      classDecl = classDecl->getSuperclass()->getClassOrBoundGenericClass();
    }

    // If all of the variables are @objc, we can use @NSManaged.
    for (auto var : vars) {
      if (!var->isObjC())
        return false;
    }

    // Okay, we can use @NSManaged.
    return true;
  }

  /// Check that all stored properties have in-class initializers.
  void checkRequiredInClassInits(ClassDecl *cd) {
    ClassDecl *source = nullptr;
    for (auto member : cd->getMembers()) {
      auto pbd = dyn_cast<PatternBindingDecl>(member);
      if (!pbd)
        continue;

      if (pbd->isStatic() || !pbd->hasStorage() || 
          isDefaultInitializable(pbd) || pbd->isInvalid())
        continue;

      // The variables in this pattern have not been
      // initialized. Diagnose the lack of initial value.
      pbd->setInvalid();
      SmallVector<VarDecl *, 4> vars;
      pbd->getPattern()->collectVariables(vars);
      bool suggestNSManaged = propertiesCanBeNSManaged(cd, vars);
      switch (vars.size()) {
      case 0:
        llvm_unreachable("should have been marked invalid");

      case 1:
        TC.diagnose(pbd->getLoc(), diag::missing_in_class_init_1,
                    vars[0]->getName(), suggestNSManaged);
        break;

      case 2:
        TC.diagnose(pbd->getLoc(), diag::missing_in_class_init_2,
                    vars[0]->getName(), vars[1]->getName(), suggestNSManaged);
        break;

      case 3:
        TC.diagnose(pbd->getLoc(), diag::missing_in_class_init_3plus,
                    vars[0]->getName(), vars[1]->getName(), vars[2]->getName(),
                    false, suggestNSManaged);
        break;

      default:
        TC.diagnose(pbd->getLoc(), diag::missing_in_class_init_3plus,
                    vars[0]->getName(), vars[1]->getName(), vars[2]->getName(),
                    true, suggestNSManaged);
        break;
      }

      // Figure out where this requirement came from.
      if (!source) {
        source = cd;
        while (true) {
          // If this class had the 'requires_stored_property_inits'
          // attribute, diagnose here.
          if (source->getAttrs().
                hasAttribute<RequiresStoredPropertyInitsAttr>())
            break;

          // If the superclass doesn't require in-class initial
          // values, the requirement was introduced at this point, so
          // stop here.
          auto superclass = cast<ClassDecl>(
                              source->getSuperclass()->getAnyNominal());
          if (!superclass->requiresStoredPropertyInits())
            break;

          // Keep looking.
          source = superclass;
        }
      }

      // Add a note describing why we need an initializer.
      TC.diagnose(source, diag::requires_stored_property_inits_here,
                  source->getDeclaredType(), cd == source, suggestNSManaged);
    }
  }


  void visitClassDecl(ClassDecl *CD) {
    // This class declaration is technically a parse error, so do not type
    // check.
    if (isa<ProtocolDecl>(CD->getParent()))
      return;

    TC.checkDeclAttributesEarly(CD);
    TC.computeAccessibility(CD);

    if (!IsSecondPass) {
      checkUnsupportedNestedGeneric(CD);

      TC.validateDecl(CD);

      TC.ValidatedTypes.remove(CD);

      {
        // Check for circular inheritance.
        SmallVector<ClassDecl *, 8> path;
        checkCircularity(TC, CD, diag::circular_class_inheritance,
                         diag::class_here, path);
      }
    }
    
    // If this class needs an implicit constructor, add it.
    if (!IsFirstPass) {
      SmallVector<Decl*, 2> ImplicitInits;
      TC.addImplicitConstructors(CD, ImplicitInits);
    }

    TC.addImplicitDestructor(CD);

    for (Decl *Member : CD->getMembers())
      visit(Member);
    for (Decl *global : CD->getDerivedGlobalDecls())
      visit(global);

    // If this class requires all of its stored properties to have
    // in-class initializers, diagnose this now.
    if (CD->requiresStoredPropertyInits())
      checkRequiredInClassInits(CD);

    if (!IsFirstPass) {
      // Check that we don't inherit from a final class.
      if (auto superclassTy = CD->getSuperclass()) {
        ClassDecl *Super = superclassTy->getClassOrBoundGenericClass();
        if (Super->isFinal()) {
          TC.diagnose(CD, diag::inheritance_from_final_class,
                      Super->getName());
          return;
        }
      }

      checkAccessibility(TC, CD);

      // Check for inconsistencies between the initializers of our
      // superclass and our own initializers.
      if (auto superclassTy = CD->getSuperclass()) {
        // Verify that if the super class is generic, the derived class is as
        // well.
        if (superclassTy->getAs<BoundGenericClassType>() &&
            !CD->getDeclaredTypeInContext()->getAs<BoundGenericClassType>())
          TC.diagnose(CD, diag::non_generic_class_with_generic_superclass);
      }
    }
    if (!(IsFirstPass || CD->isInvalid())) {
      checkExplicitConformance(CD, CD->getDeclaredTypeInContext());
    }

    TC.checkDeclAttributes(CD);
  }

  void validateAncestorProtocols(ArrayRef<ProtocolDecl *> initialProtos) {
    llvm::SmallPtrSet<ProtocolDecl *, 16> seenProtos;
    SmallVector<ProtocolDecl *, 16> queue(initialProtos.begin(),
                                          initialProtos.end());

    while (!queue.empty()) {
      ProtocolDecl *proto = queue.pop_back_val();
      if (!seenProtos.insert(proto).second)
        continue;

      queue.append(proto->getProtocols().begin(), proto->getProtocols().end());
      for (auto *member : proto->getMembers())
        if (auto *requirement = dyn_cast<ValueDecl>(member))
          TC.validateDecl(requirement);
    }
  }

  void visitProtocolDecl(ProtocolDecl *PD) {
    // This protocol declaration is technically a parse error, so do not type
    // check.
    if (isa<ProtocolDecl>(PD->getParent()))
      return;

    TC.checkDeclAttributesEarly(PD);
    TC.computeAccessibility(PD);

    if (IsSecondPass) {
      checkAccessibility(TC, PD);
      for (auto member : PD->getMembers())
        checkAccessibility(TC, member);
      return;
    }

    PD->setIsBeingTypeChecked();
    
    TC.validateDecl(PD);

    {
      // Check for circular inheritance within the protocol.
      SmallVector<ProtocolDecl *, 8> path;
      checkCircularity(TC, PD, diag::circular_protocol_def,
                       diag::protocol_here, path);

      // Make sure the parent protocols have been fully validated.
      validateAncestorProtocols(PD->getProtocols());

      if (auto *SF = PD->getParentSourceFile()) {
        if (auto *tracker = SF->getReferencedNameTracker()) {
          bool isNonPrivate =
            (PD->getAccessibility() != Accessibility::Private);
          for (auto *parentProto : PD->getProtocols())
            tracker->addUsedNominal(parentProto, isNonPrivate);
        }
      }
    }

    // Check the members.
    for (auto Member : PD->getMembers())
      visit(Member);

    TC.checkDeclAttributes(PD);
    
    PD->setIsBeingTypeChecked(false);
  }

  void visitVarDecl(VarDecl *VD) {
    // Delay type-checking on VarDecls until we see the corresponding
    // PatternBindingDecl.
  }

  bool semaFuncParamPatterns(AbstractFunctionDecl *fd,
                             GenericTypeResolver *resolver = nullptr) {
    // Type check the body patterns.
    bool badType = false;
    auto bodyPatterns = fd->getBodyParamPatterns();
    for (unsigned i = 0, e = bodyPatterns.size(); i != e; ++i) {
      auto *bodyPat = bodyPatterns[i];

      if (bodyPat->hasType())
        continue;

      if (TC.typeCheckPattern(bodyPat, fd, TR_ImmediateFunctionInput, resolver))
        badType = true;
    }

    return badType;
  }

  /// \brief Validate and apply the attributes that are applicable to the
  /// AnyFunctionType.
  ///
  /// Currently, we only allow 'noreturn' to be applied on a FuncDecl.
  AnyFunctionType::ExtInfo
  validateAndApplyFunctionTypeAttributes(FuncDecl *FD) {
    auto Info = AnyFunctionType::ExtInfo();

    // 'noreturn' is allowed on a function declaration.
    Info = Info.withIsNoReturn(FD->getAttrs().hasAttribute<NoReturnAttr>());

    return Info;
  }

  void semaFuncDecl(FuncDecl *FD, GenericTypeResolver *resolver) {
    if (FD->hasType())
      return;

    TC.checkForForbiddenPrefix(FD);

    // Observing accessors (and their generated regular accessors) may have
    // the type of the var inferred.
    if (AbstractStorageDecl *ASD = FD->getAccessorStorageDecl()) {
      if (ASD->hasObservers()) {
        TC.validateDecl(ASD);
        Type valueTy = ASD->getType()->getReferenceStorageReferent();
        if (FD->isObservingAccessor() || (FD->isSetter() && FD->isImplicit())) {
          unsigned firstParamIdx = FD->getParent()->isTypeContext();
          auto *firstParamPattern = FD->getBodyParamPatterns()[firstParamIdx];
          auto *tuplePattern = cast<TuplePattern>(firstParamPattern);
          auto *paramPattern = tuplePattern->getFields().front().getPattern();
          auto *paramTypePattern = cast<TypedPattern>(paramPattern);
          paramTypePattern->getTypeLoc().setType(valueTy, true);
        } else if (FD->isGetter() && FD->isImplicit()) {
          FD->getBodyResultTypeLoc().setType(valueTy, true);
        }
      }
    }

    FD->setIsBeingTypeChecked();

    bool badType = false;
    if (!FD->getBodyResultTypeLoc().isNull()) {
      if (TC.validateType(FD->getBodyResultTypeLoc(), FD, TR_FunctionResult,
                          resolver)) {
        badType = true;
      }
    }

    if (!badType) {
      badType = semaFuncParamPatterns(FD, resolver);
    }

    FD->setIsBeingTypeChecked(false);

    // Checking the function parameter patterns might (recursively)
    // end up setting the type.
    if (FD->hasType())
      return;

    if (badType) {
      FD->setType(ErrorType::get(TC.Context));
      FD->setInvalid();
      return;
    }

    // Reject things like "func f(Int)" if it has a body, since this will
    // implicitly name the argument 'f'.  Instead, suggest that the user write
    // this as "func f(_: Int)".
    if (FD->hasBody() && FD->getBodyParamPatterns().size() == 1) {
      Pattern *BodyPattern = FD->getBodyParamPatterns()[0];
      
      // Look through single-entry tuple elements, which can exist when there
      // are default values.
      if (auto *TP = dyn_cast<TuplePattern>(BodyPattern))
        if (TP->getNumFields() == 1 && !TP->hasVararg())
          BodyPattern =TP->getFields()[0].getPattern();
      // Look through typedpatterns and parens.
      BodyPattern = BodyPattern->getSemanticsProvidingPattern();
      
      if (auto *NP = dyn_cast<NamedPattern>(BodyPattern))
        if (NP->getDecl()->getName() == FD->getName() && NP->isImplicit()) {
          TC.diagnose(BodyPattern->getLoc(), diag::implied_name_no_argument)
            .fixItInsert(BodyPattern->getLoc(), "_: ");
          // Mark the decl as invalid to avoid inscrutable downstream errors.
          NP->getDecl()->setInvalid();
          NP->getDecl()->overwriteType(ErrorType::get(TC.Context));
        }
    }
    
    Type funcTy = FD->getBodyResultTypeLoc().getType();
    if (!funcTy) {
      funcTy = TupleType::getEmpty(TC.Context);
    }
    auto bodyResultType = funcTy;

    // Form the function type by building the curried function type
    // from the back to the front, "prepending" each of the parameter
    // patterns.
    GenericParamList *genericParams = FD->getGenericParams();
    GenericParamList *outerGenericParams = nullptr;
    auto patterns = FD->getBodyParamPatterns();
    bool hasSelf = FD->getDeclContext()->isTypeContext();
    if (hasSelf)
      outerGenericParams = FD->getDeclContext()->getGenericParamsOfContext();

    for (unsigned i = 0, e = patterns.size(); i != e; ++i) {
      if (!patterns[e - i - 1]->hasType()) {
        FD->setType(ErrorType::get(TC.Context));
        FD->setInvalid();
        return;
      }
      
      Type argTy = patterns[e - i - 1]->getType();

      // Determine the appropriate generic parameters at this level.
      GenericParamList *params = nullptr;
      if (e - i - 1 == hasSelf && genericParams) {
        params = genericParams;
      } else if (e - i - 1 == 0 && outerGenericParams) {
        params = outerGenericParams;
      }
      
      // Validate and consume the function type attributes.
      auto Info = validateAndApplyFunctionTypeAttributes(FD);
      if (params) {
        funcTy = PolymorphicFunctionType::get(argTy, funcTy, params, Info);
      } else {
        funcTy = FunctionType::get(argTy, funcTy, Info);
      }

    }
    FD->setType(funcTy);
    FD->setBodyResultType(bodyResultType);

    // For a non-generic method that returns dynamic Self, we need to
    // provide an interface type where the 'self' argument is the
    // nominal type.
    if (FD->hasDynamicSelf() && !genericParams && !outerGenericParams) {
      auto fnType = FD->getType()->castTo<FunctionType>();
      auto inputType = fnType->getInput().transform([&](Type type) -> Type {
        if (type->is<DynamicSelfType>())
          return FD->getExtensionType();
        return type;
      });
      FD->setInterfaceType(FunctionType::get(inputType, fnType->getResult(),
                                             fnType->getExtInfo()));
    }
  }

  /// Bind the given function declaration, which declares an operator, to
  /// the corresponding operator declaration.
  void bindFuncDeclToOperator(FuncDecl *FD) {
    OperatorDecl *op = nullptr;
    auto operatorName = FD->getFullName().getBaseName();
    SourceFile &SF = *FD->getDeclContext()->getParentSourceFile();
    if (FD->isUnaryOperator()) {
      if (FD->getAttrs().hasAttribute<PrefixAttr>()) {
        op = SF.lookupPrefixOperator(operatorName,
                                     FD->isCascadingContextForLookup(false),
                                     FD->getLoc());
      } else if (FD->getAttrs().hasAttribute<PostfixAttr>()) {
        op = SF.lookupPostfixOperator(operatorName,
                                      FD->isCascadingContextForLookup(false),
                                      FD->getLoc());
      } else {
        auto prefixOp =
            SF.lookupPrefixOperator(operatorName,
                                    FD->isCascadingContextForLookup(false),
                                    FD->getLoc());
        auto postfixOp =
            SF.lookupPostfixOperator(operatorName,
                                     FD->isCascadingContextForLookup(false),
                                     FD->getLoc());

        // If we found both prefix and postfix, or neither prefix nor postfix,
        // complain. We can't fix this situation.
        if (static_cast<bool>(prefixOp) == static_cast<bool>(postfixOp)) {
          TC.diagnose(FD, diag::declared_unary_op_without_attribute);

          // If we found both, point at them.
          if (prefixOp) {
            SourceLoc insertionLoc = FD->getLoc();

            TC.diagnose(prefixOp, diag::unary_operator_declaration_here,false)
              .fixItInsert(insertionLoc, "prefix ");
            TC.diagnose(postfixOp, diag::unary_operator_declaration_here, true)
              .fixItInsert(insertionLoc, "postfix ");
          } else {
            // FIXME: Introduce a Fix-It that adds the operator declaration?
          }

          // FIXME: Errors could cascade here, because name lookup for this
          // operator won't find this declaration.
          return;
        }

        // We found only one operator declaration, so we know whether this
        // should be a prefix or a postfix operator.

        // Fix the AST and determine the insertion text.
        SourceLoc insertionLoc = FD->getFuncLoc();
        const char *insertionText;
        auto &C = FD->getASTContext();
        if (postfixOp) {
          insertionText = "postfix ";
          op = postfixOp;
          FD->getAttrs().add(new (C) PostfixAttr(/*implicit*/false));
        } else {
          insertionText = "prefix ";
          op = prefixOp;
          FD->getAttrs().add(new (C) PrefixAttr(/*implicit*/false));
        }

        // Emit diagnostic with the Fix-It.
        TC.diagnose(insertionLoc, diag::unary_op_missing_prepos_attribute,
                    static_cast<bool>(postfixOp))
          .fixItInsert(insertionLoc, insertionText);
        TC.diagnose(op, diag::unary_operator_declaration_here,
                    static_cast<bool>(postfixOp));
      }
    } else if (FD->isBinaryOperator()) {
      op = SF.lookupInfixOperator(operatorName,
                                  FD->isCascadingContextForLookup(false),
                                  FD->getLoc());
    } else {
      TC.diagnose(FD, diag::invalid_arg_count_for_operator);
      return;
    }

    if (!op) {
      // FIXME: Add Fix-It introducing an operator declaration?
      TC.diagnose(FD, diag::declared_operator_without_operator_decl);
      return;
    }

    FD->setOperatorDecl(op);
  }

  /// Determine whether the given declaration requires a definition.
  ///
  /// Only valid for declarations that can have definitions, i.e.,
  /// functions, initializers, etc.
  static bool requiresDefinition(Decl *decl) {
    // Invalid, implicit, and Clang-imported declarations never
    // require a definition.
    if (decl->isInvalid() || decl->isImplicit() || decl->hasClangNode())
      return false;

    // Functions can have asmname and semantics attributes.
    if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
      if (func->getAttrs().hasAttribute<AsmnameAttr>() ||
          func->getAttrs().hasAttribute<SemanticsAttr>())
        return false;
    }

    // Declarations in SIL don't require definitions.
    if (auto sourceFile = decl->getDeclContext()->getParentSourceFile()) {
      if (sourceFile->Kind == SourceFileKind::SIL)
        return false;
    }

    // Everything else requires a definition.
    return true;
  }

  /// Check for methods that return 'DynamicResult'.
  bool checkDynamicSelfReturn(FuncDecl *func) {
    // Check whether we have a specified result type.
    auto typeRepr = func->getBodyResultTypeLoc().getTypeRepr();
    if (!typeRepr)
      return false;
      
    return checkDynamicSelfReturn(func, typeRepr, 0);
  }

  bool checkDynamicSelfReturn(FuncDecl *func, TypeRepr *typeRepr,
                              unsigned optionalDepth) {
    // Look through parentheses.
    if (auto parenRepr = dyn_cast<TupleTypeRepr>(typeRepr)) {
      if (!parenRepr->isParenType()) return false;
      return checkDynamicSelfReturn(func, parenRepr->getElements()[0],
                                    optionalDepth);
    }

    // Look through attributes.
    if (auto attrRepr = dyn_cast<AttributedTypeRepr>(typeRepr)) {
      TypeAttributes attrs = attrRepr->getAttrs();
      if (!attrs.empty())
        return false;
      return checkDynamicSelfReturn(func, attrRepr->getTypeRepr(),
                                    optionalDepth);
    }

    // Look through optional types.
    if (auto attrRepr = dyn_cast<OptionalTypeRepr>(typeRepr)) {
      // But only one level.
      if (optionalDepth != 0) return false;
      return checkDynamicSelfReturn(func, attrRepr->getBase(),
                                    optionalDepth + 1);
    }

    // Check whether we have a simple identifier type.
    auto simpleRepr = dyn_cast<SimpleIdentTypeRepr>(typeRepr);
    if (!simpleRepr)
      return false;

    // Check whether it is 'Self'.
    if (simpleRepr->getIdentifier() != TC.Context.Id_Self)
      return false;

    // Dynamic 'Self' is only permitted on methods.
    auto dc = func->getDeclContext();
    if (!dc->isTypeContext()) {
      TC.diagnose(simpleRepr->getIdLoc(), diag::dynamic_self_non_method,
                  dc->isLocalContext());
      simpleRepr->setValue(ErrorType::get(TC.Context));
      return true;
    }

    auto containerTy = dc->getDeclaredTypeOfContext();
    if (containerTy->is<ErrorType>())
      return true;

    // 'Self' is only a dynamic self on class methods.
    auto nominal = containerTy->getAnyNominal();
    assert(nominal && "Non-nominal container for method type?");
    if (!isa<ClassDecl>(nominal) && !isa<ProtocolDecl>(nominal)) {
      int which;
      if (isa<StructDecl>(nominal))
        which = 0;
      else if (isa<EnumDecl>(nominal))
        which = 1;
      else
        llvm_unreachable("Unknown nominal type");
      TC.diagnose(simpleRepr->getIdLoc(), diag::dynamic_self_struct_enum,
                  which, nominal->getName())
        .fixItReplace(simpleRepr->getIdLoc(), nominal->getName().str());
      simpleRepr->setValue(ErrorType::get(TC.Context));
      return true;
    }

    // Note that the function has a dynamic Self return type and set
    // the return type component to the dynamic self type.
    func->setDynamicSelf(true);
    auto dynamicSelfType = func->getDynamicSelf();
    simpleRepr->setValue(dynamicSelfType);
    return false;
  }

  void visitFuncDecl(FuncDecl *FD) {
    if (!IsFirstPass) {
      if (FD->hasBody()) {
        // Record the body.
        TC.definedFunctions.push_back(FD);
      } else if (requiresDefinition(FD)) {
        // Complain if we should have a body.
        TC.diagnose(FD->getLoc(), diag::func_decl_without_brace);
      }
    }

    if (IsSecondPass) {
      checkAccessibility(TC, FD);
      return;
    }

    TC.checkDeclAttributesEarly(FD);
    TC.computeAccessibility(FD);

    if (FD->hasType())
      return;

    // Bind operator functions to the corresponding operator declaration.
    if (FD->isOperator())
      bindFuncDeclToOperator(FD);

    // Validate 'static'/'class' on functions in extensions.
    auto StaticSpelling = FD->getStaticSpelling();
    if (StaticSpelling != StaticSpellingKind::None &&
        FD->getDeclContext()->isExtensionContext()) {
      if (Type T = FD->getDeclContext()->getDeclaredTypeInContext()) {
        if (auto NTD = T->getAnyNominal()) {
          if (!isa<ClassDecl>(NTD)) {
            if (StaticSpelling == StaticSpellingKind::KeywordClass) {
              TC.diagnose(FD, diag::class_func_not_in_class)
                  .fixItReplace(FD->getStaticLoc(), "static");
              TC.diagnose(NTD, diag::extended_type_declared_here);
            }
          }
        }
      }
    }

    // Validate the mutating attribute if present, and install it into the bit
    // on funcdecl (instead of just being a DeclAttribute).
    if (FD->getAttrs().hasAttribute<MutatingAttr>())
      FD->setMutating(true);
    else if (FD->getAttrs().hasAttribute<NonMutatingAttr>())
      FD->setMutating(false);

    bool isInvalid = false;

    // Check whether the return type is dynamic 'Self'.
    if (checkDynamicSelfReturn(FD))
      isInvalid = true;

    // Before anything else, set up the 'self' argument correctly if present.
    GenericParamList *outerGenericParams = nullptr;
    if (FD->getDeclContext()->isTypeContext() &&
        !FD->getImplicitSelfDecl()->hasType())
      configureImplicitSelf(TC, FD, outerGenericParams);

    // If we have generic parameters, check the generic signature now.
    if (auto gp = FD->getGenericParams()) {
      gp->setOuterParameters(outerGenericParams);

      if (TC.validateGenericFuncSignature(FD))
        isInvalid = true;
      else {
        // Create a fresh archetype builder.
        ArchetypeBuilder builder =
          TC.createArchetypeBuilder(FD->getModuleContext());
        checkGenericParamList(builder, gp, TC, FD->getDeclContext());

        // Infer requirements from parameter patterns.
        for (auto pattern : FD->getBodyParamPatterns()) {
          builder.inferRequirements(pattern);
        }

        // Infer requirements from the result type.
        if (!FD->getBodyResultTypeLoc().isNull()) {
          builder.inferRequirements(FD->getBodyResultTypeLoc());
        }

        // Revert all of the types within the signature of the function.
        TC.revertGenericFuncSignature(FD);

        finalizeGenericParamList(builder, FD->getGenericParams(), FD, TC);
      }
    } else if (outerGenericParams) {
      if (TC.validateGenericFuncSignature(FD))
        isInvalid = true;
      else if (!FD->hasType()) {
        // Revert all of the types within the signature of the function.
        TC.revertGenericFuncSignature(FD);
      } else {
        // Recursively satisfied.
        // FIXME: This is an awful hack.
        return;
      }
    }

    // Type check the parameters and return type again, now with archetypes.
    GenericTypeToArchetypeResolver resolver;
    semaFuncDecl(FD, &resolver);

    if (FD->isInvalid())
      return;

    // This type check should have created a non-dependent type.
    assert(!FD->getType()->isDependentType());

    validateAttributes(TC, FD);

    // Member functions need some special validation logic.
    if (auto contextType = FD->getDeclContext()->getDeclaredTypeInContext()) {
      // If this is a class member, mark it final if the class is final.
      if (auto cls = contextType->getClassOrBoundGenericClass()) {
        if (cls->isFinal() && !FD->isAccessor() &&
            !FD->isFinal() && !FD->isDynamic()) {
          makeFinal(TC.Context, FD);
        }
        // static func declarations in classes are synonyms
        // for `class final func` declarations.
        if (FD->getStaticSpelling() == StaticSpellingKind::KeywordStatic) {
          auto finalAttr = FD->getAttrs().getAttribute<FinalAttr>();
          if (finalAttr) {
            auto finalRange = finalAttr->getRange();
            if (finalRange.isValid())
              TC.diagnose(finalRange.Start, diag::decl_already_final)
                .highlight(finalRange)
                .fixItRemove(finalRange);
          }
          makeFinal(TC.Context, FD);
        }
      }

      if (!checkOverrides(TC, FD)) {
        // If a method has an 'override' keyword but does not
        // override anything, complain.
        if (auto *OA = FD->getAttrs().getAttribute<OverrideAttr>()) {
          if (!FD->getOverriddenDecl()) {
            TC.diagnose(FD, diag::method_does_not_override)
              .highlight(OA->getLocation());
            OA->setInvalid();
          }
        }
      }

      // A method is ObjC-compatible if:
      // - it's explicitly @objc or dynamic,
      // - it's a member of an ObjC-compatible class, or
      // - it's an accessor for an ObjC property.
      ProtocolDecl *protocolContext =
          dyn_cast<ProtocolDecl>(FD->getDeclContext());
      bool isMemberOfObjCProtocol =
          protocolContext && protocolContext->isObjC();
      ObjCReason reason = ObjCReason::DontDiagnose;
      if (FD->getAttrs().hasAttribute<ObjCAttr>())
        reason = ObjCReason::ExplicitlyObjC;
      else if (FD->getAttrs().hasAttribute<DynamicAttr>())
        reason = ObjCReason::ExplicitlyDynamic;
      else if (isMemberOfObjCProtocol)
        reason = ObjCReason::MemberOfObjCProtocol;
      bool isObjC = (reason != ObjCReason::DontDiagnose) ||
                    isImplicitlyObjC(FD);
      
      if (protocolContext && FD->isAccessor()) {
        // Don't complain about accessors in protocols.  We will emit a
        // diagnostic about the property itself.
        reason = ObjCReason::DontDiagnose;
      }
      if (!isObjC && FD->isGetterOrSetter()) {
        // If the property decl is an instance property, its accessors will
        // be instance methods and the above condition will mark them ObjC.
        // The only additional condition we need to check is if the var decl
        // had an @objc or @iboutlet property.

        ValueDecl *prop = cast<ValueDecl>(FD->getAccessorStorageDecl());
        // Validate the subscript or property because it might not be type
        // checked yet.
        if (isa<SubscriptDecl>(prop))
          TC.validateDecl(prop);
        else if (auto pat = cast<VarDecl>(prop)->getParentPattern())
          validatePatternBindingDecl(TC, pat);

        isObjC = prop->isObjC() || prop->isDynamic() ||
                 prop->getAttrs().hasAttribute<IBOutletAttr>();
        
        // If the property is dynamic, propagate to this accessor.
        if (prop->isDynamic() && !FD->isDynamic())
          FD->getAttrs().add(new (TC.Context) DynamicAttr(/*implicit*/ true));
      }

      if (isObjC &&
          (FD->isInvalid() || !TC.isRepresentableInObjC(FD, reason)))
        isObjC = false;
      markAsObjC(TC, FD, isObjC);
    }
    
    inferDynamic(TC.Context, FD);

    TC.checkDeclAttributes(FD);

    // Check whether we have parameters with default arguments that follow a
    // closure parameter; warn about such things, because the closure will not
    // be treated as a trailing closure.
    if (!FD->isImplicit()) {
      auto paramPattern = FD->getBodyParamPatterns()[
                            FD->getDeclContext()->isTypeContext() ? 1 : 0];
      if (auto paramTuple = dyn_cast<TuplePattern>(paramPattern)) {
        ArrayRef<TuplePatternElt> fields = paramTuple->getFields();
        unsigned n = fields.size();
        bool anyDefaultArguments = false;
        for (unsigned i = n; i > 0; --i) {
          // Determine whether the parameter is of (possibly lvalue, possibly
          // optional), non-autoclosure function type, which could receive a
          // closure. We look at the type sugar directly, so that one can
          // suppress this warning by adding parentheses.
          auto paramType = fields[i-1].getPattern()->getType();

          if (!isa<ParenType>(paramType.getPointer())) {
            // Look through lvalue-ness.
            paramType = paramType->getRValueType();

            // Look through optionality.
            if (auto objectType = paramType->getAnyOptionalObjectType())
              paramType = objectType;

            if (auto funcTy = paramType->getAs<AnyFunctionType>()) {
              // If we saw any default arguments before this, complain.
              // This doesn't apply to autoclosures.
              if (anyDefaultArguments && !funcTy->getExtInfo().isAutoClosure()) {
                TC.diagnose(fields[i-1].getPattern()->getStartLoc(),
                            diag::non_trailing_closure_before_default_args)
                .highlight(SourceRange(fields[i].getPattern()->getStartLoc(),
                                       fields[n-1].getPattern()->getEndLoc()));
              }

              break;
            }
          }

          // If we have a default argument, keep going.
          if (fields[i-1].getDefaultArgKind() != DefaultArgumentKind::None) {
            anyDefaultArguments = true;
            continue;
          }

          // We're done.
          break;
        }
      }
    }
  }

  /// Adjust the type of the given declaration to appear as if it were
  /// in the given subclass of its actual declared class.
  static Type adjustSuperclassMemberDeclType(TypeChecker &TC,
                                             const ValueDecl *decl,
                                             Type subclass) {
    ClassDecl *superclassDecl =
      decl->getDeclContext()->getDeclaredTypeInContext()
        ->getClassOrBoundGenericClass();
    auto superclass = subclass;
    while (superclass->getClassOrBoundGenericClass() != superclassDecl)
      superclass = TC.getSuperClassOf(superclass);
    auto type = TC.substMemberTypeWithBase(decl->getModuleContext(), decl,
                                           superclass,
                                           /*isTypeReference=*/false);
    if (auto func = dyn_cast<FuncDecl>(decl)) {
      if (func->hasDynamicSelf()) {
        type = type.transform([subclass](Type type) -> Type {
            if (type->is<DynamicSelfType>())
              return subclass;
            return type;
        });
      }
    } else if (isa<ConstructorDecl>(decl)) {
      type = type->replaceCovariantResultType(subclass, /*uncurryLevel=*/2);
    }

    return type;
  }

  /// Perform basic checking to determine whether a declaration can override a
  /// declaration in a superclass.
  static bool areOverrideCompatibleSimple(ValueDecl *decl,
                                          ValueDecl *parentDecl) {
    // If the number of argument labels does not match, these overrides cannot
    // be compatible.
    if (decl->getFullName().getArgumentNames().size() !=
          parentDecl->getFullName().getArgumentNames().size())
      return false;

    if (auto func = dyn_cast<FuncDecl>(decl)) {
      // Specific checking for methods.
      auto parentFunc = cast<FuncDecl>(parentDecl);
      if (func->isStatic() != parentFunc->isStatic())
        return false;
    } else if (auto var = dyn_cast<VarDecl>(decl)) {
      auto parentVar = cast<VarDecl>(parentDecl);
      if (var->isStatic() != parentVar->isStatic())
        return false;
    }

    return true;
  }

  static const Pattern *getTupleElementPattern(const TuplePatternElt &elt) {
    return elt.getPattern();
  }

  /// Drop the optionality of the result type of the given function type.
  static Type dropResultOptionality(Type type, unsigned uncurryLevel) {
    // We've hit the result type.
    if (uncurryLevel == 0) {
      if (auto objectTy = type->getAnyOptionalObjectType())
        return objectTy;

      return type;
    }

    // Determine the input and result types of this function.
    auto fnType = type->castTo<AnyFunctionType>();
    Type inputType = fnType->getInput();
    Type resultType = dropResultOptionality(fnType->getResult(), 
                                            uncurryLevel - 1);
    
    // Produce the resulting function type.
    if (auto genericFn = dyn_cast<GenericFunctionType>(fnType)) {
      return GenericFunctionType::get(genericFn->getGenericSignature(),
                                      inputType, resultType,
                                      fnType->getExtInfo());
    }
    
    assert(!isa<PolymorphicFunctionType>(fnType));  
    return FunctionType::get(inputType, resultType, fnType->getExtInfo());    
  }

  /// Diagnose overrides of '(T) -> T?' with '(T!) -> T!'.
  static void diagnoseUnnecessaryIUOs(TypeChecker &TC,
                                      const AbstractFunctionDecl *method,
                                      const AbstractFunctionDecl *parentMethod,
                                      Type owningTy) {
    Type plainParentTy = adjustSuperclassMemberDeclType(TC, parentMethod,
                                                        owningTy);
    const auto *parentTy = plainParentTy->castTo<AnyFunctionType>();
    parentTy = parentTy->getResult()->castTo<AnyFunctionType>();

    // Check the parameter types.
    auto checkParam = [&](const Pattern *paramPattern, Type parentParamTy) {
      Type paramTy = paramPattern->getType();
      if (!paramTy || !paramTy->getImplicitlyUnwrappedOptionalObjectType())
        return;
      if (!parentParamTy || parentParamTy->getAnyOptionalObjectType())
        return;

      if (auto parenPattern = dyn_cast<ParenPattern>(paramPattern))
        paramPattern = parenPattern->getSubPattern();
      if (auto varPattern = dyn_cast<VarPattern>(paramPattern))
        paramPattern = varPattern->getSubPattern();
      auto typedParamPattern = dyn_cast<TypedPattern>(paramPattern);
      if (!typedParamPattern)
        return;

      TypeLoc TL = typedParamPattern->getTypeLoc();

      // Allow silencing this warning using parens.
      if (isa<ParenType>(TL.getType().getPointer()))
        return;

      TC.diagnose(paramPattern->getLoc(), diag::override_unnecessary_IUO,
                  method->getDescriptiveKind(), parentParamTy, paramTy)
        .highlight(TL.getSourceRange());

      auto sugaredForm =
        dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(TL.getTypeRepr());
      if (sugaredForm) {
        TC.diagnose(sugaredForm->getExclamationLoc(),
                    diag::override_unnecessary_IUO_remove)
          .fixItRemove(sugaredForm->getExclamationLoc());
      }

      auto endLoc = Lexer::getLocForEndOfToken(TC.Context.SourceMgr,
                                               TL.getSourceRange().End);
      TC.diagnose(TL.getSourceRange().Start,
                  diag::override_unnecessary_IUO_silence)
        .fixItInsert(TL.getSourceRange().Start, "(")
        .fixItInsert(endLoc, ")");
    };

    auto rawParamPatterns = method->getBodyParamPatterns()[1];
    auto paramPatterns = dyn_cast<TuplePattern>(rawParamPatterns);

    auto parentInput = parentTy->getInput();
    auto parentTupleInput = parentInput->getAs<TupleType>();
    if (parentTupleInput) {
      if (paramPatterns) {
        // FIXME: If we ever allow argument reordering, this is incorrect.
        ArrayRef<TuplePatternElt> sharedParams = paramPatterns->getFields();
        sharedParams = sharedParams.slice(0,
                                          parentTupleInput->getNumElements());

        using PatternView = ArrayRefView<TuplePatternElt, const Pattern *,
                                         getTupleElementPattern>;
        for_each(PatternView(sharedParams), parentTupleInput->getElementTypes(),
                 checkParam);
      } else if (parentTupleInput->getNumElements() > 0) {
        checkParam(rawParamPatterns, parentTupleInput->getElementType(0));
      }
    } else {
      // Otherwise, the parent has a single parameter with no label.
      if (paramPatterns) {
        checkParam(paramPatterns->getFields().front().getPattern(),
                   parentInput);
      } else {
        checkParam(rawParamPatterns, parentInput);
      }
    }

    auto methodAsFunc = dyn_cast<FuncDecl>(method);
    if (!methodAsFunc)
      return;

    // FIXME: This is very nearly the same code as checkParam.
    auto checkResult = [&](TypeLoc resultTL, Type parentResultTy) {
      Type resultTy = resultTL.getType();
      if (!resultTy || !resultTy->getImplicitlyUnwrappedOptionalObjectType())
        return;
      if (!parentResultTy || !parentResultTy->getOptionalObjectType())
        return;

      // Allow silencing this warning using parens.
      if (isa<ParenType>(resultTy.getPointer()))
        return;

      TC.diagnose(resultTL.getSourceRange().Start,
                  diag::override_unnecessary_result_IUO,
                  method->getDescriptiveKind(), parentResultTy, resultTy)
        .highlight(resultTL.getSourceRange());

      auto sugaredForm =
        dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(resultTL.getTypeRepr());
      if (sugaredForm) {
        TC.diagnose(sugaredForm->getExclamationLoc(),
                    diag::override_unnecessary_IUO_use_strict)
          .fixItReplace(sugaredForm->getExclamationLoc(), "?");
      }

      auto endLoc = Lexer::getLocForEndOfToken(TC.Context.SourceMgr,
                                               resultTL.getSourceRange().End);
      TC.diagnose(resultTL.getSourceRange().Start,
                  diag::override_unnecessary_IUO_silence)
        .fixItInsert(resultTL.getSourceRange().Start, "(")
        .fixItInsert(endLoc, ")");
    };

    checkResult(methodAsFunc->getBodyResultTypeLoc(), parentTy->getResult());
  }

  /// Make sure that there is an invalid 'override' attribute on the
  /// given declaration.
  static void makeInvalidOverrideAttr(TypeChecker &TC, ValueDecl *decl) {
    if (auto overrideAttr = decl->getAttrs().getAttribute<OverrideAttr>()) {
      overrideAttr->setInvalid();
    } else {
      auto attr = new (TC.Context) OverrideAttr(true);
      decl->getAttrs().add(attr);
      attr->setInvalid();
    }

    if (auto storage = dyn_cast<AbstractStorageDecl>(decl)) {
      if (auto getter = storage->getGetter())
        makeInvalidOverrideAttr(TC, getter);
      if (auto setter = storage->getSetter())
        makeInvalidOverrideAttr(TC, setter);
    }
  }

  /// Determine which method or subscript this method or subscript overrides
  /// (if any).
  ///
  /// \returns true if an error occurred.
  static bool checkOverrides(TypeChecker &TC, ValueDecl *decl) {
    if (decl->isInvalid() || decl->getOverriddenDecl())
      return false;

    auto owningTy = decl->getDeclContext()->getDeclaredInterfaceType();
    if (!owningTy)
      return false;

    auto classDecl = owningTy->getClassOrBoundGenericClass();
    if (!classDecl)
      return false;

    Type superclass = classDecl->getSuperclass();
    if (!superclass)
      return false;

    // Ignore accessor methods (e.g. getters and setters), they will be handled
    // when their storage decl is processed.
    if (auto *fd = dyn_cast<FuncDecl>(decl))
      if (fd->isAccessor())
        return false;
    
    auto method = dyn_cast<AbstractFunctionDecl>(decl);
    ConstructorDecl *ctor = nullptr;
    if (method)
      ctor = dyn_cast<ConstructorDecl>(method);

    auto abstractStorage = dyn_cast<AbstractStorageDecl>(decl);
    assert((method || abstractStorage) && "Not a method or abstractStorage?");
    SubscriptDecl *subscript = nullptr;
    if (abstractStorage)
      subscript = dyn_cast<SubscriptDecl>(abstractStorage);

    // Figure out the type of the declaration that we're using for comparisons.
    auto declTy = decl->getInterfaceType()->getUnlabeledType(TC.Context);
    if (method) {
      declTy = declTy->getWithoutNoReturn(2);
      declTy = declTy->castTo<AnyFunctionType>()->getResult();
    } else {
      declTy = declTy->getReferenceStorageReferent();
    }

    // Ignore the optionality of initializers when comparing types;
    // we'll enforce this separately
    if (ctor) {
      declTy = dropResultOptionality(declTy, 1);
    }

    // Look for members with the same name and matching types as this
    // one.
    auto superclassMetaTy = MetatypeType::get(superclass);
    bool retried = false;
    DeclName name = decl->getFullName();

  retry:
    LookupResult members = TC.lookupMember(superclassMetaTy, name,
                                           decl->getDeclContext(),
                                           /*isKnownPrivate=*/false,
                                           /*allowDynamicLookup=*/false);

    typedef std::tuple<ValueDecl *, bool, Type> MatchType;
    SmallVector<MatchType, 2> matches;
    bool hadExactMatch = false;

    for (auto member : members) {
      if (member->isInvalid())
        continue;

      if (member->getKind() != decl->getKind())
        continue;

      auto parentDecl = cast<ValueDecl>(member);

      // Check whether there are any obvious reasons why the two given
      // declarations do not have an overriding relationship.
      if (!areOverrideCompatibleSimple(decl, parentDecl))
        continue;

      auto parentMethod = dyn_cast<AbstractFunctionDecl>(parentDecl);
      auto parentStorage = dyn_cast<AbstractStorageDecl>(parentDecl);
      assert(parentMethod || parentStorage);

      // If both are Objective-C, then match based on selectors or
      // subscript kind and check the types separately.
      bool objCMatch = false;
      if (parentDecl->isObjC() && decl->isObjC()) {
        if (method) {
          if (method->getObjCSelector() == parentMethod->getObjCSelector())
            objCMatch = true;
        } else if (auto *parentSubscript =
                     dyn_cast<SubscriptDecl>(parentStorage)) {
          // If the subscript kinds don't match, it's not an override.
          if (subscript->getObjCSubscriptKind(&TC)
                == parentSubscript->getObjCSubscriptKind(&TC))
            objCMatch = true;
        }

        // Properties don't need anything here since they are always
        // checked by name.
      }

      // Check whether the types are identical.
      // FIXME: It's wrong to use the uncurried types here for methods.
      auto parentDeclTy = adjustSuperclassMemberDeclType(TC, parentDecl,
                                                         owningTy);
      parentDeclTy = parentDeclTy->getUnlabeledType(TC.Context);
      if (method) {
        parentDeclTy = parentDeclTy->getWithoutNoReturn(2);
        parentDeclTy = parentDeclTy->castTo<AnyFunctionType>()->getResult();
      } else {
        parentDeclTy = parentDeclTy->getReferenceStorageReferent();
      }

      // Ignore the optionality of initializers when comparing types;
      // we'll enforce this separately
      if (ctor) {
        parentDeclTy = dropResultOptionality(parentDeclTy, 1);

        // Factory methods cannot be overridden.
        auto parentCtor = cast<ConstructorDecl>(parentDecl);
        if (parentCtor->isFactoryInit())
          continue;
      }

      if (declTy->isEqual(parentDeclTy)) {
        matches.push_back(std::make_tuple(parentDecl, true, parentDeclTy));
        hadExactMatch = true;
        continue;
      }
      
      // If this is a property, we accept the match and then reject it below if
      // the types don't line up, since you can't overload properties based on
      // types.
      if (isa<VarDecl>(parentDecl)) {
        matches.push_back(std::make_tuple(parentDecl, false, parentDeclTy));
        continue;
      }

      // Failing that, check for subtyping.
      if (declTy->canOverride(parentDeclTy, parentDecl->isObjC(), &TC)) {
        // If the Objective-C selectors match, always call it exact.
        matches.push_back(
            std::make_tuple(parentDecl, objCMatch, parentDeclTy));
        hadExactMatch |= objCMatch;
        continue;
      }

      // Not a match. If we had an Objective-C match, this is a serious problem.
      if (objCMatch) {
        if (method) {
          TC.diagnose(decl, diag::override_objc_type_mismatch_method,
                      method->getObjCSelector(), declTy);
        } else {
          TC.diagnose(decl, diag::override_objc_type_mismatch_subscript,
                      static_cast<unsigned>(
                        subscript->getObjCSubscriptKind(&TC)),
                      declTy);
        }
        TC.diagnose(parentDecl, diag::overridden_here_with_type,
                    parentDeclTy);
        
        // Put an invalid 'override' attribute here.
        makeInvalidOverrideAttr(TC, decl);

        return true;
      }
    }

    // If we have no matches.
    if (matches.empty()) {
      // If we already re-tried, or if the user didn't indicate that this is
      // an override, or we don't know what else to look for, try again.
      if (retried || name.isSimpleName() ||
          name.getArgumentNames().size() == 0 ||
          !decl->getAttrs().hasAttribute<OverrideAttr>())
        return false;

      // Try looking again, this time using just the base name, so that we'll
      // catch mismatched names.
      retried = true;
      name = name.getBaseName();
      goto retry;
    }

    // If we had an exact match, throw away any non-exact matches.
    if (hadExactMatch)
      matches.erase(std::remove_if(matches.begin(), matches.end(),
                                   [&](MatchType &match) {
                                     return !std::get<1>(match);
                                   }), matches.end());

    // If we have a single match (exact or not), take it.
    if (matches.size() == 1) {
      auto matchDecl = std::get<0>(matches[0]);
      auto matchType = std::get<2>(matches[0]);

      // If the name of our match differs from the name we were looking for,
      // complain.
      if (decl->getFullName() != matchDecl->getFullName()) {
        auto diag = TC.diagnose(decl, diag::override_argument_name_mismatch,
                                isa<ConstructorDecl>(decl),
                                decl->getFullName(),
                                matchDecl->getFullName());
        TC.fixAbstractFunctionNames(diag, cast<AbstractFunctionDecl>(decl),
                                    matchDecl->getFullName());
      }

      // If we have an explicit ownership modifier and our parent doesn't,
      // complain.
      auto parentAttr = matchDecl->getAttrs().getAttribute<OwnershipAttr>();
      if (auto ownershipAttr = decl->getAttrs().getAttribute<OwnershipAttr>()) {
        Ownership parentOwnership;
        if (parentAttr)
          parentOwnership = parentAttr->get();
        else
          parentOwnership = Ownership::Strong;
        if (parentOwnership != ownershipAttr->get()) {
          TC.diagnose(decl, diag::override_ownership_mismatch,
                      (unsigned)parentOwnership,
                      (unsigned)ownershipAttr->get());
          TC.diagnose(matchDecl, diag::overridden_here);
        }
      }

      // Check accessibility.
      // FIXME: Copied from TypeCheckProtocol.cpp.
      Accessibility requiredAccess =
        std::min(classDecl->getAccessibility(), matchDecl->getAccessibility());
      bool shouldDiagnose = false;
      bool shouldDiagnoseSetter = false;
      if (requiredAccess > Accessibility::Private &&
          !isa<ConstructorDecl>(decl)) {
        shouldDiagnose = (decl->getAccessibility() < requiredAccess);

        if (!shouldDiagnose && matchDecl->isSettable(classDecl)) {
          auto matchASD = cast<AbstractStorageDecl>(matchDecl);
          if (matchASD->isSetterAccessibleFrom(classDecl)) {
            auto ASD = cast<AbstractStorageDecl>(decl);
            const DeclContext *accessDC = nullptr;
            if (requiredAccess == Accessibility::Internal)
              accessDC = classDecl->getParentModule();
            shouldDiagnoseSetter = !ASD->isSetterAccessibleFrom(accessDC);
          }
        }
      }
      if (shouldDiagnose || shouldDiagnoseSetter) {
        bool overriddenForcesAccess =
          (requiredAccess == matchDecl->getAccessibility());
        {
          auto diag = TC.diagnose(decl, diag::override_not_accessible,
                                  shouldDiagnoseSetter,
                                  decl->getDescriptiveKind(),
                                  overriddenForcesAccess);
          fixItAccessibility(diag, decl, requiredAccess, shouldDiagnoseSetter);
        }
        TC.diagnose(matchDecl, diag::overridden_here);
      }

      // If this is an exact type match, we're successful!
      if (declTy->isEqual(matchType)) {
        // Nothing to do.
        
      } else if (method) {
        // Private migration help for overrides of Objective-C methods.
        if ((!isa<FuncDecl>(method) || !cast<FuncDecl>(method)->isAccessor()) &&
            superclass->getClassOrBoundGenericClass()->isObjC()) {
          diagnoseUnnecessaryIUOs(TC, method,
                                  cast<AbstractFunctionDecl>(matchDecl),
                                  owningTy);
        }

      } else if (auto subscript =
                   dyn_cast_or_null<SubscriptDecl>(abstractStorage)) {
        // Otherwise, if this is a subscript, validate that covariance is ok.
        // If the parent is non-mutable, it's okay to be covariant.
        auto parentSubscript = cast<SubscriptDecl>(matchDecl);
        if (parentSubscript->getSetter()) {
          TC.diagnose(subscript, diag::override_mutable_covariant_subscript,
                      declTy, matchType);
          TC.diagnose(matchDecl, diag::subscript_override_here);
          return true;
        }
      } else if (auto property = dyn_cast_or_null<VarDecl>(abstractStorage)) {
        auto propertyTy = property->getInterfaceType();
        auto parentPropertyTy = adjustSuperclassMemberDeclType(TC, matchDecl,
                                                               superclass);
        
        if (!propertyTy->canOverride(parentPropertyTy, false, &TC)) {
          TC.diagnose(property, diag::override_property_type_mismatch,
                      property->getName(), propertyTy, parentPropertyTy);
          TC.diagnose(matchDecl, diag::property_override_here);
          return true;
        }
        
        // Differing only in Optional vs. ImplicitlyUnwrappedOptional is fine.
        bool IsSilentDifference = false;
        if (auto propertyTyNoOptional = propertyTy->getAnyOptionalObjectType())
          if (auto parentPropertyTyNoOptional =
              parentPropertyTy->getAnyOptionalObjectType())
            if (propertyTyNoOptional->isEqual(parentPropertyTyNoOptional))
              IsSilentDifference = true;
        
        // The overridden property must not be mutable.
        if (cast<AbstractStorageDecl>(matchDecl)->getSetter() &&
            !IsSilentDifference) {
          TC.diagnose(property, diag::override_mutable_covariant_property,
                      property->getName(), parentPropertyTy, propertyTy);
          TC.diagnose(matchDecl, diag::property_override_here);
          return true;
        }
      }

      return recordOverride(TC, decl, matchDecl);
    }

    // We override more than one declaration. Complain.
    TC.diagnose(decl,
                retried ? diag::override_multiple_decls_arg_mismatch
                        : diag::override_multiple_decls_base,
                decl->getFullName());
    for (auto match : matches) {
      auto matchDecl = std::get<0>(match);
      if (retried) {
        auto diag = TC.diagnose(matchDecl, diag::overridden_near_match_here,
                                isa<ConstructorDecl>(matchDecl),
                                matchDecl->getFullName());
        TC.fixAbstractFunctionNames(diag, cast<AbstractFunctionDecl>(decl),
                                    matchDecl->getFullName());
        continue;
      }

      TC.diagnose(std::get<0>(match), diag::overridden_here);
    }
    return true;
  }

  /// Attribute visitor that checks how the given attribute should be
  /// considered when overriding a declaration.
  class AttributeOverrideChecker
          : public AttributeVisitor<AttributeOverrideChecker> {
    TypeChecker &TC;
    ValueDecl *Base;
    ValueDecl *Override;

  public:
    AttributeOverrideChecker(TypeChecker &tc, ValueDecl *base,
                             ValueDecl *override)
      : TC(tc), Base(base), Override(override) { }

    /// Deleting this ensures that all attributes are covered by the visitor
    /// below.
    void visitDeclAttribute(DeclAttribute *A) = delete;

#define UNINTERESTING_ATTR(CLASS)                                              \
    void visit##CLASS##Attr(CLASS##Attr *) {}

    UNINTERESTING_ATTR(Accessibility)
    UNINTERESTING_ATTR(Asmname)
    UNINTERESTING_ATTR(ClassProtocol)
    UNINTERESTING_ATTR(Exported)
    UNINTERESTING_ATTR(IBAction)
    UNINTERESTING_ATTR(IBDesignable)
    UNINTERESTING_ATTR(IBInspectable)
    UNINTERESTING_ATTR(IBOutlet)
    UNINTERESTING_ATTR(Inline)
    UNINTERESTING_ATTR(Effects)
    UNINTERESTING_ATTR(Lazy)
    UNINTERESTING_ATTR(LLDBDebuggerFunction)
    UNINTERESTING_ATTR(Mutating)
    UNINTERESTING_ATTR(NonMutating)
    UNINTERESTING_ATTR(NSApplicationMain)
    UNINTERESTING_ATTR(NSCopying)
    UNINTERESTING_ATTR(NSManaged)
    UNINTERESTING_ATTR(ObjC)
    UNINTERESTING_ATTR(ObjCBridged)
    UNINTERESTING_ATTR(Optional)
    UNINTERESTING_ATTR(Override)
    UNINTERESTING_ATTR(RawDocComment)
    UNINTERESTING_ATTR(Required)
    UNINTERESTING_ATTR(Convenience)
    UNINTERESTING_ATTR(Semantics)
    UNINTERESTING_ATTR(SetterAccessibility)
    UNINTERESTING_ATTR(UIApplicationMain)
    UNINTERESTING_ATTR(ObjCNonLazyRealization)
    UNINTERESTING_ATTR(UnsafeNoObjCTaggedPointer)

    UNINTERESTING_ATTR(Prefix)
    UNINTERESTING_ATTR(Postfix)
    UNINTERESTING_ATTR(Infix)
    UNINTERESTING_ATTR(Ownership)

    UNINTERESTING_ATTR(RequiresStoredPropertyInits)
    UNINTERESTING_ATTR(Transparent)
    UNINTERESTING_ATTR(SILStored)

#undef UNINTERESTING_ATTR

    void visitAvailabilityAttr(AvailabilityAttr *attr) {
      // FIXME: Check that this declaration is at least as available as the
      // one it overrides.
    }

    void visitFinalAttr(FinalAttr *attr) {
      // If this is an accessor, don't complain if we would have
      // complained about the storage declaration.
      if (auto func = dyn_cast<FuncDecl>(Override)) {
        if (auto storageDecl = func->getAccessorStorageDecl()) {
          if (storageDecl->getOverriddenDecl() &&
              storageDecl->getOverriddenDecl()->isFinal())
            return;
        }
      }

      // FIXME: Customize message to the kind of thing.
      TC.diagnose(Override, diag::override_final, 
                  Override->getDescriptiveKind());
      TC.diagnose(Base, diag::overridden_here);
    }

    void visitAutoClosureAttr(AutoClosureAttr *attr) {
      if (Base->getAttrs().hasAttribute<AutoClosureAttr>() !=
          Override->getAttrs().hasAttribute<AutoClosureAttr>()) {
        TC.diagnose(Override, diag::inconsistent_attribute_override,
                    attr->getAttrName());
        TC.diagnose(Base, diag::overridden_here);
      }
    }
    void visitNoEscapeAttr(NoEscapeAttr *attr) {
      if (Base->getAttrs().hasAttribute<NoEscapeAttr>() !=
          Override->getAttrs().hasAttribute<NoEscapeAttr>()) {
        TC.diagnose(Override, diag::inconsistent_attribute_override,
                    attr->getAttrName());
        TC.diagnose(Base, diag::overridden_here);
      }
    }

    void visitNoReturnAttr(NoReturnAttr *attr) {
      // Disallow overriding a @noreturn function with a returning one.
      if (Base->getAttrs().hasAttribute<NoReturnAttr>() &&
          !Override->getAttrs().hasAttribute<NoReturnAttr>()) {
        TC.diagnose(Override, diag::override_noreturn_with_return);
        TC.diagnose(Base, diag::overridden_here);
      }
    }

    void visitDynamicAttr(DynamicAttr *attr) {
      if (!Override->getAttrs().hasAttribute<DynamicAttr>())
        // Dynamic is inherited.
        Override->getAttrs().add(
                                new (TC.Context) DynamicAttr(/*implicit*/true));
    }
  };

  /// Determine whether overriding the given declaration requires a keyword.
  static bool overrideRequiresKeyword(ValueDecl *overridden) {
    if (auto ctor = dyn_cast<ConstructorDecl>(overridden)) {
      return ctor->isDesignatedInit() && !ctor->isRequired();
    }

    return true;
  }

  /// Record that the \c overriding declarations overrides the
  /// \c overridden declaration.
  ///
  /// \returns true if an error occurred.
  static bool recordOverride(TypeChecker &TC, ValueDecl *override,
                             ValueDecl *base, bool isKnownObjC = false) {
    // Check property and subscript overriding.
    if (auto *baseASD = dyn_cast<AbstractStorageDecl>(base)) {
      auto *overrideASD = cast<AbstractStorageDecl>(override);
      
      // Make sure that the overriding property doesn't have storage.
      if (overrideASD->hasStorage() && !overrideASD->hasObservers()) {
        TC.diagnose(overrideASD, diag::override_with_stored_property,
                    overrideASD->getName());
        TC.diagnose(baseASD, diag::property_override_here);
        return true;
      }

      // Make sure that an observing property isn't observing something
      // read-only.  Observing properties look at change, read-only properties
      // have nothing to observe!
      bool baseIsSettable = baseASD->isSettable(baseASD->getDeclContext());
      if (baseIsSettable && TC.Context.LangOpts.EnableAccessControl) {
        baseIsSettable =
           baseASD->isSetterAccessibleFrom(overrideASD->getDeclContext());
      }
      if (overrideASD->hasObservers() && !baseIsSettable) {
        TC.diagnose(overrideASD, diag::observing_readonly_property,
                    overrideASD->getName());
        TC.diagnose(baseASD, diag::property_override_here);
        return true;
      }

      // Make sure we're not overriding a settable property with a non-settable
      // one.  The only reasonable semantics for this would be to inherit the
      // setter but override the getter, and that would be surprising at best.
      if (baseIsSettable && !override->isSettable(override->getDeclContext())) {
        TC.diagnose(overrideASD, diag::override_mutable_with_readonly_property,
                    overrideASD->getName());
        TC.diagnose(baseASD, diag::property_override_here);
        return true;
      }
      
      
      // Make sure a 'let' property is only overridden by 'let' properties.  A
      // let property provides more guarantees than the getter of a 'var'
      // property.
      if (isa<VarDecl>(baseASD) && cast<VarDecl>(baseASD)->isLet()) {
        TC.diagnose(overrideASD, diag::override_let_property,
                    overrideASD->getName());
        TC.diagnose(baseASD, diag::property_override_here);
        return true;
      }
    }
    
    // Non-Objective-C declarations in extensions cannot override or
    // be overridden.
    if ((base->getDeclContext()->isExtensionContext() ||
         override->getDeclContext()->isExtensionContext()) &&
        !base->isObjC() && !isKnownObjC) {
      TC.diagnose(override, diag::override_decl_extension,
                  !override->getDeclContext()->isExtensionContext());
      TC.diagnose(base, diag::overridden_here);
      return true;
    }
    
    // If the overriding declaration does not have the 'override' modifier on
    // it, complain.
    if (!override->getAttrs().hasAttribute<OverrideAttr>() &&
        overrideRequiresKeyword(base)) {
      // FIXME: rdar://16320042 - For properties, we don't have a useful
      // location for the 'var' token.  Instead of emitting a bogus fixit, only
      // emit the fixit for 'func's.
      if (!isa<VarDecl>(override))
        TC.diagnose(override, diag::missing_override)
            .fixItInsert(override->getStartLoc(), "override ");
      else
        TC.diagnose(override, diag::missing_override);
      TC.diagnose(base, diag::overridden_here);
      override->getAttrs().add(
          new (TC.Context) OverrideAttr(SourceLoc()));
    }

    // FIXME: Possibly should extend to more availability checking.
    if (base->getAttrs().isUnavailable(TC.Context)) {
      TC.diagnose(override, diag::override_unavailable, override->getName());
    }
    
    // API availability ranges are contravariant: make sure the version range
    // of an overriden declaration is fully contained in the range of the
    // overriding declaration.
    if (TC.getLangOpts().EnableExperimentalAvailabilityChecking) {
      VersionRange overrideRange = TypeChecker::availableRange(override,
                                                               TC.Context);
      VersionRange baseRange = TypeChecker::availableRange(base, TC.Context);
      
      if (!baseRange.isContainedIn(overrideRange)) {
        TC.diagnose(override, diag::override_less_available, override->getName());
        TC.diagnose(base, diag::overridden_here);
      }
    }

    /// Check attributes associated with the base; some may need to merged with
    /// or checked against attributes in the overriding declaration.
    AttributeOverrideChecker attrChecker(TC, base, override);
    for (auto attr : base->getAttrs()) {
      attrChecker.visit(attr);
    }

    if (auto overridingFunc = dyn_cast<FuncDecl>(override)) {
      overridingFunc->setOverriddenDecl(cast<FuncDecl>(base));
    } else if (auto overridingCtor = dyn_cast<ConstructorDecl>(override)) {
      overridingCtor->setOverriddenDecl(cast<ConstructorDecl>(base));
    } else if (auto overridingASD = dyn_cast<AbstractStorageDecl>(override)) {
      auto *baseASD = cast<AbstractStorageDecl>(base);
      overridingASD->setOverriddenDecl(baseASD);

      // Make sure we get consistent overrides for the accessors as well.
      if (!baseASD->hasAccessorFunctions())
        addTrivialAccessorsToStorage(baseASD, TC);
      maybeAddMaterializeForSet(overridingASD, TC);

      auto recordAccessorOverride = [&](AccessorKind kind) {
        // We need the same accessor on both.
        auto baseAccessor = baseASD->getAccessorFunction(kind);
        if (!baseAccessor) return;
        auto overridingAccessor = overridingASD->getAccessorFunction(kind);
        if (!overridingAccessor) return;

        // For setter accessors, we need the base's setter to be
        // accessible from the overriding context, or it's not an override.
        if ((kind == AccessorKind::IsSetter ||
             kind == AccessorKind::IsMaterializeForSet) &&
            !baseASD->isSetterAccessibleFrom(overridingASD->getDeclContext()))
          return;

        // FIXME: Egregious hack to set an 'override' attribute.
        if (!overridingAccessor->getAttrs().hasAttribute<OverrideAttr>()) {
          auto loc = overridingASD->getOverrideLoc();
          overridingAccessor->getAttrs().add(
              new (TC.Context) OverrideAttr(loc));
        }

        recordOverride(TC, overridingAccessor, baseAccessor,
                       baseASD->isObjC());
      };

      recordAccessorOverride(AccessorKind::IsGetter);
      recordAccessorOverride(AccessorKind::IsSetter);
      recordAccessorOverride(AccessorKind::IsMaterializeForSet);
    } else {
      llvm_unreachable("Unexpected decl");
    }
    
    return false;
  }

  /// Compute the interface type of the given enum element.
  void computeEnumElementInterfaceType(EnumElementDecl *elt) {
    auto enumDecl = cast<EnumDecl>(elt->getDeclContext());
    assert(enumDecl->isGenericContext() && "Not a generic enum");

    // Build the generic function type.
    auto funcTy = elt->getType()->castTo<AnyFunctionType>();
    auto inputTy = TC.getInterfaceTypeFromInternalType(enumDecl,
                                                       funcTy->getInput());
    auto resultTy = TC.getInterfaceTypeFromInternalType(enumDecl,
                                                        funcTy->getResult());
    auto interfaceTy
      = GenericFunctionType::get(enumDecl->getGenericSignature(),
                                 inputTy, resultTy, funcTy->getExtInfo());

    // Record the interface type.
    elt->setInterfaceType(interfaceTy);
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (IsSecondPass) {
      checkAccessibility(TC, EED);
      return;
    }
    if (EED->hasType())
      return;

    TC.checkDeclAttributesEarly(EED);

    EnumDecl *ED = EED->getParentEnum();
    Type ElemTy = ED->getDeclaredTypeInContext();

    if (!EED->hasAccessibility())
      EED->setAccessibility(ED->getAccessibility());
    
    // Only attempt to validate the argument type or raw value if the element
    // is not currenly being validated.
    if (EED->getRecursiveness() == ElementRecursiveness::NotRecursive) {
      EED->setRecursiveness(ElementRecursiveness::PotentiallyRecursive);
      
      validateAttributes(TC, EED);
      
      if (!EED->getArgumentTypeLoc().isNull()) {
        if (TC.validateType(EED->getArgumentTypeLoc(), EED->getDeclContext(),
                            TR_EnumCase)) {
          EED->overwriteType(ErrorType::get(TC.Context));
          EED->setInvalid();
          return;
        }
      }
      
      // If we have a raw value, make sure there's a raw type as well.
      if (auto *rawValue = EED->getRawValueExpr()) {
        
        Type rawTy;
        if (!ED->hasRawType()) {
          TC.diagnose(rawValue->getLoc(), diag::enum_raw_value_without_raw_type);
          // Recover by setting the raw type as this element's type.
          Expr *typeCheckedExpr = rawValue;
          if (!TC.typeCheckExpression(typeCheckedExpr, ED, rawTy, Type(),
                                      /*inExpression=*/false)) {
            EED->setTypeCheckedRawValueExpr(typeCheckedExpr);
          }
        } else {
          // Wait until the second pass, when all the raw value expressions
          // can be checked together.
        }
      }
    } else if (EED->getRecursiveness() ==
                ElementRecursiveness::PotentiallyRecursive) {
      EED->setRecursiveness(ElementRecursiveness::Recursive);
    }
    
    // If the element was not already marked as recursive by a re-entrant call,
    // we can be sure it's not recursive.
    if (EED->getRecursiveness() == ElementRecursiveness::PotentiallyRecursive) {
      EED->setRecursiveness(ElementRecursiveness::NotRecursive);
    }

    // If we have a simple element, just set the type.
    if (EED->getArgumentType().isNull()) {
      Type argTy = MetatypeType::get(ElemTy);
      Type fnTy;
      if (auto gp = ED->getGenericParamsOfContext())
        fnTy = PolymorphicFunctionType::get(argTy, ElemTy, gp);
      else
        fnTy = FunctionType::get(argTy, ElemTy);
      EED->setType(fnTy);
      
      // Test for type parameters, as opposed to a generic decl context, in
      // case the enclosing enum type was illegally declared inside of a generic
      // context. (In that case, we'll post a diagnostic while visiting the
      // parent enum.)
      if (EED->getParentEnum()->getGenericParams())
        computeEnumElementInterfaceType(EED);
      return;
    }

    Type fnTy = FunctionType::get(EED->getArgumentType(), ElemTy);
    if (auto gp = ED->getGenericParamsOfContext())
      fnTy = PolymorphicFunctionType::get(MetatypeType::get(ElemTy),
                                          fnTy, gp);
    else
      fnTy = FunctionType::get(MetatypeType::get(ElemTy), fnTy);
    EED->setType(fnTy);

    if (EED->getParentEnum()->getGenericParams())
      computeEnumElementInterfaceType(EED);

    // Require the carried type to be materializable.
    if (!EED->getArgumentType()->isMaterializable()) {
      TC.diagnose(EED->getLoc(), diag::enum_element_not_materializable);
      EED->overwriteType(ErrorType::get(TC.Context));
      EED->setInvalid();
    }
    TC.checkDeclAttributes(EED);
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    TC.validateExtension(ED);

    if (ED->isInvalid()) {
      // Mark children as invalid.
      // FIXME: This is awful.
      for (auto member : ED->getMembers()) {
        member->setInvalid();
        if (ValueDecl *VD = dyn_cast<ValueDecl>(member))
          VD->overwriteType(ErrorType::get(TC.Context));
      }
      return;
    }

    TC.checkDeclAttributesEarly(ED);

    if (!IsSecondPass) {
      CanType ExtendedTy = DeclContext::getExtendedType(ED);

      if (!isa<EnumType>(ExtendedTy) &&
          !isa<StructType>(ExtendedTy) &&
          !isa<ClassType>(ExtendedTy) &&
          !isa<BoundGenericEnumType>(ExtendedTy) &&
          !isa<BoundGenericStructType>(ExtendedTy) &&
          !isa<BoundGenericClassType>(ExtendedTy) &&
          !isa<ErrorType>(ExtendedTy)) {
        TC.diagnose(ED->getStartLoc(), diag::non_nominal_extension,
                    isa<ProtocolType>(ExtendedTy), ExtendedTy);
        // FIXME: It would be nice to point out where we found the named type
        // declaration, if any.
        ED->setInvalid();
      }

      TC.checkInheritanceClause(ED);
      if (auto nominal = ExtendedTy->getAnyNominal())
        TC.validateDecl(nominal);

      validateAttributes(TC, ED);
    }

    if (!ED->isInvalid()) {
      for (Decl *Member : ED->getMembers())
        visit(Member);
    }
    
    if (!IsFirstPass) {
      computeDefaultAccessibility(TC, ED);
      checkExplicitConformance(ED, ED->getExtendedType());
    }
    TC.checkDeclAttributes(ED);
 }

  void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
    // See swift::performTypeChecking for TopLevelCodeDecl handling.
    llvm_unreachable("TopLevelCodeDecls are handled elsewhere");
  }
  
  void visitIfConfigDecl(IfConfigDecl *ICD) {
    // The active members of the #if block will be type checked along with
    // their enclosing declaration.
    TC.checkDeclAttributesEarly(ICD);
    TC.checkDeclAttributes(ICD);
  }

  void visitConstructorDecl(ConstructorDecl *CD) {
    if (CD->isInvalid()) {
      CD->overwriteType(ErrorType::get(TC.Context));
      return;
    }

    if (!IsFirstPass) {
      if (CD->getBody()) {
        TC.definedFunctions.push_back(CD);
      } else if (requiresDefinition(CD)) {
        // Complain if we should have a body.
        TC.diagnose(CD->getLoc(), diag::missing_initializer_def);
      }
    }

    if (IsSecondPass) {
      checkAccessibility(TC, CD);
      return;
    }
    if (CD->hasType())
      return;

    TC.checkDeclAttributesEarly(CD);
    TC.computeAccessibility(CD);

    assert(CD->getDeclContext()->isTypeContext()
           && "Decl parsing must prevent constructors outside of types!");

    // convenience initializers are only allowed on classes and in
    // extensions thereof.
    if (CD->isConvenienceInit()) {
      if (auto extType = CD->getExtensionType()) {
        if (!extType->getClassOrBoundGenericClass() &&
            !extType->is<ErrorType>()) {
          // FIXME: Add a Fix-It here, which requires source-location
          // information within the AST for "convenience".
          TC.diagnose(CD->getLoc(), diag::nonclass_convenience_init,
                      extType);
          CD->setInitKind(CtorInitializerKind::Designated);
        }
      }
    } else if (auto extType = CD->getExtensionType()) {
      // A designated initializer for a class must be written within the class
      // itself.
      if (extType->getClassOrBoundGenericClass() &&
          isa<ExtensionDecl>(CD->getDeclContext())) {
        TC.diagnose(CD->getLoc(), diag::designated_init_in_extension, extType)
          .fixItInsert(CD->getLoc(), "convenience ");
        CD->setInitKind(CtorInitializerKind::Convenience);
      }
    }

    GenericParamList *outerGenericParams;
    Type SelfTy = configureImplicitSelf(TC, CD, outerGenericParams);

    Optional<ArchetypeBuilder> builder;
    if (auto gp = CD->getGenericParams()) {
      // Write up generic parameters and check the generic parameter list.
      gp->setOuterParameters(outerGenericParams);

      if (TC.validateGenericFuncSignature(CD)) {
        CD->overwriteType(ErrorType::get(TC.Context));
        CD->setInvalid();
      } else {
        ArchetypeBuilder builder =
          TC.createArchetypeBuilder(CD->getModuleContext());
        checkGenericParamList(builder, gp, TC, CD->getDeclContext());

        // Type check the constructor parameters.
        if (semaFuncParamPatterns(CD)) {
          CD->overwriteType(ErrorType::get(TC.Context));
          CD->setInvalid();
        }

        // Infer requirements from the parameters of the constructor.
        builder.inferRequirements(CD->getBodyParamPatterns()[1]);

        // Revert the constructor signature so it can be type-checked with
        // archetypes below.
        TC.revertGenericFuncSignature(CD);

        // Assign archetypes.
        finalizeGenericParamList(builder, gp, CD, TC);
      }
    } else if (outerGenericParams) {
      if (TC.validateGenericFuncSignature(CD)) {
        CD->overwriteType(ErrorType::get(TC.Context));
        CD->setInvalid();
      } else {
        // Revert all of the types within the signature of the constructor.
        TC.revertGenericFuncSignature(CD);
      }
    }

    // Type check the constructor parameters.
    if (CD->isInvalid() || semaFuncParamPatterns(CD)) {
      CD->overwriteType(ErrorType::get(TC.Context));
      CD->setInvalid();
    } else {
      configureConstructorType(CD, outerGenericParams, SelfTy, 
                               CD->getBodyParamPatterns()[1]->getType());
    }

    validateAttributes(TC, CD);

    // Check whether this initializer overrides an initializer in its
    // superclass.
    if (!checkOverrides(TC, CD)) {
      // If an initializer has an override attribute but does not override
      // anything or overrides something that doesn't need an 'override'
      // keyword (e.g., a convenience initializer), complain.
      // anything, or overrides something that complain.
      if (auto *attr = CD->getAttrs().getAttribute<OverrideAttr>()) {
        if (!CD->getOverriddenDecl()) {
          TC.diagnose(CD, diag::initializer_does_not_override)
            .highlight(attr->getLocation());
          CD->setInvalid();
        } else if (!overrideRequiresKeyword(CD->getOverriddenDecl())) {
          // Special case: we are overriding a 'required' initializer, so we
          // need (only) the 'required' keyword.
          if (cast<ConstructorDecl>(CD->getOverriddenDecl())->isRequired()) {
            if (CD->getAttrs().hasAttribute<RequiredAttr>()) {
              TC.diagnose(CD, diag::required_initializer_override_keyword)
                .fixItRemove(attr->getLocation());
            } else {
              TC.diagnose(CD, diag::required_initializer_override_wrong_keyword)
                .fixItReplace(attr->getLocation(), "required");
              CD->getAttrs().add(
                new (TC.Context) RequiredAttr(/*implicit=*/true));
            }

            TC.diagnose(CD->getOverriddenDecl(),
                        diag::overridden_required_initializer_here);
          } else {
            // We tried to override a convenience initializer.
            TC.diagnose(CD, diag::initializer_does_not_override)
              .highlight(attr->getLocation());
            TC.diagnose(CD->getOverriddenDecl(),
                        diag::convenience_init_override_here);
          }
        }
      }

      // A failable initializer cannot override a non-failable one.
      // This would normally be diagnosed by the covariance rules;
      // however, those are disabled so that we can provide a more
      // specific diagnostic here.
      if (CD->getFailability() != OTK_None &&
          CD->getOverriddenDecl() &&
          CD->getOverriddenDecl()->getFailability() == OTK_None) {
        TC.diagnose(CD, diag::failable_initializer_override,
                    CD->getFullName());
        TC.diagnose(CD->getOverriddenDecl(), 
                    diag::nonfailable_initializer_override_here,
                    CD->getOverriddenDecl()->getFullName());
      }
    }

    // An initializer is ObjC-compatible if it's explicitly @objc or a member
    // of an ObjC-compatible class.
    Type ContextTy = CD->getDeclContext()->getDeclaredTypeInContext();
    if (ContextTy) {
      ProtocolDecl *protocolContext =
          dyn_cast<ProtocolDecl>(CD->getDeclContext());
      bool isMemberOfObjCProtocol =
          protocolContext && protocolContext->isObjC();
      ObjCReason reason = ObjCReason::DontDiagnose;
      if (CD->getAttrs().hasAttribute<ObjCAttr>())
        reason = ObjCReason::ExplicitlyObjC;
      else if (CD->getAttrs().hasAttribute<DynamicAttr>())
        reason = ObjCReason::ExplicitlyDynamic;
      else if (isMemberOfObjCProtocol)
        reason = ObjCReason::MemberOfObjCProtocol;
      bool isObjC = (reason != ObjCReason::DontDiagnose) ||
                    isImplicitlyObjC(CD, /*allowImplicit=*/true);
      if (isObjC &&
          (CD->isInvalid() || !TC.isRepresentableInObjC(CD, reason)))
        isObjC = false;
      markAsObjC(TC, CD, isObjC);
    }

    // If this initializer overrides a 'required' initializer, it must itself
    // be marked 'required'.
    if (!CD->getAttrs().hasAttribute<RequiredAttr>()) {
      if (CD->getOverriddenDecl() && CD->getOverriddenDecl()->isRequired()) {
        TC.diagnose(CD, diag::required_initializer_missing_keyword)
          .fixItInsert(CD->getLoc(), "required ");
        TC.diagnose(CD->getOverriddenDecl(),
                    diag::overridden_required_initializer_here);

        CD->getAttrs().add(
            new (TC.Context) RequiredAttr(/*IsImplicit=*/true));
      }
    }

    if (CD->isRequired() && ContextTy) {
      if (auto nominal = ContextTy->getAnyNominal()) {
        if (CD->getAccessibility() < nominal->getAccessibility()) {
          auto diag = TC.diagnose(CD,
                                  diag::required_initializer_not_accessible);
          fixItAccessibility(diag, CD, nominal->getAccessibility());
        }
      }
    }

    inferDynamic(TC.Context, CD);

    TC.checkDeclAttributes(CD);
  }

  void visitDestructorDecl(DestructorDecl *DD) {
    if (DD->isInvalid()) {
      DD->overwriteType(ErrorType::get(TC.Context));
      return;
    }

    if (!IsFirstPass) {
      if (DD->getBody())
        TC.definedFunctions.push_back(DD);
    }

    if (IsSecondPass || DD->hasType()) {
      return;
    }

    assert(DD->getDeclContext()->isTypeContext()
           && "Decl parsing must prevent destructors outside of types!");

    TC.checkDeclAttributesEarly(DD);
    if (!DD->hasAccessibility()) {
      auto enclosingClass = cast<ClassDecl>(DD->getParent());
      DD->setAccessibility(enclosingClass->getAccessibility());
    }

    GenericParamList *outerGenericParams;
    Type SelfTy = configureImplicitSelf(TC, DD, outerGenericParams);

    if (outerGenericParams)
      TC.validateGenericFuncSignature(DD);

    if (semaFuncParamPatterns(DD)) {
      DD->overwriteType(ErrorType::get(TC.Context));
      DD->setInvalid();
    }

    Type FnTy;
    if (outerGenericParams)
      FnTy = PolymorphicFunctionType::get(SelfTy,
                                          TupleType::getEmpty(TC.Context),
                                          outerGenericParams);
    else
      FnTy = FunctionType::get(SelfTy, TupleType::getEmpty(TC.Context));

    DD->setType(FnTy);

    // Destructors are always @objc, because their Objective-C entry point is
    // -dealloc.
    markAsObjC(TC, DD, true);

    validateAttributes(TC, DD);
    TC.checkDeclAttributes(DD);
  }
};
}; // end anonymous namespace.

bool swift::checkOverrides(TypeChecker &TC, ValueDecl *decl) {
  return DeclChecker::checkOverrides(TC, decl);
}

void TypeChecker::typeCheckDecl(Decl *D, bool isFirstPass) {
  PrettyStackTraceDecl StackTrace("type-checking", D);
  checkForForbiddenPrefix(D);
  bool isSecondPass =
    !isFirstPass && D->getDeclContext()->isModuleScopeContext();
  DeclChecker(*this, isFirstPass, isSecondPass).visit(D);
}


void TypeChecker::validateDecl(ValueDecl *D, bool resolveTypeParams) {
  if (hasEnabledForbiddenTypecheckPrefix())
    checkForForbiddenPrefix(D);

  validateAccessibility(D);

  // Validate the context. We don't do this for generic parameters, because
  // those are validated as part of their context.
  if (D->getKind() != DeclKind::GenericTypeParam) {
    auto dc = D->getDeclContext();
    if (auto nominal = dyn_cast<NominalTypeDecl>(dc))
      validateDecl(nominal, false);
    else if (auto ext = dyn_cast<ExtensionDecl>(dc))
      validateExtension(ext);
  }

  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::IfConfig:
    llvm_unreachable("not a value decl");

  case DeclKind::TypeAlias: {
    // Type aliases may not have an underlying type yet.
    auto typeAlias = cast<TypeAliasDecl>(D);
    if (typeAlias->getUnderlyingTypeLoc().getTypeRepr() &&
        !typeAlias->getUnderlyingTypeLoc().wasValidated())
      typeCheckDecl(typeAlias, true);
    
    break;
  }

  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType: {
    auto typeParam = cast<AbstractTypeParamDecl>(D);
    if (!resolveTypeParams || typeParam->getArchetype()) {
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(typeParam)) {
        DeclChecker(*this, false, false).visitAssociatedTypeDecl(assocType);
      }

      break;
    }
    
    // FIXME: Avoid full check in these cases?
    DeclContext *DC = typeParam->getDeclContext();
    switch (DC->getContextKind()) {
    case DeclContextKind::SerializedLocal:
    case DeclContextKind::Module:
    case DeclContextKind::FileUnit:
    case DeclContextKind::TopLevelCodeDecl:
    case DeclContextKind::Initializer:
      llvm_unreachable("cannot have type params");

    case DeclContextKind::NominalTypeDecl: {
      auto nominal = cast<NominalTypeDecl>(DC);
      typeCheckDecl(nominal, true);
      if (!typeParam->hasAccessibility())
        typeParam->setAccessibility(nominal->getAccessibility());
      break;
    }

    case DeclContextKind::ExtensionDecl:
      llvm_unreachable("not yet implemented");
    
    case DeclContextKind::AbstractClosureExpr:
      llvm_unreachable("cannot have type params");

    case DeclContextKind::AbstractFunctionDecl: {
      if (auto nominal = dyn_cast<NominalTypeDecl>(DC->getParent()))
        typeCheckDecl(nominal, true);
      else if (auto extension = dyn_cast<ExtensionDecl>(DC->getParent()))
        typeCheckDecl(extension, true);
      auto fn = cast<AbstractFunctionDecl>(DC);
      typeCheckDecl(fn, true);
      if (!typeParam->hasAccessibility())
        typeParam->setAccessibility(fn->getAccessibility());
      break;
    }
    }
    break;
  }
  
  case DeclKind::Enum:
  case DeclKind::Struct:
  case DeclKind::Class: {
    auto nominal = cast<NominalTypeDecl>(D);
    if (nominal->hasType())
      return;

    // Check generic parameters, if needed.
    if (auto gp = nominal->getGenericParams()) {
      gp->setOuterParameters(
        nominal->getDeclContext()->getGenericParamsOfContext());

      // Validate the generic type parameters.
      if (validateGenericTypeSignature(nominal)) {
        nominal->setInvalid();
        nominal->overwriteType(ErrorType::get(Context));
        return;
      }

      revertGenericParamList(gp);

      // If we're already validating the type declaration's generic signature,
      // avoid a potential infinite loop by not re-validating the generic
      // parameter list.
      if (!nominal->IsValidatingGenericSignature()) {
        ArchetypeBuilder builder =
          createArchetypeBuilder(nominal->getModuleContext());
        checkGenericParamList(builder, gp, *this, nominal->getDeclContext());
        finalizeGenericParamList(builder, gp, nominal, *this);
      }
    }

    // Compute the declared type.
    if (!nominal->hasType())
      nominal->computeType();

    validateAttributes(*this, D);
    checkInheritanceClause(D);

    // Mark a class as @objc. This must happen before checking its members.
    if (auto CD = dyn_cast<ClassDecl>(nominal)) {
      ClassDecl *superclassDecl = nullptr;
      if (CD->hasSuperclass())
        superclassDecl = CD->getSuperclass()->getClassOrBoundGenericClass();

      markAsObjC(*this, CD,
                 (CD->getAttrs().hasAttribute<ObjCAttr>() ||
                    (superclassDecl && superclassDecl->isObjC())));

      // Determine whether we require in-class initializers.
      if (CD->getAttrs().hasAttribute<RequiresStoredPropertyInitsAttr>() ||
          (superclassDecl && superclassDecl->requiresStoredPropertyInits()))
        CD->setRequiresStoredPropertyInits(true);
    }

    if (auto *ED = dyn_cast<EnumDecl>(nominal)) {
      // @objc enums use their raw values as the value representation, so we
      // need to force the values to be checked.
      if (ED->isObjC())
        checkEnumRawValues(*this, ED);
    }

    ValidatedTypes.insert(nominal);
    break;
  }

  case DeclKind::Protocol: {
    auto proto = cast<ProtocolDecl>(D);
    if (proto->hasType())
      return;
    proto->computeType();

    // Validate the generic type parameters.
    validateGenericTypeSignature(proto);

    revertGenericParamList(proto->getGenericParams());

    ArchetypeBuilder builder =
      createArchetypeBuilder(proto->getModuleContext());
    checkGenericParamList(builder, proto->getGenericParams(), *this,
                          proto->getDeclContext());
    finalizeGenericParamList(builder, proto->getGenericParams(), proto, *this);

    checkInheritanceClause(D);
    validateAttributes(*this, D);

    // Set the underlying type of each of the associated types to the
    // appropriate archetype.
    auto selfDecl = proto->getSelf();
    ArchetypeType *selfArchetype = builder.getArchetype(selfDecl);
    for (auto member : proto->getMembers()) {
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
        TypeLoc underlyingTy;
        ArchetypeType *archetype = selfArchetype;
        archetype = selfArchetype->getNestedType(assocType->getName())
          .getAsArchetype();
        if (!archetype)
          return;
        assocType->setArchetype(archetype);
      }
    }

    // If the protocol is @objc, it may only refine other @objc protocols.
    // FIXME: Revisit this restriction.
    if (proto->getAttrs().hasAttribute<ObjCAttr>()) {
      bool isObjC = true;

      for (auto inherited : proto->getProtocols()) {
        if (!inherited->isObjC()) {
          diagnose(proto->getLoc(),
                   diag::objc_protocol_inherits_non_objc_protocol,
                   proto->getDeclaredType(), inherited->getDeclaredType());
          diagnose(inherited->getLoc(), diag::protocol_here,
                   inherited->getName());
          isObjC = false;
        }
      }

      markAsObjC(*this, proto, isObjC);
    }
    break;
  }
      
  case DeclKind::Var:
  case DeclKind::Param: {
    auto VD = cast<VarDecl>(D);
    if (!VD->hasType()) {
      if (PatternBindingDecl *PBD = VD->getParentPattern()) {
        validatePatternBindingDecl(*this, PBD);
        if (PBD->isInvalid() || !PBD->getPattern()->hasType()) {
          PBD->getPattern()->setType(ErrorType::get(Context));
          setBoundVarsTypeError(PBD->getPattern(), Context);
          
          // If no type has been set for the initializer, we need to diagnose
          // the failure.
          if (PBD->getInit() &&
              !PBD->getInit()->getType()) {
            diagnose(PBD->getPattern()->getLoc(),
                     diag::identifier_init_failure,
                     PBD->getPattern()->getBodyName());
          }
          
          return;
        }
      } else if (VD->isImplicit() &&
                 (VD->getName() == Context.Id_self)) {
        // If the variable declaration is for a 'self' parameter, it may be
        // because the self variable was reverted whilst validating the function
        // signature.  In that case, reset the type.
        if (isa<NominalTypeDecl>(VD->getDeclContext()->getParent())) {
          if (auto funcDeclContext =
                  dyn_cast<AbstractFunctionDecl>(VD->getDeclContext())) {
            GenericParamList *outerGenericParams = nullptr;
            configureImplicitSelf(*this, funcDeclContext, outerGenericParams);
          }
        } else {
          D->setType(ErrorType::get(Context));
        }      
      } else {
        // FIXME: This case is hit when code completion occurs in a function
        // parameter list. Previous parameters are definitely in scope, but
        // we don't really know how to type-check them.
        assert(isa<AbstractFunctionDecl>(D->getDeclContext()) ||
               isa<TopLevelCodeDecl>(D->getDeclContext()));
        D->setType(ErrorType::get(Context));
      }

      // Make sure the getter and setter have valid types, since they will be
      // used by SILGen for any accesses to this variable.
      if (auto getter = VD->getGetter())
        validateDecl(getter);
      if (auto setter = VD->getSetter())
        validateDecl(setter);
    }

    // Synthesize accessors as necessary.
    maybeAddAccessorsToVariable(VD, *this);

    if (!VD->didEarlyAttrValidation()) {
      checkDeclAttributesEarly(VD);
      validateAttributes(*this, VD);

      // FIXME: Guarding the rest of these things together with early attribute
      // validation is a hack. It's necessary because properties can get types
      // before validateDecl is called.

      if (!DeclChecker::checkOverrides(*this, VD)) {
        // If a property has an override attribute but does not override
        // anything, complain.
        auto overridden = VD->getOverriddenDecl();
        if (auto *OA = VD->getAttrs().getAttribute<OverrideAttr>()) {
          if (!overridden) {
            diagnose(VD, diag::property_does_not_override)
              .highlight(OA->getLocation());
            OA->setInvalid();
          }
        }
      }

      // Properties need some special validation logic.
      if (Type contextType = VD->getDeclContext()->getDeclaredTypeInContext()) {
        // If this is a property, check if it needs to be exposed to
        // Objective-C.
        auto protocolContext = dyn_cast<ProtocolDecl>(VD->getDeclContext());
        ObjCReason reason = ObjCReason::DontDiagnose;
        if (VD->getAttrs().hasAttribute<ObjCAttr>())
          reason = ObjCReason::ExplicitlyObjC;
        else if (VD->getAttrs().hasAttribute<IBOutletAttr>())
          reason = ObjCReason::ExplicitlyIBOutlet;
        else if (VD->getAttrs().hasAttribute<NSManagedAttr>())
          reason = ObjCReason::ExplicitlyNSManaged;
        else if (VD->getAttrs().hasAttribute<DynamicAttr>())
          reason = ObjCReason::ExplicitlyDynamic;
        else if (protocolContext && protocolContext->isObjC())
          reason = ObjCReason::MemberOfObjCProtocol;

        bool isObjC = (reason != ObjCReason::DontDiagnose) ||
                      isImplicitlyObjC(VD);
        if (isObjC)
          isObjC = isRepresentableInObjC(VD, reason);

        markAsObjC(*this, VD, isObjC);

        inferDynamic(Context, VD);

        // If this variable is a class member, mark it final if the
        // class is final, or if it was declared with 'let'.
        if (auto cls = contextType->getClassOrBoundGenericClass()) {
          if (cls->isFinal() || VD->isLet()) {
            if (!VD->isFinal() && !VD->isDynamic()) {
              makeFinal(Context, VD);
            }
          }
          if (VD->isStatic()) {
            auto staticSpelling = VD->getParentPattern()->getStaticSpelling();
            if (staticSpelling == StaticSpellingKind::KeywordStatic) {
              auto finalAttr = VD->getAttrs().getAttribute<FinalAttr>();
              if (finalAttr) {
                auto finalRange = finalAttr->getRange();
                if (finalRange.isValid())
                  diagnose(finalRange.Start, diag::decl_already_final)
                  .highlight(finalRange)
                  .fixItRemove(finalRange);
              }
              makeFinal(Context, VD);
            }
          }
        }
      }


      // If this variable is marked final and has a getter or setter, mark the
      // getter and setter as final as well.
      if (VD->isFinal()) {
        makeFinal(Context, VD->getGetter());
        makeFinal(Context, VD->getSetter());
        makeFinal(Context, VD->getMaterializeForSetFunc());
      } else if (VD->isDynamic()) {
        makeDynamic(Context, VD->getGetter());
        makeDynamic(Context, VD->getSetter());
        // Skip materializeForSet -- it won't be used with a dynamic property.
      }

      if (VD->hasAccessorFunctions()) {
        maybeAddMaterializeForSet(VD, *this);
      }
    }

    break;
  }
      
  case DeclKind::Func: {
    if (D->hasType())
      return;
    typeCheckDecl(D, true);
    break;
  }

  case DeclKind::Subscript:
  case DeclKind::Constructor:
    if (D->hasType())
      return;
    typeCheckDecl(D, true);
    break;

  case DeclKind::Destructor:
  case DeclKind::EnumElement: {
    if (D->hasType())
      return;
    auto container = cast<NominalTypeDecl>(D->getDeclContext());
    validateDecl(container);
    typeCheckDecl(D, true);
    break;
  }
  }

  assert(D->hasType());
}

void TypeChecker::validateAccessibility(ValueDecl *D) {
  if (D->hasAccessibility())
    return;

  // FIXME: Encapsulate the following in computeAccessibility() ?

  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::IfConfig:
    llvm_unreachable("not a value decl");

  case DeclKind::TypeAlias:
    computeAccessibility(D);
    break;

  case DeclKind::GenericTypeParam:
    // Ultimately handled in validateDecl() with resolveTypeParams=true.
    return;

  case DeclKind::AssociatedType: {
      auto assocType = cast<AssociatedTypeDecl>(D);
      auto prot = assocType->getProtocol();
      validateAccessibility(prot);
      assocType->setAccessibility(prot->getAccessibility());
      break;
    }

  case DeclKind::Enum:
  case DeclKind::Struct:
  case DeclKind::Class:
  case DeclKind::Protocol:
  case DeclKind::Var:
  case DeclKind::Param:
  case DeclKind::Func:
  case DeclKind::Subscript:
  case DeclKind::Constructor:
    computeAccessibility(D);
    break;

  case DeclKind::Destructor:
  case DeclKind::EnumElement: {
    if (D->isInvalid()) {
      D->setAccessibility(Accessibility::Private);
    } else {
      auto container = cast<NominalTypeDecl>(D->getDeclContext());
      validateAccessibility(container);
      D->setAccessibility(container->getAccessibility());
    }
    break;
  }
  }

  assert(D->hasAccessibility());
}

static Type checkExtensionGenericParams(
              TypeChecker &tc, ExtensionDecl *ext,
              ArrayRef<ExtensionDecl::RefComponent> refComponents,
              Type type, GenericSignature *&sig) {
  // Find the nominal type declaration and its parent type.
  // FIXME: This scheme doesn't work well with type aliases.
  Type parentType;
  NominalTypeDecl *nominal;
  if (auto unbound = type->getAs<UnboundGenericType>()) {
    parentType = unbound->getParent();
    nominal = unbound->getDecl();
  } else if (auto bound = type->getAs<BoundGenericType>()) {
    parentType = bound->getParent();
    nominal = bound->getDecl();
  } else {
    auto nominalType = type->castTo<NominalType>();
    parentType = nominalType->getParent();
    nominal = nominalType->getDecl();
  }

  // Recurse to check the parent type, if there is one.
  if (parentType) {
    parentType = checkExtensionGenericParams(tc, ext, refComponents.drop_back(),
                                             parentType, sig);
    if (!parentType)
      return Type();
  }

  // If we don't need generic parameters, just rebuild the result type with the
  // new parent.
  if (!nominal->getGenericParams()) {
    assert(!refComponents.back().GenericParams);
    return NominalType::get(nominal, parentType, tc.Context);
  }

  // We have generic parameters that need to be checked.
  auto genericParams = refComponents.back().GenericParams;

  // Local function used to infer requirements from the extended type.
  TypeLoc extendedTypeInfer;
  auto inferExtendedTypeReqs = [&](ArchetypeBuilder &builder) -> bool {
    if (extendedTypeInfer.isNull()) {
      SmallVector<Type, 2> genericArgs;
      for (auto gp : *genericParams) {
        genericArgs.push_back(gp->getDeclaredInterfaceType());
      }
      
      extendedTypeInfer.setType(BoundGenericType::get(nominal, 
                                                      parentType,
                                                      genericArgs));
    }
    
    return builder.inferRequirements(extendedTypeInfer);
  };

  // Validate the generic type signature.
  bool invalid = false;
  sig = tc.validateGenericSignature(genericParams, ext->getDeclContext(), 
                                    nullptr, inferExtendedTypeReqs, invalid);
  if (invalid) {
    return nullptr;
  }

  // If the generic extension signature is not equivalent to that of the
  // nominal type, there are extraneous requirements.
  // Note that we cannot have missing requirements due to requirement
  // inference.
  // FIXME: Figure out an extraneous requirement to point to.
  if (sig->getCanonicalSignature() !=
        nominal->getGenericSignature()->getCanonicalSignature()) {
    tc.diagnose(ext->getLoc(), diag::extension_generic_extra_requirements,
                nominal->getDeclaredType())
      .highlight(genericParams->getSourceRange());
    return nullptr;
  }

  // Validate the generic parameters for the last time.
  tc.revertGenericParamList(genericParams);
  ArchetypeBuilder builder = tc.createArchetypeBuilder(ext->getModuleContext());
  checkGenericParamList(builder, genericParams, tc, ext->getModuleContext());
  inferExtendedTypeReqs(builder);
  finalizeGenericParamList(builder, genericParams, ext, tc);

  // Compute the final extended type.
  SmallVector<Type, 2> genericArgs;
  for (auto gp : *genericParams) {
    genericArgs.push_back(gp->getArchetype());
  }
  return BoundGenericType::get(nominal, parentType, genericArgs);
}
  

void TypeChecker::validateExtension(ExtensionDecl *ext) {
  // If we already validated this extension, there's nothing more to do.
  if (ext->validated())
    return;

  ext->setValidated();

  // If the extension is already known to be invalid, we're done.
  if (ext->isInvalid())
    return;

  // If the type being extended is an unbound generic type, complain and
  // conjure up generic parameters for it.

  // FIXME: We need to check whether anything is specialized, because
  // the innermost extended type might itself be a non-generic type
  // within a generic type.
  auto extendedType = ext->getExtendedType();
  if (auto unbound = extendedType->getAs<UnboundGenericType>()) {
    // Validate the nominal type declaration being extended.
    auto nominal = unbound->getDecl();
    validateDecl(nominal);

    // If the user omitted generic parameters, deal with them now.
    // FIXME: This is just to keep the existing code path working in the short
    // term. It should become an error with Fix-It that suggests the appropriate
    // generic parameters.
    auto genericParams = ext->getRefComponents().back().GenericParams;
    if (!genericParams) {
      // FIXME: Create new generic parameters with the same signature.
      genericParams = nominal->getGenericParams();
      ext->getRefComponents().back().GenericParams = genericParams;
      ext->setGenericSignature(nominal->getGenericSignature());

      // FIXME: We want to use the new generic parameters, not the old ones,
      // for this reference.
      ext->setExtendedType(nominal->getDeclaredTypeInContext());
      return;
    }

    // Check generic parameters.
    GenericSignature *sig = nullptr;
    extendedType = checkExtensionGenericParams(*this, ext, 
                                               ext->getRefComponents(),
                                               extendedType, sig);
    if (!extendedType) {
      ext->setInvalid();
      ext->setExtendedType(ErrorType::get(Context));
      return;
    }

    ext->setGenericSignature(sig);
    ext->setExtendedType(extendedType);

    // ... now complain about this, because it probably doesn't work yet.
    diagnose(ext, diag::extension_generic_args)
      .highlight(genericParams->getSourceRange());
    return;
  }
}

ArrayRef<ProtocolDecl *>
TypeChecker::getDirectConformsTo(NominalTypeDecl *nominal) {
  checkInheritanceClause(nominal);
  return nominal->getProtocols();
}

ArrayRef<ProtocolDecl *>
TypeChecker::getDirectConformsTo(ExtensionDecl *ext) {
  validateExtension(ext);
  checkInheritanceClause(ext);
  return ext->getProtocols();
}

/// Build a default initializer string for the given pattern.
///
/// This string is suitable for display in diagnostics.
static Optional<std::string> buildDefaultInitializerString(TypeChecker &tc,
                                                           DeclContext *dc,
                                                           Pattern *pattern) {
  switch (pattern->getKind()) {
#define REFUTABLE_PATTERN(Id, Parent) case PatternKind::Id:
#define PATTERN(Id, Parent)
#include "swift/AST/PatternNodes.def"
    return None;
  case PatternKind::Any:
    return None;

  case PatternKind::Named: {
    if (!pattern->hasType())
      return None;

    // Special-case the various types we might see here.
    auto type = pattern->getType();

    // For literal-convertible types, form the corresponding literal.
#define CHECK_LITERAL_PROTOCOL(Kind, String) \
    if (auto proto = tc.getProtocol(SourceLoc(), KnownProtocolKind::Kind)) { \
      if (tc.conformsToProtocol(type, proto, dc, true)) \
        return std::string(String); \
    }
    CHECK_LITERAL_PROTOCOL(ArrayLiteralConvertible, "[]")
    CHECK_LITERAL_PROTOCOL(DictionaryLiteralConvertible, "[]")
    CHECK_LITERAL_PROTOCOL(UnicodeScalarLiteralConvertible, "\"\"")
    CHECK_LITERAL_PROTOCOL(ExtendedGraphemeClusterLiteralConvertible, "\"\"")
    CHECK_LITERAL_PROTOCOL(FloatLiteralConvertible, "0.0")
    CHECK_LITERAL_PROTOCOL(IntegerLiteralConvertible, "0")
    CHECK_LITERAL_PROTOCOL(StringLiteralConvertible, "\"\"")
#undef CHECK_LITERAL_PROTOCOL

    // For optional types, use 'nil'.
    if (type->getAnyOptionalObjectType())
      return std::string("nil");

    return None;
  }

  case PatternKind::Paren: {
    if (auto sub = buildDefaultInitializerString(
                     tc, dc, cast<ParenPattern>(pattern)->getSubPattern())) {
      return "(" + *sub + ")";
    }

    return None;
  }

  case PatternKind::Tuple: {
    std::string result = "(";
    bool first = true;
    for (auto elt : cast<TuplePattern>(pattern)->getFields()) {
      if (auto sub = buildDefaultInitializerString(tc, dc, elt.getPattern())) {
        if (first) {
          first = false;
        } else {
          result += ", ";
        }

        result += *sub;
      } else {
        return None;
      }
    }
    result += ")";
    return result;
  }

  case PatternKind::Typed:
    return buildDefaultInitializerString(
             tc, dc, cast<TypedPattern>(pattern)->getSubPattern());

  case PatternKind::Var:
    return buildDefaultInitializerString(
             tc, dc, cast<VarPattern>(pattern)->getSubPattern());
  }
}

/// Diagnose a class that does not have any initializers.
static void diagnoseClassWithoutInitializers(TypeChecker &tc,
                                             ClassDecl *classDecl) {
  tc.diagnose(classDecl, diag::class_without_init,
              classDecl->getDeclaredType());

  SourceLoc lastLoc;
  for (auto member : classDecl->getMembers()) {
    auto pbd = dyn_cast<PatternBindingDecl>(member);
    if (!pbd)
      continue;

    if (pbd->isStatic() || !pbd->hasStorage() || isDefaultInitializable(pbd) ||
        pbd->isInvalid())
      continue;

    // FIXME: When we parse "var a, b: Int" we create multiple
    // PatternBindingDecls, which is convenience elsewhere but
    // unfortunate here, where it causes us to emit multiple
    // initializers.
    if (pbd->getLoc() == lastLoc)
      continue;

    lastLoc = pbd->getLoc();
    SmallVector<VarDecl *, 4> vars;
    pbd->getPattern()->collectVariables(vars);
    Optional<InFlightDiagnostic> diag;
    switch (vars.size()) {
    case 0:
      break;

    case 1: {
      diag.emplace(tc.diagnose(vars[0]->getLoc(), diag::note_no_in_class_init_1,
                               vars[0]->getName()));
      break;
    }

    case 2:
      diag.emplace(tc.diagnose(pbd->getLoc(), diag::note_no_in_class_init_2,
                               vars[0]->getName(), vars[1]->getName()));
      break;

    case 3:
      diag.emplace(tc.diagnose(pbd->getLoc(), diag::note_no_in_class_init_3plus,
                               vars[0]->getName(), vars[1]->getName(), 
                               vars[2]->getName(), false));
      break;

    default:
      diag.emplace(tc.diagnose(pbd->getLoc(), diag::note_no_in_class_init_3plus,
                               vars[0]->getName(), vars[1]->getName(), 
                               vars[2]->getName(), true));
      break;
    }

    if (diag) {
      if (auto defaultValueSuggestion
                 = buildDefaultInitializerString(tc, classDecl, 
                                                 pbd->getPattern())) {
        SourceLoc afterLoc = Lexer::getLocForEndOfToken(tc.Context.SourceMgr,
                                                        pbd->getEndLoc());
        diag->fixItInsert(afterLoc, " = " + *defaultValueSuggestion);
      }
    }
  }
}

namespace {
  /// AST stream printer that adds extra indentation to each line.
  class ExtraIndentStreamPrinter : public StreamPrinter {
    StringRef ExtraIndent;

  public:
    ExtraIndentStreamPrinter(raw_ostream &out, StringRef extraIndent)
    : StreamPrinter(out), ExtraIndent(extraIndent) { }

    virtual void printIndent() {
      printText(ExtraIndent);
      StreamPrinter::printIndent();
    }
  };
}

/// Diagnose a missing required initializer.
static void diagnoseMissingRequiredInitializer(
              TypeChecker &TC,
              ClassDecl *classDecl,
              ConstructorDecl *superInitializer) {
  // Find the location at which we should insert the new initializer.
  SourceLoc insertionLoc;
  SourceLoc indentationLoc;
  for (auto member : classDecl->getMembers()) {
    // If we don't have an indentation location yet, grab one from this
    // member.
    if (indentationLoc.isInvalid()) {
      indentationLoc = member->getLoc();
    }

    // We only want to look at explicit constructors.
    auto ctor = dyn_cast<ConstructorDecl>(member);
    if (!ctor)
      continue;

    if (ctor->isImplicit())
      continue;

    insertionLoc = ctor->getEndLoc();
    indentationLoc = ctor->getLoc();
  }

  // If no initializers were listed, start at the opening '{' for the class.
  if (insertionLoc.isInvalid()) {
    insertionLoc = classDecl->getBraces().Start;
  }
  if (indentationLoc.isInvalid()) {
    indentationLoc = classDecl->getBraces().End;
  }

  // Adjust the insertion location to point at the end of this line (i.e.,
  // the start of the next line).
  insertionLoc = Lexer::getLocForEndOfLine(TC.Context.SourceMgr,
                                           insertionLoc);

  // Find the indentation used on the indentation line.
  StringRef indentation = Lexer::getIndentationForLine(TC.Context.SourceMgr,
                                                       indentationLoc);

  // Pretty-print the superclass initializer into a string.
  // FIXME: Form a new initializer by performing the appropriate
  // substitutions of subclass types into the superclass types, so that
  // we get the right generic parameters.
  std::string initializerText;
  {
    PrintOptions options;
    options.PrintDefaultParameterPlaceholder = false;
    options.PrintImplicitAttrs = false;

    // Render the text.
    llvm::raw_string_ostream out(initializerText);
    {
      ExtraIndentStreamPrinter printer(out, indentation);
      printer.printNewline();

      // If there is no explicit 'required', print one.
      bool hasExplicitRequiredAttr = false;
      if (auto requiredAttr
            = superInitializer->getAttrs().getAttribute<RequiredAttr>())
          hasExplicitRequiredAttr = !requiredAttr->isImplicit();

      if (!hasExplicitRequiredAttr)
        printer << "required ";

      superInitializer->print(printer, options);
    }

    // FIXME: Infer body indentation from the source rather than hard-coding
    // 4 spaces.

    // Add a dummy body.
    out << " {\n";
    out << indentation << "    fatalError(\"";
    superInitializer->getFullName().printPretty(out);
    out << " has not been implemented\")\n";
    out << indentation << "}\n";
  }

  // Complain.
  TC.diagnose(insertionLoc, diag::required_initializer_missing,
              superInitializer->getFullName(),
              superInitializer->getDeclContext()->getDeclaredTypeOfContext())
    .fixItInsert(insertionLoc, initializerText);
  TC.diagnose(superInitializer, diag::required_initializer_here);
}

void TypeChecker::addImplicitConstructors(NominalTypeDecl *decl,
                                          SmallVectorImpl<Decl*> &Results) {
  // We can only synthesize implicit constructors for classes and structs.
 if (!isa<ClassDecl>(decl) && !isa<StructDecl>(decl))
   return;

  // If we already added implicit initializers, we're done.
  if (decl->addedImplicitInitializers())
    return;
  
  // Don't add implicit constructors for an invalid declaration
  if (decl->isInvalid())
    return;

  // Local function that produces the canonical parameter type of the given
  // initializer.
  // FIXME: Doesn't work properly for generics.
  auto getInitializerParamType = [](ConstructorDecl *ctor) -> CanType {
    auto interfaceTy = ctor->getInterfaceType();

    // Skip the 'self' parameter.
    auto uncurriedInitTy = interfaceTy->castTo<AnyFunctionType>()->getResult();

    // Grab the parameter type;
    auto paramTy = uncurriedInitTy->castTo<AnyFunctionType>()->getInput();

    return paramTy->getCanonicalType();
  };

  // Check whether there is a user-declared constructor or an instance
  // variable.
  bool FoundMemberwiseInitializedProperty = false;
  bool FoundUninitializedVars = false;
  bool FoundDesignatedInit = false;
  decl->setAddedImplicitInitializers();
  SmallPtrSet<CanType, 4> initializerParamTypes;
  llvm::SmallPtrSet<ConstructorDecl *, 4> overriddenInits;
  for (auto member : decl->getMembers()) {
    if (auto ctor = dyn_cast<ConstructorDecl>(member)) {
      validateDecl(ctor);

      if (ctor->isDesignatedInit())
        FoundDesignatedInit = true;

      if (!ctor->isInvalid())
        initializerParamTypes.insert(getInitializerParamType(ctor));

      if (auto overridden = ctor->getOverriddenDecl())
        overriddenInits.insert(overridden);

      continue;
    }

    if (auto var = dyn_cast<VarDecl>(member)) {
      if (var->hasStorage() && !var->isStatic() && !var->isInvalid()) {
        // Initialized 'let' properties have storage, but don't get an argument
        // to the memberwise initializer since they already have an initial
        // value that cannot be overridden.
        if (var->isLet() && var->getParentPattern() &&
            var->getParentPattern()->hasInit()) {
          
          // We cannot handle properties like:
          //   let (a,b) = (1,2)
          // for now, just disable implicit init synthesization in structs in
          // this case.
          auto SP = var->getParentPattern()->getPattern();
          if (auto *TP = dyn_cast<TypedPattern>(SP))
            SP = TP->getSubPattern();
          if (!isa<NamedPattern>(SP) && isa<StructDecl>(decl))
            return;
          
          continue;
        }
        
        FoundMemberwiseInitializedProperty = true;
      }
      continue;
    }

    if (auto pbd = dyn_cast<PatternBindingDecl>(member)) {
      if (pbd->hasStorage() && !pbd->isStatic() && !pbd->isImplicit() &&
          !isDefaultInitializable(pbd)) {
        FoundUninitializedVars = true;
      }
      continue;
    }
  }

  if (auto structDecl = dyn_cast<StructDecl>(decl)) {
    if (!FoundDesignatedInit && !structDecl->hasUnreferenceableStorage()) {
      // For a struct with memberwise initialized properties, we add a
      // memberwise init.
      if (FoundMemberwiseInitializedProperty) {
        // Create the implicit memberwise constructor.
        auto ctor = createImplicitConstructor(
                      *this, decl, ImplicitConstructorKind::Memberwise);
        decl->addMember(ctor);
        Results.push_back(ctor);
      }

      // If we found a stored property, add a default constructor.
      if (!FoundUninitializedVars)
        Results.push_back(defineDefaultConstructor(decl));
    }
    return;
  }
 
  // For a class with a superclass, automatically define overrides
  // for all of the superclass's designated initializers.
  // FIXME: Currently skipping generic classes.
  auto classDecl = cast<ClassDecl>(decl);
  assert(!classDecl->hasSuperclass() ||
         classDecl->getSuperclass()->getAnyNominal()
           ->addedImplicitInitializers());
  if (classDecl->hasSuperclass() && !classDecl->isGenericContext() &&
      !classDecl->getSuperclass()->isSpecialized()) {
    bool canInheritInitializers = !FoundDesignatedInit;

    // We can't define these overrides if we have any uninitialized
    // stored properties.
    if (FoundUninitializedVars && !FoundDesignatedInit) {
      diagnoseClassWithoutInitializers(*this, classDecl);
      return;
    }

    auto superclassTy = classDecl->getSuperclass();
    for (auto member : lookupConstructors(superclassTy, classDecl)) {
      // Skip unavailable superclass initializers.
      if (AvailabilityAttr::isUnavailable(member))
        continue;

      // Skip invalid superclass initializers.
      auto superclassCtor = dyn_cast<ConstructorDecl>(member);
      if (superclassCtor->isInvalid())
        continue;

      // We only care about required or designated initializers.
      if (!superclassCtor->isRequired() &&
          !superclassCtor->isDesignatedInit())
        continue;

      // If we have an override for this constructor, it's okay.
      if (overriddenInits.count(superclassCtor) > 0)
        continue;

      // If the superclass constructor is a convenience initializer
      // that is inherited into the current class, it's okay.
      if (superclassCtor->isInheritable() &&
          classDecl->inheritsSuperclassInitializers(this)) {
        assert(superclassCtor->isRequired());
        continue;
      }

      // Diagnose a missing override of a required initializer.
      if (superclassCtor->isRequired() && FoundDesignatedInit) {
        diagnoseMissingRequiredInitializer(*this, classDecl, superclassCtor);
        continue;
      }

      // A designated or required initializer has not been overridden.

      // Skip this designated initializer if it's in an extension.
      // FIXME: We shouldn't allow this.
      if (isa<ExtensionDecl>(superclassCtor->getDeclContext()))
        continue;

      // If we have already introduced an initializer with this parameter type,
      // don't add one now.
      if (!initializerParamTypes.insert(
             getInitializerParamType(superclassCtor)).second)
        continue;

      // We have a designated initializer. Create an override of it.
      if (auto ctor = createDesignatedInitOverride(
                        *this, classDecl, superclassCtor,
                        canInheritInitializers
                          ? DesignatedInitKind::Chaining
                          : DesignatedInitKind::Stub)) {
        classDecl->addMember(ctor);
        Results.push_back(classDecl);
      }
    }

    return;
  }

  if (!FoundDesignatedInit) {
    // For a class with no superclass, automatically define a default
    // constructor.

    // ... unless there are uninitialized stored properties.
    if (FoundUninitializedVars) {
      diagnoseClassWithoutInitializers(*this, classDecl);
      return;
    }

    Results.push_back(defineDefaultConstructor(decl));
  }
}

void TypeChecker::addImplicitStructConformances(StructDecl *SD) {
  // Type-check the protocol conformances of the struct decl to instantiate its
  // derived conformances.
  DeclChecker(*this, false, false)
    .checkExplicitConformance(SD, SD->getDeclaredTypeInContext());
}

void TypeChecker::addImplicitEnumConformances(EnumDecl *ED) {
  // Type-check the raw values of the enum.
  for (auto elt : ED->getAllElements()) {
    assert(elt->hasRawValueExpr());
    if (elt->getTypeCheckedRawValueExpr()) continue;
    Expr *typeChecked = elt->getRawValueExpr();
    Type rawTy = ArchetypeBuilder::mapTypeIntoContext(ED, ED->getRawType());
    bool error = typeCheckExpression(typeChecked, ED, rawTy, Type(), false);
    assert(!error); (void)error;
    elt->setTypeCheckedRawValueExpr(typeChecked);
  }
  
  // Type-check the protocol conformances of the enum decl to instantiate its
  // derived conformances.
  DeclChecker(*this, false, false)
    .checkExplicitConformance(ED, ED->getDeclaredTypeInContext());
}

ConstructorDecl *TypeChecker::defineDefaultConstructor(NominalTypeDecl *decl) {
  PrettyStackTraceDecl stackTrace("defining default constructor for",
                                  decl);

  // Clang-imported types should never get a default constructor, just a
  // memberwise one.
  if (decl->hasClangNode())
    return nullptr;

  // For a class, check whether the superclass (if it exists) is
  // default-initializable.
  if (isa<ClassDecl>(decl)) {
    // We need to look for a default constructor.
    if (auto superTy = getSuperClassOf(decl->getDeclaredTypeInContext())) {
      // If there are no default ctors for our supertype, we can't do anything.
      auto ctors = lookupConstructors(superTy, decl);
      if (!ctors)
        return nullptr;

      // Check whether we have a constructor that can be called with an empty
      // tuple.
      bool foundDefaultConstructor = false;
      for (auto member : ctors) {
        // Dig out the parameter tuple for this constructor.
        auto ctor = dyn_cast<ConstructorDecl>(member);
        if (!ctor || ctor->isInvalid())
          continue;

        auto paramTuple = ctor->getArgumentType()->getAs<TupleType>();
        if (!paramTuple) {
          // A designated initializer other than a default initializer
          // means we can't call super.init().
          if (ctor->isDesignatedInit())
            return nullptr;

          continue;
        }

        // Check whether any of the tuple elements are missing an initializer.
        bool missingInit = false;
        for (auto &elt : paramTuple->getFields()) {
          if (elt.hasInit())
            continue;

          missingInit = true;
          break;
        }
        if (missingInit) {
          // A designated initializer other than a default initializer
          // means we can't call super.init().
          if (ctor->isDesignatedInit())
            return nullptr;

          continue;
        }

        // We found a constructor that can be invoked with an empty tuple.
        if (foundDefaultConstructor) {
          // We found two constructors that can be invoked with an empty tuple.
          foundDefaultConstructor = false;
          break;
        }

        foundDefaultConstructor = true;
      }

      // If our superclass isn't default constructible, we aren't either.
      if (!foundDefaultConstructor) return nullptr;
    }
  }

  // Create the default constructor.
  auto ctor = createImplicitConstructor(
                *this, decl, ImplicitConstructorKind::Default);

  // Add the constructor.
  decl->addMember(ctor);

  // Create an empty body for the default constructor. The type-check of the
  // constructor body will introduce default initializations of the members.
  ctor->setBody(BraceStmt::create(Context, SourceLoc(), { }, SourceLoc()));
  return ctor;
}

static void validateAttributes(TypeChecker &TC, Decl *D) {
  const DeclAttributes &Attrs = D->getAttrs();

  auto isInClassOrProtocolContext = [](Decl *vd) {
   Type ContextTy = vd->getDeclContext()->getDeclaredTypeInContext();
    if (!ContextTy)
      return false;
    return bool(ContextTy->getClassOrBoundGenericClass()) ||
           ContextTy->is<ProtocolType>();
  };

  if (auto objcAttr = Attrs.getAttribute<ObjCAttr>()) {
    // Only classes, class protocols, instance properties, methods,
    // constructors, and subscripts can be ObjC.
    Optional<Diag<>> error;
    if (isa<ClassDecl>(D)) {
      /* ok */
    } else if (isa<FuncDecl>(D) && isInClassOrProtocolContext(D)) {
      auto func = cast<FuncDecl>(D);
      if (func->isOperator())
        error = diag::invalid_objc_decl;
      else if (func->isAccessor() && !func->isGetterOrSetter()) {
        error= diag::objc_observing_accessor;
      }
    } else if (isa<ConstructorDecl>(D) && isInClassOrProtocolContext(D)) {
      /* ok */
    } else if (isa<DestructorDecl>(D)) {
      /* ok */
    } else if (isa<SubscriptDecl>(D) && isInClassOrProtocolContext(D)) {
      /* ok */
    } else if (auto *VD = dyn_cast<VarDecl>(D)) {
      if (!isInClassOrProtocolContext(VD))
        error = diag::invalid_objc_decl;
    } else if (isa<ProtocolDecl>(D)) {
      /* ok */
    } else if (auto ED = dyn_cast<EnumDecl>(D)) {
      if (ED->isGenericContext())
        error = diag::objc_enum_generic;
    } else {
      error = diag::invalid_objc_decl;
    }

    if (error) {
      TC.diagnose(D->getStartLoc(), *error);
      const_cast<ObjCAttr *>(objcAttr)->setInvalid();
      return;
    }

    // If there is a name, check whether the kind of name is
    // appropriate.
    if (auto objcName = objcAttr->getName()) {
      if (isa<ClassDecl>(D) || isa<ProtocolDecl>(D) || isa<VarDecl>(D)) {
        // Types and properties can only have nullary
        // names. Complain and recover by chopping off everything
        // after the first name.
        if (objcName->getNumArgs() > 0) {
          int which = isa<ClassDecl>(D)? 0 
                    : isa<ProtocolDecl>(D)? 1
                    : 2;
          SourceLoc firstNameLoc = objcAttr->getNameLocs().front();
          SourceLoc afterFirstNameLoc = 
            Lexer::getLocForEndOfToken(TC.Context.SourceMgr, firstNameLoc);
          TC.diagnose(firstNameLoc, diag::objc_name_req_nullary, which)
            .fixItRemoveChars(afterFirstNameLoc, objcAttr->getRParenLoc());
          const_cast<ObjCAttr *>(objcAttr)->setName(
            ObjCSelector(TC.Context, 0, objcName->getSelectorPieces()[0]),
            /*implicit=*/false);
        }
      } else if (isa<EnumDecl>(D)) {
        // Enums don't have runtime names.
        TC.diagnose(objcAttr->getLParenLoc(), diag::objc_name_enum);
        const_cast<ObjCAttr *>(objcAttr)->clearName();
      } else if (isa<SubscriptDecl>(D)) {
        // Subscripts can never have names.
        TC.diagnose(objcAttr->getLParenLoc(), diag::objc_name_subscript);
        const_cast<ObjCAttr *>(objcAttr)->clearName();
      } else {
        // We have a function. Make sure that the number of parameters
        // matches the "number of colons" in the name.
        auto func = cast<AbstractFunctionDecl>(D);
        auto bodyPattern = func->getBodyParamPatterns()[1];
        unsigned numParameters;
        if (auto tuple = dyn_cast<TuplePattern>(bodyPattern))
          numParameters = tuple->getNumFields() - tuple->hasVararg();
        else
          numParameters = 1;

        unsigned numArgumentNames = objcName->getNumArgs();
        if (numArgumentNames != numParameters) {
          TC.diagnose(objcAttr->getNameLocs().front(), 
                      diag::objc_name_func_mismatch,
                      isa<FuncDecl>(func), 
                      numArgumentNames, 
                      numArgumentNames != 1,
                      numParameters,
                      numParameters != 1);
          D->getAttrs().add(
            ObjCAttr::createUnnamed(TC.Context,
                                    objcAttr->AtLoc,
                                    objcAttr->Range.Start));
          D->getAttrs().removeAttribute(objcAttr);
        }
      }
    }
  }

  // Only protocol members can be optional.
  if (auto *OA = Attrs.getAttribute<OptionalAttr>()) {
    if (!isa<ProtocolDecl>(D->getDeclContext())) {
      TC.diagnose(OA->getLocation(),
                  diag::optional_attribute_non_protocol);
      D->getAttrs().removeAttribute(OA);
    } else if (!cast<ProtocolDecl>(D->getDeclContext())->isObjC()) {
      TC.diagnose(OA->getLocation(),
                  diag::optional_attribute_non_objc_protocol);
      D->getAttrs().removeAttribute(OA);
    } else if (isa<ConstructorDecl>(D)) {
      TC.diagnose(OA->getLocation(),
                  diag::optional_attribute_initializer);
      D->getAttrs().removeAttribute(OA);
    }
  }

  // Only protocols that are @objc can have "unavailable" methods.
  if (auto AvAttr = Attrs.getUnavailable(TC.Context)) {
    if (auto PD = dyn_cast<ProtocolDecl>(D->getDeclContext())) {
      if (!PD->isObjC()) {
        TC.diagnose(AvAttr->getLocation(),
                    diag::unavailable_method_non_objc_protocol);
        D->getAttrs().removeAttribute(AvAttr);
      }
    }
  }
}

bool TypeChecker::typeCheckConditionalPatternBinding(PatternBindingDecl *PBD,
                                                     DeclContext *dc) {
  validatePatternBindingDecl(*this, PBD, TR_ForIn_IfLetVariable);
  if (PBD->isInvalid())
    return true;
  
  assert(PBD->getInit() && "conditional pattern binding should always have init");
  if (!PBD->wasInitChecked()) {
    if (typeCheckBinding(PBD, TR_ForIn_IfLetVariable)) {
      PBD->setInvalid();
      if (!PBD->getPattern()->hasType()) {
        PBD->getPattern()->setType(ErrorType::get(Context));
        setBoundVarsTypeError(PBD->getPattern(), Context);
        return true;
      }
    }
  }
  
  DeclChecker(*this, false, false).visitBoundVars(PBD->getPattern());
  return false;
}

/// Fix the names in the given function to match those in the given target
/// name by adding Fix-Its to the provided in-flight diagnostic.
void TypeChecker::fixAbstractFunctionNames(InFlightDiagnostic &diag,
                                           AbstractFunctionDecl *func,
                                           DeclName targetName) {
  auto name = func->getFullName();
  
  // Fix the name of the function itself.
  if (name.getBaseName() != targetName.getBaseName()) {
    diag.fixItReplace(func->getLoc(), targetName.getBaseName().str());
  }
  
  // Fix the argument names that need fixing.
  assert(name.getArgumentNames().size()
           == targetName.getArgumentNames().size());
  auto pattern
    = func->getBodyParamPatterns()[func->getDeclContext()->isTypeContext()];
  auto tuplePattern = dyn_cast<TuplePattern>(
                        pattern->getSemanticsProvidingPattern());
  for (unsigned i = 0, n = name.getArgumentNames().size(); i != n; ++i) {
    auto origArg = name.getArgumentNames()[i];
    auto targetArg = targetName.getArgumentNames()[i];
    
    if (origArg == targetArg)
      continue;
    
    // Find the location to update or insert.
    SourceLoc loc;
    bool needColon = false;
    if (tuplePattern) {
      auto origPattern = tuplePattern->getFields()[i].getPattern();
      if (auto param = cast_or_null<ParamDecl>(origPattern->getSingleVar())) {
        // The parameter has an explicitly-specified API name, and it's wrong.
        if (param->getArgumentNameLoc() != param->getLoc() &&
            param->getArgumentNameLoc().isValid()) {
          // ... but the internal parameter name was right. Just zap the
          // incorrect explicit specialization.
          if (param->getName() == targetArg) {
            diag.fixItRemoveChars(param->getArgumentNameLoc(),
                                  param->getLoc());
            continue;
          }
          
          // Fix the API name.
          StringRef targetArgStr = targetArg.empty()? "_" : targetArg.str();
          diag.fixItReplace(param->getArgumentNameLoc(), targetArgStr);
          continue;
        }
        
        // The parameter did not specify a separate API name. Insert one.
        if (targetArg.empty())
          diag.fixItInsert(param->getLoc(), "_ ");
        else {
          llvm::SmallString<8> targetArgStr;
          targetArgStr += targetArg.str();
          targetArgStr += ' ';
          diag.fixItInsert(param->getLoc(), targetArgStr);
        }

        if (param->isImplicit()) {
          needColon = true;
          loc = origPattern->getLoc();
        } else {
          continue;
        }
      }
      
      if (auto any = dyn_cast<AnyPattern>(
                       origPattern->getSemanticsProvidingPattern())) {
        if (any->isImplicit()) {
          needColon = true;
          loc = origPattern->getLoc();
        } else {
          needColon = false;
          loc = any->getLoc();
        }
      } else {
        loc = origPattern->getLoc();
        needColon = true;
      }
    } else if (auto paren = dyn_cast<ParenPattern>(pattern)) {
      loc = paren->getSubPattern()->getLoc();
      needColon = true;

      // FIXME: Representation doesn't let us fix this easily.
      if (targetArg.empty())
        continue;

    } else {
      loc = pattern->getLoc();
      needColon = true;
    }
    
    assert(!targetArg.empty() && "Must have a name here");
    llvm::SmallString<8> replacement;
    replacement += targetArg.str();
    if (needColon)
      replacement += ": ";
    
    diag.fixItInsert(loc, replacement);
  }
  
  // FIXME: Update the AST accordingly.
}
