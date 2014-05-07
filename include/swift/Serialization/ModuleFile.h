//===--- ModuleFile.h - Info about a loaded serialized module ---*- C++ -*-===//
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

#ifndef SWIFT_SERIALIZATION_MODULEFILE_H
#define SWIFT_SERIALIZATION_MODULEFILE_H

#include "swift/AST/Decl.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/KnownProtocols.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/AST/Module.h"
#include "swift/AST/RawComment.h"
#include "swift/AST/TypeLoc.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/ModuleFormat.h"
#include "swift/Basic/Fixnum.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Bitcode/BitstreamReader.h"

namespace llvm {
  class BitstreamCursor;
  class BitstreamReader;
  class MemoryBuffer;
  template <typename Info> class OnDiskIterableChainedHashTable;
}

namespace swift {
class Pattern;
class ProtocolConformance;

/// A serialized module, along with the tools to access it.
class ModuleFile : public LazyMemberLoader {
  /// A reference back to the AST representation of the file.
  FileUnit *FileContext = nullptr;

  /// The module shadowed by this module, if any.
  Module *ShadowedModule = nullptr;

  /// The module file data.
  std::unique_ptr<llvm::MemoryBuffer> ModuleInputBuffer;
  std::unique_ptr<llvm::MemoryBuffer> ModuleDocInputBuffer;

  /// The reader attached to \c ModuleInputBuffer.
  llvm::BitstreamReader ModuleInputReader;

  /// The reader attached to \c ModuleDocInputBuffer.
  llvm::BitstreamReader ModuleDocInputReader;

  /// The cursor used to lazily load things from the file.
  llvm::BitstreamCursor DeclTypeCursor;

  llvm::BitstreamCursor SILCursor;
  llvm::BitstreamCursor SILIndexCursor;

  /// The data blob containing all of the module's identifiers.
  StringRef IdentifierData;

  /// Paths to the source files used to build this module.
  SmallVector<StringRef, 4> SourcePaths;

public:
  /// Represents another module that has been imported as a dependency.
  class Dependency {
  public:
    Module::ImportedModule Import = {};
    const StringRef RawPath;

  private:
    const unsigned IsExported : 1;
    const unsigned IsHeader : 1;

    Dependency(StringRef path, bool exported, bool isHeader)
      : RawPath(path), IsExported(exported), IsHeader(isHeader){}

  public:
    Dependency(StringRef path, bool exported)
      : Dependency(path, exported, false) {}

    static Dependency forHeader(StringRef headerPath, bool exported) {
      return Dependency(headerPath, exported, true);
    }

    bool isLoaded() const {
      return Import.second != nullptr;
    }

    bool isExported() const { return IsExported; }
    bool isHeader() const { return IsHeader; }
  };

private:
  /// All modules this module depends on.
  SmallVector<Dependency, 8> Dependencies;

  /// All of this module's link-time dependencies.
  SmallVector<LinkLibrary, 8> LinkLibraries;

public:
  template <typename T>
  class Serialized {
  private:
    using RawBitOffset = decltype(DeclTypeCursor.GetCurrentBitNo());

    using ImplTy = PointerUnion<T, serialization::BitOffset>;
    ImplTy Value;

  public:
    /*implicit*/ Serialized(serialization::BitOffset offset) : Value(offset) {}

    bool isComplete() const {
      return Value.template is<T>();
    }

    T get() const {
      return Value.template get<T>();
    }

    /*implicit*/ operator T() const {
      return get();
    }

    /*implicit*/ operator serialization::BitOffset() const {
      return Value.template get<serialization::BitOffset>();
    }

    /*implicit*/ operator RawBitOffset() const {
      return Value.template get<serialization::BitOffset>();
    }

    template <typename Derived>
    Serialized &operator=(Derived deserialized) {
      assert(!isComplete() || ImplTy(deserialized) == Value);
      Value = deserialized;
      return *this;
    }

    void unsafeOverwrite(T t) {
      Value = t;
    }
  };

  /// A class for holding a value that can be partially deserialized.
  ///
  /// This class assumes that "T()" is not a valid deserialized value.
  template <typename T>
  class PartiallySerialized {
  private:
    using RawBitOffset = decltype(DeclTypeCursor.GetCurrentBitNo());

    /// The deserialized value.
    T Value;

    /// The offset.  Set to 0 when fully deserialized.
    serialization::BitOffset Offset;

