//===--- CSDiag.cpp - Constraint Diagnostics ------------------------------===//
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
// This file implements diagnostics for the type checker.
//
//===----------------------------------------------------------------------===//
#include "ConstraintSystem.h"
using namespace swift;
using namespace constraints;

/// Obtain the colloquial description for a known protocol kind.
static std::string getDescriptionForKnownProtocolKind(KnownProtocolKind kind) {
  switch (kind) {
#define PROTOCOL(Id) \
case KnownProtocolKind::Id: \
return #Id;
      
#define LITERAL_CONVERTIBLE_PROTOCOL(Id, Description) \
case KnownProtocolKind::Id: \
return #Description;
      
#define BUILTIN_LITERAL_CONVERTIBLE_PROTOCOL(Id) \
case KnownProtocolKind::Id: \
return #Id;
      
#include "swift/AST/KnownProtocols.def"
  }
  
  llvm_unreachable("unrecognized known protocol kind");
}


/// Obtain a "user friendly" type name. E.g., one that uses colloquial names
/// for literal convertible protocols if necessary, and is devoid of type
/// variables.
static std::string getUserFriendlyTypeName(Type t) {
  assert(!t.isNull());
  
  // Unwrap any l-value types.
  t = t->getRValueType();
  
  if (auto tv = t->getAs<TypeVariableType>()) {
    if (tv->getImpl().literalConformanceProto) {
      Optional<KnownProtocolKind> kind =
        tv->getImpl().literalConformanceProto->getKnownProtocolKind();
      
      if (kind.hasValue())
        return getDescriptionForKnownProtocolKind(kind.getValue());
    }
  }

  // Remove parens from the other level of the type.
  if (auto *PT = dyn_cast<ParenType>(t.getPointer()))
    t = PT->getUnderlyingType();
  
  return t.getString();
}



void Failure::dump(SourceManager *sm) const {
  dump(sm, llvm::errs());
}

void Failure::dump(SourceManager *sm, raw_ostream &out) const {
  out << "(";
  if (locator) {
    out << "@";
    locator->dump(sm, out);
    out << ": ";
  }

  switch (getKind()) {
  case DoesNotConformToProtocol:
    out << getFirstType().getString() << " does not conform to "
        << getSecondType().getString();
    break;

  case DoesNotHaveMember:
    out << getFirstType().getString() << " does not have a member named '"
        << getName() << "'";
    break;

  case DoesNotHaveNonMutatingMember:
    out << " immutable value of type " << getFirstType().getString()
        << " only has mutating members named '"
        << getName() << "'";
    break;

  case DoesNotHaveInitOnInstance:
    out << getFirstType().getString() << " instance does not have initializers";
    break;
    
  case FunctionTypesMismatch:
    out << "function type " << getFirstType().getString() << " is not equal to "
    << getSecondType().getString();
    break;

  case FunctionAutoclosureMismatch:
    out << "autoclosure mismatch " << getFirstType().getString() << " vs. "
        << getSecondType().getString();
    break;

  case FunctionNoReturnMismatch:
    out << "noreturn attribute mismatch " << getFirstType().getString()
    << " vs. " << getSecondType().getString();
    break;

  case FunctionNoEscapeMismatch:
    out << "noescape attribute mismatch " << getFirstType().getString()
        << " vs. " << getSecondType().getString();
    break;

  case FunctionThrowsMismatch:
    out << "function throws mismatch " << getFirstType().getString() << " vs. "
        << getSecondType().getString();
    break;

  case IsNotMetatype:
    out << getFirstType().getString() << " is not a metatype";
    break;

  case IsNotArchetype:
    out << getFirstType().getString() << " is not an archetype";
    break;

  case IsNotClass:
    out << getFirstType().getString() << " is not a class";
    break;
      
  case IsNotBridgedToObjectiveC:
    out << getFirstType().getString() << "is not bridged to Objective-C";
    break;

  case IsNotDynamicLookup:
    out << getFirstType().getString() << " is not a dynamic lookup value";
    break;
      
  case IsNotOptional:
    out << getFirstType().getString() << "is not an optional type";
    break;

  case TupleNameMismatch:
  case TupleNamePositionMismatch:
  case TupleSizeMismatch:
  case TupleVariadicMismatch:
  case TupleUnused:
    out << "mismatched tuple types " << getFirstType().getString() << " and "
        << getSecondType().getString();
    break;

  case TypesNotConstructible:
    out << getFirstType().getString() << " is not a constructible argument for "
        << getSecondType().getString();
    break;

  case TypesNotConvertible:
    out << getFirstType().getString() << " is not convertible to "
        << getSecondType().getString();
    break;

  case TypesNotSubtypes:
    out << getFirstType().getString() << " is not a subtype of "
        << getSecondType().getString();
    break;

  case TypesNotEqual:
    out << getFirstType().getString() << " is not equal to "
        << getSecondType().getString();
    break;

  case IsForbiddenLValue:
    out << "disallowed l-value binding of " << getFirstType().getString()
        << " and " << getSecondType().getString();
    break;

  case OutOfOrderArgument:
    out << "out-of-order argument " << getValue() << " should come before "
        << getSecondValue();
    break;

  case MissingArgument:
    out << "missing argument for parameter " << getValue();
    break;

  case ExtraArgument:
    out << "extra argument " << getValue();
    break;

  case NoPublicInitializers:
    out << getFirstType().getString()
        << " does not have any public initializers";
    break;

  case UnboundGenericParameter:
    out << getFirstType().getString()
        << " is an unbound generic parameter";
    break;

  case IsNotMaterializable:
    out << getFirstType().getString() << " is not materializable";
    break;
  }

  out << ")\n";
}

/// Given a subpath of an old locator, compute its summary flags.
static unsigned recomputeSummaryFlags(ConstraintLocator *oldLocator,
                                      ArrayRef<LocatorPathElt> path) {
  if (oldLocator->getSummaryFlags() != 0)
    return ConstraintLocator::getSummaryFlagsForPath(path);
  return 0;
}

ConstraintLocator *
constraints::simplifyLocator(ConstraintSystem &cs,
                             ConstraintLocator *locator,
                             SourceRange &range1,
                             SourceRange &range2,
                             ConstraintLocator **targetLocator) {
  // Clear out the target locator result.
  if (targetLocator)
    *targetLocator = nullptr;

  // The path to be tacked on to the target locator to identify the specific
  // target.
  Expr *targetAnchor;
  SmallVector<LocatorPathElt, 4> targetPath;

  auto path = locator->getPath();
  auto anchor = locator->getAnchor();
  simplifyLocator(anchor, path, targetAnchor, targetPath, range1, range2);


  // If we have a target anchor, build and simplify the target locator.
  if (targetLocator && targetAnchor) {
    SourceRange targetRange1, targetRange2;
    unsigned targetFlags = recomputeSummaryFlags(locator, targetPath);
    *targetLocator = simplifyLocator(cs,
                                     cs.getConstraintLocator(targetAnchor,
                                                             targetPath,
                                                             targetFlags),
                                     targetRange1, targetRange2);
  }

  // If we didn't simplify anything, just return the input.
  if (anchor == locator->getAnchor() &&
      path.size() == locator->getPath().size()) {
    return locator;
  }

  // Recompute the summary flags if we had any to begin with.  This is
  // necessary because we might remove e.g. tuple elements from the path.
  unsigned summaryFlags = recomputeSummaryFlags(locator, path);
  return cs.getConstraintLocator(anchor, path, summaryFlags);
}

