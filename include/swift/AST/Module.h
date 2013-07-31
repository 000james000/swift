//===--- Module.h - Swift Language Module ASTs ------------------*- C++ -*-===//
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
// This file defines the Module class and its subclasses.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_MODULE_H
#define SWIFT_MODULE_H

#include "swift/AST/DeclContext.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Type.h"
#include "swift/Basic/Optional.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringMap.h"

namespace clang {
  class Module;
}

namespace swift {
  class ASTContext;
  class BraceStmt;
  class Component;
  class Decl;
  enum class DeclKind : uint8_t;
  class ExtensionDecl;
  class InfixOperatorDecl;
  class LookupCache;
  class ModuleLoader;
  class NameAliasType;
  class UnionElementDecl;
  class OperatorDecl;
  class PostfixOperatorDecl;
  class PrefixOperatorDecl;
  struct PrintOptions;
  class TupleType;
  class Type;
  class TypeAliasDecl;
  class ValueDecl;
  class VisibleDeclConsumer;
  
  /// NLKind - This is a specifier for the kind of name lookup being performed
  /// by various query methods.
  enum class NLKind {
    UnqualifiedLookup,
    QualifiedLookup
  };

/// Constants used to customize name lookup.
enum NameLookupOptions {
  /// Visit supertypes (such as superclasses or inherited protocols)
  /// and their extensions as well as the current extension.
  NL_VisitSupertypes = 0x01,

  /// Consider default definitions within protocols to which the lookup
  /// context type conforms.
  NL_DefaultDefinitions = 0x02,

  /// Remove non-visible declarations from the set of results.
  NL_RemoveNonVisible = 0x04,

  /// Remove overridden declarations from the set of results.
  NL_RemoveOverridden = 0x08,

  /// The default set of options used for qualified name lookup.
  NL_QualifiedDefault = NL_VisitSupertypes | NL_DefaultDefinitions |
                        NL_RemoveNonVisible | NL_RemoveOverridden,

  /// The default set of options used for unqualified name lookup.
  NL_UnqualifiedDefault = NL_VisitSupertypes |
                          NL_RemoveNonVisible | NL_RemoveOverridden,

  /// The default set of options used for constructor lookup.
  NL_Constructor = NL_RemoveNonVisible
};

/// Module - A unit of modularity.  The current translation unit is a
/// module, as is an imported module.
class Module : public DeclContext {
protected:
  mutable void *LookupCachePimpl;
  Component *Comp;
public:
  ASTContext &Ctx;
  Identifier Name;
  
  //===--------------------------------------------------------------------===//
  // AST Phase of Translation
  //===--------------------------------------------------------------------===//
  
  /// ASTStage - Defines what phases of parsing and semantic analysis are
  /// complete for the given AST.  This should only be used for assertions and
  /// verification purposes.
  enum ASTStage_t {
    /// Parsing is underway.
    Parsing,
    /// Parsing has completed.
    Parsed,
    /// Name binding has completed.
    NameBound,
    /// Type checking has completed.
    TypeChecked
  } ASTStage;

protected:
  Module(DeclContextKind Kind, Identifier Name, Component *C, ASTContext &Ctx)
  : DeclContext(Kind, nullptr), LookupCachePimpl(0),
    Comp(C), Ctx(Ctx), Name(Name), ASTStage(Parsing) {
    assert(Comp != nullptr || Kind == DeclContextKind::BuiltinModule);
  }

public:
  typedef ArrayRef<std::pair<Identifier, SourceLoc>> AccessPathTy;
  typedef std::pair<Module::AccessPathTy, Module*> ImportedModule;

  Component *getComponent() const {
    assert(Comp && "fetching component for the builtin module");
    return Comp;
  }
  
  /// lookupValue - Look up a (possibly overloaded) value set at top-level scope
  /// (but with the specified access path, which may come from an import decl)
  /// within the current module. This does a simple local lookup, not
  /// recursively looking through imports.  
  void lookupValue(AccessPathTy AccessPath, Identifier Name, NLKind LookupKind, 
                   SmallVectorImpl<ValueDecl*> &Result);
  
  /// lookupVisibleDecls - Find ValueDecls in the module and pass them to the
  /// given consumer object.
  void lookupVisibleDecls(AccessPathTy AccessPath,
                          VisibleDeclConsumer &Consumer,
                          NLKind LookupKind) const;

  /// Look for the set of declarations with the given name within a type,
  /// its extensions and, optionally, its supertypes.
  ///
  /// This routine performs name lookup within a given type, its extensions
  /// and, optionally, its supertypes and their extensions. It can eliminate
  /// non-visible, hidden, and overridden declarations from the result set.
  /// It does not, however, perform any filtering based on the semantic
  /// usefulness of the results.
  ///
  /// \param type The type to look into.
  ///
  /// \param name The name to search for.
  ///
  /// \param options Options that control name lookup, based on the
  /// \c NL_* constants in \c NameLookupOptions.
  ///
  /// \param decls Will be populated with the declarations found by name
  /// lookup.
  ///
  /// \returns true if anything was found.
  bool lookupQualified(Type type, Identifier name, unsigned options,
                       SmallVectorImpl<ValueDecl *> &decls);