  public:
    /*implicit*/ PartiallySerialized(serialization::BitOffset offset)
      : Value(), Offset(offset) {}

    /*implicit*/ PartiallySerialized(RawBitOffset offset)
      : Value(), Offset(offset) {}

    bool isDeserialized() const {
      return Value != T();
    }

    bool isFullyDeserialized() const {
      return isDeserialized() && Offset == 0;
    }

    serialization::BitOffset getOffset() const {
      assert(!isFullyDeserialized());
      return Offset;
    }

    T get() const {
      assert(isDeserialized());
      return Value;
    }

    void set(T value, bool isFullyDeserialized) {
      assert(!isDeserialized() || Value == value);
      Value = value;
      if (isFullyDeserialized) Offset = 0;
    }
  };

private:
  /// Decls referenced by this module.
  std::vector<Serialized<Decl*>> Decls;

  /// Types referenced by this module.
  std::vector<Serialized<Type>> Types;

  /// Represents an identifier that may or may not have been deserialized yet.
  ///
  /// If \c Offset is non-zero, the identifier has not been loaded yet.
  class SerializedIdentifier {
  public:
    Identifier Ident;
    serialization::BitOffset Offset;

    template <typename IntTy>
    /*implicit*/ SerializedIdentifier(IntTy rawOffset)
      : Offset(rawOffset) {}
  };

  /// Identifiers referenced by this module.
  std::vector<SerializedIdentifier> Identifiers;

  class DeclTableInfo;
  using SerializedDeclTable =
      llvm::OnDiskIterableChainedHashTable<DeclTableInfo>;

  std::unique_ptr<SerializedDeclTable> TopLevelDecls;
  std::unique_ptr<SerializedDeclTable> OperatorDecls;
  std::unique_ptr<SerializedDeclTable> ExtensionDecls;
  std::unique_ptr<SerializedDeclTable> ClassMembersByName;
  std::unique_ptr<SerializedDeclTable> OperatorMethodDecls;

  TinyPtrVector<Decl *> ImportDecls;

  using DeclIDVector = SmallVector<serialization::DeclID, 4>;

  /// All adopters of compiler-known protocols in this module.
  DeclIDVector KnownProtocolAdopters[NumKnownProtocols];
  DeclIDVector EagerDeserializationDecls;

  class DeclCommentTableInfo;
  using SerializedDeclCommentTable =
      llvm::OnDiskIterableChainedHashTable<DeclCommentTableInfo>;

  std::unique_ptr<SerializedDeclCommentTable> DeclCommentTable;

  struct {
    /// Whether this module file comes from a framework.
    unsigned IsFramework : 1;

    /// Whether or not ImportDecls is valid.
    unsigned ComputedImportDecls : 1;

    /// Whether this module file can be used, and what's wrong if not.
    unsigned Status : 3;
    unsigned : 0;
  } Bits;
  static_assert(sizeof(Bits) <= 4, "The bit set should be small");

  void setStatus(ModuleStatus status) {
    Bits.Status = static_cast<unsigned>(status);
    assert(status == getStatus() && "not enough bits for status");
  }

  /// Constructs an new module and validates it.
  ModuleFile(std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer,
             std::unique_ptr<llvm::MemoryBuffer> moduleDocInputBuffer,
             bool isFramework);

public:
  /// Change the status of the current module. Default argument marks the module
  /// as being malformed.
  void error(ModuleStatus issue = ModuleStatus::Malformed) {
    assert(issue != ModuleStatus::Valid);
    assert((!FileContext || issue != ModuleStatus::Malformed) &&
           "error deserializing an individual record");
    setStatus(issue);
  }

  ASTContext &getContext() const {
    assert(FileContext && "no associated context yet");
    return FileContext->getParentModule()->Ctx;
  }

  Module *getAssociatedModule() const {
    assert(FileContext && "no associated context yet");
    return FileContext->getParentModule();
  }

private:
  /// Read an on-disk decl hash table stored in index_block::DeclListLayout
  /// format.
  std::unique_ptr<SerializedDeclTable>
  readDeclTable(ArrayRef<uint64_t> fields, StringRef blobData);

  /// Reads the known protocols block.
  bool readKnownProtocolsBlock(llvm::BitstreamCursor &cursor);

  /// Reads the index block, which contains global tables.
  ///
  /// Returns false if there was an error.
  bool readIndexBlock(llvm::BitstreamCursor &cursor);