void constraints::simplifyLocator(Expr *&anchor,
                                  ArrayRef<LocatorPathElt> &path,
                                  Expr *&targetAnchor,
                                  SmallVectorImpl<LocatorPathElt> &targetPath,
                                  SourceRange &range1, SourceRange &range2) {
  range1 = SourceRange();
  range2 = SourceRange();
  targetAnchor = nullptr;

  while (!path.empty()) {
    switch (path[0].getKind()) {
    case ConstraintLocator::ApplyArgument:
      // Extract application argument.
      if (auto applyExpr = dyn_cast<ApplyExpr>(anchor)) {
        // The target anchor is the function being called.
        targetAnchor = applyExpr->getFn();
        targetPath.push_back(path[0]);

        anchor = applyExpr->getArg();
        path = path.slice(1);
        continue;
      }

      if (auto objectLiteralExpr = dyn_cast<ObjectLiteralExpr>(anchor)) {
        targetAnchor = nullptr;
        targetPath.clear();

        anchor = objectLiteralExpr->getArg();
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::ApplyFunction:
      // Extract application function.
      if (auto applyExpr = dyn_cast<ApplyExpr>(anchor)) {
        // No additional target locator information.
        targetAnchor = nullptr;
        targetPath.clear();

        anchor = applyExpr->getFn();
        path = path.slice(1);
        continue;
      }

      // The unresolved member itself is the function.
      if (auto unresolvedMember = dyn_cast<UnresolvedMemberExpr>(anchor)) {
        if (unresolvedMember->getArgument()) {
          // No additional target locator information.
          targetAnchor = nullptr;
          targetPath.clear();

          anchor = unresolvedMember;
          path = path.slice(1);
        }
        continue;
      }

      break;

    case ConstraintLocator::Load:
    case ConstraintLocator::RvalueAdjustment:
    case ConstraintLocator::ScalarToTuple:
      // Loads, rvalue adjustment, and scalar-to-tuple conversions are implicit.
      path = path.slice(1);
      continue;

    case ConstraintLocator::NamedTupleElement:
    case ConstraintLocator::TupleElement:
      // Extract tuple element.
      if (auto tupleExpr = dyn_cast<TupleExpr>(anchor)) {
        // Append this extraction to the target locator path.
        if (targetAnchor) {
          targetPath.push_back(path[0]);
        }

        anchor = tupleExpr->getElement(path[0].getValue());
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::ApplyArgToParam:
      // Extract tuple element.
      if (auto tupleExpr = dyn_cast<TupleExpr>(anchor)) {
        // Append this extraction to the target locator path.
        if (targetAnchor) {
          targetPath.push_back(path[0]);
        }

        anchor = tupleExpr->getElement(path[0].getValue());
        path = path.slice(1);
        continue;
      }

      // Extract subexpression in parentheses.
      if (auto parenExpr = dyn_cast<ParenExpr>(anchor)) {
        assert(path[0].getValue() == 0);

        // Append this extraction to the target locator path.
        if (targetAnchor) {
          targetPath.push_back(path[0]);
        }

        anchor = parenExpr->getSubExpr();
        path = path.slice(1);
      }
      break;

    case ConstraintLocator::Member:
    case ConstraintLocator::MemberRefBase:
      if (auto dotExpr = dyn_cast<UnresolvedDotExpr>(anchor)) {
        // No additional target locator information.
        targetAnchor = nullptr;
        targetPath.clear();

        range1 = dotExpr->getNameLoc();
        anchor = dotExpr->getBase();
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::InterpolationArgument:
      if (auto interp = dyn_cast<InterpolatedStringLiteralExpr>(anchor)) {
        // No additional target locator information.
        // FIXME: Dig out the constructor we're trying to call?
        targetAnchor = nullptr;
        targetPath.clear();

        anchor = interp->getSegments()[path[0].getValue()];
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::AssignSource:
      if (auto assign = dyn_cast<AssignExpr>(anchor)) {
        targetAnchor = assign->getDest();
        targetPath.clear();

        anchor = assign->getSrc();
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::SubscriptIndex:
      if (auto subscript = dyn_cast<SubscriptExpr>(anchor)) {
        targetAnchor = subscript->getBase();
        targetPath.clear();

        anchor = subscript->getIndex();
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::CheckedCastOperand:
      if (auto castExpr = dyn_cast<ExplicitCastExpr>(anchor)) {
        targetAnchor = nullptr;
        targetPath.clear();
        anchor = castExpr->getSubExpr();
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::ClosureResult:
      if (auto CE = dyn_cast<ClosureExpr>(anchor)) {
        if (auto body = CE->getSingleExpressionBody()) {
          targetAnchor = nullptr;
          targetPath.clear();
          anchor = body;
          path = path.slice(1);
          continue;
        }
      }
      break;
        
    default:
      // FIXME: Lots of other cases to handle.
      break;
    }

    // If we get here, we couldn't simplify the path further.
    break;
  }
}

/// Simplify the given locator down to a specific anchor expression,
/// if possible.
///
/// \returns the anchor expression if it fully describes the locator, or
/// null otherwise.
static Expr *simplifyLocatorToAnchor(ConstraintSystem &cs,
                                     ConstraintLocator *locator) {
  if (!locator || !locator->getAnchor())
    return nullptr;

  SourceRange range1, range2;
  locator = simplifyLocator(cs, locator, range1, range2);
  if (!locator->getAnchor() || !locator->getPath().empty())
    return nullptr;

  return locator->getAnchor();
}

/// Retrieve the argument pattern for the given declaration.
///
static Pattern *getParameterPattern(ValueDecl *decl) {
  if (auto func = dyn_cast<FuncDecl>(decl))
    return func->getBodyParamPatterns()[0];
  if (auto constructor = dyn_cast<ConstructorDecl>(decl))
    return constructor->getBodyParamPatterns()[1];
  if (auto subscript = dyn_cast<SubscriptDecl>(decl))
    return subscript->getIndices();

  // FIXME: Variables of function type?
  return nullptr;
}

ResolvedLocator constraints::resolveLocatorToDecl(
   ConstraintSystem &cs,
   ConstraintLocator *locator,
   std::function<Optional<SelectedOverload>(ConstraintLocator *)> findOvlChoice,
   std::function<ConcreteDeclRef (ValueDecl *decl,
                                  Type openedType)> getConcreteDeclRef)
{
  assert(locator && "Null locator");
  if (!locator->getAnchor())
    return ResolvedLocator();

  ConcreteDeclRef declRef;
  auto anchor = locator->getAnchor();
  // Unwrap any specializations, constructor calls, implicit conversions, and
  // '.'s.
  // FIXME: This is brittle.
  do {
    if (auto specialize = dyn_cast<UnresolvedSpecializeExpr>(anchor)) {
      anchor = specialize->getSubExpr();
      continue;
    }

    if (auto implicit = dyn_cast<ImplicitConversionExpr>(anchor)) {
      anchor = implicit->getSubExpr();
      continue;
    }

    if (auto identity = dyn_cast<IdentityExpr>(anchor)) {
      anchor = identity->getSubExpr();
      continue;
    }

    if (auto constructor = dyn_cast<ConstructorRefCallExpr>(anchor)) {
      anchor = constructor->getFn();
      continue;
    }

    if (auto dotSyntax = dyn_cast<DotSyntaxBaseIgnoredExpr>(anchor)) {
      anchor = dotSyntax->getRHS();
      continue;
    }

    if (auto dotSyntax = dyn_cast<DotSyntaxCallExpr>(anchor)) {
      anchor = dotSyntax->getFn();
      continue;
    }

    break;
  } while (true);
  
  auto getConcreteDeclRefFromOverload
    = [&](const SelectedOverload &selected) -> ConcreteDeclRef {
      return getConcreteDeclRef(selected.choice.getDecl(),
                                selected.openedType);
    };
  
  if (auto dre = dyn_cast<DeclRefExpr>(anchor)) {
    // Simple case: direct reference to a declaration.
    declRef = dre->getDeclRef();
  } else if (auto mre = dyn_cast<MemberRefExpr>(anchor)) {
    // Simple case: direct reference to a declaration.
    declRef = mre->getMember();
  } else if (isa<OverloadedDeclRefExpr>(anchor) ||
             isa<OverloadedMemberRefExpr>(anchor) ||
             isa<UnresolvedDeclRefExpr>(anchor)) {
    // Overloaded and unresolved cases: find the resolved overload.
    auto anchorLocator = cs.getConstraintLocator(anchor);
    if (auto selected = findOvlChoice(anchorLocator)) {
      if (selected->choice.isDecl())
        declRef = getConcreteDeclRefFromOverload(*selected);
    }
  } else if (isa<UnresolvedMemberExpr>(anchor)) {
    // Unresolved member: find the resolved overload.
    auto anchorLocator = cs.getConstraintLocator(
                           anchor,
                           ConstraintLocator::UnresolvedMember);
    if (auto selected = findOvlChoice(anchorLocator)) {
      if (selected->choice.isDecl())
        declRef = getConcreteDeclRefFromOverload(*selected);
    }
  } else if (auto ctorRef = dyn_cast<OtherConstructorDeclRefExpr>(anchor)) {
    declRef = ctorRef->getDeclRef();
  }

  // If we didn't find the declaration, we're out of luck.
  if (!declRef)
    return ResolvedLocator();

  // Use the declaration and the path to produce a more specific result.
  // FIXME: This is an egregious hack. We'd be far better off
  // FIXME: Perform deeper path resolution?
  auto path = locator->getPath();
  Pattern *parameterPattern = nullptr;
  bool impliesFullPattern = false;
  while (!path.empty()) {
    switch (path[0].getKind()) {
    case ConstraintLocator::ApplyArgument:
      // If we're calling into something that has parameters, dig into the
      // actual parameter pattern.
      parameterPattern = getParameterPattern(declRef.getDecl());
      if (!parameterPattern)
        break;

      impliesFullPattern = true;
      path = path.slice(1);
      continue;

    case ConstraintLocator::TupleElement:
    case ConstraintLocator::NamedTupleElement:
      if (parameterPattern) {
        unsigned index = path[0].getValue();
        if (auto tuple = dyn_cast<TuplePattern>(
                           parameterPattern->getSemanticsProvidingPattern())) {
          parameterPattern = tuple->getElement(index).getPattern();
          impliesFullPattern = false;
          path = path.slice(1);
          continue;
        }
        parameterPattern = nullptr;
      }
      break;

    case ConstraintLocator::ApplyArgToParam:
      if (parameterPattern) {
        unsigned index = path[0].getValue2();
        if (auto tuple = dyn_cast<TuplePattern>(
                           parameterPattern->getSemanticsProvidingPattern())) {
          parameterPattern = tuple->getElement(index).getPattern();
          impliesFullPattern = false;
          path = path.slice(1);
          continue;
        }
        parameterPattern = nullptr;
      }
      break;

    case ConstraintLocator::ScalarToTuple:
      continue;

    default:
      break;
    }

    break;
  }

  // If we have a parameter pattern that refers to a parameter, grab it.
  if (parameterPattern) {
    parameterPattern = parameterPattern->getSemanticsProvidingPattern();
    if (impliesFullPattern) {
      if (auto tuple = dyn_cast<TuplePattern>(parameterPattern)) {
        if (tuple->getNumElements() == 1) {
          parameterPattern = tuple->getElement(0).getPattern();
          parameterPattern = parameterPattern->getSemanticsProvidingPattern();
        }
      }
    }

    if (auto named = dyn_cast<NamedPattern>(parameterPattern)) {
      return ResolvedLocator(ResolvedLocator::ForVar, named->getDecl());
    }
  }

  // Otherwise, do the best we can with the declaration we found.
  if (isa<FuncDecl>(declRef.getDecl()))
    return ResolvedLocator(ResolvedLocator::ForFunction, declRef);
  if (isa<ConstructorDecl>(declRef.getDecl()))
    return ResolvedLocator(ResolvedLocator::ForConstructor, declRef);

  // FIXME: Deal with the other interesting cases here, e.g.,
  // subscript declarations.
  return ResolvedLocator();
}

/// Emit a note referring to the target of a diagnostic, e.g., the function
/// or parameter being used.
static void noteTargetOfDiagnostic(ConstraintSystem &cs,
                                   const Failure &failure,
                                   ConstraintLocator *targetLocator) {
  // If there's no anchor, there's nothing we can do.
  if (!targetLocator->getAnchor())
    return;

  // Try to resolve the locator to a particular declaration.
  auto resolved
    = resolveLocatorToDecl(cs, targetLocator,
        [&](ConstraintLocator *locator) -> Optional<SelectedOverload> {
          for (auto resolved = failure.getResolvedOverloadSets();
               resolved; resolved = resolved->Previous) {
            if (resolved->Locator == locator)
              return SelectedOverload{resolved->Choice,
                                      resolved->OpenedFullType,
                                      // FIXME: opened type?
                                      Type()};
          }

          return None;
        },
        [&](ValueDecl *decl,
            Type openedType) -> ConcreteDeclRef {
          return decl;
        });

  // We couldn't resolve the locator to a declaration, so we're done.
  if (!resolved)
    return;

  switch (resolved.getKind()) {
  case ResolvedLocatorKind::Unresolved:
    // Can't emit any diagnostic here.
    return;

  case ResolvedLocatorKind::Function: {
    auto name = resolved.getDecl().getDecl()->getName();
    cs.getTypeChecker().diagnose(resolved.getDecl().getDecl(),
                                 name.isOperator()? diag::note_call_to_operator
                                                  : diag::note_call_to_func,
                                 resolved.getDecl().getDecl()->getName());
    return;
  }

  case ResolvedLocatorKind::Constructor:
    // FIXME: Specialize for implicitly-generated constructors.
    cs.getTypeChecker().diagnose(resolved.getDecl().getDecl(),
                                 diag::note_call_to_initializer);
    return;

  case ResolvedLocatorKind::Parameter:
    cs.getTypeChecker().diagnose(resolved.getDecl().getDecl(),
                                 diag::note_init_parameter,
                                 resolved.getDecl().getDecl()->getName());
    return;
  }
}

/// \brief Emit a diagnostic for the given failure.
///
/// \param cs The constraint system in which the diagnostic was generated.
/// \param failure The failure to emit.
/// \param expr The expression associated with the failure.
/// \param useExprLoc If the failure lacks a location, use the one associated
/// with expr.
///
/// \returns true if the diagnostic was emitted successfully.
static bool diagnoseFailure(ConstraintSystem &cs,
                            Failure &failure,
                            Expr *expr,
                            bool useExprLoc) {
  ConstraintLocator *cloc;
  if (!failure.getLocator() || !failure.getLocator()->getAnchor()) {
    if (useExprLoc)
      cloc = cs.getConstraintLocator(expr);
    else
      return false;
  } else {
    cloc = failure.getLocator();
  }
  
  SourceRange range1, range2;

  ConstraintLocator *targetLocator;
  auto locator = simplifyLocator(cs, cloc, range1, range2,
                                 &targetLocator);
  auto &tc = cs.getTypeChecker();

  auto anchor = locator->getAnchor();
  auto loc = anchor->getLoc();
  switch (failure.getKind()) {
  case Failure::TupleSizeMismatch: {
    auto tuple1 = failure.getFirstType()->castTo<TupleType>();
    auto tuple2 = failure.getSecondType()->castTo<TupleType>();
    tc.diagnose(loc, diag::invalid_tuple_size, tuple1, tuple2,
                tuple1->getNumElements(), tuple2->getNumElements())
      .highlight(range1).highlight(range2);
    return true;
  }

  case Failure::TupleUnused:
    tc.diagnose(loc, diag::invalid_tuple_element_unused,
                failure.getFirstType(),
                failure.getSecondType())
      .highlight(range1).highlight(range2);
    return true;

  case Failure::TypesNotConvertible:
  case Failure::TypesNotEqual:
  case Failure::TypesNotSubtypes:
  case Failure::TypesNotConstructible:
  case Failure::FunctionTypesMismatch: {
    // If this is conversion failure due to a return statement with an argument
    // that cannot be coerced to the result type of the function, emit a
    // specific error.
    if (expr->isReturnExpr()) {
      if (failure.getSecondType()->isVoid()) {
        tc.diagnose(loc, diag::cannot_return_value_from_void_func)
          .highlight(range1).highlight(range2);
      } else {
        tc.diagnose(loc, diag::cannot_convert_to_return_type,
                    failure.getFirstType(), failure.getSecondType())
          .highlight(range1).highlight(range2);
      }
      
      if (targetLocator && !useExprLoc)
        noteTargetOfDiagnostic(cs, failure, targetLocator);
      return true;
    }
    
    // We can do a better job of diagnosing application argument conversion
    // failures elsewhere.
    if (isa<ApplyExpr>(expr) ||
        isa<InOutExpr>(expr) ||
        isa<AssignExpr>(expr))
      return false;

    tc.diagnose(loc, diag::invalid_relation,
                failure.getKind() - Failure::TypesNotEqual,
                getUserFriendlyTypeName(failure.getFirstType()),
                getUserFriendlyTypeName(failure.getSecondType()))
      .highlight(range1).highlight(range2);
    if (targetLocator && !useExprLoc)
      noteTargetOfDiagnostic(cs, failure, targetLocator);
    return true;
  }

  case Failure::DoesNotHaveMember:
  case Failure::DoesNotHaveNonMutatingMember: {
    if (auto moduleTy = failure.getFirstType()->getAs<ModuleType>()) {
      tc.diagnose(loc, diag::no_member_of_module,
                  moduleTy->getModule()->getName(),
                  failure.getName())
        .highlight(range1).highlight(range2);
      break;
    }

    // If the base of this property access is a function that takes an empty
    // argument list, then the most likely problem is that the user wanted to
    // call the function, e.g. in "a.b.c" where they had to write "a.b().c".
    // Produce a specific diagnostic + fixit for this situation.
    auto baseFTy = failure.getFirstType()->getAs<AnyFunctionType>();
    if (baseFTy &&baseFTy->getInput()->isEqual(tc.Context.TheEmptyTupleType)){
      SourceLoc insertLoc = locator->getAnchor()->getEndLoc();
      
      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(locator->getAnchor())) {
        tc.diagnose(loc, diag::did_not_call_method, UDE->getName())
          .fixItInsertAfter(insertLoc, "()");
        break;
      }
      
      tc.diagnose(loc, diag::did_not_call_function)
        .fixItInsertAfter(insertLoc, "()");
      break;
    }
    
    bool IsNoMember = failure.getKind() == Failure::DoesNotHaveMember;
    tc.diagnose(loc, IsNoMember ? diag::does_not_have_member :
                                  diag::does_not_have_non_mutating_member,
                failure.getFirstType(),
                failure.getName())
      .highlight(range1).highlight(range2);
    break;
  }
  case Failure::DoesNotHaveInitOnInstance: {
    // Diagnose 'super.init', which can only appear inside another initializer,
    // specially.
    auto ctorRef = dyn_cast<UnresolvedConstructorExpr>(locator->getAnchor());
    if (isa<SuperRefExpr>(ctorRef->getSubExpr())) {
      tc.diagnose(loc, diag::super_initializer_not_in_initializer);
      break;
    }
  
    // Suggest inserting '.dynamicType' to construct another object of the same
    // dynamic type.
    SourceLoc fixItLoc;
    if (ctorRef) {
      // Place the '.dynamicType' right before the init.
      fixItLoc = ctorRef->getConstructorLoc().getAdvancedLoc(-1);
    }
    
    auto diag = tc.diagnose(loc, diag::init_not_instance_member);
    if (fixItLoc.isValid())
      diag.fixItInsert(fixItLoc, ".dynamicType");
    diag.flush();
    
    break;
  }

  case Failure::DoesNotConformToProtocol:
    // FIXME: Probably want to do this within the actual solver, because at
    // this point it's too late to actually recover fully.
      
    // We can do a better job of diagnosing application argument conversion
    // failures elsewhere.
    if (isa<ApplyExpr>(expr) ||
        isa<InOutExpr>(expr) ||
        isa<AssignExpr>(expr))
      return false;
      
    tc.conformsToProtocol(failure.getFirstType(),
                          failure.getSecondType()->castTo<ProtocolType>()
                            ->getDecl(),
                          cs.DC,
                          ConformanceCheckFlags::InExpression,
                          nullptr,
                          loc);
    if (targetLocator)
      noteTargetOfDiagnostic(cs, failure, targetLocator);
    break;

  case Failure::IsNotBridgedToObjectiveC:
    tc.diagnose(loc, diag::type_not_bridged, failure.getFirstType());
    if (targetLocator)
      noteTargetOfDiagnostic(cs, failure, targetLocator);
    break;

  case Failure::IsForbiddenLValue:
    if (auto iotTy = failure.getSecondType()->getAs<InOutType>()) {
      tc.diagnose(loc, diag::reference_non_inout, iotTy->getObjectType())
        .highlight(range1).highlight(range2);
      return true;
    }
    // FIXME: diagnose other cases
    return false;

  case Failure::OutOfOrderArgument: 
    if (auto tuple = dyn_cast_or_null<TupleExpr>(anchor)) {
      unsigned firstIdx = failure.getValue();
      Identifier first = tuple->getElementName(firstIdx);
      unsigned secondIdx = failure.getSecondValue();
      Identifier second = tuple->getElementName(secondIdx);
      if (!first.empty()  && !second.empty()) {
        tc.diagnose(tuple->getElementNameLoc(firstIdx),
                    diag::argument_out_of_order, first, second)
          .highlight(tuple->getElement(firstIdx)->getSourceRange())
          .highlight(SourceRange(tuple->getElementNameLoc(secondIdx),
                                 tuple->getElement(secondIdx)->getEndLoc()));
        return true;
      }
    }
    // FIXME: Can this even happen?
    return false;

  case Failure::MissingArgument: {
    Identifier name;
    unsigned idx = failure.getValue();
    if (auto tupleTy = failure.getFirstType()->getAs<TupleType>()) {
      name = tupleTy->getElement(idx).getName();
    } else {
      // Scalar.
      assert(idx == 0);
    }

    if (name.empty())
      tc.diagnose(loc, diag::missing_argument_positional, idx+1);
    else
      tc.diagnose(loc, diag::missing_argument_named, name);
    return true;
  }
    
  case Failure::ExtraArgument: {
    if (auto tuple = dyn_cast_or_null<TupleExpr>(anchor)) {
      unsigned firstIdx = failure.getValue();
      auto name = tuple->getElementName(firstIdx);
      if (name.empty())
        tc.diagnose(loc, diag::extra_argument_positional)
          .highlight(tuple->getElement(firstIdx)->getSourceRange());
      else
        tc.diagnose(loc, diag::extra_argument_named, name)
          .highlight(tuple->getElement(firstIdx)->getSourceRange());        
      return true;
    }

    return false;
  }
      
  case Failure::IsNotOptional: {
    if (auto force = dyn_cast_or_null<ForceValueExpr>(anchor)) {
      // If there was an 'as' cast in the subexpression, note it.
      if (auto *cast = findForcedDowncast(tc.Context, force->getSubExpr())) {
        tc.diagnose(force->getLoc(), diag::forcing_explicit_downcast,
                    failure.getFirstType())
          .highlight(cast->getLoc())
          .fixItRemove(force->getLoc());
        return true;
      }
      
      tc.diagnose(loc, diag::forcing_injected_optional,
                  failure.getFirstType())
        .highlight(force->getSourceRange())
        .fixItRemove(force->getExclaimLoc());
      
      return true;
    }
    
    if (auto bind = dyn_cast_or_null<BindOptionalExpr>(anchor)) {
      tc.diagnose(loc, diag::binding_injected_optional,
                  failure.getFirstType())
        .highlight(bind->getSourceRange())
        .fixItRemove(bind->getQuestionLoc());
      
      return true;      
    }
    return false;
  }

  case Failure::NoPublicInitializers: {
    tc.diagnose(loc, diag::no_accessible_initializers, failure.getFirstType())
      .highlight(range1);
    if (targetLocator && !useExprLoc)
      noteTargetOfDiagnostic(cs, failure, targetLocator);
    break;
  }

  case Failure::UnboundGenericParameter: {
    tc.diagnose(loc, diag::unbound_generic_parameter, failure.getFirstType())
      .highlight(range1);
    if (!useExprLoc)
      noteTargetOfDiagnostic(cs, failure, locator);
    break;
  }

  case Failure::IsNotMaterializable: {
    tc.diagnose(loc, diag::cannot_bind_generic_parameter_to_type,
                failure.getFirstType())
      .highlight(range1);
    if (!useExprLoc)
      noteTargetOfDiagnostic(cs, failure, locator);
    break;
  }

  case Failure::FunctionNoEscapeMismatch: {
    tc.diagnose(loc, diag::noescape_functiontype_mismatch,
                failure.getSecondType()).highlight(range2);
    if (!useExprLoc)
      noteTargetOfDiagnostic(cs, failure, locator);
    break;
  }

  case Failure::FunctionThrowsMismatch: {
    tc.diagnose(loc, diag::throws_functiontype_mismatch,
                failure.getFirstType()->getAs<AnyFunctionType>()->throws(),
                failure.getFirstType(),
                failure.getSecondType()->getAs<AnyFunctionType>()->throws(),
                failure.getSecondType())
      .highlight(range2);
    if (!useExprLoc)
      noteTargetOfDiagnostic(cs, failure, locator);
    break;
  }

  case Failure::FunctionAutoclosureMismatch:
  case Failure::FunctionNoReturnMismatch:
  case Failure::IsNotArchetype:
  case Failure::IsNotClass:
  case Failure::IsNotDynamicLookup:
  case Failure::IsNotMetatype:
  case Failure::TupleNameMismatch:
  case Failure::TupleNamePositionMismatch:
  case Failure::TupleVariadicMismatch:
    // FIXME: Handle all failure kinds
    return false;
  }

  return true;
}

/// \brief Determine the number of distinct overload choices in the
/// provided set.
static unsigned countDistinctOverloads(ArrayRef<OverloadChoice> choices) {
  llvm::SmallPtrSet<void *, 4> uniqueChoices;
  unsigned result = 0;
  for (auto choice : choices) {
    if (uniqueChoices.insert(choice.getOpaqueChoiceSimple()).second)
      ++result;
  }
  return result;
}

/// \brief Determine the name of the overload in a set of overload choices.
static Identifier getOverloadChoiceName(ArrayRef<OverloadChoice> choices) {
  for (auto choice : choices) {
    if (choice.isDecl())
      return choice.getDecl()->getName();
  }

  return Identifier();
}

static bool diagnoseAmbiguity(ConstraintSystem &cs,
                              ArrayRef<Solution> solutions) {
  // Produce a diff of the solutions.
  SolutionDiff diff(solutions);

  // Find the locators which have the largest numbers of distinct overloads.
  SmallVector<unsigned, 2> mostDistinctOverloads;
  unsigned maxDistinctOverloads = 0;
  for (unsigned i = 0, n = diff.overloads.size(); i != n; ++i) {
    auto &overload = diff.overloads[i];

    // If we can't resolve the locator to an anchor expression with no path,
    // we can't diagnose this well.
    if (!simplifyLocatorToAnchor(cs, overload.locator))
      continue;

    // If we don't have a name to hang on to, it'll be hard to diagnose this
    // overload.
    if (getOverloadChoiceName(overload.choices).empty())
      continue;

    unsigned distinctOverloads = countDistinctOverloads(overload.choices);

    // We need at least two overloads to make this interesting.
    if (distinctOverloads < 2)
      continue;

    // If we have more distinct overload choices for this locator than for
    // prior locators, just keep this locator.
    if (distinctOverloads > maxDistinctOverloads) {
      maxDistinctOverloads = distinctOverloads;
      mostDistinctOverloads.clear();
      mostDistinctOverloads.push_back(i);
      continue;
    }

    // If we have as many distinct overload choices for this locator as
    // the best so far, add this locator to the set.
    if (distinctOverloads == maxDistinctOverloads) {
      mostDistinctOverloads.push_back(i);
      continue;
    }

    // We have better results. Ignore this one.
  }

  // FIXME: Should be able to pick the best locator, e.g., based on some
  // depth-first numbering of expressions.
  if (mostDistinctOverloads.size() == 1) {
    auto &overload = diff.overloads[mostDistinctOverloads[0]];
    auto name = getOverloadChoiceName(overload.choices);
    auto anchor = simplifyLocatorToAnchor(cs, overload.locator);

    // Emit the ambiguity diagnostic.
    auto &tc = cs.getTypeChecker();
    tc.diagnose(anchor->getLoc(),
                name.isOperator() ? diag::ambiguous_operator_ref
                                  : diag::ambiguous_decl_ref,
                name);

    // Emit candidates.  Use a SmallPtrSet to make sure only emit a particular
    // candidate once.  FIXME: Why is one candidate getting into the overload
    // set multiple times?
    SmallPtrSet<Decl*, 8> EmittedDecls;
    for (auto choice : overload.choices) {
      switch (choice.getKind()) {
      case OverloadChoiceKind::Decl:
      case OverloadChoiceKind::DeclViaDynamic:
      case OverloadChoiceKind::TypeDecl:
      case OverloadChoiceKind::DeclViaBridge:
      case OverloadChoiceKind::DeclViaUnwrappedOptional:
        // FIXME: show deduced types, etc, etc.
        if (EmittedDecls.insert(choice.getDecl()).second)
          tc.diagnose(choice.getDecl(), diag::found_candidate);
        break;

      case OverloadChoiceKind::BaseType:
      case OverloadChoiceKind::TupleIndex:
        // FIXME: Actually diagnose something here.
        break;
      }
    }

    return true;
  }

  // FIXME: If we inferred different types for literals (for example),
  // could diagnose ambiguity that way as well.

  return false;
}

static Constraint *getConstraintChoice(Constraint *constraint,
                                       ConstraintKind kind,
                                       bool takeAny = false) {
  if (constraint->getKind() != ConstraintKind::Disjunction &&
      constraint->getKind() != ConstraintKind::Conjunction)
    return nullptr;
  
  auto nestedConstraints = constraint->getNestedConstraints();
  
  for (auto nestedConstraint : nestedConstraints) {
    if (!takeAny && nestedConstraint->getKind() != kind)
      continue;
      
    // If this is a last-chance search, and we have a conjunction or
    // disjunction, look within.
    if (takeAny &&
        ((nestedConstraint->getKind() == ConstraintKind::Disjunction) ||
         (nestedConstraint->getKind() == ConstraintKind::Conjunction))) {
          return getConstraintChoice(nestedConstraint, kind, takeAny);
        }
    
    return nestedConstraint;
  }

  return nullptr;
}

static Constraint *getComponentConstraint(Constraint *constraint) {
  if (constraint->getKind() != ConstraintKind::Disjunction)
    return constraint;
  
  return constraint->getNestedConstraints().front();
}

/// For a given expression type, extract the appropriate type for a constraint-
/// based diagnostic.
static Type getDiagnosticTypeFromExpr(Expr *expr) {
  
  // For a forced checked cast expression or coerce expression, use the type of
  // the sub-expression.
  if (auto fcc = dyn_cast<ForcedCheckedCastExpr>(expr))
    return fcc->getSubExpr()->getType();

  if (auto coerceExpr = dyn_cast<CoerceExpr>(expr))
    return coerceExpr->getSubExpr()->getType();
  
  // For an application expression, use the argument type.
  if (auto applyExpr = dyn_cast<ApplyExpr>(expr))
    return applyExpr->getArg()->getType();
  
  // For a subscript expression, use the index type.
  if (auto subscriptExpr = dyn_cast<SubscriptExpr>(expr))
    return subscriptExpr->getIndex()->getType();
  
  return expr->getType();
}

/// If a type variable was created for an opened literal expression, substitute
/// in the default literal for the type variable's literal conformance.
static Type substituteLiteralForTypeVariable(ConstraintSystem *CS,
                                             TypeVariableType *tv) {
  if (auto proto = tv->getImpl().literalConformanceProto) {
    
    auto kind = proto->getKnownProtocolKind();
    
    if (kind.hasValue()) {
      auto altLits = CS->getAlternativeLiteralTypes(kind.getValue());
      if (!altLits.empty()) {
        if (auto altType = altLits[0]) {
          return altType;
        }
      }
    }
  }
  
  return tv;
}

static std::pair<Type, Type>
getBoundTypesFromConstraint(ConstraintSystem *CS, Expr *expr,
                            Constraint *constraint) {
  auto type1 = getDiagnosticTypeFromExpr(expr);
  auto type2 = constraint->getSecondType();
  
  if (type1->isEqual(type2))
    if (auto firstType = constraint->getFirstType())
      type1 = firstType;
  
  if (auto typeVariableType =
      dyn_cast<TypeVariableType>(type2.getPointer())) {
    
    if (typeVariableType->getImpl().
          getRepresentative(nullptr) == typeVariableType) {
      SmallVector<Type, 4> bindings;
      CS->getComputedBindings(typeVariableType, bindings);
      auto binding = bindings.size() ? bindings.front() : Type();
      
      if (!binding.isNull()) {
        if (binding.getPointer() != type1.getPointer())
          type2 = binding;
      } else {
        auto impl = typeVariableType->getImpl();
        if (auto archetypeType = impl.getArchetype()) {
          type2 = archetypeType;
        } else {
          auto implAnchor = impl.getLocator()->getAnchor();
          auto anchorType = implAnchor->getType();
          
          // Don't re-substitute an opened type variable for itself.
          if (anchorType.getPointer() != type1.getPointer())
            type2 = anchorType;
        }
      }
    }
  }
  
  if (auto typeVariableType =
      dyn_cast<TypeVariableType>(type1.getPointer())) {
    SmallVector<Type, 4> bindings;
    
    CS->getComputedBindings(typeVariableType, bindings);
    
    for (auto binding : bindings) {
      if (type2.getPointer() != binding.getPointer()) {
        type1 = binding;
        break;
      }
    }
  }
  
  // If we still have a literal type variable, substitute in the default type.
  if (auto tv1 = type1->getAs<TypeVariableType>())
    type1 = substituteLiteralForTypeVariable(CS, tv1);
  
  if (auto tv2 = type2->getAs<TypeVariableType>())
    type2 = substituteLiteralForTypeVariable(CS, tv2);
  
  return std::pair<Type, Type>(type1->getLValueOrInOutObjectType(),
                               type2->getLValueOrInOutObjectType());
}

/// Determine if a type resulting from a failed typecheck operation is fully-
/// specialized, or if it still has type variable type arguments.
/// (This diverges slightly from hasTypeVariable, in that certain tyvars,
/// such as for nil literals, will be treated as specialized.)
static bool typeIsNotSpecialized(Type type) {
  if (type.isNull())
    return true;
  
  if (auto tv = type->getAs<TypeVariableType>()) {
    // If it's a nil-literal conformance, there's no reason to re-specialize.
    if (tv->getImpl().literalConformanceProto) {
      auto knownProtoKind =
        tv->getImpl().literalConformanceProto->getKnownProtocolKind();
        
      if (knownProtoKind.hasValue() &&
          (knownProtoKind.getValue() ==
           KnownProtocolKind::NilLiteralConvertible)) {
        return false;
      }
    }
    
    return true;
  }
  
  // Desugar, if necessary.
  if (auto sugaredTy = type->getAs<SyntaxSugarType>())
    type = sugaredTy->getBaseType();
  
  // If it's generic, check the type arguments.
  if (auto bgt = type->getAs<BoundGenericType>()) {
    for (auto tyarg : bgt->getGenericArgs()) {
      if (typeIsNotSpecialized(tyarg))
        return true;
    }
    
    return false;
  }
  
  // If it's a tuple, check the members.
  if (auto tupleTy = type->getAs<TupleType>()) {
    for (auto elementTy : tupleTy->getElementTypes()) {
      if (typeIsNotSpecialized((elementTy)))
        return true;
    }
    
    return false;
  }
  
  // If it's an inout type, check the inner type.
  if (auto inoutTy = type->getAs<InOutType>()) {
    return typeIsNotSpecialized(inoutTy->getObjectType());
  }
  
  // If it's a function, check the parameter and return types.
  if (auto functionTy = type->getAs<AnyFunctionType>()) {
    if (typeIsNotSpecialized(functionTy->getResult()) ||
        typeIsNotSpecialized(functionTy->getInput()))
      return true;
    
    return false;
  }
  
  // Otherwise, broadly check for type variables.
  return type->hasTypeVariable();
}

/// Determine if the type is an error type, or its metatype.
static bool isErrorTypeKind(Type t) {
  if (auto mt = t->getAs<MetatypeType>())
    t = mt->getInstanceType();
  
  return t->is<ErrorType>();
}

/// Conveniently unwrap a paren expression, if necessary.
static Expr *unwrapParenExpr(Expr *e) {
  if (auto parenExpr = dyn_cast<ParenExpr>(e))
    return unwrapParenExpr(parenExpr->getSubExpr());
  
  return e;
}

static SmallVector<Type, 4> decomposeArgumentType(Type ty) {
  SmallVector<Type, 4> result;

  // Assemble the parameter type list.
  if (auto parenType = dyn_cast<ParenType>(ty.getPointer())) {
    result.push_back(parenType->getUnderlyingType());
  } else if (auto tupleType = ty->getAs<TupleType>()) {
    for (auto field : tupleType->getElements())
      result.push_back(field.getType());
  } else {
    result.push_back(ty);
  }
  return result;
}

static std::string getTypeListString(Type type) {
  // Assemble the parameter type list.
  auto tupleType = type->getAs<TupleType>();
  if (!tupleType) {
    if (auto PT = dyn_cast<ParenType>(type.getPointer()))
      type = PT->getUnderlyingType();

    std::string result = "(";
    result += getUserFriendlyTypeName(type);
    result += ')';
    return result;
  }

  std::string result = "(";
  for (auto field : tupleType->getElements()) {
    if (result.size() != 1)
      result += ", ";
    if (!field.getName().empty()) {
      result += field.getName().str();
      result += ": ";
    }

    if (!field.isVararg())
      result += getUserFriendlyTypeName(field.getType());
    else {
      result += getUserFriendlyTypeName(field.getVarargBaseTy());
      result += "...";
    }
  }
  result += ")";
  return result;
}


/// If an UnresolvedDotExpr has been resolved by the constraint system, return
/// the decl that it references.
static ValueDecl *findResolvedMemberRef(ConstraintLocator *locator,
                                        ConstraintSystem &CS) {
  auto *resolvedOverloadSets = CS.getResolvedOverloadSets();
  if (!resolvedOverloadSets) return nullptr;

  // Search through the resolvedOverloadSets to see if we have a resolution for
  // this member.  This is an O(n) search, but only happens when producing an
  // error diagnostic.
  for (auto resolved = resolvedOverloadSets;
       resolved; resolved = resolved->Previous) {
    if (resolved->Locator != locator) continue;

    // We only handle the simplest decl binding.
    if (resolved->Choice.getKind() != OverloadChoiceKind::Decl)
      return nullptr;
    return resolved->Choice.getDecl();
  }

  return nullptr;
}


/// Given an expression that has a non-lvalue type, dig into it until we find
/// the part of the expression that prevents the entire subexpression from being
/// mutable.  For example, in a sequence like "x.v.v = 42" we want to complain
/// about "x" being a let property if "v.v" are both mutable.
///
/// This returns the base subexpression that looks immutable (or that can't be
/// analyzed any further) along with a decl extracted from it if we could.
///
static std::pair<Expr*, ValueDecl*>
resolveImmutableBase(Expr *expr, ConstraintSystem &CS) {
  expr = expr->getSemanticsProvidingExpr();

  // Provide specific diagnostics for assignment to subscripts whose base expr
  // is known to be an rvalue.
  if (auto *SE = dyn_cast<SubscriptExpr>(expr)) {
    // If we found a decl for the subscript, check to see if it is a set-only
    // subscript decl.
    auto loc = CS.getConstraintLocator(SE, ConstraintLocator::SubscriptMember);
    auto *member =
      dyn_cast_or_null<SubscriptDecl>(findResolvedMemberRef(loc, CS));

    // If it isn't settable, return it.
    if (member) {
      if (!member->isSettable() ||
          !member->isSetterAccessibleFrom(CS.DC))
        return { expr, member };
    }

    // If it is settable, then the base must be the problem, recurse.
    return resolveImmutableBase(SE->getBase(), CS);
  }

  // Look through property references.
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
    // If we found a decl for the UDE, check it.
    auto loc = CS.getConstraintLocator(UDE, ConstraintLocator::Member);
    auto *member = dyn_cast_or_null<VarDecl>(findResolvedMemberRef(loc, CS));

    // If the member isn't settable, then it is the problem: return it.
    if (member) {
      if (!member->isSettable(nullptr) ||
          !member->isSetterAccessibleFrom(CS.DC))
        return { expr, member };
    }

    // If we weren't able to resolve a member or if it is mutable, then the
    // problem must be with the base, recurse.
    return resolveImmutableBase(UDE->getBase(), CS);
  }

  if (auto *MRE = dyn_cast<MemberRefExpr>(expr)) {
    // If the member isn't settable, then it is the problem: return it.
    if (auto member = dyn_cast<AbstractStorageDecl>(MRE->getMember().getDecl()))
      if (!member->isSettable(nullptr) ||
          !member->isSetterAccessibleFrom(CS.DC))
        return { expr, member };

    // If we weren't able to resolve a member or if it is mutable, then the
    // problem must be with the base, recurse.
    return resolveImmutableBase(MRE->getBase(), CS);
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(expr))
    return { expr, DRE->getDecl() };

  // Look through x!
  if (auto *FVE = dyn_cast<ForceValueExpr>(expr))
    return resolveImmutableBase(FVE->getSubExpr(), CS);
  
  // Look through x?
  if (auto *BOE = dyn_cast<BindOptionalExpr>(expr))
    return resolveImmutableBase(BOE->getSubExpr(), CS);
  
  return { expr, nullptr };
}

static void diagnoseSubElementFailure(Expr *destExpr,
                                      SourceLoc loc,
                                      ConstraintSystem &CS,
                                      Diag<StringRef> diagID,
                                      Diag<Type> unknownDiagID) {
  auto &TC = CS.getTypeChecker();
  
  // Walk through the destination expression, resolving what the problem is.  If
  // we find a node in the lvalue path that is problematic, this returns it.
  auto immInfo = resolveImmutableBase(destExpr, CS);

  // Otherwise, we cannot resolve this because the available setter candidates
  // are all mutating and the base must be mutating.  If we dug out a
  // problematic decl, we can produce a nice tailored diagnostic.
  if (auto *VD = dyn_cast_or_null<VarDecl>(immInfo.second)) {
    std::string message = "'";
    message += VD->getName().str().str();
    message += "'";
 
    if (VD->isImplicit())
      message += " is immutable";
    else if (VD->isLet())
      message += " is a 'let' constant";
    else if (VD->hasAccessorFunctions() && !VD->getSetter())
      message += " is a get-only property";
    else if (!VD->isSetterAccessibleFrom(CS.DC))
      message += " setter is inaccessible";
    else {
      message += " is immutable";
    }
    TC.diagnose(loc, diagID, message)
      .highlight(immInfo.first->getSourceRange());
    
    // If this is a simple variable marked with a 'let', emit a note to fixit
    // hint it to 'var'.
    VD->emitLetToVarNoteIfSimple(CS.DC);
    return;
  }

  // If the underlying expression was a read-only subscript, diagnose that.
  if (auto *SD = dyn_cast_or_null<SubscriptDecl>(immInfo.second)) {
    StringRef message;
    if (!SD->getSetter())
      message = "subscript is get-only";
    else if (!SD->isSetterAccessibleFrom(CS.DC))
      message = "subscript setter is inaccessible";
    else
      message = "subscript is immutable";

    TC.diagnose(loc, diagID, message)
      .highlight(immInfo.first->getSourceRange());
    return;
  }

  // If the expression is the result of a call, it is an rvalue, not a mutable
  // lvalue.
  if (auto *AE = dyn_cast<ApplyExpr>(immInfo.first)) {
    std::string name = "call";
    if (isa<PrefixUnaryExpr>(AE) || isa<PostfixUnaryExpr>(AE))
      name = "unary operator";
    else if (isa<BinaryExpr>(AE))
      name = "binary operator";
    else if (isa<CallExpr>(AE))
      name = "function call";
    else if (isa<DotSyntaxCallExpr>(AE) || isa<DotSyntaxBaseIgnoredExpr>(AE))
      name = "method call";

    if (auto *DRE =
          dyn_cast<DeclRefExpr>(AE->getFn()->getSemanticsProvidingExpr()))
      name = std::string("'") + DRE->getDecl()->getName().str().str() + "'";

    TC.diagnose(loc, diagID, name + " returns immutable value")
      .highlight(AE->getSourceRange());
    return;
  }

  TC.diagnose(loc, unknownDiagID, destExpr->getType())
    .highlight(immInfo.first->getSourceRange());
}



namespace {
/// If a constraint system fails to converge on a solution for a given
/// expression, this class can produce a reasonable diagnostic for the failure
/// by analyzing the remnants of the failed constraint system. (Specifically,
/// left-over inactive, active and failed constraints.)
/// This class does not tune its diagnostics for a specific expression kind,
/// for that, you'll want to use an instance of the FailureDiagnosis class.
class FailureDiagnosis :public ASTVisitor<FailureDiagnosis, /*exprresult*/bool>{
  friend class ASTVisitor<FailureDiagnosis, /*exprresult*/bool>;
  
  Expr *expr = nullptr;
  ConstraintSystem *const CS;
  
  // Specific constraint kinds used, in conjunction with the expression node,
  // to determine the appropriate diagnostic.
  Constraint *conversionConstraint = nullptr;
  Constraint *overloadConstraint = nullptr;
  Constraint *fallbackConstraint = nullptr;
  Constraint *activeConformanceConstraint = nullptr;
  Constraint *valueMemberConstraint = nullptr;
  Constraint *argumentConstraint = nullptr;
  Constraint *disjunctionConversionConstraint = nullptr;
  Constraint *conformanceConstraint = nullptr;
  Constraint *bridgeToObjCConstraint = nullptr;
  
public:
  
  FailureDiagnosis(Expr *expr, ConstraintSystem *CS);
     
  /// Attempt to diagnose a failure without taking into account the specific
  /// kind of expression that could not be type checked.
  bool diagnoseGeneralFailure();
  
  /// Unless we've already done this, retypecheck the specified subexpression on
  /// its own, without including any contextual constraints or parent expr
  /// nodes.  This is more likely to succeed than type checking the original
  /// expression.
  ///
  /// This can return a new expression (for e.g. when a UnresolvedDeclRef gets
  /// resolved) and returns null when the subexpression fails to typecheck.
  Expr *typeCheckIndependentSubExpression(Expr *subExpr);

  Type getTypeOfTypeCheckedIndependentSubExpression(Expr *subExpr) {
    auto e = typeCheckIndependentSubExpression(subExpr);
    return e ? e->getType() : Type();
  }


  /// Attempt to diagnose a specific failure from the info we've collected from
  /// the failed constraint system.
  bool diagnoseFailure();

  
  /// Each match in an ApplyExpr is evaluated for how close of a match it is.
  /// The result is captured in this enum value, where the earlier entries are
  /// most specific.
  enum CandidateCloseness {
    CC_ExactMatch,             // This is a perfect match for the arguments.
    CC_NonLValueInOut,         // First argument is inout but no lvalue present.
    CC_OneArgumentMismatch,    // All arguments except one match.
    CC_ArgumentCountMismatch,  // This candidate has wrong # arguments.
    CC_GeneralMismatch         // Something else is wrong.
  };
  
private:

  /// Given a callee of the current node, attempt to determine a list of
  /// candidate functions that are being invoked.  If this returns an empty
  /// list, then nothing worked.
  SmallVector<ValueDecl*, 4>
  collectCalleeCandidateInfo(Expr *Fn, Type actualArgsType,
                             CandidateCloseness &Closeness);
  SmallVector<ValueDecl*, 4>
  collectCalleeCandidateInfo(ConstraintLocator *locator,
                             CandidateCloseness &Closeness,
    const std::function<CandidateCloseness(ValueDecl*)> &closenessPredicate);

    
  /// Attempt to produce a diagnostic for a mismatch between an expression's
  /// type and its assumed contextual type.
  bool diagnoseContextualConversionError(Type exprResultType);
  
  /// Produce a diagnostic for a general member-lookup failure (irrespective of
  /// the exact expression kind).
  bool diagnoseGeneralValueMemberFailure();
  
  /// Produce a diagnostic for a general overload resolution failure
  /// (irrespective of the exact expression kind).
  bool diagnoseGeneralOverloadFailure();
  
  /// Produce a diagnostic for a general conversion failure (irrespective of the
  /// exact expression kind).
  bool diagnoseGeneralConversionFailure();
     
  /// Given a set of parameter lists from an overload group, and a list of
  /// arguments, emit a diagnostic indicating any partially matching overloads.
  void suggestPotentialOverloads(StringRef functionName, SourceLoc loc,
                                 const SmallVectorImpl<Type> &paramLists,
                                 Type argType);
  void suggestPotentialOverloads(StringRef functionName, SourceLoc loc,
                                 ArrayRef<ValueDecl*> candidates,
                                 CandidateCloseness closeness);
  
  bool visitExpr(Expr *E);

  bool visitForceValueExpr(ForceValueExpr *FVE);

  bool visitBinaryExpr(BinaryExpr *BE);
  bool visitUnaryExpr(ApplyExpr *AE);
  bool visitPrefixUnaryExpr(PrefixUnaryExpr *PUE) {
    return visitUnaryExpr(PUE);
  }
  bool visitPostfixUnaryExpr(PostfixUnaryExpr *PUE) {
    return visitUnaryExpr(PUE);
  }
      
  bool visitSubscriptExpr(SubscriptExpr *SE);
  bool visitCallExpr(CallExpr *CE);
  bool visitAssignExpr(AssignExpr *AE);
  bool visitInOutExpr(InOutExpr *IOE);
  bool visitCoerceExpr(CoerceExpr *CE);
  bool visitForcedCheckedCastExpr(ForcedCheckedCastExpr *FCCE);
  bool visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *E);
};
} // end anonymous namespace.


FailureDiagnosis::FailureDiagnosis(Expr *expr, ConstraintSystem *cs)
  : expr(expr), CS(cs) {
  assert(expr && CS);
  
  // Collect and categorize constraint information from the failed system.
  
  if (!CS->getActiveConstraints().empty()) {
    // If any active conformance constraints are in the system, we know that
    // any inactive constraints are in its service. Capture the constraint and
    // present this information to the user.
    auto *constraint = &CS->getActiveConstraints().front();
    
    activeConformanceConstraint = getComponentConstraint(constraint);
  }
  
  for (auto & constraintRef : CS->getConstraints()) {
    auto constraint = &constraintRef;
    
    // Capture the first non-disjunction constraint we find. We'll use this
    // if we can't find a clearer reason for the failure.
    if ((!fallbackConstraint || constraint->isFavored()) &&
        (constraint->getKind() != ConstraintKind::Disjunction) &&
        (constraint->getKind() != ConstraintKind::Conjunction)) {
      fallbackConstraint = constraint;
    }
    
    // Store off conversion constraints, favoring existing conversion
    // constraints.
    if ((!(activeConformanceConstraint ||
           conformanceConstraint) || constraint->isFavored()) &&
        constraint->getKind() == ConstraintKind::ConformsTo) {
      conformanceConstraint = constraint;
    }
    
    // Failed binding constraints point to a missing member.
    if ((!valueMemberConstraint || constraint->isFavored()) &&
        ((constraint->getKind() == ConstraintKind::ValueMember) ||
         (constraint->getKind() == ConstraintKind::UnresolvedValueMember))) {
          valueMemberConstraint = constraint;
        }
    
    // A missed argument conversion can result in better error messages when
    // a user passes the wrong arguments to a function application.
    if ((!argumentConstraint || constraint->isFavored())) {
      argumentConstraint = getConstraintChoice(constraint,
                                               ConstraintKind::
                                               ArgumentTupleConversion);
    }
    
    // Overload resolution failures are often nicely descriptive, so store
    // off the first one we find.
    if ((!overloadConstraint || constraint->isFavored())) {
      overloadConstraint = getConstraintChoice(constraint,
                                               ConstraintKind::BindOverload);
    }
    
    // Conversion constraints are also nicely descriptive, so we'll grab the
    // first one of those as well.
    if ((!conversionConstraint || constraint->isFavored()) &&
        (constraint->getKind() == ConstraintKind::Conversion ||
         constraint->getKind() == ConstraintKind::ExplicitConversion ||
         constraint->getKind() == ConstraintKind::ArgumentTupleConversion)) {
          conversionConstraint = constraint;
        }
    
    // Also check for bridging failures.
    if ((!bridgeToObjCConstraint || constraint->isFavored()) &&
        constraint->getKind() == ConstraintKind::BridgedToObjectiveC) {
      bridgeToObjCConstraint = constraint;
    }
    
    // When all else fails, inspect a potential conjunction or disjunction for a
    // consituent conversion.
    if (!disjunctionConversionConstraint || constraint->isFavored()) {
      disjunctionConversionConstraint =
      getConstraintChoice(constraint, ConstraintKind::Conversion, true);
    }
  }
  
  // If no more descriptive constraint was found, use the fallback constraint.
  if (fallbackConstraint &&
      !(conversionConstraint || overloadConstraint || argumentConstraint)) {
        
        if (fallbackConstraint->getKind() == ConstraintKind::ArgumentConversion)
          argumentConstraint = fallbackConstraint;
        else
          conversionConstraint = fallbackConstraint;
      }
  
  // If there's still no conversion to diagnose, use the disjunction conversion.
  if (!conversionConstraint)
    conversionConstraint = disjunctionConversionConstraint;
  
  // If there was already a conversion failure, use it.
  if (!conversionConstraint &&
      CS->failedConstraint &&
      CS->failedConstraint->getKind() != ConstraintKind::Disjunction) {
    conversionConstraint = CS->failedConstraint;
  }
}

bool FailureDiagnosis::diagnoseGeneralValueMemberFailure() {
  
  if (!valueMemberConstraint) return false;
  
  assert(valueMemberConstraint->getKind() == ConstraintKind::ValueMember ||
         valueMemberConstraint->getKind() ==
          ConstraintKind::UnresolvedValueMember);
  
  auto memberName = valueMemberConstraint->getMember().getBaseName();
  
  // Get the referenced expression from the failed constraint.
  auto anchor = expr;
  if (auto locator = valueMemberConstraint->getLocator()) {
    anchor = simplifyLocatorToAnchor(*CS, locator);
    if (!anchor)
      anchor = locator->getAnchor();
  }

  auto type = anchor->getType();

  if (typeIsNotSpecialized(type)) {
    CS->TC.diagnose(anchor->getLoc(), diag::could_not_find_member,
                    memberName)
      .highlight(anchor->getSourceRange());
  } else {
    CS->TC.diagnose(anchor->getLoc(), diag::could_not_find_member_type,
                    getUserFriendlyTypeName(type), memberName)
      .highlight(anchor->getSourceRange());
  }
  
  return true;
}

bool FailureDiagnosis::diagnoseGeneralOverloadFailure() {
  
  // If this is a return expression with available conversion constraints,
  // we can produce a better diagnostic by pointing out the return expression
  // conversion failure.
  if (expr->isReturnExpr() && (conversionConstraint || argumentConstraint))
    if (diagnoseGeneralConversionFailure())
      return true;
  
  // In the absense of a better conversion constraint failure, point out the
  // inability to find an appropriate overload.
  if (!overloadConstraint)
    return false;
  
  
  auto overloadChoice = overloadConstraint->getOverloadChoice();
  std::string overloadName = overloadChoice.getDecl()->getNameStr();

  if (auto *CD = dyn_cast<ConstructorDecl>(overloadChoice.getDecl()))
    if (auto *SD = CD->getImplicitSelfDecl())
      overloadName = SD->getType()->getInOutObjectType().getString() + ".init";

  // Get the referenced expression from the failed constraint.
  auto anchor = expr;
  if (auto locator = overloadConstraint->getLocator()) {
    anchor = simplifyLocatorToAnchor(*CS, locator);
    if (!anchor)
      anchor = locator->getAnchor();
  }

  // The anchor for the constraint is almost always an OverloadedDeclRefExpr.
  // Look at the parent node in the AST to find the Apply to give a better
  // diagnostic.
  Expr *call = expr->getParentMap()[anchor];
  // Ignore parens around the callee.
  while (call && isa<IdentityExpr>(call))
    call = expr->getParentMap()[call];
  
  // Do some sanity checking based on the call: e.g. make sure we're invoking
  // the overloaded decl, not using it as an argument.
  Type argType;
  if (call && isa<ApplyExpr>(call) &&
      cast<ApplyExpr>(call)->getFn()->getSemanticsProvidingExpr() == anchor)
    argType = getDiagnosticTypeFromExpr(call);
  
  if (argType.isNull() || argType->is<TypeVariableType>()) {
    CS->TC.diagnose(anchor->getLoc(), diag::cannot_find_appropriate_overload,
                    overloadName)
       .highlight(anchor->getSourceRange());
    return true;
  }
  
  // Otherwise, we have a good grasp on what is going on: we have a call of an
  // unresolve overload set.  Try to dig out the candidates.
  auto apply = cast<ApplyExpr>(call);

  CandidateCloseness candidateCloseness;
  auto Candidates = collectCalleeCandidateInfo(apply->getFn(), argType,
                                               candidateCloseness);

  
  // A common error is to apply an operator that only has an inout LHS (e.g. +=)
  // to non-lvalues (e.g. a local let).  Produce a nice diagnostic for this
  // case.
  if (candidateCloseness == CC_NonLValueInOut) {
    Expr *firstArg = apply->getArg();
    if (auto *tuple = dyn_cast<TupleExpr>(firstArg))
      if (tuple->getNumElements())
        firstArg = tuple->getElement(0);
    
    diagnoseSubElementFailure(firstArg, apply->getLoc(), *CS,
                              diag::cannot_apply_lvalue_binop_to_subelement,
                              diag::cannot_apply_lvalue_binop_to_rvalue);
    return true;
  }

  
  if (argType->getAs<TupleType>()) {
    CS->TC.diagnose(apply->getFn()->getLoc(),
                    diag::cannot_find_appropriate_overload_with_type_list,
                    overloadName, getTypeListString(argType))
    .highlight(apply->getSourceRange());
  } else {
    CS->TC.diagnose(apply->getFn()->getLoc(),
                    diag::cannot_find_appropriate_overload_with_type,
                    overloadName, getTypeListString(argType))
    .highlight(apply->getSourceRange());
  }
  
  suggestPotentialOverloads(overloadName, apply->getLoc(),
                            Candidates, candidateCloseness);
  return true;
}

bool FailureDiagnosis::diagnoseGeneralConversionFailure() {
  
  // Otherwise, if we have a conversion constraint, use that as the basis for
  // the diagnostic.
  if (!conversionConstraint && !argumentConstraint)
    return false;
  
  auto constraint =
    argumentConstraint ? argumentConstraint : conversionConstraint;
  
  if (conformanceConstraint) {
    if (conformanceConstraint->getTypeVariables().size() <
        constraint->getTypeVariables().size()) {
      constraint = conformanceConstraint;
    }
  }
  
  auto anchor = expr;
  if (auto locator = constraint->getLocator()) {
    anchor = simplifyLocatorToAnchor(*CS, locator);
    if (!anchor)
      anchor = locator->getAnchor();
  }
  
  std::pair<Type, Type> types = getBoundTypesFromConstraint(CS, anchor,
                                                            constraint);
  
  if (argumentConstraint) {
    CS->TC.diagnose(anchor->getLoc(), diag::could_not_convert_argument,
                    types.first).
    highlight(anchor->getSourceRange());
    return true;
  }
    
  // If it's a type variable failing a conformance, avoid printing the type
  // variable and just print the conformance.
  if ((constraint->getKind() == ConstraintKind::ConformsTo) &&
      types.first->getAs<TypeVariableType>()) {
    CS->TC.diagnose(anchor->getLoc(),
                    diag::single_expression_conformance_failure,
                    types.first)
    .highlight(anchor->getSourceRange());
    
    return true;
  }

  Type fromType;
  if (auto sub = typeCheckIndependentSubExpression(anchor))
    fromType = sub->getType();
  else
    fromType = types.first.getPointer();

  fromType = fromType->getRValueType();
  
  auto toType = CS->getConversionType(anchor);
  if (!toType)
    toType = CS->getContextualType(anchor);
  if (!toType)
    toType = types.second.getPointer();
  
  // If the second type is a type variable, the expression itself is
  // ambiguous.
  if (fromType->is<UnboundGenericType>() || toType->is<TypeVariableType>() ||
      (fromType->is<TypeVariableType>() && toType->is<ProtocolType>())) {
    auto diagID = diag::type_of_expression_is_ambiguous;
    if (isa<ClosureExpr>(anchor))
      diagID = diag::cannot_infer_closure_type;
      
    CS->TC.diagnose(anchor->getLoc(), diagID)
      .highlight(anchor->getSourceRange());
    
    return true;
  }
  
  // Special case the diagnostic for a function result-type mismatch.
  if (anchor->isReturnExpr()) {
    if (toType->isVoid()) {
      CS->TC.diagnose(anchor->getLoc(),
                      diag::cannot_return_value_from_void_func);
      
      return true;
    }
    
    CS->TC.diagnose(anchor->getLoc(), diag::cannot_convert_to_return_type,
                    fromType, toType)
      .highlight(anchor->getSourceRange());
    
    return true;
  }
  
  CS->TC.diagnose(anchor->getLoc(), diag::invalid_relation,
                  Failure::TypesNotConvertible - Failure::TypesNotEqual,
                  getUserFriendlyTypeName(fromType),
                  getUserFriendlyTypeName(toType))
  .highlight(anchor->getSourceRange());
  
  return true;
}

bool FailureDiagnosis::diagnoseGeneralFailure() {
  
  return diagnoseGeneralValueMemberFailure() ||
         diagnoseGeneralOverloadFailure() ||
         diagnoseGeneralConversionFailure();
}


/// Unless we've already done this, retypecheck the specified subexpression on
/// its own, without including any contextual constraints or parent expr
/// nodes.  This is more likely to succeed than type checking the original
/// expression.
///
/// This can return a new expression (for e.g. when a UnresolvedDeclRef gets
/// resolved) and returns null when the subexpression fails to typecheck.
Expr *FailureDiagnosis::typeCheckIndependentSubExpression(Expr *subExpr) {
  // Track if this sub-expression is currently being diagnosed.
  if (Expr *res = CS->TC.exprIsBeingDiagnosed(subExpr))
    return res;
  
  // FIXME: expressions are never removed from this set.
  CS->TC.addExprForDiagnosis(subExpr, subExpr);
  
  if (!isa<ClosureExpr>(subExpr) &&
      (isa<ApplyExpr>(subExpr) || isa<ArrayExpr>(subExpr) ||
       isa<ForceValueExpr>(subExpr) ||
       typeIsNotSpecialized(subExpr->getType()))) {
    
    // Store off the sub-expression, in case a new one is provided via the
    // type check operation.
    Expr *preCheckedExpr = subExpr;

    CS->TC.eraseTypeData(subExpr);
        
    // Passing 'true' to the 'discardedExpr' arg preserves the lvalue type of
    // the expression.
    bool hadError = CS->TC.typeCheckExpression(subExpr, CS->DC, Type(), Type(),
                                               /*discardedExpr=*/true);

    // This is a terrible hack to get around the fact that typeCheckExpression()
    // might change subExpr to point to a new OpenExistentialExpr. In that case,
    // since the caller passed subExpr by value here, they would be left
    // holding on to an expression containing open existential types but
    // no OpenExistentialExpr, which breaks invariants enforced by the
    // ASTChecker.
    CS->TC.eraseOpenedExistentials(subExpr);
        
    // If recursive type checking failed, then an error was emitted, tell the
    // caller that we are done diagnosing things to avoid multiple
    // diagnostics.
    if (hadError) {
      preCheckedExpr->setType(ErrorType::get(CS->getASTContext()));
      return nullptr;
    }

    CS->TC.addExprForDiagnosis(preCheckedExpr, subExpr);
  }
  
  return subExpr;
}

bool FailureDiagnosis::diagnoseContextualConversionError(Type exprResultType) {
  TypeBase *contextualType = CS->getConversionType(expr);
  
  if (!contextualType) {
    contextualType = CS->getContextualType(expr);
    if (!contextualType) {
      return false;
    }
  }
  if (exprResultType->isEqual(contextualType))
    return false;
  
  if (exprResultType->getAs<TypeVariableType>())
    return false;
  
  
  // If this is conversion failure due to a return statement with an argument
  // that cannot be coerced to the result type of the function, emit a
  // specific error.
  if (expr->isReturnExpr()) {
    if (contextualType->isVoid()) {
      CS->TC.diagnose(expr->getLoc(), diag::cannot_return_value_from_void_func)
        .highlight(expr->getSourceRange());
    } else {
      CS->TC.diagnose(expr->getLoc(), diag::cannot_convert_to_return_type,
                      exprResultType, contextualType)
        .highlight(expr->getSourceRange());
    }
    return true;
  }

  
  CS->TC.diagnose(expr->getLoc(),
                  diag::invalid_relation,
                  Failure::FailureKind::TypesNotConvertible -
                      Failure::FailureKind::TypesNotEqual,
                  getUserFriendlyTypeName(exprResultType),
                  getUserFriendlyTypeName(contextualType))
  .highlight(expr->getSourceRange());
  
  return true;
}

// FIXME: Remove this!
void FailureDiagnosis::suggestPotentialOverloads(StringRef functionName,
                                                 SourceLoc loc,
                               const SmallVectorImpl<Type> &paramLists,
                                                 Type argType) {
  // FIXME: This is arbitrary.
  if (argType->isVoid())
    return;

  auto argTypeElts = decomposeArgumentType(argType);

  std::string suggestionText = "";
  std::map<std::string, bool> dupes;

  for (auto paramList : paramLists) {
    auto paramTypeElts = decomposeArgumentType(paramList);

    if (paramTypeElts.size() != argTypeElts.size())
      continue;
  /// FIXME: Right now, a "matching" overload is one with a parameter whose type
  /// is identical to one of the argument types. We can obviously do something
  /// more sophisticated with this.
    for (size_t i = 0, e = paramTypeElts.size(); i != e; i++) {
      auto pt = paramTypeElts[i];
      auto at = argTypeElts[i];
      if (pt->isEqual(at->getRValueType())) {
        auto typeListString = getTypeListString(paramList);
        if (!dupes[typeListString]) {
          dupes[typeListString] = true;
          if (!suggestionText.empty())
            suggestionText += ", ";
          suggestionText += typeListString;
        }
        break;
      }
    }
  }
  
  if (suggestionText.empty())
    return;
  
  CS->TC.diagnose(loc, diag::suggest_partial_overloads,
                  functionName, suggestionText);
}

void FailureDiagnosis::
suggestPotentialOverloads(StringRef functionName, SourceLoc loc,
                          ArrayRef<ValueDecl*> candidates,
                          CandidateCloseness closeness) {
  
  // If the candidate list is has no near matches to the actual types, don't
  // print out a candidate list, it will just be noise.
  if (closeness == CC_ArgumentCountMismatch ||
      closeness == CC_GeneralMismatch)
    return;
  
  std::string suggestionText = "";
  std::set<std::string> dupes;
  
  // FIXME2: For (T,T) & (Self, Self), emit this as two candidates, one using
  // the LHS and one using the RHS type for T's.
  
  for (auto decl : candidates) {
    Type paramListType;
    
    if (auto *FD = dyn_cast<AbstractFunctionDecl>(decl)) {
      paramListType = FD->getType();
      if (FD->getImplicitSelfDecl()) // Strip the self member.
        paramListType = paramListType->castTo<AnyFunctionType>()->getResult();
      paramListType = paramListType->castTo<AnyFunctionType>()->getInput();
    } else if (auto *SD = dyn_cast<SubscriptDecl>(decl)) {
      paramListType = SD->getIndicesType();
    }
    if (paramListType.isNull())
      continue;
    
    // If we've already seen this (e.g. decls overridden on the result type),
    // ignore this one.
    auto name = getTypeListString(paramListType);
    if (!dupes.insert(name).second)
      continue;
    
    if (!suggestionText.empty())
      suggestionText += ", ";
    suggestionText += name;
  }
  
  if (suggestionText.empty())
    return;
  
  CS->TC.diagnose(loc, diag::suggest_partial_overloads,
                  functionName, suggestionText);
}


/// When an assignment to an expression is detected and the destination is
/// invalid, emit a detailed error about the condition.
void ConstraintSystem::diagnoseAssignmentFailure(Expr *dest, Type destTy,
                                                 SourceLoc equalLoc) {
  auto &TC = getTypeChecker();

  // Diagnose obvious assignments to literals.
  if (isa<LiteralExpr>(dest->getSemanticsProvidingExpr())) {
    TC.diagnose(equalLoc, diag::cannot_assign_to_literal);
    return;
  }

  Diag<StringRef> diagID;
  if (isa<DeclRefExpr>(dest))
    diagID = diag::assignment_lhs_is_immutable_variable;
  else if (isa<ForceValueExpr>(dest))
    diagID = diag::assignment_bang_has_immutable_subcomponent;
  else if (isa<UnresolvedDotExpr>(dest) || isa<MemberRefExpr>(dest))
    diagID = diag::assignment_lhs_is_immutable_property;
  else if (isa<SubscriptExpr>(dest))
    diagID = diag::assignment_subscript_has_immutable_base;
  else {
    diagID = diag::assignment_lhs_is_immutable_variable;
  }
  
  diagnoseSubElementFailure(dest, equalLoc, *this, diagID,
                            diag::assignment_lhs_not_lvalue);
}

/// Determine how close an argument list is to an already decomposed argument
/// list.
static FailureDiagnosis::CandidateCloseness
evaluateCloseness(Type candArgListType, ArrayRef<Type> actualArgs) {
  auto candArgs = decomposeArgumentType(candArgListType);

  // FIXME: This isn't handling varargs.
  if (actualArgs.size() != candArgs.size())
    return FailureDiagnosis::CC_ArgumentCountMismatch;
  
  // Count the number of mismatched arguments.
  unsigned mismatchingArgs = 0;
  for (unsigned i = 0, e = actualArgs.size(); i != e; ++i) {
    // FIXME: Right now, a "matching" overload is one with a parameter whose
    // type is identical to one of the argument types. We can obviously do
    // something more sophisticated with this.
    if (!actualArgs[i]->getRValueType()->isEqual(candArgs[i]))
      ++mismatchingArgs;
  }
  
  // If the arguments match up exactly, then we have an exact match.  This
  // handles the no-argument cases as well.
  if (mismatchingArgs == 0)
    return FailureDiagnosis::CC_ExactMatch;
  
  // Check to see if the first argument expects an inout argument, but is not
  // an lvalue.
  if (candArgs[0]->is<InOutType>() && !actualArgs[0]->isLValueType())
    return FailureDiagnosis::CC_NonLValueInOut;
  
  if (mismatchingArgs == 1)
    return FailureDiagnosis::CC_OneArgumentMismatch;

  return FailureDiagnosis::CC_GeneralMismatch;
}


SmallVector<ValueDecl*, 4>
FailureDiagnosis::collectCalleeCandidateInfo(Expr *fn, Type actualArgsType,
                                             CandidateCloseness &Closeness) {
  SmallVector<ValueDecl*, 4> result;
  Closeness = CC_GeneralMismatch;

  if (auto declRefExpr = dyn_cast<DeclRefExpr>(fn)) {
    result.push_back(declRefExpr->getDecl());
  } else if (auto overloadedDRE = dyn_cast<OverloadedDeclRefExpr>(fn)) {
    result.append(overloadedDRE->getDecls().begin(),
                  overloadedDRE->getDecls().end());
  } else if (overloadConstraint) {
    result.push_back(overloadConstraint->getOverloadChoice().getDecl());
  } else {
    return result;
  }

  // Now that we have the candidate list, figure out what the best matches from
  // the candidate list are, and remove all the ones that aren't at that level.
  auto actualArgs = decomposeArgumentType(actualArgsType);
  SmallVector<CandidateCloseness, 4> closenessList;
  closenessList.reserve(result.size());
  for (auto decl : result) {
    // If the decl has a non-function type, it obviously doesn't match.
    auto fnType = decl->getType()->getAs<AnyFunctionType>();
    if (!fnType) {
      closenessList.push_back(FailureDiagnosis::CC_GeneralMismatch);
      continue;
    }

    closenessList.push_back(evaluateCloseness(fnType->getInput(), actualArgs));
    Closeness = std::min(Closeness, closenessList.back());
  }

  // Now that we know the minimum closeness, remove all the elements that aren't
  // as close.
  unsigned NextElt = 0;
  for (unsigned i = 0, e = result.size(); i != e; ++i) {
    // If this decl in the result list isn't a close match, ignore it.
    if (Closeness != closenessList[i])
      continue;
    
    // Otherwise, preserve it.
    result[NextElt++] = result[i];
  }
  
  result.erase(result.begin()+NextElt, result.end());
  return result;
}

bool FailureDiagnosis::visitBinaryExpr(BinaryExpr *binop) {
  auto argExpr =
    cast_or_null<TupleExpr>(typeCheckIndependentSubExpression(binop->getArg()));
  if (!argExpr) return true;

  auto argTuple = argExpr->getType()->getAs<TupleType>();

  // If the argument type is not a tuple, we've posted the diagnostic
  // recursively.
  if (!argTuple)
    return true;
  
  CandidateCloseness candidateCloseness;
  auto Candidates = collectCalleeCandidateInfo(binop->getFn(), argTuple,
                                               candidateCloseness);
  assert(!Candidates.empty() && "unrecognized binop function kind");

  if (candidateCloseness == CC_ExactMatch) {
    
    // Otherwise, whatever the result type of the call happened to be must not
    // have been what we were looking for.
    auto resultTy = getTypeOfTypeCheckedIndependentSubExpression(binop);
    if (!resultTy)
      return true;
    
    if (typeIsNotSpecialized(resultTy))
      resultTy = Candidates[0]->getType()->castTo<FunctionType>()->getResult();
    
    CS->TC.diagnose(binop->getLoc(), diag::result_type_no_match,
                    getUserFriendlyTypeName(resultTy))
      .highlight(binop->getSourceRange());
    return true;
  }
  
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);

  expr->setType(ErrorType::get(CS->getASTContext()));
  
  // A common error is to apply an operator that only has an inout LHS (e.g. +=)
  // to non-lvalues (e.g. a local let).  Produce a nice diagnostic for this
  // case.
  if (candidateCloseness == CC_NonLValueInOut) {
    diagnoseSubElementFailure(argExpr->getElement(0), binop->getLoc(), *CS,
                              diag::cannot_apply_lvalue_binop_to_subelement,
                              diag::cannot_apply_lvalue_binop_to_rvalue);
    return true;
  }
  
  auto argTyName1 = getUserFriendlyTypeName(argTuple->getElementType(0));
  auto argTyName2 = getUserFriendlyTypeName(argTuple->getElementType(1));
  std::string overloadName = Candidates[0]->getNameStr();
  assert(!overloadName.empty());
  if (argTyName1.compare(argTyName2)) {
    CS->TC.diagnose(binop->getLoc(), diag::cannot_apply_binop_to_args,
                    overloadName, argTyName1, argTyName2)
      .highlight(argExpr->getElement(0)->getSourceRange())
      .highlight(argExpr->getElement(1)->getSourceRange());
  } else {
    CS->TC.diagnose(binop->getLoc(), diag::cannot_apply_binop_to_same_args,
                    overloadName, argTyName1)
      .highlight(argExpr->getElement(0)->getSourceRange())
      .highlight(argExpr->getElement(1)->getSourceRange());
  }
  
  // FIXME: This is a hack to avoid printing candidate sets
  if (isa<OverloadedDeclRefExpr>(binop->getFn()))
    suggestPotentialOverloads(overloadName, binop->getLoc(),
                              Candidates, candidateCloseness);
  return true;
}


bool FailureDiagnosis::visitUnaryExpr(ApplyExpr *applyExpr) {
  assert(expr->getKind() == ExprKind::PostfixUnary ||
         expr->getKind() == ExprKind::PrefixUnary);
  
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);

  auto argExpr = typeCheckIndependentSubExpression(applyExpr->getArg());

  // If the argument type is an error, we've posted the diagnostic recursively.
  if (!argExpr) return true;

  auto argType = argExpr->getType();

  CandidateCloseness candidateCloseness;
  auto Candidates = collectCalleeCandidateInfo(applyExpr->getFn(), argType,
                                               candidateCloseness);
  assert(!Candidates.empty() && "unrecognized unop function kind");
  
  
  if (candidateCloseness == CC_ExactMatch) {
    // Otherwise, whatever the result type of the call happened to be must not
    // have been what we were looking for.
    auto resultTy = getTypeOfTypeCheckedIndependentSubExpression(applyExpr);
    if (!resultTy)
      return true;
    
    if (typeIsNotSpecialized(resultTy))
      resultTy = Candidates[0]->getType()->castTo<FunctionType>()->getResult();
    
    CS->TC.diagnose(applyExpr->getLoc(), diag::result_type_no_match,
                    getUserFriendlyTypeName(resultTy))
      .highlight(applyExpr->getSourceRange());
    return true;
  }
  
  expr->setType(ErrorType::get(CS->getASTContext()));

  
  
  // A common error is to apply an operator that only has inout forms (e.g. ++)
  // to non-lvalues (e.g. a local let).  Produce a nice diagnostic for this
  // case.
  if (candidateCloseness == CC_NonLValueInOut) {
    // Diagnose the case when the failure.
    diagnoseSubElementFailure(argExpr, applyExpr->getFn()->getLoc(), *CS,
                              diag::cannot_apply_lvalue_unop_to_subelement,
                              diag::cannot_apply_lvalue_unop_to_rvalue);
    return true;
  }

  auto argTyName = getUserFriendlyTypeName(argType);
  std::string overloadName = Candidates[0]->getNameStr();
  assert(!overloadName.empty());

  CS->TC.diagnose(argExpr->getLoc(), diag::cannot_apply_unop_to_arg,
                  overloadName, argTyName);
  
  // FIXME: This is a hack to avoid printing candidate sets
  if (isa<OverloadedDeclRefExpr>(applyExpr->getFn()))
    suggestPotentialOverloads(overloadName, argExpr->getLoc(),
                              Candidates, candidateCloseness);
  return true;
}

SmallVector<ValueDecl*, 4>
FailureDiagnosis::collectCalleeCandidateInfo(ConstraintLocator *locator,
                                             CandidateCloseness &Closeness,
     const std::function<CandidateCloseness(ValueDecl*)> &closenessPredicate) {
  SmallVector<ValueDecl*, 4> result;
  Closeness = CC_GeneralMismatch;
  
  if (auto decl = findResolvedMemberRef(locator, *CS)) {
    // If the decl is fully resolved, add it.
    result.push_back(decl);
  } else {
    // Otherwise, look for a disjunction between different candidates.
    for (auto &constraint : CS->getConstraints()) {
      if (constraint.getLocator() != locator) continue;
      
      // Okay, we found our constraint.  Check to see if it is a disjunction.
      if (constraint.getKind() != ConstraintKind::Disjunction) continue;
      
      for (auto *bindOverload : constraint.getNestedConstraints()) {
        auto c = bindOverload->getOverloadChoice();
        if (c.isDecl())
          result.push_back(c.getDecl());
      }
    }
  }
  
  // Now that we have the candidate list, figure out what the best matches from
  // the candidate list are, and remove all the ones that aren't at that level.
  SmallVector<CandidateCloseness, 4> closenessList;
  closenessList.reserve(result.size());
  for (auto decl : result) {
    closenessList.push_back(closenessPredicate(decl));
    Closeness = std::min(Closeness, closenessList.back());
  }
  
  // Now that we know the minimum closeness, remove all the elements that aren't
  // as close.
  unsigned NextElt = 0;
  for (unsigned i = 0, e = result.size(); i != e; ++i) {
    // If this decl in the result list isn't a close match, ignore it.
    if (Closeness != closenessList[i])
      continue;
    
    // Otherwise, preserve it.
    result[NextElt++] = result[i];
  }
  
  result.erase(result.begin()+NextElt, result.end());
  return result;
}

bool FailureDiagnosis::visitSubscriptExpr(SubscriptExpr *SE) {
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);
  
  auto indexExpr = typeCheckIndependentSubExpression(SE->getIndex());
  if (!indexExpr) return true;

  auto baseExpr = typeCheckIndependentSubExpression(SE->getBase());
  if (!baseExpr) return true;

  auto indexType = indexExpr->getType();
  auto baseType = baseExpr->getType();

  // See if the subscript got resolved.
  auto loc = CS->getConstraintLocator(SE, ConstraintLocator::SubscriptMember);

  auto decomposedIndexType = decomposeArgumentType(indexType);
  
  CandidateCloseness candidateCloseness;
  auto Candidates = collectCalleeCandidateInfo(loc, candidateCloseness,
     [&](ValueDecl *decl) -> CandidateCloseness {
    // Classify how close this match is.  Non-subscript decls don't match.
    auto *SD = dyn_cast<SubscriptDecl>(decl);
    if (!SD) return CC_GeneralMismatch;
    
    // TODO: Do we need to consider the baseType matching or not?

    // Explode out multi-index subscripts to find the best match.
    return evaluateCloseness(SD->getIndicesType(), decomposedIndexType);
  });
 
  // TODO: Is there any reason to check for CC_NonLValueInOut here?
  
  if (candidateCloseness == CC_ExactMatch) {
    // Otherwise, the return type of the subscript happened to not have been
    // what we were looking for.
    auto resultTy = getTypeOfTypeCheckedIndependentSubExpression(SE);
    if (!resultTy)
      return true;
    
    if (!typeIsNotSpecialized(resultTy)) {
      // If we got a strong type back, then we know what the subscript produced.
    } else if (Candidates.size() == 1) {
      // If we have one candidate, the result must be what that candidate
      // produced.
      resultTy = Candidates[0]->getType()->castTo<FunctionType>()->getResult();
    } else {
      CS->TC.diagnose(SE->getLoc(), diag::result_type_no_match_ambiguous)
        .highlight(SE->getSourceRange());
      suggestPotentialOverloads("subscript", SE->getLoc(),
                                Candidates, candidateCloseness);
      return true;
    }
    
    // Only one choice.
    CS->TC.diagnose(SE->getLoc(), diag::result_type_no_match,
                    getUserFriendlyTypeName(resultTy))
      .highlight(SE->getSourceRange());
    return true;
  }

  expr->setType(ErrorType::get(CS->getASTContext()));

  auto indexTypeName = getUserFriendlyTypeName(indexType);
  auto baseTypeName = getUserFriendlyTypeName(baseType);
  
  assert(!indexTypeName.empty() && !baseTypeName.empty());
  
  CS->TC.diagnose(indexExpr->getLoc(), diag::cannot_subscript_with_index,
                  baseTypeName, indexTypeName);
  suggestPotentialOverloads("subscript", SE->getLoc(),
                            Candidates, candidateCloseness);
  return true;
}

bool FailureDiagnosis::visitCallExpr(CallExpr *callExpr) {
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);
  
  auto fnExpr = callExpr->getFn();
  auto argExpr = callExpr->getArg();
  
  // An error was posted elsewhere.
  if (isErrorTypeKind(fnExpr->getType()))
    return true;


  std::string overloadName = "";

  bool isClosureInvocation = false;
  bool isInvalidTrailingClosureTarget = false;
  bool isInitializer = false;
  bool isOverloadedFn = false;
  
  llvm::SmallVector<Type, 16> paramLists;

  // Obtain the function's name, and collect any parameter lists for diffing
  // purposes.
  if (auto DRE = dyn_cast<DeclRefExpr>(fnExpr)) {
    overloadName = DRE->getDecl()->getNameStr();
    
    if (auto fnType = DRE->getDecl()->getType()->getAs<AnyFunctionType>()) {
      paramLists.push_back(fnType->getInput());
    }
    
  } else if (auto ODRE = dyn_cast<OverloadedDeclRefExpr>(fnExpr)) {
    isOverloadedFn = true;
    overloadName = ODRE->getDecls()[0]->getNameStr().str();
    
    // Collect the parameters for later use.
    for (auto D : ODRE->getDecls()) {
      if (auto fnType = D->getType()->getAs<AnyFunctionType>()) {
        paramLists.push_back(fnType->getInput());
      }
    }
    
  } else if (auto TE = dyn_cast<TypeExpr>(fnExpr)) {
    isInitializer = true;
    
    // It's always a metatype type, so use the instance type name.
    auto instanceType = TE->getType()->getAs<MetatypeType>()->getInstanceType();
    overloadName = instanceType->getString();

    // TODO: figure out right value for isKnownPrivate
    if (!instanceType->getAs<TupleType>()) {
      auto ctors = CS->TC.lookupConstructors(CS->DC, instanceType);
      for (auto ctor : ctors) {
        if (auto fnType = ctor->getType()->getAs<AnyFunctionType>()) {
          // skip type argument
          if (auto fnType2 = fnType->getResult()->getAs<AnyFunctionType>()) {
            paramLists.push_back(fnType2->getInput());
          }
        }
      }
    }
    if (paramLists.size() > 1)
      isOverloadedFn = true;

  } else if (auto UDE = dyn_cast<UnresolvedDotExpr>(fnExpr)) {
    overloadName = UDE->getName().str().str();
  } else if (isa<UnresolvedConstructorExpr>(fnExpr)) {
    overloadName = "init";
  } else {
    isClosureInvocation = true;
    
    auto unwrappedExpr = unwrapParenExpr(fnExpr);
    isInvalidTrailingClosureTarget = !isa<ClosureExpr>(unwrappedExpr);
  }
  // TODO: Handle dot_syntax_call_expr "fn" as a non-closure value.


  Type argType;

  if (auto *PE = dyn_cast<ParenExpr>(argExpr)) {
    argType = getTypeOfTypeCheckedIndependentSubExpression(PE->getSubExpr());
  } else if (auto *TE = dyn_cast<TupleExpr>(argExpr)) {
    // FIXME: This should all just be a matter of getting type type of the
    // sub-expression, but this doesn't work well when the argument list contains
    // InOutExprs.  Special case them to avoid producing poor diagnostics.
    bool containsInOutExprs = false;
    for (auto elt : TE->getElements())
      containsInOutExprs |= isa<InOutExpr>(elt);
    
    if (!containsInOutExprs) {
      argType = getTypeOfTypeCheckedIndependentSubExpression(TE);
    } else {
      // If InOutExprs are in play, get the simplified type of each element and
      // rebuild the aggregate :-(
      SmallVector<TupleTypeElt, 4> resultElts;

      for (unsigned i = 0, e = TE->getNumElements(); i != e; i++) {
        auto elType = getTypeOfTypeCheckedIndependentSubExpression(TE->getElement(i));
        if (!elType)
          return true; // already diagnosed.

        Identifier elName = TE->getElementName(i);

        resultElts.push_back({elType, elName});
      }
      
      argType = TupleType::get(resultElts, CS->getASTContext());
    }
    
  } else {
    argType =
        getTypeOfTypeCheckedIndependentSubExpression(unwrapParenExpr(argExpr));
  }
  
  if (!argType)
    return true; // already diagnosed.

  // If we have an argument list (i.e., a scalar, or a non-zero-element tuple)
  // then diagnose with some specificity about the arguments.
  if (!isa<TupleExpr>(argExpr) ||
      cast<TupleExpr>(argExpr)->getNumElements() != 0) {
    std::string argString = getTypeListString(argType);
    
    if (isOverloadedFn) {
      CS->TC.diagnose(fnExpr->getLoc(),
                      isInitializer ?
                      diag::cannot_find_appropriate_initializer_with_list :
                      diag::cannot_find_appropriate_overload_with_list,
                      overloadName,
                      argString);
    } else if (!isClosureInvocation) {
      CS->TC.diagnose(fnExpr->getLoc(),
                      isInitializer ?
                      diag::cannot_apply_initializer_to_args :
                      diag::cannot_apply_function_to_args,
                      overloadName,
                      argString);
    } else if (isInvalidTrailingClosureTarget) {
      CS->TC.diagnose(fnExpr->getLoc(),
                      diag::invalid_trailing_closure_target);
    } else {
      CS->TC.diagnose(fnExpr->getLoc(),
                      diag::cannot_invoke_closure,
                      argString);
    }
  } else {
    // Otherwise, emit diagnostics that say "no arguments".
    if (isClosureInvocation) {
      CS->TC.diagnose(fnExpr->getLoc(), diag::cannot_infer_closure_type);
      
      if (!isInvalidTrailingClosureTarget) {
        auto closureExpr = cast<ClosureExpr>(unwrapParenExpr(fnExpr));
        
        if (!closureExpr->hasSingleExpressionBody() &&
            !closureExpr->hasExplicitResultType() &&
            !closureExpr->getBody()->getElements().empty()) {
          CS->TC.diagnose(fnExpr->getLoc(),
                          diag::mult_stmt_closures_require_explicit_result);
        }
      }
      
    } else {
      CS->TC.diagnose(fnExpr->getLoc(),
                      isInitializer ?
                        diag::cannot_find_initializer_with_no_params :
                        diag::cannot_find_overload_with_no_params,
                      overloadName);
    }
  }
  
  // Did the user intend on invoking a different overload?
  if (!paramLists.empty()) {
    if (!isOverloadedFn) {
      std::string paramString = "";

      if (!paramLists[0]->isVoid()) {
        paramString = getTypeListString(paramLists[0]);
        
        CS->TC.diagnose(argExpr->getLoc(),
                        diag::expected_certain_args,
                        paramString);
      }
    } else {
      suggestPotentialOverloads(overloadName, fnExpr->getLoc(),
                                paramLists, argType);
    }
  }
  
  expr->setType(ErrorType::get(CS->getASTContext()));
  return true;
}

