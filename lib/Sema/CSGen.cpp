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
      CS.addConstraint(ConstraintKind::ConformsTo, tv,
                       protocol->getDeclaredType());
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
                       interpolationProto->getDeclaredType());

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
      auto methodTy = FunctionType::get(argsTy, baseTy);
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
      // We might have an operator that couldn't be resolved earlier.
      if (expr->getRefKind() != DeclRefKind::Ordinary) {
        auto &tc = CS.getTypeChecker();
        tc.diagnose(expr->getLoc(), diag::use_nonmatching_operator, 
                    expr->getName(),
                    expr->getRefKind() == DeclRefKind::BinaryOperator ? 0 :
                    expr->getRefKind() == DeclRefKind::PrefixOperator ? 1 : 2);

        return Type();
      }

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
      CS.addValueMemberConstraint(baseMetaTy, expr->getName(), memberTy,
                                  memberLocator);

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

      return memberTy;
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
                             typeVars[i], specializations[i].getType());
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
      if (ctx.LangOpts.StrictKeywordArguments) {
        expr->setType(ParenType::get(ctx, expr->getSubExpr()->getType()));
      } else {
        expr->setType(expr->getSubExpr()->getType());
      }
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

        // For @weak variables, use Optional<T>.
        if (!forFunctionParam && var->getAttrs().isWeak()) {
          ty = CS.getTypeChecker().getOptionalType(var->getLoc(), ty);
          if (!ty) return Type();
        // For @IBOutlet variables, use T!.
        } else if (var->getAttrs().hasAttribute<IBOutletAttr>()) {
          ty = CS.getTypeChecker().getImplicitlyUnwrappedOptionalType(var->getLoc(), ty);
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

        // For a typed pattern, simply return the opened type of the pattern.
        // FIXME: Error recovery if the type is an error type?
        Type openedType = CS.openType(typedPattern->getType());

        // Somewhat crazy special cases: add a level of ImplicitlyUnwrappedOptional
        // if we don't have one and this pattern is directly bound to an
        // IBOutlet, or turn a @weak into an optional
        if (auto var = typedPattern->getSingleVar()) {
          if (var->getAttrs().isWeak()) {
            if (auto weakTy = openedType->getAs<WeakStorageType>())
              openedType = weakTy->getReferentType();
            if (!openedType->getAnyOptionalObjectType())
              openedType = OptionalType::get(openedType);
          } else if (var->getAttrs().hasAttribute<IBOutletAttr>() &&
                     !openedType->getAnyOptionalObjectType()) {
            openedType = CS.getTypeChecker()
                           .getImplicitlyUnwrappedOptionalType(var->getLoc(), openedType);
            if (!openedType) return Type();
          }
        }
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
      auto locator = CS.getConstraintLocator(expr,
                                             ConstraintLocator::AddressOf);
      // Don't track failures on the conversion constraints, so that we don't
      // spend energy trying to diagnose them. Inout conversions should be
      // rare.
      auto addrConversionLocator = CS.getConstraintLocator(expr,
                                           ConstraintLocator::InOutConversion);
      addrConversionLocator->setDiscardFailures(true);
      auto writebackConversionLocator = CS.getConstraintLocator(expr,
                                       ConstraintLocator::WritebackConversion);
      writebackConversionLocator->setDiscardFailures(true);
      
      // Create a type variable for the writeback type of writeback conversions.
      auto writebackTy
        = CS.createTypeVariable(writebackConversionLocator, /*options=*/0);
      // The solver doesn't like unbound type variables so create a useless
      // binding of writebackTy to Void for disjunction choices that don't need
      // the variable.
      auto voidWriteback = Constraint::create(CS, ConstraintKind::Bind,
                                        writebackTy,
                                        TupleType::getEmpty(CS.getASTContext()),
                                        DeclName(),
                                        writebackConversionLocator);
      // Create a type variable to represent the argument type to the writeback
      // setter, which may be a labeled tuple and thus needs to be convertible
      // from the writeback type.
      auto lvalueArgTy
        = CS.createTypeVariable(writebackConversionLocator, /*options=*/0);
      CS.addConstraint(Constraint::create(CS, ConstraintKind::Conversion,
                                          lvalue, lvalueArgTy,
                                          DeclName(),
                                          writebackConversionLocator));
      
      auto writebackArgTy
        = CS.createTypeVariable(writebackConversionLocator, /*options=*/0);
      CS.addConstraint(Constraint::create(CS, ConstraintKind::Conversion,
                                          writebackTy, writebackArgTy,
                                          DeclName(),
                                          writebackConversionLocator));
      
      auto writebackInout = InOutType::get(writebackTy);
      auto writebackInoutArg = CS.createTypeVariable(writebackConversionLocator,
                                                     /*options=*/0);
      CS.addConstraint(Constraint::create(CS, ConstraintKind::Conversion,
                                          writebackInout, writebackInoutArg,
                                          DeclName(),
                                          writebackConversionLocator));
      
      CS.addConstraint(ConstraintKind::Subtype,
                       expr->getSubExpr()->getType(), bound,
                       locator);
      
      // The result can either directly be the 'inout T' type or be the result
      // of an inout conversion.
      auto result = CS.createTypeVariable(addrConversionLocator, /*options=*/0);
      
      //
      // Form the constraints for the inout nonconversion case.
      // The result will be bound to the inout T type of the lvalue.
      auto inout = InOutType::get(lvalue);
      auto inoutArg = CS.createTypeVariable(addrConversionLocator,
                                            /*options=*/0);
      CS.addConstraint(Constraint::create(CS, ConstraintKind::Conversion,
                                          inout, inoutArg,
                                          DeclName(),
                                          writebackConversionLocator));
      
      SmallVector<Constraint*, 3> disjunctions;
      
      Constraint *inoutConstraints[] = {
        Constraint::create(CS, ConstraintKind::Bind,
                           inout, result, DeclName(),
                           addrConversionLocator),
        voidWriteback,
      };
      
      disjunctions.push_back(Constraint::createConjunction(CS, inoutConstraints,
                                                       addrConversionLocator));
      
      //
      // Form the constraints for the address conversion case.
      // The result will be of some type that has a static __inout_conversion
      // method taking the metatype and a RawPointer as a parameter:
      //
      //   static func __inout_conversion(
      //     Builtin.RawPointer,
      //     $LValue.Type
      //   ) -> $Result
      //
      auto &C = CS.getASTContext();
      auto resultMeta = MetatypeType::get(result);
      
      /// Create a member method constraint with the given argument and
      /// result types.
      auto createMethodConstraint = [&](std::initializer_list<TupleTypeElt> argTys,
                                    Type resultTy,
                                    StringRef name,
                                    ConstraintLocator *locator) -> Constraint* {
        auto argTuple = TupleType::get(llvm::makeArrayRef(argTys.begin(),
                                                          argTys.end()), C);
        auto methodTy = FunctionType::get(argTuple, resultTy);
        
        return Constraint::create(CS, ConstraintKind::ValueMember,
                                  resultMeta, methodTy, C.getIdentifier(name),
                                  locator);
      };
      
      Constraint *addrConversionConstraints[] = {
        createMethodConstraint({inoutArg}, result,
                               "__inout_conversion", addrConversionLocator),
        voidWriteback,
      };
      
      disjunctions.push_back(
                   Constraint::createConjunction(CS, addrConversionConstraints,
                                                 addrConversionLocator));
      
      //
      // Form the constraints for the writeback conversion case.
      // The result will be of some type that has the following static methods:
      //
      //   static func __writeback_conversion(inout $LValue) -> $Result
      //   static func __writeback_conversion_get($LValue) -> $Writeback
      //   static func __writeback_conversion_set($Writeback) -> $LValue
      auto getLocator = CS.getConstraintLocator(expr,
                                   ConstraintLocator::WritebackConversionGet);
      auto setLocator = CS.getConstraintLocator(expr,
                                   ConstraintLocator::WritebackConversionSet);
      getLocator->setDiscardFailures(true);
      setLocator->setDiscardFailures(true);
      
      Constraint *writebackConversionRequirements[] = {
        createMethodConstraint({writebackInoutArg}, result,
                               "__writeback_conversion",
                               writebackConversionLocator),
        createMethodConstraint({lvalueArgTy}, writebackTy,
                               "__writeback_conversion_get", getLocator),
        createMethodConstraint({writebackArgTy}, lvalue,
                               "__writeback_conversion_set", setLocator),
      };
      
      disjunctions.push_back(Constraint::createConjunction(CS,
                 writebackConversionRequirements, writebackConversionLocator));
      
      //
      // Build the final disjunction constraint.
      CS.addConstraint(Constraint::createDisjunction(CS, disjunctions,
                                       addrConversionLocator, RememberChoice));
      return result;
    }

    Type visitNewArrayExpr(NewArrayExpr *expr) {
      // Validate the element type.
      auto &tc = CS.getTypeChecker();
      if (tc.validateType(expr->getElementTypeLoc(), CS.DC,
                          TR_AllowUnboundGenerics))
        return nullptr;

      // Open up the element type.
      auto elementTy = CS.openType(expr->getElementTypeLoc().getType());
      auto resultTy = elementTy;
      for (unsigned i = expr->getBounds().size(); i != 1; --i) {
        // FIXME: To support multidimensional arrays, we'll need to look at
        // the expressions in here.
        auto &bound = expr->getBounds()[i-1];
        resultTy = tc.getArraySliceType(bound.Brackets.Start, resultTy);
      }

      // The outer bound must be an ArrayBound.
      auto &outerBound = expr->getBounds()[0];
      auto arrayBoundProto = tc.getProtocol(expr->getLoc(),
                                            KnownProtocolKind::ArrayBound);
      if (!arrayBoundProto)
        return nullptr;

      CS.addConstraint(ConstraintKind::ConformsTo, outerBound.Value->getType(),
                       arrayBoundProto->getDeclaredType(),
                       CS.getConstraintLocator(outerBound.Value));

      // If we have an explicit constructor, make sure we can call it.
      // Either we have an explicit constructor closure or else ElementType must
      // be default constructible.
      if (expr->hasConstructionFunction()) {
        // FIXME: Assume the index type is DefaultIntegerLiteralType for now.
        auto intProto = tc.getProtocol(
                          expr->getConstructionFunction()->getLoc(),
                          KnownProtocolKind::IntegerLiteralConvertible);
        Type intTy = tc.getDefaultType(intProto, CS.DC);
        assert(intTy && "No default integer type?");

        Expr *constructionFn = expr->getConstructionFunction();
        Type constructionTy = FunctionType::get(intTy, elementTy);

        CS.addConstraint(ConstraintKind::Conversion, constructionFn->getType(),
                         constructionTy,
                         CS.getConstraintLocator(
                           expr, ConstraintLocator::NewArrayConstructor));
      } else {
        // Otherwise, ElementType must be default constructible.
        Type defaultCtorTy = FunctionType::get(TupleType::getEmpty(tc.Context),
                                               elementTy);
        CS.addValueMemberConstraint(elementTy,
          tc.Context.Id_init,
          defaultCtorTy,
          CS.getConstraintLocator(expr, ConstraintLocator::NewArrayElement));
      }
      
      return tc.getArraySliceType(outerBound.Brackets.Start, resultTy);
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

    Type visitApplyExpr(ApplyExpr *expr) {
      // The function subexpression has some rvalue type T1 -> T2 for fresh
      // variables T1 and T2.
      auto outputTy
        = CS.createTypeVariable(
            CS.getConstraintLocator(expr, ConstraintLocator::ApplyFunction),
            /*options=*/0);

      auto funcTy = FunctionType::get(expr->getArg()->getType(), outputTy);
      
      // Check to see if the type checker has any newly created functions with
      // this name - if it does, they were created before the list of overloads
      // was created, so we'll need to add a new disjunction constraint for the
      // new set of overloads.
      if (expr->IsGlobalDelayedOperatorApply) {
        if (CS.TC.HasForcedExternalDecl &&
            CS.TC.implicitlyDefinedFunctions.size()) {
          
          // This will only occur if the new bindings were added while solving
          // the system, so disable the flag to prevent further unnecessary
          // checks.
          CS.TC.HasForcedExternalDecl = false;
          
          auto declRef = dyn_cast<OverloadedDeclRefExpr>(expr->getFn());
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
            SmallVector<Constraint *, 4> constraints;
            
            auto fnType = expr->getFn()->getType().getPointer();
            auto tyvarType = cast<TypeVariableType>(fnType);
            auto &CG = CS.getConstraintGraph();
            
            // This type variable is only currently associated with the function
            // being applied, and the only constraint attached to it should
            // be the disjunction constraint for the overload group.
            CG.gatherConstraints(tyvarType, constraints);
            
            if (constraints.size()) {
              for (auto constraint : constraints) {
                if (constraint->getKind() == ConstraintKind::Disjunction) {
                  SmallVector<Constraint *, 4> newConstraints;
                  
                  auto oldConstraints = constraint->getNestedConstraints();
                  auto csLoc = CS.getConstraintLocator(expr->getFn());
                  
                  // Only replace the disjunctive overload constraint.
                  if (oldConstraints[0]->getKind() !=
                          ConstraintKind::BindOverload) {
                    continue;
                  }
                  
                  // Copy over the existing bindings.
                  for (auto oldConstraint : oldConstraints) {
                    newConstraints.push_back(oldConstraint);
                  }
                  
                  // Now add the new bindings as overloads.
                  for (auto choice : choices) {
                    auto overload = Constraint::createBindOverload(CS,
                                                                   tyvarType,
                                                                   choice,
                                                                   csLoc);
                    newConstraints.push_back(overload);
                  }

                  // Remove the original constraint from the inactive constraint
                  // list and add the new one.
                  CS.removeInactiveConstraint(constraint);
                  CS.addConstraint(Constraint::createDisjunction(CS,
                                                                 newConstraints,
                                                                 csLoc));
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
                                          KnownProtocolKind::LogicValue);
      if (!logicValue)
        return Type();

      CS.addConstraint(ConstraintKind::ConformsTo, condExpr->getType(),
                       logicValue->getDeclaredType(),
                       CS.getConstraintLocator(expr));

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
    
    Type visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *expr) {
      auto &tc = CS.getTypeChecker();
      
      // Validate the resulting type.
      if (tc.validateType(expr->getCastTypeLoc(), CS.DC,
                          TR_AllowUnboundGenerics))
        return nullptr;

      // Open the type we're casting to.
      auto toType = CS.openType(expr->getCastTypeLoc().getType());
      expr->getCastTypeLoc().setType(toType, /*validated=*/true);

      // Create a type variable to describe the result.
      auto locator = CS.getConstraintLocator(expr);
      auto typeVar = CS.createTypeVariable(locator, /*options=*/0);

      // Form the constraints for the implicit conversion case.
      auto fromType = expr->getSubExpr()->getType();
      Constraint *convConstraints[2] = {
        Constraint::create(CS, ConstraintKind::Conversion, fromType, toType,
                           Identifier(), locator),
        Constraint::create(CS, ConstraintKind::Equal, typeVar, toType,
                           Identifier(), locator)
      };
      auto convConstraint = Constraint::createConjunction(CS, convConstraints,
                                                          locator);

      // Form the constraints for the checked cast case.
      auto optToType = tc.getOptionalType(expr->getLoc(), toType);
      if (!optToType)
        return nullptr;
      Constraint *checkConstraints[2] = {
        Constraint::create(CS, ConstraintKind::CheckedCast, fromType, toType,
                           Identifier(), locator),
        Constraint::create(CS, ConstraintKind::Equal, typeVar, optToType,
                           Identifier(), locator)
      };
      auto checkConstraint = Constraint::createConjunction(CS,
                                                           checkConstraints,
                                                           locator);

      // Form the disjunction of the two kinds of constraints.
      Constraint *constraints[2] = { convConstraint, checkConstraint };
      CS.addConstraint(Constraint::createDisjunction(CS, constraints,
                                                     locator));

      return typeVar;
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
      return expr->getCastTypeLoc().getType();
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
      auto valueTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                            /*options*/ 0);

      Type optTy = getOptionalType(expr->getQuestionLoc(), valueTy);
      if (!optTy)
        return Type();

      CS.addConstraint(ConstraintKind::Conversion,
                       expr->getSubExpr()->getType(), optTy,
                       CS.getConstraintLocator(expr));
      return valueTy;
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
      // The value can be forced in two different ways:
      //   - Either the value is coercible to T? and the result is T, which
      //     retrieves the value stored in the optional
      //   - The value is of rvalue type AnyObject, and the result is
      //     some class type T.
      auto valueTy = CS.createTypeVariable(CS.getConstraintLocator(expr),
                                           TVO_PrefersSubtypeBinding);

      Type optTy = getOptionalType(expr->getSubExpr()->getLoc(), valueTy);
      if (!optTy)
        return Type();
      
      auto locator = CS.getConstraintLocator(expr);
      
      auto bridgeConstraint = Constraint::create(CS, ConstraintKind::BridgedToObjectiveC,
                                                 valueTy,
                                                 Type(),
                                                 Identifier(),
                                                 locator);
      Constraint *downcastConstraints[2] = {
        Constraint::create(CS, ConstraintKind::DynamicLookupValue,
                           expr->getSubExpr()->getType(),
                           Type(),
                           Identifier(),
                           locator),
        bridgeConstraint
      };
      
      Constraint *constraints[2] = {
        // The subexpression is convertible to T?
        Constraint::create(CS, ConstraintKind::Conversion,
                           expr->getSubExpr()->getType(), optTy,
                           Identifier(),
                           locator),
        // The subexpression is a AnyObject value and the resulting value
        // is of class type.
        Constraint::createConjunction(CS, downcastConstraints, locator)
      };
      CS.addConstraint(Constraint::createDisjunction(CS, constraints, locator,
                                                     RememberChoice));

      // The result is of type T.
      return valueTy;
    }

    Type visitOpenExistentialExpr(OpenExistentialExpr *expr) {
      llvm_unreachable("Already type-checked");
    }
    Type visitInOutConversionExpr(InOutConversionExpr *expr) {
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