  /// Look up an InfixOperatorDecl for the given operator
  /// name in this module (which must be NameBound) and return it, or return
  /// null if there is no operator decl. Returns Nothing if there was an error
  /// resolving the operator name (such as if there were conflicting importing
  /// operator declarations).
  Optional<InfixOperatorDecl *> lookupInfixOperator(Identifier name,
                                              SourceLoc diagLoc = SourceLoc());
  
  /// Look up an PrefixOperatorDecl for the given operator
  /// name in this module (which must be NameBound) and return it, or return
  /// null if there is no operator decl. Returns Nothing if there was an error
  /// resolving the operator name (such as if there were conflicting importing
  /// operator declarations).
  Optional<PrefixOperatorDecl *> lookupPrefixOperator(Identifier name,
                                              SourceLoc diagLoc = SourceLoc());
  /// Look up an PostfixOperatorDecl for the given operator
  /// name in this module (which must be NameBound) and return it, or return
  /// null if there is no operator decl. Returns Nothing if there was an error
  /// resolving the operator name (such as if there were conflicting importing
  /// operator declarations).
  Optional<PostfixOperatorDecl *> lookupPostfixOperator(Identifier name,
                                              SourceLoc diagLoc = SourceLoc());

  /// Looks up which modules are re-exported by this module.
  void getReexportedModules(SmallVectorImpl<ImportedModule> &modules) const;

  /// Perform an action for every module visible from this module.
  ///
  /// For most modules this means any re-exports, but for a translation unit
  /// all imports are considered.
  ///
  /// \param topLevelAccessPath If present, include the top-level module in the
  ///                           results, with the given access path.
  /// \param fn A callback of type bool(ImportedModule). Return \c false to
  ///           abort iteration.
  template <typename F>
  void forAllVisibleModules(Optional<AccessPathTy> topLevelAccessPath,
                            F fn);

  static bool classof(const DeclContext *DC) {
    return DC->isModuleContext();
  }

private:
  // Make placement new and vanilla new/delete illegal for DeclVarNames.
  void *operator new(size_t Bytes) throw() = delete;
  void operator delete(void *Data) throw() = delete;
  void *operator new(size_t Bytes, void *Mem) throw() = delete;
public:
  // Only allow allocation of Modules using the allocator in ASTContext
  // or by doing a placement new.
  void *operator new(size_t Bytes, ASTContext &C,
                     unsigned Alignment = alignof(Module));
};
  
/// TranslationUnit - This contains information about all of the decls and
/// external references in a translation unit, which is one file.
class TranslationUnit : public Module {
private:

  /// ImportedModules - This is the list of modules that are imported by this
  /// module.  This is filled in by the Name Binding phase.
  ArrayRef<ImportedModule> ImportedModules;

public:
  /// Kind - This is the sort of file the translation unit was parsed for, which
  /// can affect some type checking and other behavior.
  enum TUKind {
    Library,
    Main,
    REPL,
    SIL       // Came from a .sil file.
  } Kind;

  /// If this is true, then the translation unit is allowed to access the
  /// Builtin module with an explicit import.
  bool HasBuiltinModuleAccess = false;
  
  /// The list of top-level declarations for a translation unit.
  std::vector<Decl*> Decls;
  
  /// A map of operator names to InfixOperatorDecls.
  /// Populated during name binding; the mapping will be incomplete until name
  /// binding is complete.
  llvm::StringMap<InfixOperatorDecl*> InfixOperators;

  /// A map of operator names to PostfixOperatorDecls.
  /// Populated during name binding; the mapping will be incomplete until name
  /// binding is complete.
  llvm::StringMap<PostfixOperatorDecl*> PostfixOperators;

  /// A map of operator names to PrefixOperatorDecls.
  /// Populated during name binding; the mapping will be incomplete until name
  /// binding is complete.
  llvm::StringMap<PrefixOperatorDecl*> PrefixOperators;

  TranslationUnit(Identifier Name, Component *Comp, ASTContext &C, TUKind Kind)
    : Module(DeclContextKind::TranslationUnit, Name, Comp, C), Kind(Kind) {
  }
  
  /// ImportedModules - This is the list of modules that are imported by this
  /// module.  This is filled in as the first thing that the Name Binding phase
  /// does.
  ArrayRef<ImportedModule> getImportedModules() const {
    assert(ASTStage >= Parsed);
    return ImportedModules;
  }
  void setImportedModules(ArrayRef<ImportedModule> IM) {
    ImportedModules = IM;
  }

