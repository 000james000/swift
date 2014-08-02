//===--- TypeCheckAttr.cpp - Type Checking for Attributes -----------------===//
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
// This file implements semantic analysis for attributes.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "MiscDiagnostics.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/Parse/Lexer.h"

using namespace swift;

namespace {
/// This visits each attribute on a decl early, before the majority of type
/// checking has been performed for the decl.  The visitor should return true if
/// the attribute is invalid and should be marked as such.
class AttributeEarlyChecker : public AttributeVisitor<AttributeEarlyChecker> {
  TypeChecker &TC;
  Decl *D;

public:
  AttributeEarlyChecker(TypeChecker &TC, Decl *D) : TC(TC), D(D) {}

  /// This emits a diagnostic with a fixit to remove the attribute.
  template<typename ...ArgTypes>
  void diagnoseAndRemoveAttr(DeclAttribute *attr, ArgTypes &&...Args) {
    TC.diagnose(attr->getLocation(), std::forward<ArgTypes>(Args)...)
      .fixItRemove(attr->getRange());
    attr->setInvalid();
  }

  /// Deleting this ensures that all attributes are covered by the visitor
  /// below.
  bool visitDeclAttribute(DeclAttribute *A) = delete;

#define IGNORED_ATTR(X) void visit##X##Attr(X##Attr *) {}
  IGNORED_ATTR(Asmname)
  IGNORED_ATTR(Availability)
  IGNORED_ATTR(ClassProtocol)
  IGNORED_ATTR(Final)
  IGNORED_ATTR(IBDesignable)
  IGNORED_ATTR(NSCopying)
  IGNORED_ATTR(NoReturn)
  IGNORED_ATTR(ObjC)
  IGNORED_ATTR(Optional)
  IGNORED_ATTR(RawDocComment)
  IGNORED_ATTR(Required)
  IGNORED_ATTR(Convenience)
  IGNORED_ATTR(Semantics)
  IGNORED_ATTR(UnsafeNoObjCTaggedPointer)
  IGNORED_ATTR(Inline)
  IGNORED_ATTR(Exported)
  IGNORED_ATTR(UIApplicationMain)
  IGNORED_ATTR(Infix)
  IGNORED_ATTR(Postfix)
  IGNORED_ATTR(Prefix)
  IGNORED_ATTR(RequiresStoredPropertyInits)
#undef IGNORED_ATTR

  void visitTransparentAttr(TransparentAttr *attr);
  void visitMutationAttr(DeclAttribute *attr);
  void visitMutatingAttr(MutatingAttr *attr) { visitMutationAttr(attr); }
  void visitNonMutatingAttr(NonMutatingAttr *attr)  { visitMutationAttr(attr); }
  void visitDynamicAttr(DynamicAttr *attr);

  void visitOwnershipAttr(OwnershipAttr *attr) {
    TC.checkOwnershipAttr(cast<VarDecl>(D), attr);
  }

  void visitIBActionAttr(IBActionAttr *attr);
  void visitLazyAttr(LazyAttr *attr);
  void visitIBInspectableAttr(IBInspectableAttr *attr);
  void visitIBOutletAttr(IBOutletAttr *attr);
  void visitLLDBDebuggerFunctionAttr(LLDBDebuggerFunctionAttr *attr);
  void visitNSManagedAttr(NSManagedAttr *attr);
  void visitOverrideAttr(OverrideAttr *attr);
  void visitAccessibilityAttr(AccessibilityAttr *attr);
  void visitSetterAccessibilityAttr(SetterAccessibilityAttr *attr);
  bool visitAbstractAccessibilityAttr(AbstractAccessibilityAttr *attr);
  void visitSILStoredAttr(SILStoredAttr *attr);
};
} // end anonymous namespace


void AttributeEarlyChecker::visitTransparentAttr(TransparentAttr *attr) {
  if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    CanType ExtendedTy = DeclContext::getExtendedType(ED);
    // Only Struct and Enum extensions can be transparent.
    if (!isa<StructType>(ExtendedTy) && !isa<EnumType>(ExtendedTy))
      return diagnoseAndRemoveAttr(attr,diag::transparent_on_invalid_extension);
    return;
  }
  
  DeclContext *Ctx = D->getDeclContext();
  // Protocol declarations cannot be transparent.
  if (isa<ProtocolDecl>(Ctx))
    return diagnoseAndRemoveAttr(attr,
                                 diag::transparent_in_protocols_not_supported);
  // Class declarations cannot be transparent.
  if (isa<ClassDecl>(Ctx)) {
    
    // @transparent is always ok on implicitly generated accessors: they can
    // be dispatched (even in classes) when the references are within the
    // class themself.
    if (!(isa<FuncDecl>(D) && cast<FuncDecl>(D)->isAccessor() &&
        D->isImplicit()))
      return diagnoseAndRemoveAttr(attr,
                                   diag::transparent_in_classes_not_supported);
  }
  
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    // Stored properties and variables can't be transparent.
    if (VD->hasStorage())
      return diagnoseAndRemoveAttr(attr, diag::transparent_stored_property);
  }
}