  /// Read an on-disk decl hash table stored in
  /// \c comment_block::DeclCommentListLayout format.
  std::unique_ptr<SerializedDeclCommentTable>
  readDeclCommentTable(ArrayRef<uint64_t> fields, StringRef blobData);

  /// Reads the comment block, which contains USR to comment mappings.
  ///
  /// Returns false if there was an error.
  bool readCommentBlock(llvm::BitstreamCursor &cursor);

  /// Recursively reads a pattern from \c DeclTypeCursor.
  ///
  /// If the record at the cursor is not a pattern, returns null.
  Pattern *maybeReadPattern();

  /// Read a referenced conformance, such as the underlying conformance for a
  /// specialized or inherited protocol conformance.
  ProtocolConformance *
  readReferencedConformance(ProtocolDecl *proto,
                            serialization::DeclID typeID,
                            serialization::ModuleID moduleID,
                            llvm::BitstreamCursor &Cursor);

  GenericParamList *maybeGetOrReadGenericParams(serialization::DeclID contextID,
                                                DeclContext *DC,
                                                llvm::BitstreamCursor &Cursor);

  /// Reads a set of requirements from \c DeclTypeCursor.
  void readGenericRequirements(SmallVectorImpl<Requirement> &requirements);

  /// Reads members of a DeclContext from \c DeclTypeCursor.
  ///
  /// The returned array is owned by the ASTContext.
  /// Returns Nothing if there is an error.
  ///
  /// Note: this destroys the cursor's position in the stream. Furthermore,
  /// because it reads from the cursor, it is not possible to reset the cursor
  /// after reading. Nothing should ever follow a DECL_CONTEXT record.
  Optional<MutableArrayRef<Decl *>> readMembers();

  /// Resolves a cross-reference, starting from the given module.
  ///
  /// Note: this destroys the cursor's position in the stream. Furthermore,
  /// because it reads from the cursor, it is not possible to reset the cursor
  /// after reading. Nothing should ever follow an XREF record except
  /// XREF_PATH_PIECE records.
  Decl *resolveCrossReference(Module *M, uint32_t pathLen);

  /// Populates TopLevelIDs for name lookup.
  void buildTopLevelDeclMap();

public:
  /// Returns the decl context with the given ID, deserializing it if needed.
  DeclContext *getDeclContext(serialization::DeclID DID);

  /// Loads a module from the given memory buffer.
  ///
  /// \param moduleInputBuffer A memory buffer containing the serialized module
  /// data.  The created module takes ownership of the buffer, even if there's
  /// an error in loading.
  /// \param[out] theModule The loaded module.
  /// \returns Whether the module was successfully loaded, or what went wrong
  ///          if it was not.
  static ModuleStatus
  load(std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer,
       std::unique_ptr<llvm::MemoryBuffer> moduleDocInputBuffer,
       bool isFramework, std::unique_ptr<ModuleFile> &theModule) {
    theModule.reset(new ModuleFile(std::move(moduleInputBuffer),
                                   std::move(moduleDocInputBuffer),
                                   isFramework));
    return theModule->getStatus();
  }

  // Out of line to avoid instantiation OnDiskChainedHashTable here.
  ~ModuleFile();

  /// Associates this module file with an AST module.
  ///
  /// Returns false if the association failed.
  bool associateWithFileContext(FileUnit *file);

  /// Checks whether this module can be used.
  ModuleStatus getStatus() const {
    return static_cast<ModuleStatus>(Bits.Status);
  }

  /// Returns paths to the source files that were used to build this module.
  ArrayRef<StringRef> getInputSourcePaths() const {
    assert(getStatus() == ModuleStatus::Valid);
    return SourcePaths;
  }

  /// Returns the list of modules this module depends on.
  ArrayRef<Dependency> getDependencies() const {
    return Dependencies;
  }

  /// The module shadowed by this module, if any.
  Module *getShadowedModule() const { return ShadowedModule; }

  /// Searches the module's top-level decls for the given identifier.
  void lookupValue(DeclName name, SmallVectorImpl<ValueDecl*> &results);

  /// Searches the module's operators for one with the given name and fixity.
  ///
  /// If none is found, returns null.
  OperatorDecl *lookupOperator(Identifier name, DeclKind fixity);

  /// Adds any imported modules to the given vector.
  void getImportedModules(SmallVectorImpl<Module::ImportedModule> &results,
                          Module::ImportFilter filter);

  void getImportDecls(SmallVectorImpl<Decl *> &Results);

