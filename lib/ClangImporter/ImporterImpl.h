//===--- ImporterImpl.h - Import Clang Modules - Implementation------------===//
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
// This file provides the implementation class definitions for the Clang
// module loader.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_CLANG_IMPORTER_IMPL_H
#define SWIFT_CLANG_IMPORTER_IMPL_H

#include "swift/ClangImporter/ClangImporter.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/Type.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Basic/Optional.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include <set>

namespace clang {
class APValue;
class Decl;
class DeclarationName;
class EnumDecl;
class MacroInfo;
class NamedDecl;
class ObjCInterfaceDecl;
class ObjCMethodDecl;
class ObjCPropertyDecl;
class ParmVarDecl;
class Parser;
class QualType;
class TypedefNameDecl;
}

namespace swift {

class ASTContext;
class ClangModuleUnit;
class ClassDecl;
class ConstructorDecl;
class Decl;
class DeclContext;
class Expr;
class ExtensionDecl;
class FuncDecl;
class Identifier;
class Pattern;
class SubscriptDecl;
class ValueDecl;

/// \brief Describes the kind of conversion to apply to a constant value.
enum class ConstantConvertKind {
  /// \brief No conversion required.
  None,
  /// \brief Coerce the constant to the given type.
  Coerce,
  /// \brief Construct the given type from the constant value.
  Construction,
  /// \brief Perform an unchecked downcast to the given type.
  Downcast
};

/// \brief Describes the kind of type import we're performing.
enum class ImportTypeKind {
  /// \brief Import an abstract type reference, like the underlying
  /// type of a typedef.
  ///
  /// This provides special treatment for class reference types:
  /// the typedef itself is left as a non-optional type.
  Abstract,

  /// \brief Import the type of a literal value.
  Value,

  /// \brief Import the declared type of a variable.
  Variable,

  /// \brief Import the declared type of an audited variable.
  ///
  /// This is exactly like ImportTypeKind::Variable, except it
  /// disables wrapping CF class types in Unmanaged.
  AuditedVariable,

  /// \brief Import the result type of a function.
  ///
  /// This provides special treatment for 'void', among other things, and
  /// enables the conversion of bridged types.
  Result,

  /// \brief Import the result type of an audited function.
  ///
  /// This is exactly like ImportTypeKind::Result, except it
  /// disables wrapping CF class types in Unmanaged.
  AuditedResult,

  /// \brief Import the type of a function parameter.
  ///
  /// This provides special treatment for C++ references (which become
  /// [inout] parameters) and C pointers (which become magic [inout]-able types),
  /// among other things, and enables the conversion of bridged types.
  /// Parameters are always considered CF-audited.
  Parameter,
  
  /// \brief Import the type pointed to by a pointer or reference.
  ///
  /// This provides special treatment for pointer-to-ObjC-pointer
  /// types, which get imported as pointers to *checked* optional,
  /// *Pointer<NSFoo?>, instead of implicitly unwrapped optional as usual.
  Pointee,

  /// \brief Import the type of an ObjC property.
  ///
  /// This enables the conversion of bridged types. Properties are always
  /// considered CF-audited.
  Property,

  /// \brief Import the type of an ObjC property accessor.
  ///
  /// This behaves exactly like Property except that it accepts Void.
  PropertyAccessor,