bool FailureDiagnosis::visitAssignExpr(AssignExpr *assignExpr) {
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);
  // Diagnose obvious assignments to literals.
  if (isa<LiteralExpr>(assignExpr->getDest()->getSemanticsProvidingExpr())) {
    CS->TC.diagnose(assignExpr->getLoc(), diag::cannot_assign_to_literal);
    return true;
  }

  // If the source type is already an error type, we've already posted an error.
  auto srcExpr = typeCheckIndependentSubExpression(assignExpr->getSrc());
  if (!srcExpr) return true;

  auto destExpr = typeCheckIndependentSubExpression(assignExpr->getDest());
  if (!destExpr) return true;

  auto destType = destExpr->getType();
  auto srcType = srcExpr->getType();

  // If the result type is a non-lvalue, then we are failing because it is
  // immutable and that's not a great thing to assign to.
  if (!destType->isLValueType()) {
    CS->diagnoseAssignmentFailure(destExpr, destType, assignExpr->getLoc());
    return true;
  }

  auto destTypeName = getUserFriendlyTypeName(destType);
  auto srcTypeName = getUserFriendlyTypeName(srcType);
  CS->TC.diagnose(srcExpr->getLoc(), diag::cannot_assign_values, srcTypeName,
                  destTypeName);
  return true;
}