void AttributeEarlyChecker::visitMutationAttr(DeclAttribute *attr) {
  FuncDecl *FD = cast<FuncDecl>(D);

  if (!FD->getDeclContext()->isTypeContext())
    return diagnoseAndRemoveAttr(attr, diag::mutating_invalid_global_scope);
  if (FD->getDeclContext()->getDeclaredTypeInContext()->hasReferenceSemantics())
    return diagnoseAndRemoveAttr(attr, diag::mutating_invalid_classes);
  
  // Verify we don't have both mutating and nonmutating.
  if (FD->getAttrs().hasAttribute<MutatingAttr>())
    if (auto *NMA = FD->getAttrs().getAttribute<NonMutatingAttr>()) {
      diagnoseAndRemoveAttr(NMA, diag::functions_mutating_and_not);
      if (NMA == attr) return;
    }
  
  // Verify that we don't have a static function.
  if (FD->isStatic())
    return diagnoseAndRemoveAttr(attr, diag::static_functions_not_mutating);
}

void AttributeEarlyChecker::visitDynamicAttr(DynamicAttr *attr) {
  // Only instance members of classes can be dynamic.
  auto contextTy = D->getDeclContext()->getDeclaredTypeInContext();
  if (!contextTy || !contextTy->getClassOrBoundGenericClass())
    return diagnoseAndRemoveAttr(attr, diag::dynamic_not_in_class);
    
  // Members cannot be both dynamic and final.
  if (D->getAttrs().hasAttribute<FinalAttr>())
    return diagnoseAndRemoveAttr(attr, diag::dynamic_with_final);
}


void AttributeEarlyChecker::visitIBActionAttr(IBActionAttr *attr) {
  // Only instance methods returning () can be IBActions.
  const FuncDecl *FD = cast<FuncDecl>(D);
  if (!FD->getDeclContext()->isClassOrClassExtensionContext() ||
      FD->isStatic() || FD->isAccessor())
    return diagnoseAndRemoveAttr(attr, diag::invalid_ibaction_decl);

}

void AttributeEarlyChecker::visitIBInspectableAttr(IBInspectableAttr *attr) {
  // Only instance properties can be 'IBInspectable'.
  auto *VD = cast<VarDecl>(D);
  if (!VD->getDeclContext()->isClassOrClassExtensionContext() ||
      VD->isStatic())
    return diagnoseAndRemoveAttr(attr, diag::invalid_ibinspectable);
}

void AttributeEarlyChecker::visitSILStoredAttr(SILStoredAttr *attr) {
  auto *VD = cast<VarDecl>(D);
  if (!VD->getDeclContext()->isClassOrClassExtensionContext())
    return diagnoseAndRemoveAttr(attr, diag::invalid_decl_attribute_simple);
}

static Optional<Diag<bool,Type>>
isAcceptableOutletType(Type type, bool &isArray, TypeChecker &TC) {
  if (type->isObjCExistentialType())
    return {}; // @objc existential types are okay

  auto nominal = type->getAnyNominal();

  if (auto classDecl = dyn_cast_or_null<ClassDecl>(nominal)) {
    if (classDecl->isObjC())
      return {}; // @objc class types are okay.
    return diag::iboutlet_nonobjc_class;
  }

  if (nominal == TC.Context.getStringDecl()) {
    // String is okay because it is bridged to NSString.
    // FIXME: BridgesTypes.def is almost sufficient for this.
    return {};
  }

  if (nominal == TC.Context.getArrayDecl()) {
    // Arrays of arrays are not allowed.
    if (isArray)
      return diag::iboutlet_nonobject_type;

    isArray = true;

    // Handle Array<T>. T must be an Objective-C class or protocol.
    auto boundTy = type->castTo<BoundGenericStructType>();
    auto boundArgs = boundTy->getGenericArgs();
    assert(boundArgs.size() == 1 && "invalid Array declaration");
    Type elementTy = boundArgs.front();
    return isAcceptableOutletType(elementTy, isArray, TC);
  }

  // No other types are permitted.
  return diag::iboutlet_nonobject_type;
}


void AttributeEarlyChecker::visitIBOutletAttr(IBOutletAttr *attr) {
  // Only instance properties can be 'IBOutlet'.
  auto *VD = cast<VarDecl>(D);
  if (!VD->getDeclContext()->isClassOrClassExtensionContext() ||
      VD->isStatic())
    return diagnoseAndRemoveAttr(attr, diag::invalid_iboutlet);

  if (!VD->isSettable(nullptr))
    return diagnoseAndRemoveAttr(attr, diag::iboutlet_only_mutable);

  // Verify that the field type is valid as an outlet.
  auto type = VD->getType();

  if (VD->isInvalid())
    return;

  // Look through ownership types, and optionals.
  type = type->getReferenceStorageReferent();
  bool wasOptional = false;
  if (Type underlying = type->getAnyOptionalObjectType()) {
    type = underlying;
    wasOptional = true;
  }

  bool isArray = false;
  if (auto isError = isAcceptableOutletType(type, isArray, TC))
    return diagnoseAndRemoveAttr(attr, isError.getValue(),
                                 /*array=*/isArray, type);

  // If the type wasn't optional, an array, or unowned, complain.
  if (!wasOptional && !isArray) {
    auto symbolLoc = Lexer::getLocForEndOfToken(
                       TC.Context.SourceMgr,
                       VD->getTypeSourceRangeForDiagnostics().End);
    TC.diagnose(attr->getLocation(), diag::iboutlet_non_optional,
                type);
    TC.diagnose(symbolLoc, diag::note_make_optional,
                OptionalType::get(type))
      .fixItInsert(symbolLoc, "?");
    TC.diagnose(symbolLoc, diag::note_make_implicitly_unwrapped_optional,
                ImplicitlyUnwrappedOptionalType::get(type))
      .fixItInsert(symbolLoc, "!");

    // Recover by setting the implicitly-unwrapped optional type.
    type = ImplicitlyUnwrappedOptionalType::get(type);
    if (auto refStorageType = VD->getType()->getAs<ReferenceStorageType>())
      type = ReferenceStorageType::get(type, refStorageType->getOwnership(),
                                       TC.Context);

    VD->overwriteType(type);
  }
}