  /// \brief Import the underlying type of an enum.
  ///
  /// This provides special treatment for 'NSUInteger'.
  Enum
};

/// \brief Describes the kind of the C type that can be mapped to a stdlib
/// swift type.
enum class MappedCTypeKind {
  UnsignedInt,
  SignedInt,
  UnsignedWord,
  SignedWord,
  FloatIEEEsingle,
  FloatIEEEdouble,
  FloatX87DoubleExtended,
  VaList,
  ObjCBool,
  ObjCSel,
  ObjCId,
  ObjCClass,
};

/// \brief Describes what to do with the C name of a type that can be mapped to
/// a Swift standard library type.
enum class MappedTypeNameKind {
  DoNothing,
  DefineOnly,
  DefineAndUse
};

/// \brief Bitmask constants for language dialects where a certain C to Swift
/// type mapping applies.
enum class MappedLanguages {
  ObjC1 = 0x1,
  All = ObjC1
};

/// \brief Describes certain kinds of methods that need to be specially
/// handled by the importer.
enum class SpecialMethodKind {
  Regular,
  Constructor,
  PropertyAccessor,
  NSDictionarySubscriptGetter
};

#define SWIFT_NATIVE_ANNOTATION_STRING "__swift native"

/// \brief Implementation of the Clang importer.
class LLVM_LIBRARY_VISIBILITY ClangImporter::Implementation 
  : public LazyMemberLoader 
{
  friend class ClangImporter;

public:
  /// \brief Describes how a particular C enumeration type will be imported
  /// into Swift. All of the possibilities have the same storage
  /// representation, but can be used in different ways.
  enum class EnumKind {
    /// \brief The enumeration type should map to an enum, which means that
    /// all of the cases are independent.
    Enum,
    /// \brief The enumeration type should map to an option set, which means that
    /// the constants represent combinations of independent flags.
    Options,
    /// \brief The enumeration type should map to a distinct type, but we don't
    /// know the intended semantics of the enum constants, so conservatively
    /// map them to independent constants.
    Unknown,
    /// \brief The enumeration constants should simply map to the appropriate
    /// integer values.
    Constants
  };

  Implementation(ASTContext &ctx, const ClangImporterOptions &opts);
  ~Implementation();

  /// \brief Swift AST context.
  ASTContext &SwiftContext;

  const bool SplitPrepositions;
  const bool InferImplicitProperties;

private:
  /// \brief A count of the number of load module operations.
  /// FIXME: Horrible, horrible hack for \c loadModule().
  unsigned ImportCounter = 0;

  /// \brief The value of \c ImportCounter last time when imported modules were
  /// verified.
  unsigned VerifiedImportCounter = 0;

  /// \brief Clang compiler invocation.
  llvm::IntrusiveRefCntPtr<clang::CompilerInvocation> Invocation;

  /// \brief Clang compiler instance, which is used to actually load Clang
  /// modules.
  std::unique_ptr<clang::CompilerInstance> Instance;

  /// \brief Clang compiler action, which is used to actually run the
  /// parser.
  std::unique_ptr<clang::FrontendAction> Action;

  /// \brief Clang parser, which is used to load textual headers.
  std::unique_ptr<clang::Parser> Parser;

  /// The active type checker, or null if there is no active type checker.
  LazyResolver *typeResolver = nullptr;

public:
  /// \brief Mapping of already-imported declarations.
  llvm::DenseMap<const clang::Decl *, Decl *> ImportedDecls;

  /// \brief The set of "special" typedef-name declarations, which are
  /// mapped to specific Swift types.
  ///
  /// Normal typedef-name declarations imported into Swift will maintain
  /// equality between the imported declaration's underlying type and the
  /// import of the underlying type. A typedef-name declaration is special
  /// when this is not the case, e.g., Objective-C's "BOOL" has an underlying
  /// type of "signed char", but is mapped to a special Swift struct type
  /// ObjCBool.
  llvm::SmallDenseMap<const clang::TypedefNameDecl *, MappedTypeNameKind, 16>
    SpecialTypedefNames;

  /// Mapping from Objective-C selectors to method names.
  llvm::DenseMap<std::pair<ObjCSelector, char>, DeclName> SelectorMappings;

  /// Mapping that describes the designated initializers of
  /// Objective-C classes.
  ///
  /// This table, generated from DesignatedInits.def, describes the
  /// designated initializers for a specific set of known
  /// classes. When this information is available, and the class
  llvm::StringMap<llvm::SmallVector<ObjCSelector, 1> >
    KnownDesignatedInits;

  /// Populate the table of known designated initializers from the
  /// DesignatedInits.def file.
  void populateKnownDesignatedInits();

  /// Determine whether the given class has designated initializers,
  /// consulting 
  bool hasDesignatedInitializers(const clang::ObjCInterfaceDecl *classDecl);

  /// Determine whether the given method is a designated initializer
  /// of the given class.
  bool isDesignatedInitializer(const clang::ObjCInterfaceDecl *classDecl,
                               const clang::ObjCMethodDecl *method);

  /// \brief Typedefs that we should not be importing.  We should be importing
  /// underlying decls instead.
  llvm::DenseSet<const clang::Decl *> SuperfluousTypedefs;

  using ClangDeclAndFlag = llvm::PointerIntPair<const clang::Decl *, 1, bool>;

  /// \brief Mapping of already-imported declarations from protocols, which
  /// can (and do) get replicated into classes.
  llvm::DenseMap<std::pair<ClangDeclAndFlag, DeclContext *>, Decl *>
    ImportedProtocolDecls;

  /// \brief Mapping of already-imported macros.
  llvm::DenseMap<clang::MacroInfo *, ValueDecl *> ImportedMacros;

  // FIXME: An extra level of caching of visible decls, since lookup needs to
  // be filtered by module after the fact.
  SmallVector<ValueDecl *, 0> CachedVisibleDecls;
  enum class CacheState {
    Invalid,
    InProgress,
    Valid
  } CurrentCacheState = CacheState::Invalid;

private:
  /// \brief Generation number that is used for crude versioning.
  ///
  /// This value is incremented every time a new module is imported.
  unsigned Generation = 1;

  /// \brief A cached set of extensions for a particular Objective-C class.
  struct CachedExtensions {
    CachedExtensions()
      : Extensions(nullptr), Generation(0) { }

    CachedExtensions(const CachedExtensions &) = delete;
    CachedExtensions &operator=(const CachedExtensions &) = delete;

    CachedExtensions(CachedExtensions &&other)
      : Extensions(other.Extensions), Generation(other.Generation)
    {
      other.Extensions = nullptr;
      other.Generation = 0;
    }

    CachedExtensions &operator=(CachedExtensions &&other) {
      delete Extensions;
      Extensions = other.Extensions;
      Generation = other.Generation;
      other.Extensions = nullptr;
      other.Generation = 0;
      return *this;
    }

    ~CachedExtensions() { delete Extensions; }

    /// \brief The cached extensions.
    SmallVector<ExtensionDecl *, 4> *Extensions;

    /// \brief Generation number used to tell when this cache has gone stale.
    unsigned Generation;
  };

  void bumpGeneration() {
    ++Generation;
    SwiftContext.bumpGeneration();
    CachedVisibleDecls.clear();
    CurrentCacheState = CacheState::Invalid;
  }

  /// \brief Cache of the class extensions.
  llvm::DenseMap<ClassDecl *, CachedExtensions> ClassExtensions;

public:
  /// \brief Keep track of subscript declarations based on getter/setter
  /// pairs.
  llvm::DenseMap<std::pair<FuncDecl *, FuncDecl *>, SubscriptDecl *> Subscripts;

  /// \brief Keep track of enum constant name prefixes in enums.
  llvm::DenseMap<const clang::EnumDecl *, StringRef> EnumConstantNamePrefixes;
  
  /// \brief Keep track of enum constant values that have been imported.
  std::set<std::pair<const clang::EnumDecl *, llvm::APSInt>>
    EnumConstantValues;

  /// \brief Keep track of initializer declarations that correspond to
  /// imported methods.
  llvm::DenseMap<std::pair<const clang::ObjCMethodDecl *, DeclContext *>,
                 ConstructorDecl *>
    Constructors;

private:
  /// \brief NSObject, imported into Swift.
  Type NSObjectTy;

  /// A pair containing a ClangModuleUnit,
  /// and whether the adapters of its re-exported modules have all been forced
  /// to load already.
  using ModuleInitPair = llvm::PointerIntPair<ClangModuleUnit *, 1, bool>;

public:
  /// A map from Clang modules to their Swift wrapper modules.
  llvm::SmallDenseMap<const clang::Module *, ModuleInitPair, 16> ModuleWrappers;

  /// The module unit that contains declarations from imported headers.
  ClangModuleUnit *ImportedHeaderUnit = nullptr;

  /// The modules re-exported by imported headers.
  llvm::SmallVector<Module::ImportedModule, 8> ImportedHeaderExports;

  /// The modules that requested imported headers.
  ///
  /// These are used to look up Swift classes forward-declared with \@class.
  TinyPtrVector<Module *> ImportedHeaderOwners;


  /// \brief Clang's objectAtIndexedSubscript: selector.
  clang::Selector objectAtIndexedSubscript;

  /// \brief Clang's setObjectAt:indexedSubscript: selector.
  clang::Selector setObjectAtIndexedSubscript;

  /// \brief Clang's objectForKeyedSubscript: selector.
  clang::Selector objectForKeyedSubscript;

  /// \brief Clang's setObject:forKeyedSubscript: selector.
  clang::Selector setObjectForKeyedSubscript;

private:
  Optional<Module *> checkedFoundationModule;

  /// External Decls that we have imported but not passed to the ASTContext yet.
  SmallVector<Decl *, 4> RegisteredExternalDecls;

  unsigned NumCurrentImportingEntities = 0;

  void startedImportingEntity();
  void finishedImportingEntity();
  void finishPendingActions();

  struct ImportingEntityRAII {
    Implementation &Impl;

    ImportingEntityRAII(Implementation &Impl) : Impl(Impl) {
      Impl.startedImportingEntity();
    }
    ~ImportingEntityRAII() {
      Impl.finishedImportingEntity();
    }
  };

public:
  void registerExternalDecl(Decl *D) {
    RegisteredExternalDecls.push_back(D);
  }

  /// \brief Retrieve the Clang AST context.
  clang::ASTContext &getClangASTContext() const {
    return Instance->getASTContext();
  }

  /// \brief Retrieve the Clang Sema object.
  clang::Sema &getClangSema() const {
    return Instance->getSema();
  }

  /// \brief Retrieve the Clang AST context.
  clang::Preprocessor &getClangPreprocessor() const {
    return Instance->getPreprocessor();
  }

  /// Returns the module \p D comes from, or \c Nothing if \p D does not have
  /// a valid associated module.
  ///
  /// The returned module may be null (but not \c Nothing) if \p D comes from
  /// imported header.
  Optional<clang::Module *>
  getClangSubmoduleForDecl(const clang::Decl *D,
                           bool allowForwardDeclaration = false);

  /// \brief Retrieve the imported module that should contain the given
  /// Clang decl.
  ClangModuleUnit *getClangModuleForDecl(const clang::Decl *D,
                                         bool allowForwardDeclaration = false);

  /// Returns the module \p MI comes from, or \c Nothing if \p MI does not have
  /// a valid associated module.
  ///
  /// The returned module may be null (but not \c Nothing) if \p MI comes from
  /// an imported header.
  Optional<clang::Module *>
  getClangSubmoduleForMacro(const clang::MacroInfo *MI);

  ClangModuleUnit *getClangModuleForMacro(const clang::MacroInfo *MI);

  /// \brief Import the given Swift identifier into Clang.
  clang::DeclarationName importName(Identifier name);
  
  /// \brief Import the given Clang name into Swift.
  ///
  /// \param name The Clang name to map into Swift.
  ///
  /// \param suffix The suffix to append to the Clang name to produce the
  /// Swift name.
  ///
  /// \param removePrefix The prefix to remove from the Clang name to produce
  /// the Swift name.
  Identifier importName(clang::DeclarationName name, StringRef suffix = "",
                        StringRef removePrefix = "");

  /// Import an Objective-C selector.
  ObjCSelector importSelector(clang::Selector selector);

  /// Import a Swift name as a Clang selector.
  clang::Selector importSelector(DeclName name, bool allowSimpleName = true);

  /// Map the given selector to a declaration name.
  ///
  /// \param selector The selector to map.
  ///
  /// \param isInitializer Whether this name should be mapped as an
  /// initializer.
  DeclName mapSelectorToDeclName(ObjCSelector selector, bool isInitializer);

  /// Try to map the given selector, which may be the name of a factory method,
  /// to the name of an initializer.
  ///
  /// \param selector The selector to map.
  ///
  /// \param className The name of the class in which the method occurs.
  ///
  /// \returns the initializer name for this factory method, or an empty
  /// name if this selector does not fit the pattern.
  DeclName mapFactorySelectorToInitializerName(ObjCSelector selector,
                                               StringRef className);

  /// \brief Import the given Swift source location into Clang.
  clang::SourceLocation importSourceLoc(SourceLoc loc);

  /// \brief Import the given Clang source location into Swift.
  SourceLoc importSourceLoc(clang::SourceLocation loc);

  /// \brief Import the given Clang source range into Swift.
  SourceRange importSourceRange(clang::SourceRange loc);

  /// \brief Import the given Clang preprocessor macro as a Swift value decl.
  ///
  /// \returns The imported declaration, or null if the macro could not be
  /// translated into Swift.
  ValueDecl *importMacro(Identifier name, clang::MacroInfo *macro);

  /// \brief Classify the given Clang enumeration type to describe how it
  /// should be imported 
  EnumKind classifyEnum(const clang::EnumDecl *decl);

  /// If we already imported a given decl, return the corresponding Swift decl.
  /// Otherwise, return nullptr.
  Decl *importDeclCached(const clang::NamedDecl *ClangDecl);

  Decl *importDeclImpl(const clang::NamedDecl *ClangDecl,
                       bool &TypedefIsSuperfluous,
                       bool &HadForwardDeclaration);

  Decl *importDeclAndCacheImpl(const clang::NamedDecl *ClangDecl,
                               bool SuperfluousTypedefsAreTransparent);

  /// \brief Same as \c importDeclReal, but for use inside importer
  /// implementation.
  ///
  /// Unlike \c importDeclReal, this function for convenience transparently
  /// looks through superfluous typedefs and returns the imported underlying
  /// decl in that case.
  Decl *importDecl(const clang::NamedDecl *ClangDecl) {
    return importDeclAndCacheImpl(ClangDecl,
                                  /*SuperfluousTypedefsAreTransparent=*/true);
  }

  /// \brief Import the given Clang declaration into Swift.  Use this function
  /// outside of the importer implementation, when importing a decl requested by
  /// Swift code.
  ///
  /// \returns The imported declaration, or null if this declaration could
  /// not be represented in Swift.
  Decl *importDeclReal(const clang::NamedDecl *ClangDecl) {
    return importDeclAndCacheImpl(ClangDecl,
                                  /*SuperfluousTypedefsAreTransparent=*/false);
  }

  /// \brief Import a cloned version of the given declaration, which is part of
  /// an Objective-C protocol and currently must be a method or property, into
  /// the given declaration context.
  ///
  /// \returns The imported declaration, or null if this declaration could not
  /// be represented in Swift.
  Decl *importMirroredDecl(const clang::NamedDecl *decl, DeclContext *dc,
                           bool forceClassMethod = false);

  /// \brief Import the given Clang declaration context into Swift.
  ///
  /// Usually one will use \c importDeclContextOf instead.
  ///
  /// \returns The imported declaration context, or null if it could not
  /// be converted.
  DeclContext *importDeclContextImpl(const clang::DeclContext *dc);

  /// \brief Import the declaration context of a given Clang declaration into
  /// Swift.
  ///
  /// \returns The imported declaration context, or null if it could not
  /// be converted.
  DeclContext *importDeclContextOf(const clang::Decl *D);

  /// \brief Create a new named constant with the given value.
  ///
  /// \param name The name of the constant.
  /// \param dc The declaration context into which the name will be introduced.
  /// \param type The type of the named constant.
  /// \param value The value of the named constant.
  /// \param convertKind How to convert the constant to the given type.
  /// \param isStatic Whether the constant should be a static member of \p dc.
  ValueDecl *createConstant(Identifier name, DeclContext *dc,
                            Type type, const clang::APValue &value,
                            ConstantConvertKind convertKind,
                            bool isStatic,
                            ClangNode ClangN);

  /// \brief Create a new named constant with the given value.
  ///
  /// \param name The name of the constant.
  /// \param dc The declaration context into which the name will be introduced.
  /// \param type The type of the named constant.
  /// \param value The value of the named constant.
  /// \param convertKind How to convert the constant to the given type.
  /// \param isStatic Whether the constant should be a static member of \p dc.
  ValueDecl *createConstant(Identifier name, DeclContext *dc,
                            Type type, StringRef value,
                            ConstantConvertKind convertKind,
                            bool isStatic,
                            ClangNode ClangN);

  /// \brief Create a new named constant using the given expression.
  ///
  /// \param name The name of the constant.
  /// \param dc The declaration context into which the name will be introduced.
  /// \param type The type of the named constant.
  /// \param valueExpr An expression to use as the value of the constant.
  /// \param convertKind How to convert the constant to the given type.
  /// \param isStatic Whether the constant should be a static member of \p dc.
  ValueDecl *createConstant(Identifier name, DeclContext *dc,
                            Type type, Expr *valueExpr,
                            ConstantConvertKind convertKind,
                            bool isStatic,
                            ClangNode ClangN);

  /// \brief Retrieve the standard library module.
  Module *getStdlibModule();

  /// \brief Retrieve the named module.
  ///
  /// \param name The name of the module.
  ///
  /// \returns The named module, or null if the module has not been imported.
  Module *getNamedModule(StringRef name);

  /// \brief Returns true if the "Foundation" module can be loaded.
  ///
  /// After this has been called, the Foundation module will or won't be loaded
  /// into the ASTContext.
  bool hasFoundationModule();

  /// \brief Returns the "Foundation" module.  Requires that it is already
  /// loaded (such as being preceded by a call to hasFoundationModule()).
  Module *getFoundationModule() {
    return checkedFoundationModule.getValue();
  }

  /// \brief Retrieves the Swift wrapper for the given Clang module, creating
  /// it if necessary.
  ClangModuleUnit *getWrapperForModule(ClangImporter &importer,
                                       const clang::Module *underlying);

  /// \brief Constructs a Swift module for the given Clang module.
  Module *finishLoadingClangModule(ClangImporter &importer,
                                   const clang::Module *clangModule,
                                   bool preferAdapter);

  /// \brief Retrieve the named Swift type, e.g., Int32.
  ///
  /// \param module The name of the module in which the type should occur.
  ///
  /// \param name The name of the type to find.
  ///
  /// \returns The named type, or null if the type could not be found.
  Type getNamedSwiftType(Module *module, StringRef name);

  /// \brief Retrieve a specialization of the the named Swift type, e.g.,
  /// UnsafePointer<T>.
  ///
  /// \param module The name of the module in which the type should occur.
  ///
  /// \param name The name of the type to find.
  ///
  /// \param args The arguments to use in the specialization.
  ///
  /// \returns The named type, or null if the type could not be found.
  Type getNamedSwiftTypeSpecialization(Module *module, StringRef name,
                                       ArrayRef<Type> args);

  /// \brief Retrieve the NSObject type.
  Type getNSObjectType();

  /// \brief Retrieve the NSCopying protocol type.
  Type getNSCopyingType();

  /// \brief Look up and attempt to import a Clang declaration with
  /// the given name.
  Decl *importDeclByName(StringRef name);

  /// \brief Import the given Clang type into Swift.
  ///
  /// \param type The Clang type to import.
  ///
  /// \param kind The kind of type import we're performing.
  ///
  /// \returns The imported type, or null if this type could
  /// not be represented in Swift.
  Type importType(clang::QualType type, ImportTypeKind kind);

  /// \brief Import the given function type.
  ///
  /// This routine should be preferred when importing function types for
  /// which we have actual function parameters, e.g., when dealing with a
  /// function declaration, because it produces a function type whose input
  /// tuple has argument names.
  ///
  /// \param clangDecl The underlying declaration, if any; should only be
  ///   considered for any attributes it might carry.
  /// \param resultType The result type of the function.
  /// \param params The parameter types to the function.
  /// \param isVariadic Whether the function is variadic.
  /// \param isNoReturn Whether the function is noreturn.
  /// \param bodyPatterns The patterns visible inside the function body.
  ///
  /// \returns the imported function type, or null if the type cannot be
  /// imported.
  Type importFunctionType(const clang::Decl *clangDecl,
                          clang::QualType resultType,
                          ArrayRef<const clang::ParmVarDecl *> params,
                          bool isVariadic, bool isNoReturn,
                          SmallVectorImpl<Pattern*> &bodyPatterns);

  Type importPropertyType(const clang::ObjCPropertyDecl *clangDecl);

  /// \brief Import the type of an Objective-C method.
  ///
  /// This routine should be preferred when importing function types for
  /// which we have actual function parameters, e.g., when dealing with a
  /// function declaration, because it produces a function type whose input
  /// tuple has argument names.
  ///
  /// \param clangDecl The underlying declaration, if any; should only be
  ///   considered for any attributes it might carry.
  /// \param resultType The result type of the function.
  /// \param params The parameter types to the function.
  /// \param isVariadic Whether the function is variadic.
  /// \param isNoReturn Whether the function is noreturn.
  /// \param bodyPatterns The patterns visible inside the function body.
  ///   whether the created arg/body patterns are different (selector-style).
  /// \param methodName The name of the imported method.
  /// \param kind Controls whether we're building a type for a method that
  ///        needs special handling.
  ///
  /// \returns the imported function type, or null if the type cannot be
  /// imported.
  Type importMethodType(const clang::Decl *clangDecl,
                        clang::QualType resultType,
                        ArrayRef<const clang::ParmVarDecl *> params,
                        bool isVariadic, bool isNoReturn,
                        SmallVectorImpl<Pattern*> &bodyPatterns,
                        DeclName methodName,
                        SpecialMethodKind kind);

  /// \brief Determine whether the given typedef-name is "special", meaning
  /// that it has performed some non-trivial mapping of its underlying type
  /// based on the name of the typedef.
  Optional<MappedTypeNameKind>
  getSpecialTypedefKind(clang::TypedefNameDecl *decl);

  /// \brief Look up a name, accepting only typedef results.
  const clang::TypedefNameDecl *lookupTypedef(clang::DeclarationName);

  LazyResolver *getTypeResolver() const {
    return typeResolver;
  }
  void setTypeResolver(LazyResolver *newResolver) {
    assert((!typeResolver || !newResolver) && "already have a type resolver");
    typeResolver = newResolver;
  }

  virtual ArrayRef<Decl *> loadAllMembers(const Decl *D,
                                          uint64_t unused) override;

  template <typename DeclTy, typename ...Targs>
  DeclTy *createDeclWithClangNode(ClangNode ClangN, Targs &&... Args) {
    static_assert(alignof(DeclTy) <= alignof(void*),
                  "adding ClangNode violates alignment");
    assert(ClangN);
    void *Mem = SwiftContext.Allocate(sizeof(DeclTy) + sizeof(void *),
                                      alignof(DeclTy));
    void *DeclPtr = reinterpret_cast<void **>(Mem) + 1;
    auto D = ::new (DeclPtr) DeclTy(std::forward<Targs>(Args)...);
    D->setClangNode(ClangN);
    D->setEarlyAttrValidation(true);
    return D;
  }
};

}

#endif