bool FailureDiagnosis::visitInOutExpr(InOutExpr *IOE) {
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);
  auto subExpr = typeCheckIndependentSubExpression(IOE->getSubExpr());

  auto subExprType = subExpr->getType();

  // The common cause is that the operand is not an lvalue.
  if (!subExprType->isLValueType()) {
    diagnoseSubElementFailure(subExpr, IOE->getLoc(), *CS,
                              diag::cannot_pass_rvalue_inout_subelement,
                              diag::cannot_pass_rvalue_inout);
    return true;
  }
  
  return diagnoseGeneralFailure();
}

bool FailureDiagnosis::visitCoerceExpr(CoerceExpr *CE) {
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);

  Expr *subExpr = typeCheckIndependentSubExpression(CE->getSubExpr());
  if (!subExpr) return true;
  Type subType = subExpr->getType();

  std::pair<Type, Type> conversionTypes(nullptr, nullptr);
  if (conversionConstraint &&
      conversionConstraint->getKind() == ConstraintKind::ExplicitConversion &&
      conversionConstraint->getLocator()->getAnchor() == expr) {
    conversionTypes = getBoundTypesFromConstraint(CS, CE, conversionConstraint);
  } else {
    conversionTypes.first = subType->getLValueOrInOutObjectType();
    conversionTypes.second = CE->getType();
  }

  if (conversionTypes.first && conversionTypes.second) {
    CS->TC.diagnose(CE->getLoc(), diag::invalid_relation,
                    Failure::TypesNotConvertible - Failure::TypesNotEqual,
                    getUserFriendlyTypeName(conversionTypes.first),
                    getUserFriendlyTypeName(conversionTypes.second))
      .highlight(CE->getSourceRange());
    return true;
  }

  return diagnoseGeneralFailure();
}