void AttributeEarlyChecker::visitNSManagedAttr(NSManagedAttr *attr) {
  // @NSManaged may only be used on properties.
  auto *VD = cast<VarDecl>(D);

  // NSManaged only applies to non-class properties within a class.
  if (VD->isStatic() || !VD->getDeclContext()->isClassOrClassExtensionContext())
    return diagnoseAndRemoveAttr(attr, diag::attr_NSManaged_not_property);

  if (VD->isLet())
    return diagnoseAndRemoveAttr(attr, diag::attr_NSManaged_let_property);

  // @NSManaged properties must be written as stored.
  switch (VD->getStorageKind()) {
  case AbstractStorageDecl::Stored:
    // @NSManaged properties end up being computed; complain if there is
    // an initializer.
    if (VD->getParentPattern()->hasInit()) {
      TC.diagnose(attr->getLocation(), diag::attr_NSManaged_initial_value)
        .highlight(VD->getParentPattern()->getInit()->getSourceRange());
      VD->getParentPattern()->setInit(nullptr, false);
    }
    // Otherwise, ok.
    break;

  case AbstractStorageDecl::StoredWithTrivialAccessors:
    llvm_unreachable("Already created accessors?");

  case AbstractStorageDecl::Computed:
  case AbstractStorageDecl::Observing:
    TC.diagnose(attr->getLocation(), diag::attr_NSManaged_not_stored,
                VD->getStorageKind() == AbstractStorageDecl::Observing);
    return attr->setInvalid();
  }

  // @NSManaged properties cannot be @NSCopying
  if (auto *NSCopy = VD->getAttrs().getAttribute<NSCopyingAttr>())
    return diagnoseAndRemoveAttr(NSCopy, diag::attr_NSManaged_NSCopying);

}

void AttributeEarlyChecker::
visitLLDBDebuggerFunctionAttr(LLDBDebuggerFunctionAttr *attr) {
  // This is only legal when debugger support is on.
  if (!D->getASTContext().LangOpts.DebuggerSupport)
    return diagnoseAndRemoveAttr(attr, diag::attr_for_debugger_support_only);
}

void AttributeEarlyChecker::visitOverrideAttr(OverrideAttr *attr) {
  if (!isa<ClassDecl>(D->getDeclContext()) &&
      !isa<ExtensionDecl>(D->getDeclContext()))
    return diagnoseAndRemoveAttr(attr, diag::override_nonclass_decl);
}

void AttributeEarlyChecker::visitLazyAttr(LazyAttr *attr) {
  // lazy may only be used on properties.
  auto *VD = cast<VarDecl>(D);

  // It cannot currently be used on let's since we don't have a mutability model
  // that supports it.
  if (VD->isLet())
    return diagnoseAndRemoveAttr(attr, diag::lazy_not_on_let);

  // lazy is not allowed on a protocol requirement.
  auto varDC = VD->getDeclContext();
  if (isa<ProtocolDecl>(varDC))
    return diagnoseAndRemoveAttr(attr, diag::lazy_not_in_protocol);

  // It only works with stored properties.
  if (!VD->hasStorage() && VD->getGetter() && !VD->getGetter()->isImplicit())
    return diagnoseAndRemoveAttr(attr, diag::lazy_not_on_computed);

  // lazy is not allowed on a lazily initiailized global variable or on a
  // static property (which is already lazily initialized).
  if (VD->isStatic() ||
      (varDC->isModuleScopeContext() &&
       !varDC->getParentSourceFile()->isScriptMode()))
    return diagnoseAndRemoveAttr(attr, diag::lazy_on_already_lazy_global);

  // lazy must have an initializer, and the pattern binding must be a simple
  // one.
  auto *PBD = VD->getParentPattern();
  if (!PBD->getInit())
    return diagnoseAndRemoveAttr(attr, diag::lazy_requires_initializer);

  if (!PBD->getSingleVar())
    return diagnoseAndRemoveAttr(attr, diag::lazy_requires_single_var);

  // TODO: we can't currently support lazy properties on non-type-contexts.
  if (!VD->getDeclContext()->isTypeContext())
    return diagnoseAndRemoveAttr(attr, diag::lazy_must_be_property);

  // TODO: Lazy properties can't yet be observed.
  if (VD->getStorageKind() == VarDecl::Observing)
    return diagnoseAndRemoveAttr(attr, diag::lazy_not_observable);
}