  void clearLookupCache();

  void dump() const;

  /// \brief Pretty-print the entire contents of this translation unit.
  ///
  /// \param os The stream to which the contents will be printed.
  void print(raw_ostream &os);

  /// \brief Pretty-print the contents of this translation unit.
  ///
  /// \param os The stream to which the contents will be printed.
  ///
  /// \param options Options controlling the printing process.
  void print(raw_ostream &os, const PrintOptions &options);

  static bool classof(const DeclContext *DC) {
    return DC->getContextKind() == DeclContextKind::TranslationUnit;
  }
};

  
/// BuiltinModule - This module represents the compiler's implicitly generated
/// declarations in the builtin module.
class BuiltinModule : public Module {
public:
  BuiltinModule(Identifier Name, ASTContext &Ctx)
    : Module(DeclContextKind::BuiltinModule, Name, nullptr, Ctx) {
    // The Builtin module is always well formed.
    ASTStage = TypeChecked;
  }

  static bool classof(const DeclContext *DC) {
    return DC->getContextKind() == DeclContextKind::BuiltinModule;
  }
};


/// Represents a serialized module that has been imported into Swift.
///
/// This may be a Swift module or a Clang module.
class LoadedModule : public Module {
protected:
  LoadedModule(DeclContextKind kind, Identifier name,
               std::string DebugModuleName, Component *comp,
               ASTContext &ctx, ModuleLoader &owner)
    : Module(kind, name, comp, ctx),
      DebugModuleName(DebugModuleName) {
    // Loaded modules are always well-formed.
    ASTStage = TypeChecked;
    LookupCachePimpl = static_cast<void *>(&owner);
  }

  ModuleLoader &getOwner() const {
    return *static_cast<ModuleLoader *>(LookupCachePimpl);
  }

  std::string DebugModuleName;

public:
  // Inherited from Module.
  void lookupValue(AccessPathTy accessPath, Identifier name, NLKind lookupKind,
                   SmallVectorImpl<ValueDecl*> &result);

  /// Look up an operator declaration.
  ///
  /// \param name The operator name ("+", ">>", etc.)
  ///
  /// \param fixity One of PrefixOperator, InfixOperator, or PostfixOperator.
  OperatorDecl *lookupOperator(Identifier name, DeclKind fixity);

  /// Look up an operator declaration.
  template <typename T>
  T *lookupOperator(Identifier name) {
    // Make any non-specialized instantiations fail with a "nice" error message.
    static_assert(static_cast<T*>(nullptr),
                  "Must specify prefix, postfix, or infix operator decl");
  }

  /// Adds any modules re-exported by this module to the given vector.
  void getReexportedModules(SmallVectorImpl<ImportedModule> &modules) const;

  /// Find ValueDecls in the module and pass them to the given consumer object.
  void lookupVisibleDecls(AccessPathTy accessPath,
                          VisibleDeclConsumer &consumer,
                          NLKind lookupKind) const;

  static bool classof(const DeclContext *DC) {
    return DC->getContextKind() >= DeclContextKind::First_LoadedModule &&
           DC->getContextKind() <= DeclContextKind::Last_LoadedModule;
  }

  /// \brief Get the debug name for the module.
  std::string getDebugModuleName() const { return DebugModuleName; }
};

template <>
PrefixOperatorDecl *
LoadedModule::lookupOperator<PrefixOperatorDecl>(Identifier name);

template <>
PostfixOperatorDecl *
LoadedModule::lookupOperator<PostfixOperatorDecl>(Identifier name);

template <>
InfixOperatorDecl *
LoadedModule::lookupOperator<InfixOperatorDecl>(Identifier name);

template <typename F>
void Module::forAllVisibleModules(Optional<AccessPathTy> thisPath, F fn) {
  class OrderImportedModules {
  public:
    bool operator()(const ImportedModule &lhs, const ImportedModule &rhs) {
      if (lhs.second != rhs.second)
        return std::less<const Module *>()(lhs.second, rhs.second);
      if (lhs.first.data() != rhs.first.data())
        return std::less<AccessPathTy::iterator>()(lhs.first.begin(),
                                                   rhs.first.begin());
      return lhs.first.size() < rhs.first.size();
    }
  };

  llvm::SmallSet<ImportedModule, 32, OrderImportedModules> visited;
  SmallVector<ImportedModule, 32> queue;

  if (thisPath.hasValue()) {
    queue.push_back(ImportedModule(thisPath.getValue(), this));
  } else {
    // FIXME: The same module with different access paths may have different
    // re-exports.
    visited.insert(ImportedModule({}, this));
    getReexportedModules(queue);
  }

  while (!queue.empty()) {
    auto next = queue.pop_back_val();
    if (!visited.insert(next))
      continue;

    if (!fn(next))
      break;
    next.second->getReexportedModules(queue);
  }
}


} // end namespace swift

#endif