bool FailureDiagnosis::
visitForcedCheckedCastExpr(ForcedCheckedCastExpr *FCE) {
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);

  Expr *subExpr = typeCheckIndependentSubExpression(FCE->getSubExpr());
  if (!subExpr) return true;

  Type subType = subExpr->getType();


  std::pair<Type, Type> conversionTypes(nullptr, nullptr);
  if (conversionConstraint &&
      conversionConstraint->getKind() == ConstraintKind::CheckedCast &&
      conversionConstraint->getLocator()->getAnchor() == expr) {
    conversionTypes = getBoundTypesFromConstraint(CS, FCE,
                                                  conversionConstraint);
  } else {
    conversionTypes.first = subType->getLValueOrInOutObjectType();
    conversionTypes.second = FCE->getType();
  }

  if (conversionTypes.first && conversionTypes.second) {
    CS->TC.diagnose(FCE->getLoc(), diag::invalid_relation,
                    Failure::TypesNotConvertible - Failure::TypesNotEqual,
                    getUserFriendlyTypeName(conversionTypes.first),
                    getUserFriendlyTypeName(conversionTypes.second))
      .highlight(FCE->getSourceRange());
    return true;
  }

  return diagnoseGeneralFailure();
}

bool FailureDiagnosis::visitForceValueExpr(ForceValueExpr *FVE) {
  CleanupIllFormedExpressionRAII cleanup(CS->getASTContext(), expr);
  
  auto argExpr = typeCheckIndependentSubExpression(FVE->getSubExpr());
  if (!argExpr) return true;
  auto argType = argExpr->getType();

  // If the subexpression type checks as a non-optional type, then that is the
  // error.  Produce a specific diagnostic about this.
  if (argType->getOptionalObjectType().isNull()) {
    CS->TC.diagnose(FVE->getLoc(), diag::invalid_force_unwrap,
                    getUserFriendlyTypeName(argType))
      .fixItRemove(FVE->getExclaimLoc())
      .highlight(FVE->getSourceRange());
    expr->setType(ErrorType::get(CS->getASTContext()));
    return true;
  }
  
  return diagnoseGeneralFailure();
}