bool AttributeEarlyChecker::visitAbstractAccessibilityAttr(
    AbstractAccessibilityAttr *attr) {
  // Accessibility attr may only be used on value decls and extensions.
  if (!isa<ValueDecl>(D) && !isa<ExtensionDecl>(D)) {
    diagnoseAndRemoveAttr(attr, diag::invalid_decl_modifier, attr);
    return true;
  }

  if (auto extension = dyn_cast<ExtensionDecl>(D)) {
    if (!extension->getInherited().empty()) {
      diagnoseAndRemoveAttr(attr, diag::extension_access_with_conformances,
                            attr);
      return true;
    }
  }

  // And not on certain value decls.
  if (isa<DestructorDecl>(D) || isa<EnumElementDecl>(D)) {
    diagnoseAndRemoveAttr(attr, diag::invalid_decl_modifier, attr);
    return true;
  }

  // Or within protocols.
  if (isa<ProtocolDecl>(D->getDeclContext())) {
    diagnoseAndRemoveAttr(attr, diag::access_control_in_protocol, attr);
    return true;
  }

  return false;
}

void AttributeEarlyChecker::visitAccessibilityAttr(AccessibilityAttr *attr) {
  visitAbstractAccessibilityAttr(attr);
}

void AttributeEarlyChecker::visitSetterAccessibilityAttr(
    SetterAccessibilityAttr *attr) {
  auto storage = dyn_cast<AbstractStorageDecl>(D);
  if (!storage)
    return diagnoseAndRemoveAttr(attr, diag::access_control_setter,
                                 attr->getAccess());

  if (visitAbstractAccessibilityAttr(attr))
    return;

  if (!storage->isSettable(storage->getDeclContext())) {
    // This must stay in sync with diag::access_control_setter_read_only.
    enum {
      SK_Constant = 0,
      SK_Variable,
      SK_Property,
      SK_Subscript
    } storageKind;
    if (isa<SubscriptDecl>(storage))
      storageKind = SK_Subscript;
    else if (storage->getDeclContext()->isTypeContext())
      storageKind = SK_Property;
    else if (cast<VarDecl>(storage)->isLet())
      storageKind = SK_Constant;
    else
      storageKind = SK_Variable;
    return diagnoseAndRemoveAttr(attr, diag::access_control_setter_read_only,
                                 attr->getAccess(), storageKind);
  }
}


void TypeChecker::checkDeclAttributesEarly(Decl *D) {
  // Don't perform early attribute validation more than once.
  // FIXME: Crummy way to get idempotency.
  if (D->didEarlyAttrValidation())
    return;

  D->setEarlyAttrValidation();

  AttributeEarlyChecker Checker(*this, D);
  for (auto attr : D->getAttrs()) {
    if (!attr->isValid()) continue;

    // If Attr.def says that the attribute cannot appear on this kind of
    // declaration, diagnose it and disable it.
    if (attr->canAppearOnDecl(D)) {
      // Otherwise, check it.
      Checker.visit(attr);
      continue;
    }

    // Otherwise, this attribute cannot be applied to this declaration.  If the
    // attribute is only valid on one kind of declaration (which is pretty
    // common) give a specific helpful error.
    unsigned PossibleDeclKinds = attr->getOptions() & DeclAttribute::OnAnyDecl;
    StringRef OnlyKind;
    if (PossibleDeclKinds == DeclAttribute::OnVar)
      OnlyKind = "var";
    else if (PossibleDeclKinds == DeclAttribute::OnFunc)
      OnlyKind = "func";
    else if (PossibleDeclKinds == DeclAttribute::OnClass)
      OnlyKind = "class";
    else if (PossibleDeclKinds == DeclAttribute::OnStruct)
      OnlyKind = "struct";
    else if (PossibleDeclKinds == DeclAttribute::OnConstructor)
      OnlyKind = "init";
    else if (PossibleDeclKinds == DeclAttribute::OnProtocol)
      OnlyKind = "protocol";

    if (!OnlyKind.empty())
      Checker.diagnoseAndRemoveAttr(attr, diag::attr_only_only_one_decl_kind,
                                    attr, OnlyKind);
    else if (attr->isDeclModifier())
      Checker.diagnoseAndRemoveAttr(attr, diag::invalid_decl_modifier, attr);
    else
      Checker.diagnoseAndRemoveAttr(attr, diag::invalid_decl_attribute, attr);
  }
}

namespace {
class AttributeChecker : public AttributeVisitor<AttributeChecker> {
  TypeChecker &TC;
  Decl *D;

public:
  AttributeChecker(TypeChecker &TC, Decl *D) : TC(TC), D(D) {}

  /// Deleting this ensures that all attributes are covered by the visitor
  /// below.
  void visitDeclAttribute(DeclAttribute *A) = delete;

#define IGNORED_ATTR(CLASS)                                              \
    void visit##CLASS##Attr(CLASS##Attr *) {}

