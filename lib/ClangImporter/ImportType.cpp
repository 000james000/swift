//===--- ImportType.cpp - Import Clang Types ------------------------------===//
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
// This file implements support for importing Clang types as Swift types.
//
//===----------------------------------------------------------------------===//

#include "ImporterImpl.h"
#include "swift/Strings.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Types.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/TypeVisitor.h"
#include "swift/ClangImporter/ClangModule.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"

using namespace swift;

namespace {
  class SwiftTypeConverter : public clang::TypeVisitor<SwiftTypeConverter, Type>
  {
    ClangImporter::Implementation &Impl;
    ImportTypeKind kind;

  public:
    SwiftTypeConverter(ClangImporter::Implementation &impl, ImportTypeKind kind)
      : Impl(impl), kind(kind) { }


#define DEPENDENT_TYPE(Class, Base)                            \
    Type Visit##Class##Type(const clang::Class##Type *) {      \
      llvm_unreachable("Dependent types cannot be converted"); \
    }
#define TYPE(Class, Base)
#include "clang/AST/TypeNodes.def"
    
    /// True if we're converting a function parameter, property type, or
    /// function result type, and can thus safely apply representation
    /// conversions for bridged types.
    bool canBridgeTypes() const {
      return kind == ImportTypeKind::Parameter ||
             kind == ImportTypeKind::Result ||
             kind == ImportTypeKind::Property;
    }