bool FailureDiagnosis::
visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *E) {
  // Don't walk the children for this node, it leads to multiple diagnostics
  // because of how sema injects this node into the type checker.
  return diagnoseGeneralFailure();
}


bool FailureDiagnosis::visitExpr(Expr *E) {
  // Check each of our immediate children to see if any of them are
  // independently invalid.
  bool errorInSubExpr = false;
  
  E->forEachChildExpr([&](Expr *Child) {
    // If we already found an error, stop checking.
    if (errorInSubExpr) return;
    
    // Otherwise this subexpr is an error if type checking it produces an error.
    errorInSubExpr |= !getTypeOfTypeCheckedIndependentSubExpression(Child);
  });
  
  // If any of the children were errors, we're done.
  if (errorInSubExpr)
    return true;
  
  // Otherwise, produce a more generic error.
  return diagnoseGeneralFailure();
}


bool FailureDiagnosis::diagnoseFailure() {
  assert(CS && expr);
  
  // If a bridging conversion slips through, treat it as ambiguous.
  if (bridgeToObjCConstraint) {
    CS->TC.diagnose(expr->getLoc(), diag::type_of_expression_is_ambiguous);
    return true;
  }
  
  // Our general approach is to do a depth first traversal of the broken
  // expression tree, type checking as we go.  If we find a subtree that cannot
  // be type checked on its own (even to an incomplete type) then that is where
  // we focus our attention.  If we do find a type, we use it to check for
  // contextual type mismatches.
  auto subExprTy = getTypeOfTypeCheckedIndependentSubExpression(expr);
  
  // We've already diagnosed the error.
  if (!subExprTy)
    return true;
  
  // If there is a contextual type that mismatches, diagnose it as the problem.
  if (diagnoseContextualConversionError(subExprTy))
    return true;

  return visit(expr);
}

