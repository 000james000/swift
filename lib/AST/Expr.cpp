//===--- Expr.cpp - Swift Language Expression ASTs ------------------------===//
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
//  This file implements the Expr class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Expr.h"
#include "swift/Basic/Unicode.h"
#include "swift/AST/Decl.h" // FIXME: Bad dependency
#include "swift/AST/Stmt.h"
#include "swift/AST/AST.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeLoc.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

//===----------------------------------------------------------------------===//
// Expr methods.
//===----------------------------------------------------------------------===//

// Only allow allocation of Stmts using the allocator in ASTContext.
void *Expr::operator new(size_t Bytes, ASTContext &C,
                         unsigned Alignment) {
  return C.Allocate(Bytes, Alignment);
}

StringRef Expr::getKindName(ExprKind K) {
  switch (K) {
#define EXPR(Id, Parent) case ExprKind::Id: return #Id;
#include "swift/AST/ExprNodes.def"
  }
}

// Helper functions to verify statically whether the getSourceRange()
// function has been overridden.
typedef const char (&TwoChars)[2];

template<typename Class> 
inline char checkSourceRangeType(SourceRange (Class::*)() const);

inline TwoChars checkSourceRangeType(SourceRange (Expr::*)() const);

SourceRange Expr::getSourceRange() const {
  switch (getKind()) {
#define EXPR(ID, PARENT) \
case ExprKind::ID: \
static_assert(sizeof(checkSourceRangeType(&ID##Expr::getSourceRange)) == 1, \
              #ID "Expr is missing getSourceRange()"); \
return cast<ID##Expr>(this)->getSourceRange();
#include "swift/AST/ExprNodes.def"
  }
  
  llvm_unreachable("expression type not handled!");
}

/// getLoc - Return the caret location of the expression.
SourceLoc Expr::getLoc() const {
  switch (getKind()) {
#define EXPR(ID, PARENT) \
  case ExprKind::ID: \
    if (&Expr::getLoc != &ID##Expr::getLoc) \
      return cast<ID##Expr>(this)->getLoc(); \
    break;
#include "swift/AST/ExprNodes.def"
  }

  return getStartLoc();
}

Expr *Expr::getSemanticsProvidingExpr() {
  if (IdentityExpr *PE = dyn_cast<IdentityExpr>(this))
    return PE->getSubExpr()->getSemanticsProvidingExpr();

  if (DefaultValueExpr *DE = dyn_cast<DefaultValueExpr>(this))
    return DE->getSubExpr()->getSemanticsProvidingExpr();
  
  return this;
}

Expr *Expr::getValueProvidingExpr() {
  // For now, this is totally equivalent to the above.
  // TODO:
  //   - tuple literal projection, which may become interestingly idiomatic
  return getSemanticsProvidingExpr();
}

Initializer *Expr::findExistingInitializerContext() {
  struct FindExistingInitializer : ASTWalker {
    Initializer *TheInitializer = nullptr;
    std::pair<bool,Expr*> walkToExprPre(Expr *E) override {
      assert(!TheInitializer && "continuing to walk after finding context?");
      if (auto closure = dyn_cast<AbstractClosureExpr>(E)) {
        TheInitializer = cast<Initializer>(closure->getParent());
        return { false, nullptr };
      }
      return { true, E };
    }
  } finder;
  walk(finder);
  return finder.TheInitializer;
}

bool Expr::isStaticallyDerivedMetatype() const {
  // IF the result isn't a metatype, there's nothing else to do.
  if (!getType()->is<AnyMetatypeType>())
    return false;

  const Expr *expr = this;
  do {
    // Skip syntax.
    expr = expr->getSemanticsProvidingExpr();

    // Direct reference to a type.
    if (auto declRef = dyn_cast<DeclRefExpr>(expr))
      return isa<TypeDecl>(declRef->getDecl());
    if (isa<TypeExpr>(expr))
      return true;

    // A "." expression that refers to a member.
    if (auto memberRef = dyn_cast<MemberRefExpr>(expr))
      return isa<TypeDecl>(memberRef->getMember().getDecl());

    // When the base of a "." expression is ignored, look at the member.
    if (auto ignoredDot = dyn_cast<DotSyntaxBaseIgnoredExpr>(expr)) {
      expr = ignoredDot->getRHS();
      continue;
    }

    // A synthesized metatype.
    if (auto metatype = dyn_cast<DynamicTypeExpr>(expr)) {
      // Recurse into the base.
      expr = metatype->getBase();
      continue;
    }

    // Anything else is not statically derived.
    return false;
  } while (true);
}

bool Expr::isSuperExpr() const {
  const Expr *expr = this;
  do {
    expr = expr->getSemanticsProvidingExpr();

    if (isa<SuperRefExpr>(expr))
      return true;

    if (auto derivedToBase = dyn_cast<DerivedToBaseExpr>(expr)) {
      expr = derivedToBase->getSubExpr();
      continue;
    }

    return false;
  } while (true);
}

//===----------------------------------------------------------------------===//
// Support methods for Exprs.
//===----------------------------------------------------------------------===//

static APInt getIntegerLiteralValue(bool IsNegative, StringRef Text,
                                    unsigned BitWidth) {
  llvm::APInt Value(BitWidth, 0);
  // swift encodes octal differently from C
  bool IsCOctal = Text.size() > 1 && Text[0] == '0' && isdigit(Text[1]);
  bool Error = Text.getAsInteger(IsCOctal ? 10 : 0, Value);
  assert(!Error && "Invalid IntegerLiteral formed"); (void)Error;
  if (IsNegative)
    Value = -Value;
  if (Value.getBitWidth() != BitWidth)
    Value = Value.sextOrTrunc(BitWidth);
  return Value;
}

APInt IntegerLiteralExpr::getValue(StringRef Text, unsigned BitWidth) {
  return getIntegerLiteralValue(/*IsNegative=*/false, Text, BitWidth);
}

APInt IntegerLiteralExpr::getValue() const {
  assert(!getType().isNull() && "Semantic analysis has not completed");
  assert(!getType()->is<ErrorType>() && "Should have a valid type");
  return getIntegerLiteralValue(
      isNegative(), getDigitsText(),
      getType()->castTo<BuiltinIntegerType>()->getGreatestWidth());
}

APFloat FloatLiteralExpr::getValue(StringRef Text,
                                   const llvm::fltSemantics &Semantics) {
  APFloat Val(Semantics);
  APFloat::opStatus Res =
    Val.convertFromString(Text, llvm::APFloat::rmNearestTiesToEven);
  assert(Res != APFloat::opInvalidOp && "Sema didn't reject invalid number");
  (void)Res;
  return Val;
}

llvm::APFloat FloatLiteralExpr::getValue() const {
  assert(!getType().isNull() && "Semantic analysis has not completed");
  
  return getValue(getText(),
                  getType()->castTo<BuiltinFloatType>()->getAPFloatSemantics());
}

StringLiteralExpr::StringLiteralExpr(StringRef Val, SourceRange Range)
    : LiteralExpr(ExprKind::StringLiteral, /*Implicit=*/false), Val(Val),
      Range(Range) {
  StringLiteralExprBits.Encoding = static_cast<unsigned>(UTF8);
  StringLiteralExprBits.IsSingleExtendedGraphemeCluster =
      unicode::isSingleExtendedGraphemeCluster(Val);
}

void DeclRefExpr::setDeclRef(ConcreteDeclRef ref) {
  if (auto spec = getSpecInfo())
    spec->D = ref;
  else
    DOrSpecialized = ref;
}

void DeclRefExpr::setSpecialized() {
  if (isSpecialized())
    return;

  ConcreteDeclRef ref = getDeclRef();
  void *Mem = ref.getDecl()->getASTContext().Allocate(sizeof(SpecializeInfo),
                                                      alignof(SpecializeInfo));
  auto Spec = new (Mem) SpecializeInfo;
  Spec->D = ref;
  DOrSpecialized = Spec;
}

void DeclRefExpr::setGenericArgs(ArrayRef<TypeRepr*> GenericArgs) {
  ValueDecl *D = getDecl();
  assert(D);
  setSpecialized();
  getSpecInfo()->GenericArgs = D->getASTContext().AllocateCopy(GenericArgs);
}

ConstructorDecl *OtherConstructorDeclRefExpr::getDecl() const {
  return cast_or_null<ConstructorDecl>(Ctor.getDecl());
}

MemberRefExpr::MemberRefExpr(Expr *base, SourceLoc dotLoc,
                             ConcreteDeclRef member, SourceRange nameRange,
                             bool Implicit, bool UsesDirectPropertyAccess)
  : Expr(ExprKind::MemberRef, Implicit), Base(base),
    Member(member), DotLoc(dotLoc), NameRange(nameRange) {
   
  MemberRefExprBits.IsDirectPropertyAccess = UsesDirectPropertyAccess;
  MemberRefExprBits.IsSuper = false;
}

Type OverloadSetRefExpr::getBaseType() const {
  if (isa<OverloadedDeclRefExpr>(this))
    return Type();
  if (auto *DRE = dyn_cast<OverloadedMemberRefExpr>(this)) {
    return DRE->getBase()->getType()->getRValueType();
  }
  
  llvm_unreachable("Unhandled overloaded set reference expression");
}

bool OverloadSetRefExpr::hasBaseObject() const {
  if (Type BaseTy = getBaseType())
    return !BaseTy->is<AnyMetatypeType>();

  return false;
}

SequenceExpr *SequenceExpr::create(ASTContext &ctx, ArrayRef<Expr*> elements) {
  void *Buffer = ctx.Allocate(sizeof(SequenceExpr) +
                              elements.size() * sizeof(Expr*),
                              alignof(SequenceExpr));
  return ::new(Buffer) SequenceExpr(elements);
}

NewArrayExpr *NewArrayExpr::create(ASTContext &ctx, SourceLoc newLoc,
                                   TypeLoc elementTy, ArrayRef<Bound> bounds,
                                   Expr *constructionFn) {
  void *buffer = ctx.Allocate(sizeof(NewArrayExpr) +
                              bounds.size() * sizeof(Bound),
                              alignof(NewArrayExpr));
  NewArrayExpr *E =
    ::new (buffer) NewArrayExpr(newLoc, elementTy, bounds.size(),
                                constructionFn);
  memcpy(E->getBoundsBuffer(), bounds.data(), bounds.size() * sizeof(Bound));
  return E;
}

SourceRange TupleExpr::getSourceRange() const {
  if (LParenLoc.isValid() && !hasTrailingClosure()) {
    assert(RParenLoc.isValid() && "Mismatched parens?");
    return SourceRange(LParenLoc, RParenLoc);
  }
  if (getElements().empty())
    return SourceRange();
  
  SourceLoc Start = LParenLoc.isValid()? LParenLoc
                                       : getElement(0)->getStartLoc();
  SourceLoc End = getElement(getElements().size()-1)->getEndLoc();
  return SourceRange(Start, End);
}

TupleExpr::TupleExpr(SourceLoc LParenLoc, ArrayRef<Expr *> SubExprs,
                     ArrayRef<Identifier> ElementNames, 
                     ArrayRef<SourceLoc> ElementNameLocs,
                     SourceLoc RParenLoc, bool HasTrailingClosure, 
                     bool Implicit, Type Ty)
  : Expr(ExprKind::Tuple, Implicit, Ty),
    LParenLoc(LParenLoc), RParenLoc(RParenLoc),
    NumElements(SubExprs.size())
{
  TupleExprBits.HasTrailingClosure = HasTrailingClosure;
  TupleExprBits.HasElementNames = !ElementNames.empty();
  TupleExprBits.HasElementNameLocations = !ElementNameLocs.empty();
  
  assert(LParenLoc.isValid() == RParenLoc.isValid() &&
         "Mismatched parenthesis location information validity");
  assert(ElementNames.empty() || ElementNames.size() == SubExprs.size());
  assert(ElementNameLocs.empty() || 
         ElementNames.size() == ElementNameLocs.size());

  // Copy elements.
  memcpy(getElements().data(), SubExprs.data(), 
         SubExprs.size() * sizeof(Expr *));

  // Copy element names, if provided.
  if (hasElementNames()) {
    memcpy(getElementNamesBuffer().data(), ElementNames.data(),
           ElementNames.size() * sizeof(Identifier));
  }

  // Copy element name locations, if provided.
  if (hasElementNameLocs()) {
    memcpy(getElementNameLocsBuffer().data(), ElementNameLocs.data(),
           ElementNameLocs.size() * sizeof(SourceLoc));
  }
}

TupleExpr *TupleExpr::create(ASTContext &ctx,
                             SourceLoc LParenLoc, 
                             ArrayRef<Expr *> SubExprs,
                             ArrayRef<Identifier> ElementNames, 
                             ArrayRef<SourceLoc> ElementNameLocs,
                             SourceLoc RParenLoc, bool HasTrailingClosure, 
                             bool Implicit, Type Ty) {
  unsigned size = sizeof(TupleExpr);
  size += SubExprs.size() * sizeof(Expr*);
  size += ElementNames.size() * sizeof(Identifier);
  size += ElementNameLocs.size() * sizeof(SourceLoc);
  void *mem = ctx.Allocate(size, alignof(TupleExpr));
  return new (mem) TupleExpr(LParenLoc, SubExprs, ElementNames, ElementNameLocs,
                             RParenLoc, HasTrailingClosure, Implicit, Ty);
}

TupleExpr *TupleExpr::createEmpty(ASTContext &ctx, SourceLoc LParenLoc, 
                                  SourceLoc RParenLoc, bool Implicit) {
  return create(ctx, LParenLoc, { }, { }, { }, RParenLoc, 
                /*HasTrailingClosure=*/false, Implicit, 
                TupleType::getEmpty(ctx));
}

TupleExpr *TupleExpr::createImplicit(ASTContext &ctx, ArrayRef<Expr *> SubExprs,
                                     ArrayRef<Identifier> ElementNames) {
  return create(ctx, SourceLoc(), SubExprs, ElementNames, { }, SourceLoc(),
                /*HasTrailingClosure=*/false, /*Implicit=*/true, Type());
}

ArrayRef<Expr *> CollectionExpr::getElements() const {
  if (auto paren = dyn_cast<ParenExpr>(SubExpr)) {
    // FIXME: Hack. When this goes away, remove IdentityExpr's friendship of
    // CollectionExpr.
    return llvm::makeArrayRef(&paren->SubExpr, 1);
  }

  return cast<TupleExpr>(SubExpr)->getElements();
}

static ValueDecl *getCalledValue(Expr *E) {
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  Expr *E2 = E->getValueProvidingExpr();
  if (E != E2) return getCalledValue(E2);
  return nullptr;
}

ValueDecl *ApplyExpr::getCalledValue() const {
  return ::getCalledValue(Fn);
}

RebindSelfInConstructorExpr::RebindSelfInConstructorExpr(Expr *SubExpr,
                                                         VarDecl *Self)
  : Expr(ExprKind::RebindSelfInConstructor, /*Implicit=*/true,
         TupleType::getEmpty(Self->getASTContext())),
    SubExpr(SubExpr), Self(Self)
{}

void AbstractClosureExpr::setParams(Pattern *P) {
  ParamPattern = P;
  // Change the DeclContext of any parameters to be this closure.
  if (P) {
    P->forEachVariable([&](VarDecl *VD) {
      VD->setDeclContext(this);
    });
  }
}


Type AbstractClosureExpr::getResultType() const {
  if (getType()->is<ErrorType>())
    return getType();

  return getType()->castTo<FunctionType>()->getResult();
}

SourceRange ClosureExpr::getSourceRange() const {
  return Body.getPointer()->getSourceRange();
}

SourceLoc ClosureExpr::getLoc() const {
  return Body.getPointer()->getStartLoc();
}

Expr *ClosureExpr::getSingleExpressionBody() const {
  assert(hasSingleExpressionBody() && "Not a single-expression body");
  return cast<ReturnStmt>(Body.getPointer()->getElements()[0].get<Stmt *>())
           ->getResult();
}

void ClosureExpr::setSingleExpressionBody(Expr *NewBody) {
  cast<ReturnStmt>(Body.getPointer()->getElements()[0].get<Stmt *>())
    ->setResult(NewBody);
}

SourceRange AutoClosureExpr::getSourceRange() const {
  return Body->getSourceRange();
}

void AutoClosureExpr::setBody(Expr *E) {
  auto &Context = getASTContext();
  auto *RS = new (Context) ReturnStmt(SourceLoc(), E);
  Body = BraceStmt::create(Context, E->getStartLoc(), { RS }, E->getEndLoc());
}

Expr *AutoClosureExpr::getSingleExpressionBody() const {
  return cast<ReturnStmt>(Body->getElements()[0].get<Stmt *>())->getResult();
}

SourceRange AssignExpr::getSourceRange() const {
  if (isFolded())
    return SourceRange(Dest->getStartLoc(), Src->getEndLoc());
  return EqualLoc;
}

SourceLoc UnresolvedPatternExpr::getLoc() const { return subPattern->getLoc(); }
SourceRange UnresolvedPatternExpr::getSourceRange() const {
  return subPattern->getSourceRange();
}

UnresolvedSelectorExpr::UnresolvedSelectorExpr(Expr *subExpr, SourceLoc dotLoc,
                                               DeclName name,
                                               ArrayRef<ComponentLoc> components)
  : Expr(ExprKind::UnresolvedSelector, /*implicit*/ false),
    SubExpr(subExpr), DotLoc(dotLoc), Name(name)
{
  assert(name.getArgumentNames().size() + 1 == components.size() &&
         "number of component locs does not match number of name components");
  auto buf = getComponentsBuf();
  std::uninitialized_copy(components.begin(), components.end(),
                          buf.begin());
}

UnresolvedSelectorExpr *UnresolvedSelectorExpr::create(ASTContext &C,
             Expr *subExpr, SourceLoc dotLoc,
             DeclName name,
             ArrayRef<ComponentLoc> components) {
  assert(name.getArgumentNames().size() + 1 == components.size() &&
         "number of component locs does not match number of name components");
  
  void *buf = C.Allocate(sizeof(UnresolvedSelectorExpr)
                           + (name.getArgumentNames().size() + 1)
                               * sizeof(ComponentLoc),
                         alignof(UnresolvedSelectorExpr));
  return ::new (buf) UnresolvedSelectorExpr(subExpr, dotLoc, name, components);
}

unsigned ScalarToTupleExpr::getScalarField() const {
  unsigned result = std::find(Elements.begin(), Elements.end(), Element())
                      - Elements.begin();
  assert(result != Elements.size()
         && "Tuple elements are missing the scalar 'hole'");
  return result;
}

TypeExpr::TypeExpr(TypeLoc TyLoc)
  : Expr(ExprKind::Type, /*implicit*/false), Info(TyLoc) {
  Type Ty = TyLoc.getType();
  if (Ty && Ty->hasCanonicalTypeComputed())
    setType(MetatypeType::get(Ty, Ty->getASTContext()));
}

TypeExpr::TypeExpr(Type Ty)
  : Expr(ExprKind::Type, /*implicit*/true), Info(TypeLoc::withoutLoc(Ty)) {
  if (Ty->hasCanonicalTypeComputed())
    setType(MetatypeType::get(Ty, Ty->getASTContext()));
}

/// Return a TypeExpr for a simple identifier and the specified location.
TypeExpr *TypeExpr::createForDecl(SourceLoc Loc, TypeDecl *Decl) {
  ASTContext &C = Decl->getASTContext();
  assert(Loc.isValid());
  auto *Repr = new (C) SimpleIdentTypeRepr(Loc, Decl->getName());
  Repr->setValue(Decl);
  return new (C) TypeExpr(TypeLoc(Repr, Type()));
}

TypeExpr *TypeExpr::createForSpecializedDecl(SourceLoc Loc, TypeDecl *D,
                                             ArrayRef<TypeRepr*> args,
                                             SourceRange AngleLocs) {
  ASTContext &C = D->getASTContext();
  assert(Loc.isValid());
  auto *Repr = new (C) GenericIdentTypeRepr(Loc, D->getName(),
                                            args, AngleLocs);
  Repr->setValue(D);
  return new (C) TypeExpr(TypeLoc(Repr, Type()));
}


// Create an implicit TypeExpr, with location information even though it
// shouldn't have one.  This is presently used to work around other location
// processing bugs.  If you have an implicit location, use createImplicit.
TypeExpr *TypeExpr::createImplicitHack(SourceLoc Loc, Type Ty, ASTContext &C) {
  // FIXME: This is horrible.
  if (Loc.isInvalid()) return createImplicit(Ty, C);
  auto Name = C.getIdentifier("<<IMPLICIT>>");
  auto *Repr = new (C) SimpleIdentTypeRepr(Loc, Name);
  Repr->setValue(Ty);
  auto *Res = new (C) TypeExpr(TypeLoc(Repr, Ty));
  Res->setImplicit();
  Res->setType(MetatypeType::get(Ty, C));
  return Res;
}


SourceRange DynamicTypeExpr::getSourceRange() const {
  if (MetatypeLoc.isValid())
    return SourceRange(getBase()->getStartLoc(), MetatypeLoc);

  return getBase()->getSourceRange();
}

SourceRange UnresolvedMemberExpr::getSourceRange() const { 
  if (Argument)
    return SourceRange(DotLoc, Argument->getEndLoc());

  return SourceRange(DotLoc, NameLoc);
}

ArchetypeType *OpenExistentialExpr::getOpenedArchetype() const {
  auto type = getOpaqueValue()->getType();
  if (auto metaTy = type->getAs<MetatypeType>())
    type = metaTy->getInstanceType();
  return type->castTo<ArchetypeType>();
}