    Type VisitBuiltinType(const clang::BuiltinType *type) {
      switch (type->getKind()) {
      case clang::BuiltinType::Void:
        // 'void' can only be imported as a function result type.
        if (kind == ImportTypeKind::Result)
          return Impl.getNamedSwiftType(Impl.getStdlibModule(), "Void");

        return nullptr;

#define MAP_BUILTIN_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME) \
      case clang::BuiltinType::CLANG_BUILTIN_KIND:            \
        return Impl.getNamedSwiftType(Impl.getStdlibModule(),  \
                                      #SWIFT_TYPE_NAME);
#include "swift/ClangImporter/BuiltinMappedTypes.def"

      // Types that cannot be mapped into Swift, and probably won't ever be.
      case clang::BuiltinType::Dependent:
      case clang::BuiltinType::ARCUnbridgedCast:
      case clang::BuiltinType::BoundMember:
      case clang::BuiltinType::BuiltinFn:
      case clang::BuiltinType::Overload:
      case clang::BuiltinType::PseudoObject:
      case clang::BuiltinType::UnknownAny:
        return Type();

      // FIXME: Types that can be mapped, but aren't yet.
      case clang::BuiltinType::Half:
      case clang::BuiltinType::LongDouble:
      case clang::BuiltinType::NullPtr:
        return Type();

      // Objective-C types that aren't mapped directly; rather, pointers to
      // these types will be mapped.
      case clang::BuiltinType::ObjCClass:
      case clang::BuiltinType::ObjCId:
      case clang::BuiltinType::ObjCSel:
        return Type();

      // OpenCL types that don't have Swift equivalents.
      case clang::BuiltinType::OCLImage1d:
      case clang::BuiltinType::OCLImage1dArray:
      case clang::BuiltinType::OCLImage1dBuffer:
      case clang::BuiltinType::OCLImage2d:
      case clang::BuiltinType::OCLImage2dArray:
      case clang::BuiltinType::OCLImage3d:
      case clang::BuiltinType::OCLEvent:
      case clang::BuiltinType::OCLSampler:
        return Type();
      }
    }

    Type VisitComplexType(const clang::ComplexType *type) {
      // FIXME: Implement once Complex is in the library.
      return Type();
    }

    Type VisitPointerType(const clang::PointerType *type) {
      // FIXME: Function pointer types can be mapped to Swift function types
      // once we have the notion of a "thin" function that does not capture
      // anything.
      if (type->getPointeeType()->isFunctionType())
        return Type();

      // "const char *" maps to Swift's CString.
      clang::ASTContext &clangContext = Impl.getClangASTContext();
      if (clangContext.hasSameType(type->getPointeeType(),
                                   clangContext.CharTy.withConst())) {
        return Impl.getNamedSwiftType(Impl.getStdlibModule(), "CString");
      }

      // Import void* as COpaquePointer.
      if (type->isVoidPointerType()) {
        return Impl.getNamedSwiftType(Impl.getStdlibModule(), "COpaquePointer");
      }

      // Special case for NSZone*, which has its own Swift wrapper.
      if (auto pointee = type->getPointeeType()->getAs<clang::TypedefType>()) {
        const clang::RecordType *pointeeStruct = pointee->getAsStructureType();
        if (pointeeStruct &&
            !pointeeStruct->getDecl()->isCompleteDefinition() &&
            pointee->getDecl()->getName() == "NSZone") {
          Module *Foundation = Impl.getNamedModule(FOUNDATION_MODULE_NAME);
          Type wrapperTy = Impl.getNamedSwiftType(Foundation, "NSZone");
          if (wrapperTy)
            return wrapperTy;
        }
      }

      // All other C pointers to concrete types map to UnsafePointer<T>.
      auto pointeeType = Impl.importType(type->getPointeeType(),
                                         ImportTypeKind::Normal);
      if (pointeeType)
        return Impl.getNamedSwiftTypeSpecialization(Impl.getStdlibModule(),
                                                    "UnsafePointer", pointeeType);
      
      // If the pointed-to type is unrepresentable in Swift, import as
      // COpaquePointer.
      // FIXME: Should use something with a stronger type.
      return Impl.getNamedSwiftType(Impl.getStdlibModule(), "COpaquePointer");
    }

    Type VisitBlockPointerType(const clang::BlockPointerType *type) {
      // Block pointer types are mapped to function types.
      // FIXME: As a temporary hack, block function types are annotated with
      // an [objc_block] attribute.
      Type pointeeType = Impl.importType(type->getPointeeType(),
                                         ImportTypeKind::Normal);
      if (!pointeeType)
        return Type();
      FunctionType *fTy = pointeeType->castTo<FunctionType>();
      fTy = FunctionType::get(fTy->getInput(), fTy->getResult(),
                              fTy->getExtInfo().withIsBlock(true));

      if (Impl.EnableOptional)
        return UncheckedOptionalType::get(fTy);
      return fTy;
    }

    Type VisitReferenceType(const clang::ReferenceType *type) {
      // Reference types are only permitted as function parameter types.
      if (kind != ImportTypeKind::Parameter)
        return nullptr;

      // Import the underlying type.
      auto objectType = Impl.importType(type->getPointeeType(),
                                        ImportTypeKind::Normal);
      if (!objectType)
        return nullptr;

      return InOutType::get(objectType);
    }

    Type VisitMemberPointer(const clang::MemberPointerType *type) {
      // FIXME: Member function pointers can be mapped to curried functions,
      // but only when we can express the notion of a function that does
      // not capture anything from its enclosing context.
      return Type();
    }

    Type VisitArrayType(const clang::ArrayType *type) {
      // FIXME: Array types will need to be mapped differently depending on
      // context.
      return Type();
    }
    
    Type VisitConstantArrayType(const clang::ConstantArrayType *type) {
      // FIXME: In a function argument context, arrays should import as
      // pointers.
      
      // FIXME: Map to a real fixed-size Swift array type when we have those.
      // Importing as a tuple at least fills the right amount of space, and
      // we can cheese static-offset "indexing" using .$n operations.
      
      Type elementType = Impl.importType(type->getElementType(),
                                         ImportTypeKind::Normal);
      if (!elementType)
        return Type();
      
      TupleTypeElt elt(elementType);
      SmallVector<TupleTypeElt, 8> elts;
      for (size_t i = 0, size = type->getSize().getZExtValue(); i < size; ++i)
        elts.push_back(elt);
      
      return TupleType::get(elts, elementType->getASTContext());
    }

    Type VisitVectorType(const clang::VectorType *type) {
      // FIXME: We could map these.
      return Type();
    }

    Type VisitExtVectorType(const clang::ExtVectorType *type) {
      // FIXME: We could map these.
      return Type();
    }

    Type VisitFunctionProtoType(const clang::FunctionProtoType *type) {
      // C-style variadic functions cannot be called from Swift.
      if (type->isVariadic())
        return Type();

      // Import the result type.
      auto resultTy = Impl.importType(type->getReturnType(),
                                      ImportTypeKind::Result);
      if (!resultTy)
        return Type();

      SmallVector<TupleTypeElt, 4> params;
      for (auto param = type->param_type_begin(),
             paramEnd = type->param_type_end();
           param != paramEnd; ++param) {
        auto swiftParamTy = Impl.importType(*param, ImportTypeKind::Parameter);
        if (!swiftParamTy)
          return Type();

        // FIXME: If we were walking TypeLocs, we could actually get parameter
        // names. The probably doesn't matter outside of a FuncDecl, which
        // we'll have to special-case, but it's an interesting bit of data loss.
        params.push_back(swiftParamTy);
      }

      // Form the parameter tuple.
      auto paramsTy = TupleType::get(params, Impl.SwiftContext);

      // Form the function type.
      return FunctionType::get(paramsTy, resultTy);
    }

    Type VisitFunctionNoProtoType(const clang::FunctionNoProtoType *type) {
      // Import functions without prototypes as functions with no parameters.
      auto resultTy = Impl.importType(type->getReturnType(),
                                      ImportTypeKind::Result);
      if (!resultTy)
        return Type();

      return FunctionType::get(TupleType::getEmpty(Impl.SwiftContext),resultTy);
    }

    Type VisitParenType(const clang::ParenType *type) {
      auto inner = Impl.importType(type->getInnerType(), kind);
      if (!inner)
        return Type();

      return ParenType::get(Impl.SwiftContext, inner);
    }

    Type VisitTypedefType(const clang::TypedefType *type) {
      // When BOOL is the type of a function parameter or a function
      // result type, map it to swift's Bool.
      if (canBridgeTypes() && type->getDecl()->getName() == "BOOL")
        return Impl.getNamedSwiftType(Impl.getStdlibModule(), "Bool");

      // When NSUInteger is used as an enum's underlying type, make sure it
      // stays unsigned.
      if (kind == ImportTypeKind::Enum &&
          type->getDecl()->getName() == "NSUInteger")
        return Impl.getNamedSwiftType(Impl.getStdlibModule(), "UInt");
      
      // Import the underlying declaration.
      auto decl = dyn_cast_or_null<TypeDecl>(Impl.importDecl(type->getDecl()));

      // The type of the underlying declaration is always imported as a "normal"
      // type. If we're asked to import a normal type, or if the typedef is
      // one of the special set of typedefs for which we provide a special
      // mapping, just return the type of the imported declaration.
      if (auto specialKind = Impl.getSpecialTypedefKind(type->getDecl())) {
        if (!decl)
          return nullptr;
        switch (specialKind.getValue()) {
        case MappedTypeNameKind::DoNothing:
        case MappedTypeNameKind::DefineAndUse:
          return decl->getDeclaredType();
        case MappedTypeNameKind::DefineOnly:
          return cast<TypeAliasDecl>(decl)->getUnderlyingType();
        }
      }
      if (kind == ImportTypeKind::Normal)
        return decl ? decl->getDeclaredType() : nullptr;

      // For non-normal type imports 

      // Import the underlying type directly. Due to the import kind, it may
      // differ from directly referencing the declaration (including being
      // defined in cases where the typedef can't be referenced directly).
      auto underlyingType
        = Impl.importType(type->getDecl()->getUnderlyingType(), kind);

      // If the underlying type is in fact the same as the declaration's
      // imported type, use the declaration's type to maintain more sugar.
      if (decl && underlyingType->isEqual(decl->getDeclaredType()))
        return decl->getDeclaredType();

      return underlyingType;
    }

    Type VisitDecayedType(const clang::DecayedType *Type) {
      return Impl.importType(Type->getDecayedType(), kind);
    }

    Type VisitTypeOfExpr(const clang::TypeOfExprType *type) {
      return Impl.importType(
               Impl.getClangASTContext().getCanonicalType(clang::QualType(type,
                                                                          0)),
               kind);
    }

    Type VisitTypeOfType(const clang::TypeOfType *type) {
      return Impl.importType(type->getUnderlyingType(), kind);
    }

    Type VisitDecltypeType(const clang::DecltypeType *type) {
      return Impl.importType(type->getUnderlyingType(), kind);
    }

    Type VisitUnaryTransformType(const clang::UnaryTransformType *type) {
      return Impl.importType(type->getUnderlyingType(), kind);
    }

    Type VisitRecordType(const clang::RecordType *type) {
      auto decl = dyn_cast_or_null<TypeDecl>(Impl.importDecl(type->getDecl()));
      if (!decl)
        return nullptr;

      return decl->getDeclaredType();
    }

    Type VisitEnumType(const clang::EnumType *type) {
      auto clangDecl = type->getDecl();
      switch (Impl.classifyEnum(clangDecl)) {
      case ClangImporter::Implementation::EnumKind::Constants: {
        auto clangDef = clangDecl->getDefinition();
        // Map anonymous enums with no fixed underlying type to Int /if/
        // they fit in an Int32. If not, this mapping isn't guaranteed to be
        // consistent for all platforms we care about.
        if (!clangDef->isFixed() &&
            clangDef->getNumPositiveBits() < 32 &&
            clangDef->getNumNegativeBits() <= 32)
          return Impl.getNamedSwiftType(Impl.getStdlibModule(), "Int");

        // Import the underlying integer type.
        return Impl.importType(clangDecl->getIntegerType(), kind);
      }
      case ClangImporter::Implementation::EnumKind::Enum:
      case ClangImporter::Implementation::EnumKind::Unknown:
      case ClangImporter::Implementation::EnumKind::Options: {
        auto decl = dyn_cast_or_null<TypeDecl>(Impl.importDecl(clangDecl));
        if (!decl)
          return nullptr;

        return decl->getDeclaredType();
      }
      }
    }

    Type VisitElaboratedType(const clang::ElaboratedType *type) {
      return Impl.importType(type->getNamedType(), kind);
    }

    Type VisitAttributedType(const clang::AttributedType *type) {
      return Impl.importType(type->getEquivalentType(), kind);
    }

    Type VisitSubstTemplateTypeParmType(
           const clang::SubstTemplateTypeParmType *type) {
      return Impl.importType(type->getReplacementType(), kind);
    }

    Type VisitTemplateSpecializationType(
           const clang::TemplateSpecializationType *type) {
      return Impl.importType(type->desugar(), kind);
    }

    Type VisitAutoType(const clang::AutoType *type) {
      return Impl.importType(type->getDeducedType(), kind);
    }

    Type VisitObjCObjectType(const clang::ObjCObjectType *type) {
      // If this is id<P> , turn this into a protocol type.
      // FIXME: What about Class<P>?
      if (type->isObjCQualifiedId()) {
        SmallVector<Type, 4> protocols;
        for (auto cp = type->qual_begin(), cpEnd = type->qual_end();
             cp != cpEnd; ++cp) {
          auto proto = cast_or_null<ProtocolDecl>(Impl.importDecl(*cp));
          if (!proto)
            return Type();

          protocols.push_back(proto->getDeclaredType());
        }

        return ProtocolCompositionType::get(Impl.SwiftContext, protocols);
      }

      // FIXME: Swift cannot express qualified object pointer types, e.g.,
      // NSObject<Proto>, so we drop the <Proto> part.
      return Visit(type->getBaseType().getTypePtr());
    }

    Type VisitObjCInterfaceType(const clang::ObjCInterfaceType *type) {
      auto imported = cast_or_null<ClassDecl>(Impl.importDecl(type->getDecl()));
      if (!imported)
        return nullptr;

      // When NSString* is the type of a function parameter or a function
      // result type, map it to String.
      if (canBridgeTypes() &&
          imported->hasName() &&
          imported->getName().str() == "NSString" &&
          Impl.hasFoundationModule()) {
        return Impl.getNamedSwiftType(Impl.getStdlibModule(), "String");
      }

      return imported->getDeclaredType();
    }

    Type
    VisitObjCObjectPointerTypeImpl(const clang::ObjCObjectPointerType *type) {
      // If this object pointer refers to an Objective-C class (possibly
      // qualified),
      if (auto interface = type->getInterfaceType()) {
        // FIXME: Swift cannot express qualified object pointer types, e.g.,
        // NSObject<Proto>, so we drop the <Proto> part.
        return VisitObjCInterfaceType(interface);
      }

      // If this is id<P>, turn this into a protocol type.
      // FIXME: What about Class<P>?
      if (type->isObjCQualifiedIdType()) {
        SmallVector<Type, 4> protocols;
        for (auto cp = type->qual_begin(), cpEnd = type->qual_end();
             cp != cpEnd; ++cp) {
          auto proto = cast_or_null<ProtocolDecl>(Impl.importDecl(*cp));
          if (!proto)
            return Type();

          protocols.push_back(proto->getDeclaredType());
        }

        return ProtocolCompositionType::get(Impl.SwiftContext, protocols);
      }

      // Beyond here, we're using DynamicLookup.
      auto proto = Impl.SwiftContext.getProtocol(
                     KnownProtocolKind::DynamicLookup);
      if (!proto)
        return Type();

      // id maps to DynamicLookup.
      if (type->isObjCIdType()) {
        return proto->getDeclaredType();
      }

      // Class maps to DynamicLookup.metatype.
      assert(type->isObjCClassType() || type->isObjCQualifiedClassType());
      return MetatypeType::get(proto->getDeclaredType(), Impl.SwiftContext);
    }

    Type VisitObjCObjectPointerType(const clang::ObjCObjectPointerType *type) {
      Type result = VisitObjCObjectPointerTypeImpl(type);
      if (!result || !Impl.EnableOptional)
        return result;
      return UncheckedOptionalType::get(result);
    }
  };
}