    IGNORED_ATTR(Asmname)
    IGNORED_ATTR(Dynamic)
    IGNORED_ATTR(Exported)
    IGNORED_ATTR(Convenience)
    IGNORED_ATTR(IBDesignable)
    IGNORED_ATTR(IBInspectable)
    IGNORED_ATTR(IBOutlet) // checked early.
    IGNORED_ATTR(Inline)
    IGNORED_ATTR(Lazy)      // checked early.
    IGNORED_ATTR(LLDBDebuggerFunction)
    IGNORED_ATTR(Mutating)
    IGNORED_ATTR(NonMutating)
    IGNORED_ATTR(NoReturn)
    IGNORED_ATTR(NSManaged) // checked early.
    IGNORED_ATTR(ObjC)
    IGNORED_ATTR(Optional)
    IGNORED_ATTR(Ownership)
    IGNORED_ATTR(Override)
    IGNORED_ATTR(RawDocComment)
    IGNORED_ATTR(Semantics)
    IGNORED_ATTR(Transparent)
    IGNORED_ATTR(RequiresStoredPropertyInits)
    IGNORED_ATTR(SILStored)
#undef IGNORED_ATTR

  void visitAvailabilityAttr(AvailabilityAttr *attr) {
    // FIXME: Check that this declaration is at least as available as the
    // one it overrides.
  }

  void visitClassProtocolAttr(ClassProtocolAttr *attr);
  void visitFinalAttr(FinalAttr *attr);
  void visitIBActionAttr(IBActionAttr *attr);
  void visitNSCopyingAttr(NSCopyingAttr *attr);
  void visitRequiredAttr(RequiredAttr *attr);

  bool visitAbstractAccessibilityAttr(AbstractAccessibilityAttr *attr);
  void visitAccessibilityAttr(AccessibilityAttr *attr);
  void visitSetterAccessibilityAttr(SetterAccessibilityAttr *attr);

  void visitUIApplicationMainAttr(UIApplicationMainAttr *attr);
  void visitUnsafeNoObjCTaggedPointerAttr(UnsafeNoObjCTaggedPointerAttr *attr);

  void checkOperatorAttribute(DeclAttribute *attr);

  void visitInfixAttr(InfixAttr *attr) { checkOperatorAttribute(attr); }
  void visitPostfixAttr(PostfixAttr *attr) { checkOperatorAttribute(attr); }
  void visitPrefixAttr(PrefixAttr *attr) { checkOperatorAttribute(attr); }
};
} // end anonymous namespace


static bool checkObjectOrOptionalObjectType(TypeChecker &TC, Decl *D,
                                            const Pattern *argPattern) {
  Type ty = argPattern->getType();
  if (auto unwrapped = ty->getAnyOptionalObjectType())
    ty = unwrapped;

  if (auto classDecl = ty->getClassOrBoundGenericClass()) {
    // @objc class types are okay.
    if (!classDecl->isObjC()) {
      TC.diagnose(D, diag::ibaction_nonobjc_class_argument,
                  argPattern->getType())
        .highlight(argPattern->getSourceRange());
      return true;
    }
  } else if (ty->isObjCExistentialType()) {
    // @objc existential types are okay
    // Nothing to do.
  } else {
    // No other types are permitted.
    TC.diagnose(D, diag::ibaction_nonobject_argument,
                argPattern->getSemanticsProvidingPattern()->getType())
      .highlight(argPattern->getSourceRange());
    return true;
  }

  return false;
}

static bool isiOS(TypeChecker &TC) {
  // FIXME: This is a very ugly way of checking the OS.
  return TC.getLangOpts().getTargetConfigOption("os") == "iOS";
}

void AttributeChecker::visitIBActionAttr(IBActionAttr *attr) {
  // IBActions instance methods must have type Class -> (...) -> ().
  auto *FD = cast<FuncDecl>(D);
  Type CurriedTy = FD->getType()->castTo<AnyFunctionType>()->getResult();
  Type ResultTy = CurriedTy->castTo<AnyFunctionType>()->getResult();
  if (!ResultTy->isEqual(TupleType::getEmpty(TC.Context))) {
    TC.diagnose(D, diag::invalid_ibaction_result, ResultTy);
    attr->setInvalid();
    return;
  }

  auto Arguments = FD->getBodyParamPatterns()[1];
  auto ArgTuple = dyn_cast<TuplePattern>(Arguments);

  bool iOSOnlyUsedOnOSX = false;
  bool Valid = true;
  if (ArgTuple) {
    auto fields = ArgTuple->getFields();
    switch (ArgTuple->getNumFields()) {
    case 0:
      // (iOS only) No arguments.
      if (!isiOS(TC)) {
        iOSOnlyUsedOnOSX = true;
        break;
      }
      break;
    case 1:
      // One argument.
      if (checkObjectOrOptionalObjectType(TC, D, fields[0].getPattern()))
        Valid = false;
      break;
    case 2:
      // (iOS only) Two arguments, the second of which is a UIEvent.
      // We don't currently enforce the UIEvent part.
      if (!isiOS(TC)) {
        iOSOnlyUsedOnOSX = true;
        break;
      }
      if (checkObjectOrOptionalObjectType(TC, D, fields[0].getPattern()))
        Valid = false;
      if (checkObjectOrOptionalObjectType(TC, D, fields[1].getPattern()))
        Valid = false;
      break;
    default:
      // No platform allows an action signature with more than two arguments.
      TC.diagnose(D, diag::invalid_ibaction_argument_count, isiOS(TC));
      Valid = false;
      break;
    }
  } else {
    // One argument without a name.
    if (checkObjectOrOptionalObjectType(TC, D, Arguments))
      Valid = false;
  }

  if (iOSOnlyUsedOnOSX) {
    TC.diagnose(D, diag::invalid_ibaction_argument_count, /*iOS=*/false);
    Valid = false;
  }

  if (!Valid)
    attr->setInvalid();
}

