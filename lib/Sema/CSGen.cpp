//===--- CSGen.cpp - Constraint Generator ---------------------------------===//
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
// This file implements constraint generation for the type checker.
//
//===----------------------------------------------------------------------===//
#include "ConstraintGraph.h"
#include "ConstraintSystem.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Expr.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/APInt.h"

using namespace swift;
using namespace swift::constraints;

/// \brief Skip any implicit conversions applied to this expression.
static Expr *skipImplicitConversions(Expr *expr) {
  while (auto ice = dyn_cast<ImplicitConversionExpr>(expr))
    expr = ice->getSubExpr();
  return expr;
}

/// \brief Find the declaration directly referenced by this expression.
static ValueDecl *findReferencedDecl(Expr *expr, SourceLoc &loc) {
  do {
    expr = expr->getSemanticsProvidingExpr();

    if (auto ice = dyn_cast<ImplicitConversionExpr>(expr)) {
      expr = ice->getSubExpr();
      continue;
    }

    if (auto dre = dyn_cast<DeclRefExpr>(expr)) {
      loc = dre->getLoc();
      return dre->getDecl();
    }

    return nullptr;
  } while (true);
}

/// \brief Return 'true' if the decl in question refers to an operator that
/// could be added to the global scope via a delayed protcol conformance.
/// Currently, this is only true for '==', which is added via an Equatable
/// conformance.
static bool isDelayedOperatorDecl(ValueDecl *vd) {
  return vd && (vd->getName().str() == "==");
}

namespace {
  class ConstraintGenerator : public ExprVisitor<ConstraintGenerator, Type> {
    ConstraintSystem &CS;

    /// \brief Add constraints for a reference to a named member of the given
    /// base type, and return the type of such a reference.
    Type addMemberRefConstraints(Expr *expr, Expr *base, DeclName name) {
      // The base must have a member of the given name, such that accessing
      // that member through the base returns a value convertible to the type
      // of this expression.
      auto baseTy = base->getType();
      auto tv = CS.createTypeVariable(
                  CS.getConstraintLocator(expr, ConstraintLocator::Member),
                  TVO_CanBindToLValue);
      // FIXME: Constraint below should be a ::Member constraint?
      CS.addValueMemberConstraint(baseTy, name, tv,
        CS.getConstraintLocator(expr, ConstraintLocator::MemberRefBase));
      return tv;
    }

    /// \brief Add constraints for a reference to a specific member of the given
    /// base type, and return the type of such a reference.
    Type addMemberRefConstraints(Expr *expr, Expr *base, ValueDecl *decl) {
      // If we're referring to an invalid declaration, fail.
      if (!decl)
        return nullptr;
      
      CS.getTypeChecker().validateDecl(decl, true);
      if (decl->isInvalid())
        return nullptr;

      auto memberLocator =
        CS.getConstraintLocator(expr, ConstraintLocator::Member);
      auto tv = CS.createTypeVariable(memberLocator, TVO_CanBindToLValue);
      OverloadChoice choice(base->getType(), decl, /*isSpecialized=*/false);
      auto locator = CS.getConstraintLocator(expr, ConstraintLocator::Member);
      CS.addBindOverloadConstraint(tv, choice, locator);
      return tv;
    }

    /// \brief Add constraints for a subscript operation.
    Type addSubscriptConstraints(Expr *expr, Expr *base, Expr *index) {
      ASTContext &Context = CS.getASTContext();

      // Locators used in this expression.
      auto indexLocator
        = CS.getConstraintLocator(expr, ConstraintLocator::SubscriptIndex);
      auto resultLocator
        = CS.getConstraintLocator(expr, ConstraintLocator::SubscriptResult);

      // The base type must have a subscript declaration with type
      // I -> inout? O, where I and O are fresh type variables. The index
      // expression must be convertible to I and the subscript expression
      // itself has type inout? O, where O may or may not be an lvalue.
      auto inputTv = CS.createTypeVariable(indexLocator, /*options=*/0);
      auto outputTv = CS.createTypeVariable(resultLocator,
                                            TVO_CanBindToLValue);

      auto subscriptMemberLocator
        = CS.getConstraintLocator(expr, ConstraintLocator::SubscriptMember);

      // Add the member constraint for a subscript declaration.
      // FIXME: lame name!
      auto baseTy = base->getType();
      auto fnTy = FunctionType::get(inputTv, outputTv);
      CS.addValueMemberConstraint(baseTy, Context.Id_subscript,
                                  fnTy, subscriptMemberLocator);

      // Add the constraint that the index expression's type be convertible
      // to the input type of the subscript operator.
      CS.addConstraint(ConstraintKind::ArgumentTupleConversion,
                       index->getType(), inputTv, indexLocator);
      return outputTv;
    }

  public:
    ConstraintGenerator(ConstraintSystem &CS) : CS(CS) { }

    ConstraintSystem &getConstraintSystem() const { return CS; }
    
    Type visitErrorExpr(ErrorExpr *E) {
      // FIXME: Can we do anything with error expressions at this point?
      return nullptr;
    }

    Type visitLiteralExpr(LiteralExpr *expr) {
      auto protocol = CS.getTypeChecker().getLiteralProtocol(expr);
      if (!protocol) {
        return nullptr;
      }

      auto tv = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                      TVO_PrefersSubtypeBinding);
      
      tv->getImpl().literalConformanceProto = protocol;
      
      CS.addConstraint(ConstraintKind::ConformsTo, tv,
                       protocol->getDeclaredType(),
                       CS.getConstraintLocator(CS.rootExpr));
      return tv;
    }