Type ClangImporter::Implementation::importType(clang::QualType type,
                                               ImportTypeKind kind) {
  if (type.isNull())
    return Type();

  // The "built-in" Objective-C types id, Class, and SEL can actually be (and
  // are) defined within the library. Clang tracks the redefinition types
  // separately, so it can provide fallbacks in certain cases. For Swift, we
  // map the redefinition types back to the equivalent of the built-in types.
  // This bans some trickery that the redefinition types enable, but is a more
  // sane model overall.
  auto &clangContext = getClangASTContext();
  if (clangContext.getLangOpts().ObjC1) {
    if (clangContext.hasSameUnqualifiedType(
          type, clangContext.getObjCIdRedefinitionType()) &&
        !clangContext.hasSameUnqualifiedType(
           clangContext.getObjCIdType(),
           clangContext.getObjCIdRedefinitionType()))
      type = clangContext.getObjCIdType();
    else if (clangContext.hasSameUnqualifiedType(
                type, clangContext.getObjCClassRedefinitionType()) &&
             !clangContext.hasSameUnqualifiedType(
                clangContext.getObjCClassType(),
                clangContext.getObjCClassRedefinitionType()))
      type = clangContext.getObjCClassType();
    else if (clangContext.hasSameUnqualifiedType(
               type, clangContext.getObjCSelRedefinitionType()) &&
             !clangContext.hasSameUnqualifiedType(
                clangContext.getObjCSelType(),
                clangContext.getObjCSelRedefinitionType()))
      type = clangContext.getObjCSelType();
  }
  
  SwiftTypeConverter converter(*this, kind);
  return converter.Visit(type.getTypePtr());
}