void AttributeChecker::visitClassProtocolAttr(ClassProtocolAttr *attr) {
  // FIXME: The @class_protocol attribute is dead. Retain this code so that we
  // diagnose uses of @class_protocol for non-protocols.
  if (!isa<ProtocolDecl>(D)) {
    TC.diagnose(attr->getLocation(),
                diag::class_protocol_not_protocol);
    attr->setInvalid();
  }
}

void AttributeChecker::visitUnsafeNoObjCTaggedPointerAttr(
                                          UnsafeNoObjCTaggedPointerAttr *attr) {
  // Only class protocols can have the attribute.
  auto proto = dyn_cast<ProtocolDecl>(D);
  if (!proto) {
    TC.diagnose(attr->getLocation(),
                diag::no_objc_tagged_pointer_not_class_protocol);
    attr->setInvalid();
  }
  
  if (!proto->requiresClass()
      && !proto->getAttrs().hasAttribute<ObjCAttr>()) {
    TC.diagnose(attr->getLocation(),
                diag::no_objc_tagged_pointer_not_class_protocol);
    attr->setInvalid();    
  }
}

void AttributeChecker::visitFinalAttr(FinalAttr *attr) {
  // final on classes marks all members with final.
  if (isa<ClassDecl>(D))
    return;

  // 'final' only makes sense in the context of a class
  // declaration.  Reject it on global functions, structs, enums, etc.
  if (!D->getDeclContext()->isClassOrClassExtensionContext()) {
    TC.diagnose(attr->getLocation(), diag::member_cannot_be_final);
    return;
  }

  // We currently only support final on var/let, func and subscript
  // declarations.
  if (!isa<VarDecl>(D) && !isa<FuncDecl>(D) && !isa<SubscriptDecl>(D)) {
    TC.diagnose(attr->getLocation(), diag::final_not_allowed_here);
    return;
  }

  if (auto *FD = dyn_cast<FuncDecl>(D)) {
    if (FD->isAccessor() && !attr->isImplicit()) {
      unsigned Kind = 2;
      if (auto *VD = dyn_cast<VarDecl>(FD->getAccessorStorageDecl()))
        Kind = VD->isLet() ? 1 : 0;
      TC.diagnose(attr->getLocation(), diag::final_not_on_accessors, Kind);
      return;
    }
  }
}

/// Return true if this is a builtin operator that cannot be defined in user
/// code.
static bool isBuiltinOperator(StringRef name, DeclAttribute *attr) {
  return ((isa<PrefixAttr>(attr)  && name == "&") ||   // lvalue to inout
          (isa<PostfixAttr>(attr) && name == "!") ||   // optional unwrapping
          (isa<PostfixAttr>(attr) && name == "?") ||   // optional chaining
          (isa<PostfixAttr>(attr) && name == ">") ||   // generic argument list
          (isa<PrefixAttr>(attr)  && name == "<"));    // generic argument list
}

void AttributeChecker::checkOperatorAttribute(DeclAttribute *attr) {
  // Check out the operator attributes.  They may be attached to an operator
  // declaration or a function.
  if (auto *OD = dyn_cast<OperatorDecl>(D)) {
    // Reject attempts to define builtin operators.
    if (isBuiltinOperator(OD->getName().str(), attr)) {
      TC.diagnose(D->getStartLoc(), diag::redefining_builtin_operator,
                  attr->getAttrName(), OD->getName().str());
      attr->setInvalid();
      return;
    }

    // Otherwise, the attribute is always ok on an operator.
    return;
  }

  // Operators implementations may only be defined as functions.
  auto *FD = dyn_cast<FuncDecl>(D);
  if (!FD) {
    TC.diagnose(D->getLoc(), diag::operator_not_func);
    attr->setInvalid();
    return;
  }

  // Only functions with an operator identifier can be declared with as an
  // operator.
  if (!FD->getName().isOperator()) {
    TC.diagnose(D->getStartLoc(), diag::attribute_requires_operator_identifier,
                attr->getAttrName());
    attr->setInvalid();
    return;
  }

  // Reject attempts to define builtin operators.
  if (isBuiltinOperator(FD->getName().str(), attr)) {
    TC.diagnose(D->getStartLoc(), diag::redefining_builtin_operator,
                attr->getAttrName(), FD->getName().str());
    attr->setInvalid();
    return;
  }

  // Infix operator is only allowed on operator declarations, not on func.
  if (isa<InfixAttr>(attr)) {
    TC.diagnose(attr->getLocation(), diag::invalid_infix_on_func)
      .fixItRemove(attr->getLocation());
    attr->setInvalid();
    return;
  }

  // Otherwise, must be unary.
  if (!FD->isUnaryOperator()) {
    TC.diagnose(attr->getLocation(), diag::attribute_requires_single_argument,
                attr->getAttrName());
    attr->setInvalid();
    return;
  }
}