    Type
    visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *expr) {
      // Dig out the StringInterpolationConvertible protocol.
      auto &tc = CS.getTypeChecker();
      auto &C = CS.getASTContext();
      auto interpolationProto
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::StringInterpolationConvertible);
      if (!interpolationProto) {
        tc.diagnose(expr->getStartLoc(), diag::interpolation_missing_proto);
        return nullptr;
      }

      // The type of the expression must conform to the
      // StringInterpolationConvertible protocol.
      auto tv = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                      TVO_PrefersSubtypeBinding);
      CS.addConstraint(ConstraintKind::ConformsTo, tv,
                       interpolationProto->getDeclaredType(),
                       CS.getConstraintLocator(CS.rootExpr));

      // Each of the segments is passed as an argument to
      // convertFromStringInterpolationSegment().
      unsigned index = 0;
      auto tvMeta = MetatypeType::get(tv);
      for (auto segment : expr->getSegments()) {
        auto locator = CS.getConstraintLocator(
                         expr,
                         LocatorPathElt::getInterpolationArgument(index++));
        auto segmentTyV = CS.createTypeVariable(locator, /*options=*/0);
        auto returnTyV = CS.createTypeVariable(locator, /*options=*/0);
        auto methodTy = FunctionType::get(segmentTyV, returnTyV);

        CS.addConstraint(Constraint::create(CS, ConstraintKind::Conversion,
                                            segment->getType(),
                                            segmentTyV,
                                            Identifier(),
                                            locator));

        CS.addConstraint(Constraint::create(CS, ConstraintKind::ValueMember,
                                            tvMeta,
                                            methodTy,
                                            C.Id_ConvertFromStringInterpolationSegment,
                                            locator));

      }
      
      return tv;
    }

    Type visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *expr) {
      switch (expr->getKind()) {
      case MagicIdentifierLiteralExpr::Column:
      case MagicIdentifierLiteralExpr::File:
      case MagicIdentifierLiteralExpr::Function:
      case MagicIdentifierLiteralExpr::Line:
        return visitLiteralExpr(expr);

      case MagicIdentifierLiteralExpr::DSOHandle: {
        // __DSO_HANDLE__ has type UnsafeMutablePointer<Void>.
        auto &tc = CS.getTypeChecker();
        if (tc.requirePointerArgumentIntrinsics(expr->getLoc()))
          return nullptr;

        return CS.DC->getParentModule()->getDSOHandle()->getInterfaceType();
      }
      }
    }
    Type visitDeclRefExpr(DeclRefExpr *E) {
      // If we're referring to an invalid declaration, don't type-check.
      //
      // FIXME: If the decl is in error, we get no information from this.
      // We may, alternatively, want to use a type variable in that case,
      // and possibly infer the type of the variable that way.
      CS.getTypeChecker().validateDecl(E->getDecl(), true);
      if (E->getDecl()->isInvalid())
        return nullptr;

      auto locator = CS.getConstraintLocator(E);

      // Create an overload choice referencing this declaration and immediately
      // resolve it. This records the overload for use later.
      auto tv = CS.createTypeVariable(locator, TVO_CanBindToLValue);
      CS.resolveOverload(locator, tv,
                         OverloadChoice(Type(), E->getDecl(),
                                        E->isSpecialized()));

      return tv;
    }

    Type visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *E) {
      return E->getType();
    }

    Type visitSuperRefExpr(SuperRefExpr *E) {
      if (E->getType())
        return E->getType();

      // Resolve the super type of 'self'.
      return getSuperType(E->getSelf(), E->getLoc(),
                          diag::super_not_in_class_method,
                          diag::super_with_no_base_class);
    }

    Type visitTypeExpr(TypeExpr *E) {
      Type type;
      // If this is an implicit TypeExpr, don't validate its contents.
      if (auto *rep = E->getTypeRepr())
        type = CS.TC.resolveType(rep, CS.DC, TR_AllowUnboundGenerics);
      else
        type = E->getTypeLoc().getType();
      if (!type) return Type();
      
      type = CS.openType(type);
      E->getTypeLoc().setType(type, /*validated=*/true);
      return MetatypeType::get(CS.openType(type));
    }

    Type visitUnresolvedConstructorExpr(UnresolvedConstructorExpr *expr) {
      ASTContext &C = CS.getASTContext();
      
      // Open a member constraint for constructors on the subexpr type.
      // FIXME: the getRValueInstanceType() here is a hack to make the
      //   T.init withFoo(foo)
      // syntax type-check. We shouldn't rely on any kinds of adjustments to
      // the subexpression's type here, but dealing with this requires us to
      // clarify when we can refer to constructors with ".init".
      auto baseTy = expr->getSubExpr()->getType()
                      ->getLValueOrInOutObjectType()->getRValueInstanceType();
      auto argsTy = CS.createTypeVariable(
                      CS.getConstraintLocator(expr),
                      TVO_CanBindToLValue|TVO_PrefersSubtypeBinding);
      auto resultTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                            /*options=*/0);
      auto methodTy = FunctionType::get(argsTy, resultTy);
      CS.addValueMemberConstraint(baseTy,
                                  C.Id_init,
        methodTy,
        CS.getConstraintLocator(expr, ConstraintLocator::ConstructorMember));
      
      // The result of the expression is the partial application of the
      // constructor to the subexpression.
      return methodTy;
    }
    
    Type visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Type visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *expr) {
      // For a reference to an overloaded declaration, we create a type variable
      // that will be equal to different types depending on which overload
      // is selected.
      auto locator = CS.getConstraintLocator(expr);
      auto tv = CS.createTypeVariable(locator, TVO_CanBindToLValue);
      ArrayRef<ValueDecl*> decls = expr->getDecls();
      SmallVector<OverloadChoice, 4> choices;
      
      if (decls.size()) {
        if (isDelayedOperatorDecl(decls[0])) {
          expr->setIsPotentiallyDelayedGlobalOperator();
        }
      }
      
      for (unsigned i = 0, n = decls.size(); i != n; ++i) {
        // If the result is invalid, skip it.
        // FIXME: Note this as invalid, in case we don't find a solution,
        // so we don't let errors cascade further.
        CS.getTypeChecker().validateDecl(decls[i], true);
        if (decls[i]->isInvalid())
          continue;

        choices.push_back(OverloadChoice(Type(), decls[i],
                                         expr->isSpecialized()));
      }

      // If there are no valid overloads, give up.
      if (choices.empty())
        return nullptr;

      // Record this overload set.
      CS.addOverloadSet(tv, choices, locator);
      return tv;
    }

    Type visitOverloadedMemberRefExpr(OverloadedMemberRefExpr *expr) {
      // For a reference to an overloaded declaration, we create a type variable
      // that will be bound to different types depending on which overload
      // is selected.
      auto tv = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                      TVO_CanBindToLValue);
      ArrayRef<ValueDecl*> decls = expr->getDecls();
      SmallVector<OverloadChoice, 4> choices;
      auto baseTy = expr->getBase()->getType();
      for (unsigned i = 0, n = decls.size(); i != n; ++i) {
        // If the result is invalid, skip it.
        // FIXME: Note this as invalid, in case we don't find a solution,
        // so we don't let errors cascade further.
        CS.getTypeChecker().validateDecl(decls[i], true);
        if (decls[i]->isInvalid())
          continue;

        choices.push_back(OverloadChoice(baseTy, decls[i],
                                         /*isSpecialized=*/false));
      }

      // If there are no valid overloads, give up.
      if (choices.empty())
        return nullptr;

      // Record this overload set.
      auto locator = CS.getConstraintLocator(expr, ConstraintLocator::Member);
      CS.addOverloadSet(tv, choices, locator);
      return tv;
    }
    
    Type visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *expr) {
      // This is an error case, where we're trying to use type inference
      // to help us determine which declaration the user meant to refer to.
      // FIXME: Do we need to note that we're doing some kind of recovery?
      return CS.createTypeVariable(CS.getConstraintLocator(expr),
                                   TVO_CanBindToLValue);
    }
    
    Type visitMemberRefExpr(MemberRefExpr *expr) {
      return addMemberRefConstraints(expr, expr->getBase(),
                                     expr->getMember().getDecl());
    }
    
    Type visitDynamicMemberRefExpr(DynamicMemberRefExpr *expr) {
      return addMemberRefConstraints(expr, expr->getBase(),
                                     expr->getMember().getDecl());
    }
    
    Type visitUnresolvedMemberExpr(UnresolvedMemberExpr *expr) {
      auto baseLocator = CS.getConstraintLocator(
                            expr,
                            ConstraintLocator::MemberRefBase);
      auto memberLocator
        = CS.getConstraintLocator(expr, ConstraintLocator::UnresolvedMember);
      auto baseTy = CS.createTypeVariable(baseLocator, /*options=*/0);
      auto memberTy = CS.createTypeVariable(memberLocator, TVO_CanBindToLValue);

      // An unresolved member expression '.member' is modeled as a value member
      // constraint
      //
      //   T0.Type[.member] == T1
      //
      // for fresh type variables T0 and T1, which pulls out a static
      // member, i.e., an enum case or a static variable.
      auto baseMetaTy = MetatypeType::get(baseTy);
      CS.addUnresolvedValueMemberConstraint(baseMetaTy, expr->getName(),
                                            memberTy, memberLocator);

      // If there is an argument, apply it.
      if (auto arg = expr->getArgument()) {
        // The result type of the function must be convertible to the base type.
        // TODO: we definitely want this to include ImplicitlyUnwrappedOptional; does it
        // need to include everything else in the world?
        auto outputTy
          = CS.createTypeVariable(
              CS.getConstraintLocator(expr, ConstraintLocator::ApplyFunction),
              /*options=*/0);
        CS.addConstraint(ConstraintKind::Conversion, outputTy, baseTy,
          CS.getConstraintLocator(expr, ConstraintLocator::RvalueAdjustment));

        // The function/enum case must be callable with the given argument.
        auto funcTy = FunctionType::get(arg->getType(), outputTy);
        CS.addConstraint(ConstraintKind::ApplicableFunction, funcTy,
          memberTy,
          CS.getConstraintLocator(expr, ConstraintLocator::ApplyFunction));
        
        return baseTy;
      }

      // Otherwise, the member needs to be convertible to the base type.
      CS.addConstraint(ConstraintKind::Conversion, memberTy, baseTy,
        CS.getConstraintLocator(expr, ConstraintLocator::RvalueAdjustment));
      
      // The member type also needs to be convertible to the context type, which
      // preserves lvalue-ness.
      auto resultTy = CS.createTypeVariable(memberLocator, TVO_CanBindToLValue);
      CS.addConstraint(ConstraintKind::Conversion, memberTy, resultTy,
                       memberLocator);
      CS.addConstraint(ConstraintKind::Equal, resultTy, baseTy,
                       memberLocator);
      return resultTy;
    }

    Type visitUnresolvedDotExpr(UnresolvedDotExpr *expr) {
      return addMemberRefConstraints(expr, expr->getBase(), expr->getName());
    }
    
    Type visitUnresolvedSelectorExpr(UnresolvedSelectorExpr *expr) {
      return addMemberRefConstraints(expr, expr->getBase(), expr->getName());
    }
    
    Type visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *expr) {
      auto baseTy = expr->getSubExpr()->getType();
      
      // We currently only support explicit specialization of generic types.
      // FIXME: We could support explicit function specialization.
      auto &tc = CS.getTypeChecker();
      if (baseTy->is<AnyFunctionType>()) {
        tc.diagnose(expr->getSubExpr()->getLoc(),
                    diag::cannot_explicitly_specialize_generic_function);
        tc.diagnose(expr->getLAngleLoc(),
                    diag::while_parsing_as_left_angle_bracket);
        return Type();
      }
      
      if (AnyMetatypeType *meta = baseTy->getAs<AnyMetatypeType>()) {
        if (BoundGenericType *bgt
              = meta->getInstanceType()->getAs<BoundGenericType>()) {
          ArrayRef<Type> typeVars = bgt->getGenericArgs();
          ArrayRef<TypeLoc> specializations = expr->getUnresolvedParams();

          // If we have too many generic arguments, complain.
          if (specializations.size() > typeVars.size()) {
            tc.diagnose(expr->getSubExpr()->getLoc(),
                        diag::type_parameter_count_mismatch,
                        bgt->getDecl()->getName(),
                        typeVars.size(), specializations.size(),
                        false)
              .highlight(SourceRange(expr->getLAngleLoc(),
                                     expr->getRAngleLoc()));
            tc.diagnose(bgt->getDecl(), diag::generic_type_declared_here,
                        bgt->getDecl()->getName());
            return Type();
          }

          // Bind the specified generic arguments to the type variables in the
          // open type.
          for (size_t i = 0, size = specializations.size(); i < size; ++i) {
            CS.addConstraint(ConstraintKind::Equal,
                             typeVars[i], specializations[i].getType(),
                             CS.getConstraintLocator(CS.rootExpr));
          }
          
          return baseTy;
        } else {
          tc.diagnose(expr->getSubExpr()->getLoc(), diag::not_a_generic_type,
                      meta->getInstanceType());
          tc.diagnose(expr->getLAngleLoc(),
                      diag::while_parsing_as_left_angle_bracket);
          return Type();
        }
      }

      // FIXME: If the base type is a type variable, constrain it to a metatype
      // of a bound generic type.
      
      tc.diagnose(expr->getSubExpr()->getLoc(),
                  diag::not_a_generic_definition);
      tc.diagnose(expr->getLAngleLoc(),
                  diag::while_parsing_as_left_angle_bracket);
      return Type();
    }
    
    Type visitSequenceExpr(SequenceExpr *expr) {
      llvm_unreachable("Didn't even parse?");
    }

    Type visitIdentityExpr(IdentityExpr *expr) {
      expr->setType(expr->getSubExpr()->getType());
      return expr->getType();
    }

    Type visitParenExpr(ParenExpr *expr) {
      auto &ctx = CS.getASTContext();
      expr->setType(ParenType::get(ctx, expr->getSubExpr()->getType()));
      return expr->getType();
    }

    Type visitTupleExpr(TupleExpr *expr) {
      // The type of a tuple expression is simply a tuple of the types of
      // its subexpressions.
      SmallVector<TupleTypeElt, 4> elements;
      elements.reserve(expr->getNumElements());
      for (unsigned i = 0, n = expr->getNumElements(); i != n; ++i) {
        elements.push_back(TupleTypeElt(expr->getElement(i)->getType(),
                                        expr->getElementName(i)));
      }

      return TupleType::get(elements, CS.getASTContext());
    }

    Type visitSubscriptExpr(SubscriptExpr *expr) {
      return addSubscriptConstraints(expr, expr->getBase(), expr->getIndex());
    }
    
    Type visitArrayExpr(ArrayExpr *expr) {
      ASTContext &C = CS.getASTContext();
      
      // An array expression can be of a type T that conforms to the
      // ArrayLiteralConvertible protocol.
      auto &tc = CS.getTypeChecker();
      ProtocolDecl *arrayProto
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::ArrayLiteralConvertible);
      if (!arrayProto) {
        return Type();
      }

      // FIXME: Protect against broken standard library.
      auto elementAssocTy = cast<AssociatedTypeDecl>(
                              arrayProto->lookupDirect(
                                C.getIdentifier("Element")).front());

      auto locator = CS.getConstraintLocator(expr);
      auto contextualType = CS.getContextualType(expr);
      Type *contextualArrayType = nullptr;
      Type contextualArrayElementType = nullptr;
      
      // If a contextual type exists for this expression, apply it directly.
      if (contextualType && CS.isArrayType(*contextualType)) {
        // Is the array type a contextual type
        contextualArrayType = contextualType;
        contextualArrayElementType =
            CS.getBaseTypeForArrayType(contextualType->getPointer());
        
        CS.addConstraint(ConstraintKind::ConformsTo, *contextualType,
                         arrayProto->getDeclaredType(),
                         locator);
        
        unsigned index = 0;
        for (auto element : expr->getElements()) {
          CS.addConstraint(ConstraintKind::Conversion,
                           element->getType(),
                           contextualArrayElementType,
                           CS.getConstraintLocator(expr,
                                                   LocatorPathElt::
                                                    getTupleElement(index++)));
        }
        
        return *contextualArrayType;
      }
      
      auto arrayTy = CS.createTypeVariable(locator, TVO_PrefersSubtypeBinding);

      // The array must be an array literal type.
      CS.addConstraint(ConstraintKind::ConformsTo, arrayTy,
                       arrayProto->getDeclaredType(),
                       locator);
      
      // Its subexpression should be convertible to a tuple (T.Element...).
      // FIXME: We should really go through the conformance above to extract
      // the element type, rather than just looking for the element type.
      // FIXME: Member constraint is still weird here.
      ConstraintLocatorBuilder builder(locator);
      auto arrayElementTy = CS.getMemberType(arrayTy, elementAssocTy,
                                             builder.withPathElement(
                                               ConstraintLocator::Member),
                                             /*options=*/0);

      // Introduce conversions from each element to the element type of the
      // array.
      unsigned index = 0;
      for (auto element : expr->getElements()) {
        CS.addConstraint(ConstraintKind::Conversion,
                         element->getType(),
                         arrayElementTy,
                         CS.getConstraintLocator(
                           expr,
                           LocatorPathElt::getTupleElement(index++)));
      }

      return arrayTy;
    }

    Type visitDictionaryExpr(DictionaryExpr *expr) {
      ASTContext &C = CS.getASTContext();
      // A dictionary expression can be of a type T that conforms to the
      // DictionaryLiteralConvertible protocol.
      // FIXME: This isn't actually used for anything at the moment.
      auto &tc = CS.getTypeChecker();
      ProtocolDecl *dictionaryProto
        = tc.getProtocol(expr->getLoc(),
                         KnownProtocolKind::DictionaryLiteralConvertible);
      if (!dictionaryProto) {
        return Type();
      }

      // FIXME: Protect against broken standard library.
      auto keyAssocTy = cast<AssociatedTypeDecl>(
                          dictionaryProto->lookupDirect(
                            C.getIdentifier("Key")).front());
      auto valueAssocTy = cast<AssociatedTypeDecl>(
                            dictionaryProto->lookupDirect(
                              C.getIdentifier("Value")).front());

      auto locator = CS.getConstraintLocator(expr);
      auto dictionaryTy = CS.createTypeVariable(locator,
                                                TVO_PrefersSubtypeBinding);

      // The array must be a dictionary literal type.
      CS.addConstraint(ConstraintKind::ConformsTo, dictionaryTy,
                       dictionaryProto->getDeclaredType(),
                       locator);


      // Its subexpression should be convertible to a tuple ((T.Key,T.Value)...).
      ConstraintLocatorBuilder locatorBuilder(locator);
      auto dictionaryKeyTy = CS.getMemberType(dictionaryTy,
                                              keyAssocTy,
                                              locatorBuilder.withPathElement(
                                                ConstraintLocator::Member),
                                              /*options=*/0);
      auto dictionaryValueTy = CS.getMemberType(dictionaryTy,
                                                valueAssocTy,
                                                locatorBuilder.withPathElement(
                                                  ConstraintLocator::Member),
                                                /*options=*/0);
      
      TupleTypeElt tupleElts[2] = { TupleTypeElt(dictionaryKeyTy),
                                    TupleTypeElt(dictionaryValueTy) };
      Type elementTy = TupleType::get(tupleElts, C);

      // Introduce conversions from each element to the element type of the
      // dictionary.
      unsigned index = 0;
      for (auto element : expr->getElements()) {
        CS.addConstraint(ConstraintKind::Conversion,
                         element->getType(),
                         elementTy,
                         CS.getConstraintLocator(
                           expr,
                           LocatorPathElt::getTupleElement(index++)));
      }

      return dictionaryTy;
    }

    Type visitDynamicSubscriptExpr(DynamicSubscriptExpr *expr) {
      return addSubscriptConstraints(expr, expr->getBase(), expr->getIndex());
    }

    Type visitTupleElementExpr(TupleElementExpr *expr) {
      ASTContext &context = CS.getASTContext();
      Identifier name
        = context.getIdentifier(llvm::utostr(expr->getFieldNumber()));
      return addMemberRefConstraints(expr, expr->getBase(), name);
    }

    /// \brief Produces a type for the given pattern, filling in any missing
    /// type information with fresh type variables.
    ///
    /// \param pattern The pattern.
    Type getTypeForPattern(Pattern *pattern, bool forFunctionParam,
                           ConstraintLocatorBuilder locator) {
      switch (pattern->getKind()) {
      case PatternKind::Paren:
        // Parentheses don't affect the type.
        return getTypeForPattern(cast<ParenPattern>(pattern)->getSubPattern(),
                                 forFunctionParam, locator);
      case PatternKind::Var:
        // Var doesn't affect the type.
        return getTypeForPattern(cast<VarPattern>(pattern)->getSubPattern(),
                                 forFunctionParam, locator);
      case PatternKind::Any:
        // For a pattern of unknown type, create a new type variable.
        return CS.createTypeVariable(CS.getConstraintLocator(locator),
                                     forFunctionParam? TVO_CanBindToLValue : 0);

      case PatternKind::Named: {
        auto var = cast<NamedPattern>(pattern)->getDecl();

        // For a named pattern without a type, create a new type variable
        // and use it as the type of the variable.
        Type ty = CS.createTypeVariable(CS.getConstraintLocator(locator),
                                        forFunctionParam? TVO_CanBindToLValue
                                                        : 0);

        // For weak variables, use Optional<T>.
        if (auto *OA = var->getAttrs().getAttribute<OwnershipAttr>())
          if (!forFunctionParam && OA->get() == Ownership::Weak) {
            ty = CS.getTypeChecker().getOptionalType(var->getLoc(), ty);
            if (!ty) return Type();
          }

        // We want to set the variable's type here when type-checking
        // a function's parameter clauses because we're going to
        // type-check the entire function body within the context of
        // the constraint system.  In contrast, when type-checking a
        // variable binding, we really don't want to set the
        // variable's type because it can easily escape the constraint
        // system and become a dangling type reference.
        if (forFunctionParam)
          var->setType(ty);
        return ty;
      }

      case PatternKind::Typed: {
        auto typedPattern = cast<TypedPattern>(pattern);

        Type openedType = CS.openType(typedPattern->getType());
        if (auto weakTy = openedType->getAs<WeakStorageType>())
          openedType = weakTy->getReferentType();

        // For a typed pattern, simply return the opened type of the pattern.
        // FIXME: Error recovery if the type is an error type?
        return openedType;
      }

      case PatternKind::Tuple: {
        auto tuplePat = cast<TuplePattern>(pattern);
        SmallVector<TupleTypeElt, 4> tupleTypeElts;
        tupleTypeElts.reserve(tuplePat->getNumFields());
        for (unsigned i = 0, e = tuplePat->getFields().size(); i != e; ++i) {
          auto tupleElt = tuplePat->getFields()[i];
          bool isVararg = tuplePat->hasVararg() && i == e-1;
          Type eltTy = getTypeForPattern(tupleElt.getPattern(),forFunctionParam,
                                         locator.withPathElement(
                                           LocatorPathElt::getTupleElement(i)));

          Type varArgBaseTy;
          tupleTypeElts.push_back(TupleTypeElt(eltTy, Identifier(),
                                               tupleElt.getDefaultArgKind(),
                                               isVararg));
        }
        return TupleType::get(tupleTypeElts, CS.getASTContext());
      }
      
      // TODO
#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) case PatternKind::Id:
#include "swift/AST/PatternNodes.def"
        llvm_unreachable("not implemented");
      }

      llvm_unreachable("Unhandled pattern kind");
    }

    Type visitClosureExpr(ClosureExpr *expr) {
      // Closure expressions always have function type. In cases where a
      // parameter or return type is omitted, a fresh type variable is used to
      // stand in for that parameter or return type, allowing it to be inferred
      // from context.
      Type funcTy;
      if (expr->hasExplicitResultType()) {
        funcTy = expr->getExplicitResultTypeLoc().getType();
      } else {
        // If no return type was specified, create a fresh type
        // variable for it.
        funcTy = CS.createTypeVariable(
                   CS.getConstraintLocator(expr,
                                           ConstraintLocator::ClosureResult),
                   /*options=*/0);
      }

      // Walk through the patterns in the func expression, backwards,
      // computing the type of each pattern (which may involve fresh type
      // variables where parameter types where no provided) and building the
      // eventual function type.
      auto paramTy = getTypeForPattern(
                       expr->getParams(), /*forFunctionParam*/ true,
                       CS.getConstraintLocator(
                         expr,
                         LocatorPathElt::getTupleElement(0)));

      // FIXME: If we want keyword arguments for closures, add them here.
      funcTy = FunctionType::get(paramTy, funcTy);

      return funcTy;
    }

    Type visitAutoClosureExpr(AutoClosureExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Type visitModuleExpr(ModuleExpr *expr) {
      // Module expressions always have a fixed type.
      return expr->getType();
    }

    Type visitInOutExpr(InOutExpr *expr) {
      // The address-of operator produces an explicit inout T from an lvalue T.
      // We model this with the constraint
      //
      //     S < lvalue T
      //
      // where T is a fresh type variable.
      auto lvalue = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                          /*options=*/0);
      auto bound = LValueType::get(lvalue);
      auto result = InOutType::get(lvalue);
      CS.addConstraint(ConstraintKind::Conversion,
                       expr->getSubExpr()->getType(), bound,
                       CS.getConstraintLocator(expr,
                                               ConstraintLocator::AddressOf));
      return result;
    }

    Type visitDynamicTypeExpr(DynamicTypeExpr *expr) {
      auto tv = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                      /*options=*/0);
      CS.addConstraint(ConstraintKind::DynamicTypeOf, tv,
                       expr->getBase()->getType(),
           CS.getConstraintLocator(expr, ConstraintLocator::RvalueAdjustment));
      return tv;
    }

    Type visitOpaqueValueExpr(OpaqueValueExpr *expr) {
      return expr->getType();
    }

    Type visitDefaultValueExpr(DefaultValueExpr *expr) {
      expr->setType(expr->getSubExpr()->getType());
      return expr->getType();
    }

    /// Determine whether the given parameter and argument type should be
    /// "favored" because they match exactly.
    bool isFavoredParamAndArg(Type paramTy, Type argTy) {
      // Do the types match exactly?
      if (paramTy->isEqual(argTy))
        return true;

      // If the argument is a type variable created for a literal that has a
      // default type, this is a favored param/arg pair if the parameter is of
      // that default type.
      // Is the argument a type variable...
      if (auto argTypeVar = argTy->getAs<TypeVariableType>()) {
        if (auto proto = argTypeVar->getImpl().literalConformanceProto) {
          if (auto defaultTy = CS.TC.getDefaultType(proto, CS.DC)) {
            if (paramTy->isEqual(defaultTy))
              return true;
          }
        }
      }

      return false;
    }

    Type visitApplyExpr(ApplyExpr *expr) {
      // The function subexpression has some rvalue type T1 -> T2 for fresh
      // variables T1 and T2.
      auto outputTy
        = CS.createTypeVariable(
            CS.getConstraintLocator(expr, ConstraintLocator::ApplyFunction),
            /*options=*/0);

      auto funcTy = FunctionType::get(expr->getArg()->getType(), outputTy);
      
      auto isBinaryExpr = isa<BinaryExpr>(expr);
      
      // If we're generating constraints for a binary operator application,
      // there are two special situations to consider:
      //  1. If the type checker has any newly created functions with the
      //     operator's name. If it does, the overloads were created after the
      //     associated overloaded id expression was created, and we'll need to
      //     add a new disjunction constraint for the new set of overloads.
      //  2. If any component argument expressions (nested or otherwise) are
      //     literals, we can favor operator overloads whose argument types are
      //     identical to the literal type, or whose return types are identical
      //     to any contextual type associated with the application expression.
      if (isBinaryExpr) {
        if (auto declRef = dyn_cast<OverloadedDeclRefExpr>(expr->getFn())) {
        SmallVector<Constraint *, 4> constraints;
        
        auto fnType = expr->getFn()->getType().getPointer();
        
        if (auto tyvarType = dyn_cast<TypeVariableType>(fnType)) {
          auto &CG = CS.getConstraintGraph();
          
          // This type variable is only currently associated with the function
          // being applied, and the only constraint attached to it should
          // be the disjunction constraint for the overload group.
          CG.gatherConstraints(tyvarType, constraints);
          
          if (constraints.size()) {
            for (auto constraint : constraints) {
              if (constraint->getKind() == ConstraintKind::Disjunction) {
                SmallVector<Constraint *, 4> newConstraints;
                SmallVector<Constraint *, 4> favoredConstraints;
                
                auto oldConstraints = constraint->getNestedConstraints();
                auto csLoc = CS.getConstraintLocator(expr->getFn());
                
                // Only replace the disjunctive overload constraint.
                if (oldConstraints[0]->getKind() !=
                        ConstraintKind::BindOverload) {
                  continue;
                }

                // Find the argument types.
                auto argTy = expr->getArg()->getType();
                auto argTupleTy = argTy->castTo<TupleType>();
                Type firstArgTy = argTupleTy->getFields()[0].getType();
                Type secondArgTy = argTupleTy->getFields()[1].getType();

                // Copy over the existing bindings, dividing the constraints up
                // into "favored" and non-favored lists, should all of the
                // parameter/argument comparisons be favored.
                for (auto oldConstraint : oldConstraints) {
                  auto overloadChoice = oldConstraint->getOverloadChoice();
                  auto overloadDecl = overloadChoice.getDecl();
                  auto overloadTy = overloadDecl->getType();
                  
                  if (auto fnTy = overloadTy->getAs<AnyFunctionType>()) {
                    // Figure out the parameter type.
                    if (overloadDecl->getDeclContext()->isTypeContext()) {
                      fnTy = fnTy->castTo<AnyFunctionType>()->getResult()
                               ->castTo<AnyFunctionType>();
                    }

                    Type paramTy = fnTy->getInput();
                    auto paramTupleTy = paramTy->castTo<TupleType>();
                    auto firstParamTy = paramTupleTy->getFields()[0].getType();
                    auto secondParamTy = paramTupleTy->getFields()[1].getType();

                    auto resultTy = fnTy->getResult();
                    auto contextualTy = CS.getContextualType(expr);

                    if ((isFavoredParamAndArg(firstParamTy, firstArgTy) ||
                         isFavoredParamAndArg(secondParamTy, secondArgTy)) &&
                        firstParamTy->isEqual(secondParamTy) &&
                        (!contextualTy || (*contextualTy)->isEqual(resultTy))) {
                      oldConstraint->setFavored();
                      favoredConstraints.push_back(oldConstraint);
                    }
                  }
                  
                  newConstraints.push_back(oldConstraint);
                }
                
                // Now add the new bindings as overloads.
                // This will only occur if the new bindings were added while
                // solving the system, so disable the flag to prevent further
                // unnecessary checks.
                if (expr->IsGlobalDelayedOperatorApply) {
                  if(CS.TC.HasForcedExternalDecl &&
                     CS.TC.implicitlyDefinedFunctions.size()) {
                    
                    CS.TC.HasForcedExternalDecl = false;
                    
                    auto declName = declRef->getDecls()[0]->getName();
                    SmallVector<OverloadChoice, 4> choices;
                    
                    for (auto implicitFn : CS.TC.implicitlyDefinedFunctions) {
                      if (implicitFn->getName() == declName) {
                        CS.TC.validateDecl(implicitFn, true);
                        choices.push_back(OverloadChoice(Type(),
                                                         implicitFn,
                                                         declRef->isSpecialized()));
                      }
                    }
                  
                    if (!choices.empty()) {
                      for (auto choice : choices) {
                        auto overload = Constraint::createBindOverload(CS,
                                                                       tyvarType,
                                                                       choice,
                                                                       csLoc);
                        newConstraints.push_back(overload);
                      }
                    }
                  }
                }

                // Remove the original constraint from the inactive constraint
                // list and add the new one.
                CS.removeInactiveConstraint(constraint);
                
                if (favoredConstraints.size()) {
                  auto favoredConstraintsDisjunction =
                          Constraint::createDisjunction(CS,
                                                        favoredConstraints,
                                                        csLoc);
                  favoredConstraintsDisjunction->setFavored();
                  
                  if (newConstraints.size()) {
                    auto newConstraintsDisjunction =
                            Constraint::createDisjunction(CS,
                                                          newConstraints,
                                                          csLoc);
                    auto aggregateConstraints = {
                                                  favoredConstraintsDisjunction,
                                                  newConstraintsDisjunction
                                                };
                    CS.addConstraint(
                        Constraint::createDisjunction(CS,
                                                      aggregateConstraints,
                                                      csLoc));
                  } else {
                    CS.addConstraint(
                        Constraint::createDisjunction(CS,
                                                      favoredConstraints,
                                                      csLoc));
                  }
                } else {
                  CS.addConstraint(Constraint::createDisjunction(CS,
                                                                 newConstraints,
                                                                 csLoc));
                }
                break;
              }
            }
          }
        }
      }
    }

      CS.addConstraint(ConstraintKind::ApplicableFunction, funcTy,
        expr->getFn()->getType(),
        CS.getConstraintLocator(expr, ConstraintLocator::ApplyFunction));

      return outputTy;
    }

    Type getSuperType(ValueDecl *selfDecl,
                      SourceLoc diagLoc,
                      Diag<> diag_not_in_class,
                      Diag<> diag_no_base_class) {
      DeclContext *typeContext = selfDecl->getDeclContext()->getParent();
      assert(typeContext && "constructor without parent context?!");
      auto &tc = CS.getTypeChecker();
      ClassDecl *classDecl = typeContext->getDeclaredTypeInContext()
                               ->getClassOrBoundGenericClass();
      if (!classDecl) {
        tc.diagnose(diagLoc, diag_not_in_class);
        return Type();
      }
      if (!classDecl->hasSuperclass()) {
        tc.diagnose(diagLoc, diag_no_base_class);
        return Type();
      }

      Type superclassTy = typeContext->getDeclaredTypeInContext()
                            ->getSuperclass(&tc);
      if (selfDecl->getType()->is<AnyMetatypeType>())
        superclassTy = MetatypeType::get(superclassTy);
      return superclassTy;
    }
    
    Type visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *expr) {
      // The result is void.
      return TupleType::getEmpty(CS.getASTContext());
    }
    
    Type visitIfExpr(IfExpr *expr) {
      // The conditional expression must conform to LogicValue.
      Expr *condExpr = expr->getCondExpr();
      auto logicValue
        = CS.getTypeChecker().getProtocol(expr->getQuestionLoc(),
                                          KnownProtocolKind::BooleanType);
      if (!logicValue)
        return Type();

      CS.addConstraint(ConstraintKind::ConformsTo, condExpr->getType(),
                       logicValue->getDeclaredType(),
                       CS.getConstraintLocator(condExpr));

      // The branches must be convertible to a common type.
      auto resultTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                            TVO_PrefersSubtypeBinding);
      CS.addConstraint(ConstraintKind::Conversion,
                       expr->getThenExpr()->getType(), resultTy,
                       CS.getConstraintLocator(expr,
                                               ConstraintLocator::IfThen));
      CS.addConstraint(ConstraintKind::Conversion,
                       expr->getElseExpr()->getType(), resultTy,
                       CS.getConstraintLocator(expr,
                                               ConstraintLocator::IfElse));
      return resultTy;
    }
    
    Type visitImplicitConversionExpr(ImplicitConversionExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Type visitUnresolvedCheckedCastExpr(UnresolvedCheckedCastExpr *expr) {
      auto &tc = CS.getTypeChecker();
      
      // Validate the resulting type.
      if (tc.validateType(expr->getCastTypeLoc(), CS.DC,
                          TR_AllowUnboundGenerics))
        return nullptr;

      // Open the type we're casting to.
      auto toType = CS.openType(expr->getCastTypeLoc().getType());
      expr->getCastTypeLoc().setType(toType, /*validated=*/true);

      auto locator = CS.getConstraintLocator(expr,
                                     ConstraintLocator::CheckedCastOperand);

      // Form the disjunction of the two possible type checks.
      auto fromType = expr->getSubExpr()->getType();
      Constraint *constraints[2] = {
        // The source type can be coerced to the destination type.
        Constraint::create(CS, ConstraintKind::Conversion, fromType, toType,
                           Identifier(), locator),
        // The source type can be downcast to the destination type.
        Constraint::create(CS, ConstraintKind::CheckedCast, fromType, toType,
                           Identifier(), locator),
      };
      CS.addConstraint(Constraint::createDisjunction(CS, constraints, locator,
                                                     RememberChoice));

      return toType;
    }

    Type visitForcedCheckedCastExpr(ForcedCheckedCastExpr *expr) {
      llvm_unreachable("Already type checked");
    }

    Type visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *expr) {
      auto &tc = CS.getTypeChecker();

      // Validate the resulting type.
      if (tc.validateType(expr->getCastTypeLoc(), CS.DC,
                          TR_AllowUnboundGenerics))
        return nullptr;

      // Open the type we're casting to.
      auto toType = CS.openType(expr->getCastTypeLoc().getType());
      expr->getCastTypeLoc().setType(toType, /*validated=*/true);

      auto fromType = expr->getSubExpr()->getType();
      auto locator = CS.getConstraintLocator(
                       expr, ConstraintLocator::CheckedCastOperand);
      CS.addConstraint(ConstraintKind::CheckedCast, fromType, toType, locator);
      return OptionalType::get(toType);
    }

    Type visitIsaExpr(IsaExpr *expr) {
      // Validate the type.
      auto &tc = CS.getTypeChecker();
      if (tc.validateType(expr->getCastTypeLoc(), CS.DC,
                          TR_AllowUnboundGenerics))
        return nullptr;

      // Open up the type we're checking.
      auto toType = CS.openType(expr->getCastTypeLoc().getType());
      expr->getCastTypeLoc().setType(toType, /*validated=*/true);

      // Add a checked cast constraint.
      auto fromType = expr->getSubExpr()->getType();
      
      CS.addConstraint(ConstraintKind::CheckedCast, fromType, toType,
                       CS.getConstraintLocator(expr));

      // The result is Bool.
      return CS.getTypeChecker().lookupBoolType(CS.DC);
    }

    Type visitCoerceExpr(CoerceExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Type visitDiscardAssignmentExpr(DiscardAssignmentExpr *expr) {
      // '_' is only allowed in assignments, so give it an AssignDest locator.
      auto locator = CS.getConstraintLocator(expr, 
                                             ConstraintLocator::AssignDest);
      auto typeVar = CS.createTypeVariable(locator, /*options=*/0);
      return LValueType::get(typeVar);
    }
    
    Type visitAssignExpr(AssignExpr *expr) {
      // Compute the type to which the source must be converted to allow
      // assignment to the destination.
      auto destTy = CS.computeAssignDestType(expr->getDest(), expr->getLoc());
      if (!destTy)
        return Type();
      
      // The source must be convertible to the destination.
      auto assignLocator = CS.getConstraintLocator(expr,
                                               ConstraintLocator::AssignSource);
      CS.addConstraint(ConstraintKind::Conversion,
                       expr->getSrc()->getType(), destTy,
                       assignLocator);
      
      expr->setType(TupleType::getEmpty(CS.getASTContext()));
      return expr->getType();
    }
    
    Type visitUnresolvedPatternExpr(UnresolvedPatternExpr *expr) {
      // If there are UnresolvedPatterns floating around after name binding,
      // they are pattern productions in invalid positions.
      CS.TC.diagnose(expr->getLoc(), diag::pattern_in_expr,
                     expr->getSubPattern()->getKind());
      return Type();
    }

    /// Get the type T?
    ///
    ///  This is not the ideal source location, but it's only used for
    /// diagnosing ill-formed standard libraries, so it really isn't
    /// worth QoI efforts.
    Type getOptionalType(SourceLoc optLoc, Type valueTy) {
      auto optTy = CS.getTypeChecker().getOptionalType(optLoc, valueTy);
      if (!optTy || CS.getTypeChecker().requireOptionalIntrinsics(optLoc))
        return Type();

      return optTy;
    }

    Type visitBindOptionalExpr(BindOptionalExpr *expr) {
      // The operand must be coercible to T?, and we will have type T.
      auto locator = CS.getConstraintLocator(expr);

      auto objectTy = CS.createTypeVariable(locator,
                                            TVO_PrefersSubtypeBinding
                                            | TVO_CanBindToLValue);
      
      // The result is the object type of the optional subexpression.
      CS.addConstraint(ConstraintKind::OptionalObject,
                       expr->getSubExpr()->getType(), objectTy,
                       locator);
      return objectTy;
    }
    
    Type visitOptionalEvaluationExpr(OptionalEvaluationExpr *expr) {
      // The operand must be coercible to T? for some type T.  We'd
      // like this to be the smallest possible nesting level of
      // optional types, e.g. T? over T??; otherwise we don't really
      // have a preference.
      auto valueTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                           TVO_PrefersSubtypeBinding);

      Type optTy = getOptionalType(expr->getSubExpr()->getLoc(), valueTy);
      if (!optTy)
        return Type();

      CS.addConstraint(ConstraintKind::Conversion,
                       expr->getSubExpr()->getType(), optTy,
                       CS.getConstraintLocator(expr));
      return optTy;
    }

    Type visitForceValueExpr(ForceValueExpr *expr) {
      // Force-unwrap an optional of type T? to produce a T.
      auto locator = CS.getConstraintLocator(expr);

      auto objectTy = CS.createTypeVariable(locator,
                                            TVO_PrefersSubtypeBinding
                                            | TVO_CanBindToLValue);
      
      // The result is the object type of the optional subexpression.
      CS.addConstraint(ConstraintKind::OptionalObject,
                       expr->getSubExpr()->getType(), objectTy,
                       locator);
      return objectTy;
    }

    Type visitOpenExistentialExpr(OpenExistentialExpr *expr) {
      llvm_unreachable("Already type-checked");
    }
  };

  /// \brief AST walker that "sanitizes" an expression for the
  /// constraint-based type checker.
  ///
  /// This is only necessary because Sema fills in too much type information
  /// before the type-checker runs, causing redundant work.
  class SanitizeExpr : public ASTWalker {
    TypeChecker &TC;
  public:
    SanitizeExpr(TypeChecker &tc) : TC(tc) { }

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      // Don't recurse into default-value expressions.
      return { !isa<DefaultValueExpr>(expr), expr };
    }

    Expr *walkToExprPost(Expr *expr) override {
      if (auto implicit = dyn_cast<ImplicitConversionExpr>(expr)) {
        // Skip implicit conversions completely.
        return implicit->getSubExpr();
      }

      if (auto dotCall = dyn_cast<DotSyntaxCallExpr>(expr)) {
        // A DotSyntaxCallExpr is a member reference that has already been
        // type-checked down to a call; turn it back into an overloaded
        // member reference expression.
        SourceLoc memberLoc;
        if (auto member = findReferencedDecl(dotCall->getFn(), memberLoc)) {
          auto base = skipImplicitConversions(dotCall->getArg());
          auto members
            = TC.Context.AllocateCopy(ArrayRef<ValueDecl *>(&member, 1));
          return new (TC.Context) OverloadedMemberRefExpr(base,
                                   dotCall->getDotLoc(), members, memberLoc,
                                   expr->isImplicit());
        }
      }

      if (auto dotIgnored = dyn_cast<DotSyntaxBaseIgnoredExpr>(expr)) {
        // A DotSyntaxBaseIgnoredExpr is a static member reference that has
        // already been type-checked down to a call where the argument doesn't
        // actually matter; turn it back into an overloaded member reference
        // expression.
        SourceLoc memberLoc;
        if (auto member = findReferencedDecl(dotIgnored->getRHS(), memberLoc)) {
          auto base = skipImplicitConversions(dotIgnored->getLHS());
          auto members
            = TC.Context.AllocateCopy(ArrayRef<ValueDecl *>(&member, 1));
          return new (TC.Context) OverloadedMemberRefExpr(base,
                                    dotIgnored->getDotLoc(), members,
                                    memberLoc, expr->isImplicit());
        }
      }

      if (auto forced = dyn_cast<ForcedCheckedCastExpr>(expr)) {
        expr = new (TC.Context) UnresolvedCheckedCastExpr(
                                  forced->getSubExpr(),
                                  forced->getLoc(),
                                  forced->getCastTypeLoc());
        if (forced->isImplicit())
          expr->setImplicit();
        return expr;
      }
     
      return expr;
    }

    /// \brief Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }
  };

  class ConstraintWalker : public ASTWalker {
    ConstraintGenerator &CG;

  public:
    ConstraintWalker(ConstraintGenerator &CG) : CG(CG) { }

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      // For closures containing only a single expression, the body participates
      // in type checking.
      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        if (closure->hasSingleExpressionBody()) {
          // Visit the closure itself, which produces a function type.
          auto funcTy = CG.visit(expr)->castTo<FunctionType>();
          expr->setType(funcTy);
        }

        return { true, expr };
      }

      // We don't visit default value expressions; they've already been
      // type-checked.
      if (isa<DefaultValueExpr>(expr)) {
        return { false, expr };
      }

      // FIXME: This is a bit of a hack, recording the CallExpr that consumes
      // an UnresolvedDotExpr so that we can do dynamic lookups more
      // efficiently. Really we should just have the arguments be part of the
      // UnresolvedDotExpr from the start.
      if (auto call = dyn_cast<CallExpr>(expr)) {
        if (Expr *fn = call->getFn()) {
          if (auto optionalWrapper = dyn_cast<BindOptionalExpr>(fn))
            fn = optionalWrapper->getSubExpr();
          else if (auto forceWrapper = dyn_cast<ForceValueExpr>(fn))
            fn = forceWrapper->getSubExpr();

          if (auto UDE = dyn_cast<UnresolvedDotExpr>(fn))
            CG.getConstraintSystem().recordPossibleDynamicCall(UDE, call);
        }
      }
      
      return { true, expr };
    }

    /// \brief Once we've visited the children of the given expression,
    /// generate constraints from the expression.
    Expr *walkToExprPost(Expr *expr) override {
      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        if (closure->hasSingleExpressionBody()) {
          // Visit the body. It's type needs to be convertible to the function's
          // return type.
          auto resultTy = closure->getResultType();
          auto bodyTy = closure->getSingleExpressionBody()->getType();
          CG.getConstraintSystem()
            .addConstraint(ConstraintKind::Conversion, bodyTy,
                           resultTy,
                           CG.getConstraintSystem()
                             .getConstraintLocator(
                               expr,
                               ConstraintLocator::ClosureResult));
          return expr;
        }
      }

      if (auto type = CG.visit(expr)) {
        expr->setType(CG.getConstraintSystem().simplifyType(type));
        return expr;
      }

      return nullptr;
    }

    /// \brief Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }

    /// \brief Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }
  };
} // end anonymous namespace

Expr *ConstraintSystem::generateConstraints(Expr *expr) {
  // Remove implicit conversions from the expression.
  expr = expr->walk(SanitizeExpr(getTypeChecker()));

  // Walk the expression, generating constraints.
  ConstraintGenerator cg(*this);
  ConstraintWalker cw(cg);
  return expr->walk(cw);
}

Expr *ConstraintSystem::generateConstraintsShallow(Expr *expr) {
  // Sanitize the expression.
  expr = SanitizeExpr(getTypeChecker()).walkToExprPost(expr);

  // Visit the top-level expression generating constraints.
  ConstraintGenerator cg(*this);
  auto type = cg.visit(expr);
  if (!type)
    return nullptr;
  expr->setType(type);
  return expr;
}

Type ConstraintSystem::generateConstraints(Pattern *pattern,
                                           ConstraintLocatorBuilder locator) {
  ConstraintGenerator cg(*this);
  return cg.getTypeForPattern(pattern, /*forFunctionParam*/ false, locator);
}