/// Given the first selector piece for an init method, e.g., \c initWithFoo,
/// produce the
///
/// \param piece The first selector piece, e.g., \c initWithFoo.
/// \param buffer A scratch buffer.
/// \returns the name of the parameter that corresponds to this selector piece.
static StringRef getFirstInitParameterName(StringRef piece,
                                           SmallVectorImpl<char> &buffer) {
  assert(piece.startswith("init") && "Must be in the init family");
  piece = piece.substr(4);

  // If the second character is uppercase, we have an acronym, so don't
  // make any changes. Similarly, if there's nothing to change, or lowercasing
  // the first letter would have no effect, there's nothing more to do.
  // just re
  if (piece.empty() ||
      (piece.size() > 1 && isupper(piece[1])) ||
      tolower(piece[0]) == piece[0]) {
    return piece;
  }

  // Lowercase the first letter.
  buffer.clear();
  buffer.reserve(piece.size());
  buffer.push_back(tolower(piece[0]));
  buffer.append(piece.begin() + 1, piece.end());
  return StringRef(buffer.data(), buffer.size());
}

Type ClangImporter::Implementation::importFunctionType(
       clang::QualType resultType,
       ArrayRef<const clang::ParmVarDecl *> params,
       bool isVariadic,
       SmallVectorImpl<Pattern*> &argPatterns,
       SmallVectorImpl<Pattern*> &bodyPatterns,
       bool *pHasSelectorStyleSignature,
       clang::Selector selector,
       SpecialMethodKind kind) {

  if (pHasSelectorStyleSignature)
    *pHasSelectorStyleSignature = false;

  // Cannot import variadic types.
  if (isVariadic)
    return Type();

  // Import the result type.
  auto swiftResultTy = importType(resultType, ImportTypeKind::Result);
  if (!swiftResultTy)
    return Type();

  // Import the parameters.
  SmallVector<TupleTypeElt, 4> swiftArgParams;
  SmallVector<TupleTypeElt, 4> swiftBodyParams;
  SmallVector<TuplePatternElt, 4> argPatternElts;
  SmallVector<TuplePatternElt, 4> bodyPatternElts;
  unsigned index = 0;
  for (auto param : params) {
    auto paramTy = param->getType();
    if (paramTy->isVoidType()) {
      ++index;
      continue;
    }

    // Import the parameter type into Swift.
    Type swiftParamTy;
    if (kind == SpecialMethodKind::NSDictionarySubscriptGetter &&
        paramTy->isObjCIdType()) {
      swiftParamTy = getNSCopyingType();
    }
    if (!swiftParamTy)
      swiftParamTy = importType(paramTy, ImportTypeKind::Parameter);
    if (!swiftParamTy)
      return Type();

    // Figure out the name for this parameter.
    Identifier bodyName = importName(param->getDeclName());
    Identifier name = bodyName;
    if ((index > 0 || kind == SpecialMethodKind::Constructor) &&
        index < selector.getNumArgs()) {
      // For parameters after the first, or all parameters in a constructor,
      // the name comes from the selector.
      name = importName(selector.getIdentifierInfoForSlot(index));

      // For the first selector piece in a constructor, strip off the 'init'
      // prefer and lowercase the first letter of the remainder (unless the
      // second letter is also uppercase, in which case we probably have an
      // acronym anyway).
      if (index == 0 && kind == SpecialMethodKind::Constructor &&
          !name.empty()) {
        llvm::SmallString<32> buffer;
        auto newName = getFirstInitParameterName(name.str(), buffer);
        if (newName.empty())
          name = Identifier();
        else
          name = SwiftContext.getIdentifier(newName);
      }
    }

    // Compute the pattern to put into the body.
    Pattern *bodyPattern;
    if (bodyName.empty()) {
      bodyPattern = new (SwiftContext) AnyPattern(SourceLoc());
    } else {
      auto bodyVar
        = new (SwiftContext) VarDecl(/*static*/ false, /*IsVal*/ false,
                                     importSourceLoc(param->getLocation()),
                                     bodyName, swiftParamTy, firstClangModule);
      bodyVar->setClangNode(param);
      bodyPattern = new (SwiftContext) NamedPattern(bodyVar);
    }
    bodyPattern->setType(swiftParamTy);
    bodyPattern
      = new (SwiftContext) TypedPattern(bodyPattern,
                                        TypeLoc::withoutLoc(swiftParamTy));
    bodyPattern->setType(swiftParamTy);
    bodyPatternElts.push_back(TuplePatternElt(bodyPattern));

    // Compute the pattern to put into the argument list, which may be
    // different (when there is a selector involved).
    Pattern *argPattern = bodyPattern;
    if (bodyName != name) {
      if (name.empty()) {
        argPattern = new (SwiftContext) AnyPattern(SourceLoc(),
                                                   /*Implicit=*/true);
      } else {
        auto argVar = new (SwiftContext) VarDecl(/*static*/ false,
                                                 /*IsVal*/ false,
                                                 SourceLoc(), name,
                                                 swiftParamTy,
                                                 firstClangModule);
        argVar->setImplicit();
        argVar->setClangNode(param);
        argPattern = new (SwiftContext) NamedPattern(argVar);
      }
      argPattern->setType(swiftParamTy);

      argPattern
        = new (SwiftContext) TypedPattern(argPattern,
                                          TypeLoc::withoutLoc(swiftParamTy),
                                          /*Implicit=*/true);
      argPattern->setType(swiftParamTy);
    }
    argPatternElts.push_back(TuplePatternElt(argPattern));
    
    if (argPattern != bodyPattern && pHasSelectorStyleSignature)
      *pHasSelectorStyleSignature = true;

    // Add the tuple elements for the function types.
    swiftArgParams.push_back(TupleTypeElt(swiftParamTy, name));
    swiftBodyParams.push_back(TupleTypeElt(swiftParamTy, bodyName));
    ++index;
  }

  // If we have a constructor with no parameters and a unary selector that is
  // not 'init', synthesize a Void parameter with the name following 'init',
  // suitably modified for a parameter name.
  if (kind == SpecialMethodKind::Constructor && selector.isUnarySelector() &&
      params.empty()) {
    llvm::SmallString<32> buffer;
    auto paramName = getFirstInitParameterName(
                       selector.getIdentifierInfoForSlot(0)->getName(),
                       buffer);
    if (!paramName.empty()) {
      auto name = SwiftContext.getIdentifier(paramName);
      auto type = TupleType::getEmpty(SwiftContext);
      auto var = new (SwiftContext) VarDecl(/*static*/ false,
                                            /*IsVal*/ true,
                                            SourceLoc(), name, type,
                                            firstClangModule);
      Pattern *pattern = new (SwiftContext) NamedPattern(var);
      pattern->setType(type);
      pattern = new (SwiftContext) TypedPattern(pattern,
                                                TypeLoc::withoutLoc(type));
      pattern->setType(type);

      argPatternElts.push_back(TuplePatternElt(pattern));
      bodyPatternElts.push_back(TuplePatternElt(pattern));
      swiftArgParams.push_back(TupleTypeElt(type, name));
      swiftBodyParams.push_back(TupleTypeElt(type, name));
    }
  }

  // Form the parameter tuples.
  auto argParamsTy = TupleType::get(swiftArgParams, SwiftContext);
  auto bodyParamsTy = TupleType::get(swiftBodyParams, SwiftContext);

  // Form the body and argument patterns.
  bodyPatterns.push_back(TuplePattern::create(SwiftContext, SourceLoc(),
                                              bodyPatternElts, SourceLoc()));
  bodyPatterns.back()->setType(bodyParamsTy);
  argPatterns.push_back(TuplePattern::create(SwiftContext, SourceLoc(),
                                             argPatternElts, SourceLoc(),
                                             false, SourceLoc(),
                                             /*Implicit=*/true));
  argPatterns.back()->setType(argParamsTy);

  // Form the function type.
  return FunctionType::get(argParamsTy, swiftResultTy);
}