void AttributeChecker::visitNSCopyingAttr(NSCopyingAttr *attr) {
  // The @NSCopying attribute is only allowed on stored properties.
  auto *VD = cast<VarDecl>(D);

  // It may only be used on class members.
  auto typeContext = D->getDeclContext()->getDeclaredTypeInContext();
  auto contextTypeDecl =
  typeContext ? typeContext->getNominalOrBoundGenericNominal() : nullptr;
  if (!contextTypeDecl || !isa<ClassDecl>(contextTypeDecl)) {
    TC.diagnose(attr->getLocation(), diag::nscopying_only_on_class_properties);
    attr->setInvalid();
    return;
  }

  if (!VD->isSettable(VD->getDeclContext())) {
    TC.diagnose(attr->getLocation(), diag::nscopying_only_mutable);
    attr->setInvalid();
    return;
  }

  if (!VD->hasStorage()) {
    TC.diagnose(attr->getLocation(), diag::nscopying_only_stored_property);
    attr->setInvalid();
    return;
  }

  assert(VD->getOverriddenDecl() == nullptr &&
         "Can't have value with storage that is an override");

  // Check the type.  It must be must be [unchecked]optional, weak, a normal
  // class, AnyObject, or classbound protocol.
  // must conform to the NSCopying protocol.
  
}

void AttributeChecker::visitUIApplicationMainAttr(UIApplicationMainAttr *attr) {
  //if (attr->isInvalid())
  //  return;
  
  auto *CD = dyn_cast<ClassDecl>(D);
  
  // The applicant not being a class should have been diagnosed by the early
  // checker.
  if (!CD) return;

  // The class cannot be generic.
  if (CD->isGenericContext()) {
    TC.diagnose(attr->getLocation(),
                diag::attr_generic_UIApplicationMain_not_supported);
    attr->setInvalid();
    return;
  }
  
  // @UIApplicationMain classes must conform to UIKit's UIApplicationDelegate
  // protocol.
  auto &C = D->getASTContext();
  Identifier Id_UIApplicationDelegate
    = C.getIdentifier("UIApplicationDelegate");
  Identifier Id_UIKit
    = C.getIdentifier("UIKit");
  
  bool conformsToDelegate = false;
  Module *UIKit = nullptr;
  for (auto proto : CD->getProtocols()) {
    if (proto->getName() != Id_UIApplicationDelegate)
      continue;
    if (proto->getModuleContext()->Name != Id_UIKit)
      continue;
    
    conformsToDelegate = true;
    UIKit = proto->getModuleContext();
    break;
  }
  
  if (!conformsToDelegate) {
    TC.diagnose(attr->getLocation(),
                diag::attr_UIApplicationMain_not_UIApplicationDelegate);
    attr->setInvalid();
  }
  
  if (attr->isInvalid())
    return;
  
  // Register the class as the main class in the module. If there are multiples
  // they will be diagnosed.
  if (CD->getModuleContext()->registerMainClass(CD, attr->getLocation()))
    attr->setInvalid();
  
  // Check that we have the needed symbols in the frameworks.
  SmallVector<ValueDecl*, 4> results;
  UIKit->lookupValue({}, C.getIdentifier("UIApplicationMain"),
                     NLKind::QualifiedLookup, results);
  auto Foundation = TC.Context.getLoadedModule(C.getIdentifier("Foundation"));
  Foundation->lookupValue({}, C.getIdentifier("NSStringFromClass"),
                          NLKind::QualifiedLookup, results);
  for (auto D : results)
    TC.validateDecl(D);
}

void AttributeChecker::visitRequiredAttr(RequiredAttr *attr) {
  // The required attribute only applies to constructors.
  auto ctor = cast<ConstructorDecl>(D);
  auto parentTy = ctor->getExtensionType();
  if (!parentTy) {
    // Constructor outside of nominal type context; we've already complained
    // elsewhere.
    attr->setInvalid();
    return;
  }
  // Only classes can have required constructors.
  if (parentTy->getClassOrBoundGenericClass()) {
    // The constructor must be declared within the class itself.
    if (!isa<ClassDecl>(ctor->getDeclContext())) {
      TC.diagnose(ctor, diag::required_initializer_in_extension, parentTy)
        .highlight(attr->getLocation());
      attr->setInvalid();
      return;
    }
  } else {
    if (!parentTy->is<ErrorType>()) {
      TC.diagnose(ctor, diag::required_initializer_nonclass, parentTy)
        .highlight(attr->getLocation());
    }
    attr->setInvalid();
    return;
  }
}