/// Given a specific expression and the remnants of the failed constraint
/// system, produce a specific diagnostic.
bool ConstraintSystem::diagnoseFailureForExpr(Expr *expr) {
  FailureDiagnosis diagnosis(expr, this);
  
  // Now, attempt to diagnose the failure from the info we've collected.
  if (diagnosis.diagnoseFailure())
    return true;
  
  // A DiscardAssignmentExpr is special in that it introduces a new type
  // variable but places no constraints upon it. Instead, it relies on the rhs
  // of its assignment expression to determine its type. Unfortunately, in the
  // case of error recovery, the "_" expression may be left alone with no
  // constraints for us to derive an error from. In that case, we'll fall back
  // to the "outside assignment" error.
  if (ActiveConstraints.empty() && InactiveConstraints.empty() &&
      !failedConstraint) {

    if (isa<DiscardAssignmentExpr>(expr)) {
      TC.diagnose(expr->getLoc(), diag::discard_expr_outside_of_assignment)
        .highlight(expr->getSourceRange());
      
      return true;
    }
    
    if (auto dot = dyn_cast<UnresolvedDotExpr>(expr)) {
      TC.diagnose(expr->getLoc(),
                  diag::not_enough_context_for_generic_method_reference,
                  dot->getName());
      
      return true;
    }
    
    // If there are no posted constraints or failures, then there was
    // not enough contextual information available to infer a type for the
    // expression.
    TC.diagnose(expr->getLoc(), diag::type_of_expression_is_ambiguous);
  
    return true;
  }
  
  return false;
}