Module *ClangImporter::Implementation::getStdlibModule() {
  return SwiftContext.getStdlibModule();
}

Module *ClangImporter::Implementation::getNamedModule(StringRef name) {
  return SwiftContext.getLoadedModule(SwiftContext.getIdentifier(name));
}

bool ClangImporter::Implementation::hasFoundationModule() {
  if (!checkedFoundationModule) {
    Identifier name = SwiftContext.getIdentifier(FOUNDATION_MODULE_NAME);
    auto mod = SwiftContext.getModule({ {name, SourceLoc()} });
    checkedFoundationModule = (mod != nullptr);
  }
  return checkedFoundationModule.getValue();
}


Type ClangImporter::Implementation::getNamedSwiftType(Module *module,
                                                      StringRef name) {
  if (!module)
    return Type();

  // Look for the type.
  UnqualifiedLookup lookup(SwiftContext.getIdentifier(name), module, nullptr);
  if (auto type = lookup.getSingleTypeResult()) {
    return type->getDeclaredType();
  }

  return Type();
}

Type
ClangImporter::Implementation::
getNamedSwiftTypeSpecialization(Module *module, StringRef name,
                                ArrayRef<Type> args) {
  if (!module)
    return Type();

  UnqualifiedLookup lookup(SwiftContext.getIdentifier(name), module, nullptr);
  if (TypeDecl *typeDecl = lookup.getSingleTypeResult()) {
    if (auto nominalDecl = dyn_cast<NominalTypeDecl>(typeDecl)) {
      if (auto params = nominalDecl->getGenericParams()) {
        if (params->size() == args.size()) {
          auto *BGT = BoundGenericType::get(nominalDecl, Type(), args);
          // FIXME: How do we ensure that this type gets validated?
          // Instead of going through the type checker, we do this hack to
          // create substitutions.
          SwiftContext.createTrivialSubstitutions(
              BGT->getCanonicalType()->castTo<BoundGenericType>());
          return BGT;
        }
      }
    }
  }

  return Type();
}