bool AttributeChecker::visitAbstractAccessibilityAttr(
    AbstractAccessibilityAttr *attr) {
  if (Type ty = D->getDeclContext()->getDeclaredTypeInContext()) {
    Accessibility typeAccess = ty->getAnyNominal()->getAccessibility();
    if (attr->getAccess() > typeAccess) {
      auto diag = TC.diagnose(attr->getLocation(),
                              diag::access_control_member_more,
                              attr->getAccess(),
                              D->getDescriptiveKind(),
                              typeAccess,
                              ty->getAnyNominal()->getDescriptiveKind());
      swift::fixItAccessibility(diag, cast<ValueDecl>(D), typeAccess);
      return true;
    }
  }
  return false;
}

void AttributeChecker::visitAccessibilityAttr(AccessibilityAttr *attr) {
  if (auto extension = dyn_cast<ExtensionDecl>(D)) {
    Type extendedTy = extension->getExtendedType();
    Accessibility typeAccess = extendedTy->getAnyNominal()->getAccessibility();
    if (attr->getAccess() > typeAccess) {
      TC.diagnose(attr->getLocation(), diag::access_control_extension_more,
                  typeAccess,
                  extendedTy->getAnyNominal()->getDescriptiveKind(),
                  attr->getAccess())
        .fixItRemove(attr->getRange());
      attr->setInvalid();
      return;
    }
  } else if (auto extension = dyn_cast<ExtensionDecl>(D->getDeclContext())) {
    auto extAttr = extension->getAttrs().getAttribute<AccessibilityAttr>();
    if (extAttr && attr->getAccess() > extAttr->getAccess()) {
      auto diag = TC.diagnose(attr->getLocation(),
                              diag::access_control_ext_member_more,
                              attr->getAccess(),
                              D->getDescriptiveKind(),
                              extAttr->getAccess());
      return;
    }
  }

  visitAbstractAccessibilityAttr(attr);
}

void
AttributeChecker::visitSetterAccessibilityAttr(SetterAccessibilityAttr *attr) {
  auto getterAccess = cast<ValueDecl>(D)->getAccessibility();
  if (attr->getAccess() > getterAccess) {
    // This must stay in sync with diag::access_control_setter_more.
    enum {
      SK_Variable = 0,
      SK_Property,
      SK_Subscript
    } storageKind;
    if (isa<SubscriptDecl>(D))
      storageKind = SK_Subscript;
    else if (D->getDeclContext()->isTypeContext())
      storageKind = SK_Property;
    else
      storageKind = SK_Variable;
    TC.diagnose(attr->getLocation(), diag::access_control_setter_more,
                getterAccess, storageKind, attr->getAccess());
    attr->setInvalid();
    return;
  }

  visitAbstractAccessibilityAttr(attr);
}

void TypeChecker::checkDeclAttributes(Decl *D) {
  AttributeChecker Checker(*this, D);

  for (auto attr : D->getAttrs()) {
    if (attr->isValid())
      Checker.visit(attr);
  }
}

void TypeChecker::checkOwnershipAttr(VarDecl *var, OwnershipAttr *attr) {
  Type type = var->getType();

  // Just stop if we've already processed this declaration.
  if (type->is<ReferenceStorageType>())
    return;

  auto ownershipKind = attr->get();
  assert(ownershipKind != Ownership::Strong &&
         "Cannot specify 'strong' in an ownership attribute");

  // A weak variable must have type R? or R! for some ownership-capable type R.
  Type underlyingType = type;
  if (ownershipKind == Ownership::Weak) {
    if (var->isLet()) {
      diagnose(var->getStartLoc(), diag::invalid_weak_let);
      attr->setInvalid();
      return;
    }

    if (Type objType = type->getAnyOptionalObjectType())
      underlyingType = objType;
    else if (type->allowsOwnership()) {
      // Use this special diagnostic if it's actually a reference type but just
      // isn't Optional.
      if (var->getAttrs().hasAttribute<IBOutletAttr>()) {
        // Let @IBOutlet complain about this; it's more specific.
        attr->setInvalid();
        return;
      }

      diagnose(var->getStartLoc(), diag::invalid_weak_ownership_not_optional,
               OptionalType::get(type));
      attr->setInvalid();

      return;
    } else {
      // This is also an error, but the code below will diagnose it.
    }
  } else if (ownershipKind == Ownership::Strong) {
    // We allow strong on optional-qualified reference types.
    if (Type objType = type->getAnyOptionalObjectType())
      underlyingType = objType;
  }

  if (!underlyingType->allowsOwnership()) {
    // If we have an opaque type, suggest the possibility of adding
    // a class bound.
    if (type->isExistentialType() || type->is<ArchetypeType>()) {
      diagnose(var->getStartLoc(), diag::invalid_ownership_opaque_type,
               (unsigned) ownershipKind, underlyingType);
    } else {
      diagnose(var->getStartLoc(), diag::invalid_ownership_type,
               (unsigned) ownershipKind, underlyingType);
    }
    attr->setInvalid();
    return;
  }

  // Change the type to the appropriate reference storage type.
  var->overwriteType(ReferenceStorageType::get(type, ownershipKind, Context));
}