  /// Reports all visible top-level members in this module.
  void lookupVisibleDecls(Module::AccessPathTy accessPath,
                          VisibleDeclConsumer &consumer,
                          NLKind lookupKind);

  /// Loads extensions for the given decl.
  ///
  /// Note that this may cause other decls to load as well.
  void loadExtensions(NominalTypeDecl *nominal);

  /// Loads decls that conform to the given protocol.
  ///
  /// Note that this may cause other decls to load as well.
  void loadDeclsConformingTo(KnownProtocolKind kind);

  /// Reports all class members in the module to the given consumer.
  ///
  /// This is intended for use with id-style lookup and code completion.
  void lookupClassMembers(Module::AccessPathTy accessPath,
                          VisibleDeclConsumer &consumer);

  /// Adds class members in the module with the given name to the given vector.
  ///
  /// This is intended for use with id-style lookup.
  void lookupClassMember(Module::AccessPathTy accessPath,
                         DeclName name,
                         SmallVectorImpl<ValueDecl*> &results);

  /// Reports all link-time dependencies.
  void collectLinkLibraries(Module::LinkLibraryCallback callback) const;

  /// Adds all top-level decls to the given vector.
  void getTopLevelDecls(SmallVectorImpl<Decl*> &Results);

  /// Adds all top-level decls to the given vector.
  ///
  /// This includes all decls that should be displayed to clients of the module.
  /// This can differ from \c getTopLevelDecls, e.g. it returns decls from a
  /// shadowed clang module.
  void getDisplayDecls(SmallVectorImpl<Decl*> &results);

  StringRef getModuleFilename() const {
    // FIXME: This seems fragile, maybe store the filename separately ?
    return ModuleInputBuffer->getBufferIdentifier();
  }

  llvm::BitstreamCursor getSILCursor() const {
    return SILCursor;
  }
  llvm::BitstreamCursor getSILIndexCursor() const {
    return SILIndexCursor;
  }

  /// Returns the type with the given ID, deserializing it if needed.
  Type getType(serialization::TypeID TID);

  /// Returns the identifier with the given ID, deserializing it if needed.
  Identifier getIdentifier(serialization::IdentifierID IID);

  /// Returns the decl with the given ID, deserializing it if needed.
  ///
  /// \param DID The ID for the decl within this module.
  /// \param ForcedContext Optional override for the decl context of certain
  ///                      kinds of decls, used to avoid re-entrant
  ///                      deserialization.
  Decl *getDecl(serialization::DeclID DID,
                Optional<DeclContext *> ForcedContext = {});

  /// Returns the appropriate module for the given ID.
  Module *getModule(serialization::ModuleID MID);

  /// Returns the appropriate module for the given name.
  ///
  /// If the name matches the name of the current module, a shadowed module
  /// is loaded instead.
  Module *getModule(Identifier name);

  /// Reads a substitution record from \c DeclTypeCursor.
  ///
  /// If the record at the cursor is not a substitution, returns Nothing.
  Optional<Substitution> maybeReadSubstitution(llvm::BitstreamCursor &Cursor);

  /// Recursively reads a protocol conformance from \c DeclTypeCursor.
  ///
  /// The conformance will be newly-created; it's likely that it already exists
  /// in the AST, and will need to be canonicalized.
  ///
  /// If the record at the cursor is not a protocol conformance, returns
  /// Nothing. Note that a null pointer is a valid conformance value.
  Optional<ProtocolConformance *>
  maybeReadConformance(Type conformingType, llvm::BitstreamCursor &Cursor);

  /// Reads a generic param list from \c DeclTypeCursor.
  ///
  /// If the record at the cursor is not a generic param list, returns null
  /// without moving the cursor.
  GenericParamList *maybeReadGenericParams(DeclContext *DC,
                                     llvm::BitstreamCursor &Cursor,
                                     GenericParamList *outerParams = nullptr);

  virtual ArrayRef<Decl *> loadAllMembers(const Decl *D,
                                          uint64_t contextData) override;

  virtual ArrayRef<ProtocolConformance *>
  loadAllConformances(const Decl *D, uint64_t contextData) override;

  virtual TypeLoc loadAssociatedTypeDefault(const AssociatedTypeDecl *ATD,
                                            uint64_t contextData) override;

   Optional<BriefAndRawComment> getCommentForDecl(const Decl *D);
   Optional<BriefAndRawComment> getCommentForDeclByUSR(StringRef USR);
};

} // end namespace swift

#endif