Type ClangImporter::Implementation::getNSObjectType() {
  if (NSObjectTy)
    return NSObjectTy;

  auto &sema = Instance->getSema();

  // Map the name. If we can't represent the Swift name in Clang, bail out now.
  auto clangName = &getClangASTContext().Idents.get("NSObject");

  // Perform name lookup into the global scope.
  // FIXME: Map source locations over.
  clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                   clang::Sema::LookupOrdinaryName);
  if (!sema.LookupName(lookupResult, /*Scope=*/0)) {
    return Type();
  }

  for (auto decl : lookupResult) {
    if (auto swiftDecl = importDecl(decl->getUnderlyingDecl())) {
      if (auto classDecl = dyn_cast<ClassDecl>(swiftDecl)) {
        NSObjectTy = classDecl->getDeclaredType();
        return NSObjectTy;
      }
    }
  }

  return Type();
}

Type ClangImporter::Implementation::getNSCopyingType() {
  auto &sema = Instance->getSema();
  auto clangName = &getClangASTContext().Idents.get("NSCopying");
  assert(clangName);

  // Perform name lookup into the global scope.
  clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                   clang::Sema::LookupObjCProtocolName);
  if (!sema.LookupName(lookupResult, /*Scope=*/0))
    return Type();

  for (auto decl : lookupResult) {
    if (auto swiftDecl = importDecl(decl->getUnderlyingDecl())) {
      if (auto protoDecl = dyn_cast<ProtocolDecl>(swiftDecl)) {
        return protoDecl->getDeclaredType();
      }
    }
  }

  return Type();
}