bool ConstraintSystem::salvage(SmallVectorImpl<Solution> &viable,
                               Expr *expr,
                               bool onlyFailures) {
  // If there were any unavoidable failures, emit the first one we can.
  if (!unavoidableFailures.empty()) {
    for (auto failure : unavoidableFailures) {
      
      // In the 'onlyFailures' case, we'll want to synthesize a locator if one
      // does not exist. That allows us to emit decent diagnostics for
      // constraint application failures where the constraints themselves lack
      // a valid location.
      if (diagnoseFailure(*this, *failure, expr, onlyFailures))
        return true;
    }
    
    if (onlyFailures)
      return true;

    // If we can't make sense of the existing constraints (or none exist), go
    // ahead and try the unavoidable failures again, but with locator
    // substitutions in place.
    if (!diagnoseFailureForExpr(expr) &&
        !unavoidableFailures.empty()) {
      for (auto failure : unavoidableFailures) {
        if (diagnoseFailure(*this, *failure, expr, true))
          return true;
      }
    }
    
    return true;
  }

  // There were no unavoidable failures, so attempt to solve again, capturing
  // any failures that come from our attempts to select overloads or bind
  // type variables.
  {
    viable.clear();

    // Set up solver state.
    SolverState state(*this);
    state.recordFailures = true;
    this->solverState = &state;

    // Solve the system.
    solve(viable);

    // Check whether we have a best solution; this can happen if we found
    // a series of fixes that worked.
    if (auto best = findBestSolution(viable, /*minimize=*/true)) {
      if (*best != 0)
        viable[0] = std::move(viable[*best]);
      viable.erase(viable.begin() + 1, viable.end());
      return false;
    }

    // FIXME: If we were able to actually fix things along the way,
    // we may have to hunt for the best solution. For now, we don't care.

    // If there are multiple solutions, try to diagnose an ambiguity.
    if (viable.size() > 1) {
      if (getASTContext().LangOpts.DebugConstraintSolver) {
        auto &log = getASTContext().TypeCheckerDebug->getStream();
        log << "---Ambiguity error: "
            << viable.size() << " solutions found---\n";
        int i = 0;
        for (auto &solution : viable) {
          log << "---Ambiguous solution #" << i++ << "---\n";
          solution.dump(log);
          log << "\n";
        }
      }        

      if (diagnoseAmbiguity(*this, viable)) {
        return true;
      }
    }

    // Remove the solver state.
    this->solverState = nullptr;

    // Fall through to produce diagnostics.
  }

  if (failures.size() == 1) {
    auto &failure = unavoidableFailures.empty()? *failures.begin()
                                               : **unavoidableFailures.begin();
    
    if (diagnoseFailure(*this, failure, expr, false))
      return true;
  }
  
  if (getExpressionTooComplex()) {
    TC.diagnose(expr->getLoc(), diag::expression_too_complex).
    highlight(expr->getSourceRange());
    return true;
  }
  
  // If all else fails, attempt to diagnose the failure by looking through the
  // system's constraints.
  bool result = diagnoseFailureForExpr(expr);
  assert(result && "didn't diagnose any failure?"); (void)result;
    
  return true;
}
