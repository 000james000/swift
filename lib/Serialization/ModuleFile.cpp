//===--- ModuleFile.cpp - Loading a serialized module -----------*- c++ -*-===//
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

#include "ModuleFile.h"
#include "ModuleFormat.h"
#include "swift/AST/AST.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/Basic/Range.h"
#include "swift/Serialization/BCReadingExtras.h"

// This is a template-only header; eventually it should move to llvm/Support.
#include "clang/Basic/OnDiskHashTable.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"

using namespace swift;
using namespace swift::serialization;

static bool checkSignature(llvm::BitstreamCursor &cursor) {
  for (unsigned char byte : SIGNATURE)
    if (cursor.AtEndOfStream() || cursor.Read(8) != byte)
      return false;
  return true;
}

static bool enterTopLevelModuleBlock(llvm::BitstreamCursor &cursor,
                                     bool shouldReadBlockInfo = true) {
  auto next = cursor.advance();

  if (next.Kind != llvm::BitstreamEntry::SubBlock)
    return false;

  if (next.ID == llvm::bitc::BLOCKINFO_BLOCK_ID) {
    if (shouldReadBlockInfo) {
      if (cursor.ReadBlockInfoBlock())
        return false;
    } else {
      if (cursor.SkipBlock())
        return false;
    }
    return enterTopLevelModuleBlock(cursor, false);
  }

  if (next.ID != MODULE_BLOCK_ID)
    return false;

  cursor.EnterSubBlock(MODULE_BLOCK_ID);
  return true;
}

static std::pair<ModuleStatus, StringRef>
validateControlBlock(llvm::BitstreamCursor &cursor,
                     SmallVectorImpl<uint64_t> &scratch) {
  // The control block is malformed until we've at least read a major version
  // number.
  ModuleStatus result = ModuleStatus::Malformed;
  bool versionSeen = false;
  StringRef name;

  auto next = cursor.advance();
  while (next.Kind != llvm::BitstreamEntry::EndBlock) {
    if (next.Kind == llvm::BitstreamEntry::Error)
      return { ModuleStatus::Malformed, name };

    if (next.Kind == llvm::BitstreamEntry::SubBlock) {
      // Unknown metadata sub-block, possibly for use by a future version of the
      // module format.
      if (cursor.SkipBlock())
        return { ModuleStatus::Malformed, name };
      next = cursor.advance();
      continue;
    }

    scratch.clear();
    StringRef blobData;
    unsigned kind = cursor.readRecord(next.ID, scratch, &blobData);
    switch (kind) {
    case control_block::METADATA: {
      if (versionSeen) {
        result = ModuleStatus::Malformed;
      } else {
        uint16_t versionMajor = scratch[0];
        if (versionMajor > VERSION_MAJOR)
          result = ModuleStatus::FormatTooNew;
        else
          result = ModuleStatus::Valid;
        versionSeen = true;
      }
      break;
    }
    case control_block::MODULE_NAME:
      name = blobData;
      break;
    default:
      // Unknown metadata record, possibly for use by a future version of the
      // module format.
      break;
    }

    next = cursor.advance();
  }

  return { result, name };
}

SerializedModuleLoader::ValidationInfo
SerializedModuleLoader::validateSerializedAST(StringRef data) {
  llvm::BitstreamReader reader(reinterpret_cast<const uint8_t *>(data.begin()),
                               reinterpret_cast<const uint8_t *>(data.end()));
  llvm::BitstreamCursor cursor(reader);
  SmallVector<uint64_t, 32> scratch;

  ValidationInfo result = { {}, 0, ModuleStatus::Malformed };

  if (!checkSignature(cursor) || !enterTopLevelModuleBlock(cursor, false))
    return result;

  auto topLevelEntry = cursor.advance();
  while (topLevelEntry.Kind == llvm::BitstreamEntry::SubBlock) {
    if (topLevelEntry.ID == CONTROL_BLOCK_ID) {
      cursor.EnterSubBlock(CONTROL_BLOCK_ID);
      llvm::tie(result.status, result.name) =
        validateControlBlock(cursor, scratch);
      if (result.status == ModuleStatus::Malformed)
        return result;

    } else {
      if (cursor.SkipBlock()) {
        result.status = ModuleStatus::Malformed;
        return result;
      }
    }
    
    topLevelEntry = cursor.advance(AF_DontPopBlockAtEnd);
  }
  
  if (topLevelEntry.Kind == llvm::BitstreamEntry::EndBlock) {
    cursor.ReadBlockEnd();
    assert(cursor.GetCurrentBitNo() % CHAR_BIT == 0);
    result.bytes = cursor.GetCurrentBitNo() / CHAR_BIT;
  } else {
    result.status = ModuleStatus::Malformed;
  }

  return result;
}

namespace {
  class PrettyModuleFileDeserialization : public llvm::PrettyStackTraceEntry {
    const ModuleFile &File;
  public:
    explicit PrettyModuleFileDeserialization(const ModuleFile &file)
        : File(file) {}

    virtual void print(raw_ostream &os) const override {
      os << "While reading from " << File.getModuleFilename() << "\n";
    }
  };
} // end anonymous namespace

/// Used to deserialize entries in the on-disk decl hash table.
class ModuleFile::DeclTableInfo {
public:
  using internal_key_type = StringRef;
  using external_key_type = Identifier;
  using data_type = SmallVector<std::pair<uint8_t, DeclID>, 8>;

  internal_key_type GetInternalKey(external_key_type ID) {
    return ID.str();
  }

  uint32_t ComputeHash(internal_key_type key) {
    return llvm::HashString(key);
  }

  static bool EqualKey(internal_key_type lhs, internal_key_type rhs) {
    return lhs == rhs;
  }

  static std::pair<unsigned, unsigned> ReadKeyDataLength(const uint8_t *&data) {
    using namespace clang::io;
    unsigned keyLength = ReadUnalignedLE16(data);
    unsigned dataLength = ReadUnalignedLE16(data);
    return { keyLength, dataLength };
  }

  static internal_key_type ReadKey(const uint8_t *data, unsigned length) {
    return StringRef(reinterpret_cast<const char *>(data), length);
  }

  static data_type ReadData(internal_key_type key, const uint8_t *data,
                            unsigned length) {
    using namespace clang::io;

    data_type result;
    while (length > 0) {
      uint8_t kind = *data++;
      DeclID offset = ReadUnalignedLE32(data);
      result.push_back({ kind, offset });
      length -= 5;
    }

    return result;
  }
};

std::unique_ptr<ModuleFile::SerializedDeclTable>
ModuleFile::readDeclTable(ArrayRef<uint64_t> fields, StringRef blobData) {
  uint32_t tableOffset;
  index_block::DeclListLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedDeclTable>;
  return OwnedTable(SerializedDeclTable::Create(base + tableOffset, base));
}

static Optional<KnownProtocolKind> getActualKnownProtocol(unsigned rawKind) {
  auto stableKind = static_cast<index_block::KnownProtocolKind>(rawKind);
  if (stableKind != rawKind)
    return Nothing;

  switch (stableKind) {
#define PROTOCOL(Id) \
  case index_block::Id: return KnownProtocolKind::Id;
#include "swift/AST/KnownProtocols.def"
  }

  // If there's a new case value in the module file, ignore it.
  return Nothing;
}

bool ModuleFile::readKnownProtocolsBlock(llvm::BitstreamCursor &cursor) {
  cursor.EnterSubBlock(KNOWN_PROTOCOL_BLOCK_ID);

  SmallVector<uint64_t, 8> scratch;

  do {
    auto next = cursor.advanceSkippingSubblocks();
    switch (next.Kind) {
    case llvm::BitstreamEntry::EndBlock:
      return true;

    case llvm::BitstreamEntry::Error:
      return false;

    case llvm::BitstreamEntry::SubBlock:
      llvm_unreachable("subblocks skipped");

    case llvm::BitstreamEntry::Record: {
      scratch.clear();
      unsigned rawKind = cursor.readRecord(next.ID, scratch);

      DeclIDVector *list;
      if (auto actualKind = getActualKnownProtocol(rawKind)) {
        auto index = static_cast<unsigned>(actualKind.getValue());
        list = &KnownProtocolAdopters[index];
      } else {
        // Ignore this record.
        break;
      }

      list->append(scratch.begin(), scratch.end());
      break;
    }
    }
  } while (true);
}

bool ModuleFile::readIndexBlock(llvm::BitstreamCursor &cursor) {
  cursor.EnterSubBlock(INDEX_BLOCK_ID);

  SmallVector<uint64_t, 4> scratch;
  StringRef blobData;

  do {
    auto next = cursor.advance();
    switch (next.Kind) {
    case llvm::BitstreamEntry::EndBlock:
      return true;

    case llvm::BitstreamEntry::Error:
      return false;

    case llvm::BitstreamEntry::SubBlock:
      switch (next.ID) {
      case KNOWN_PROTOCOL_BLOCK_ID:
        if (!readKnownProtocolsBlock(cursor))
          return false;
        break;
      default:
        // Unknown sub-block, which this version of the compiler won't use.
        if (cursor.SkipBlock())
          return false;
        break;
      }
      break;

    case llvm::BitstreamEntry::Record:
      scratch.clear();
      unsigned kind = cursor.readRecord(next.ID, scratch, &blobData);

      switch (kind) {
      case index_block::DECL_OFFSETS:
        assert(blobData.empty());
        Decls.assign(scratch.begin(), scratch.end());
        break;
      case index_block::TYPE_OFFSETS:
        assert(blobData.empty());
        Types.assign(scratch.begin(), scratch.end());
        break;
      case index_block::IDENTIFIER_OFFSETS:
        assert(blobData.empty());
        Identifiers.assign(scratch.begin(), scratch.end());
        break;
      case index_block::TOP_LEVEL_DECLS:
        TopLevelDecls = readDeclTable(scratch, blobData);
        break;
      case index_block::OPERATORS:
        OperatorDecls = readDeclTable(scratch, blobData);
        break;
      case index_block::EXTENSIONS:
        ExtensionDecls = readDeclTable(scratch, blobData);
        break;
      case index_block::CLASS_MEMBERS:
        ClassMembersByName = readDeclTable(scratch, blobData);
        break;
      case index_block::OPERATOR_METHODS:
        OperatorMethodDecls = readDeclTable(scratch, blobData);
        break;
      default:
        // Unknown index kind, which this version of the compiler won't use.
        break;
      }
      break;
    }
  } while (true);
}

static Optional<swift::LibraryKind> getActualLibraryKind(unsigned rawKind) {
  auto stableKind = static_cast<serialization::LibraryKind>(rawKind);
  if (stableKind != rawKind)
    return Nothing;

  switch (stableKind) {
  case serialization::LibraryKind::Library:
    return swift::LibraryKind::Library;
  case serialization::LibraryKind::Framework:
    return swift::LibraryKind::Framework;
  }

  // If there's a new case value in the module file, ignore it.
  return Nothing;
}


ModuleFile::ModuleFile(std::unique_ptr<llvm::MemoryBuffer> input)
  : FileContext(nullptr),
    InputFile(std::move(input)),
    InputReader(reinterpret_cast<const uint8_t *>(InputFile->getBufferStart()),
                reinterpret_cast<const uint8_t *>(InputFile->getBufferEnd())),
    Status(ModuleStatus::Valid) {
  PrettyModuleFileDeserialization stackEntry(*this);

  llvm::BitstreamCursor cursor{InputReader};

  if (!checkSignature(cursor) || !enterTopLevelModuleBlock(cursor)) {
    error();
    return;
  }

  // Future-proofing: make sure we validate the control block before we try to
  // read any other blocks.
  bool hasValidControlBlock = false;
  SmallVector<uint64_t, 64> scratch;

  auto topLevelEntry = cursor.advance();
  while (topLevelEntry.Kind == llvm::BitstreamEntry::SubBlock) {
    switch (topLevelEntry.ID) {
    case CONTROL_BLOCK_ID: {
      cursor.EnterSubBlock(CONTROL_BLOCK_ID);

      ModuleStatus err = validateControlBlock(cursor, scratch).first;
      if (err != ModuleStatus::Valid) {
        error(err);
        return;
      }

      hasValidControlBlock = true;
      break;
    }

    case INPUT_BLOCK_ID: {
      if (!hasValidControlBlock) {
        error();
        return;
      }

      cursor.EnterSubBlock(INPUT_BLOCK_ID);

      auto next = cursor.advance();
      while (next.Kind == llvm::BitstreamEntry::Record) {
        scratch.clear();
        StringRef blobData;
        unsigned kind = cursor.readRecord(next.ID, scratch, &blobData);
        switch (kind) {
        case input_block::SOURCE_FILE:
          assert(scratch.empty());
          SourcePaths.push_back(blobData);
          break;
        case input_block::IMPORTED_MODULE: {
          bool exported;
          input_block::ImportedModuleLayout::readRecord(scratch, exported);
          Dependencies.push_back({blobData, exported});
          break;
        }
        case input_block::LINK_LIBRARY: {
          uint8_t rawKind;
          input_block::ImportedModuleLayout::readRecord(scratch, rawKind);
          if (auto libKind = getActualLibraryKind(rawKind))
            LinkLibraries.push_back({blobData, *libKind});
          // else ignore the dependency...it'll show up as a linker error.
          break;
        }
        default:
          // Unknown input kind, possibly for use by a future version of the
          // module format.
          // FIXME: Should we warn about this?
          break;
        }

        next = cursor.advance();
      }

      if (next.Kind != llvm::BitstreamEntry::EndBlock)
        error();

      break;
    }

    case DECLS_AND_TYPES_BLOCK_ID: {
      if (!hasValidControlBlock) {
        error();
        return;
      }

      // The decls-and-types block is lazily loaded. Save the cursor and load
      // any abbrev records at the start of the block.
      DeclTypeCursor = cursor;
      DeclTypeCursor.EnterSubBlock(DECLS_AND_TYPES_BLOCK_ID);
      if (DeclTypeCursor.advance().Kind == llvm::BitstreamEntry::Error)
        error();

      // With the main cursor, skip over the block and continue.
      if (cursor.SkipBlock()) {
        error();
        return;
      }
      break;
    }

    case IDENTIFIER_DATA_BLOCK_ID: {
      if (!hasValidControlBlock) {
        error();
        return;
      }

      cursor.EnterSubBlock(IDENTIFIER_DATA_BLOCK_ID);

      auto next = cursor.advanceSkippingSubblocks();
      while (next.Kind == llvm::BitstreamEntry::Record) {
        scratch.clear();
        StringRef blobData;
        unsigned kind = cursor.readRecord(next.ID, scratch, &blobData);

        switch (kind) {
        case identifier_block::IDENTIFIER_DATA:
          assert(scratch.empty());
          IdentifierData = blobData;
          break;
        default:
          // Unknown identifier data, which this version of the compiler won't
          // use.
          break;
        }

        next = cursor.advanceSkippingSubblocks();
      }

      if (next.Kind != llvm::BitstreamEntry::EndBlock) {
        error();
        return;
      }

      break;
    }

    case INDEX_BLOCK_ID: {
      if (!hasValidControlBlock || !readIndexBlock(cursor)) {
        error();
        return;
      }
      break;
    }

    case SIL_INDEX_BLOCK_ID: {
      // Save the cursor.
      SILIndexCursor = cursor;
      SILIndexCursor.EnterSubBlock(SIL_INDEX_BLOCK_ID);

      // With the main cursor, skip over the block and continue.
      if (cursor.SkipBlock()) {
        error();
        return;
      }
      break;
    }

    case SIL_BLOCK_ID: {
      // Save the cursor.
      SILCursor = cursor;
      SILCursor.EnterSubBlock(SIL_BLOCK_ID);

      // With the main cursor, skip over the block and continue.
      if (cursor.SkipBlock()) {
        error();
        return;
      }
      break;
    }

    default:
      // Unknown top-level block, possibly for use by a future version of the
      // module format.
      if (cursor.SkipBlock()) {
        error();
        return;
      }
      break;
    }
    
    topLevelEntry = cursor.advance(AF_DontPopBlockAtEnd);
  }
  
  if (topLevelEntry.Kind != llvm::BitstreamEntry::EndBlock) {
    error();
    return;
  }
}

bool ModuleFile::associateWithFileContext(FileUnit *file) {
  PrettyModuleFileDeserialization stackEntry(*this);

  assert(Status == ModuleStatus::Valid && "invalid module file");
  assert(!FileContext && "already associated with an AST module");
  FileContext = file;

  ASTContext &ctx = getContext();
  bool missingDependency = false;
  for (auto &dependency : Dependencies) {
    assert(!dependency.isLoaded() && "already loaded?");

    StringRef modulePath, scopePath;
    llvm::tie(modulePath, scopePath) = dependency.RawAccessPath.split('\0');

    auto moduleID = ctx.getIdentifier(modulePath);
    assert(!moduleID.empty() &&
           "invalid module name (submodules not yet supported)");
    auto module = getModule(moduleID);
    if (!module) {
      // If we're missing the module we're shadowing, treat that specially.
      if (moduleID == file->getParentModule()->Name) {
        error(ModuleStatus::MissingShadowedModule);
        return false;
      }

      // Otherwise, continue trying to load dependencies, so that we can list
      // everything that's missing.
      missingDependency = true;
      continue;
    }

    if (scopePath.empty()) {
      dependency.Import = { {}, module };
    } else {
      auto scopeID = ctx.getIdentifier(scopePath);
      assert(!scopeID.empty() &&
             "invalid decl name (non-top-level decls not supported)");
      auto path = Module::AccessPathTy({scopeID, SourceLoc()});
      dependency.Import = { ctx.AllocateCopy(path), module };
    }
  }

  if (missingDependency) {
    error(ModuleStatus::MissingDependency);
    return false;
  }

  return Status == ModuleStatus::Valid;
}

ModuleFile::~ModuleFile() = default;

void ModuleFile::lookupValue(Identifier name,
                             SmallVectorImpl<ValueDecl*> &results) {
  PrettyModuleFileDeserialization stackEntry(*this);

  if (TopLevelDecls) {
    // Find top-level declarations with the given name.
    auto iter = TopLevelDecls->find(name);
    if (iter != TopLevelDecls->end()) {
      for (auto item : *iter) {
        auto VD = cast<ValueDecl>(getDecl(item.second));
        results.push_back(VD);
      }
    }
  }

  // If the name is an operator name, also look for operator methods.
  if (name.isOperator() && OperatorMethodDecls) {
    auto iter = OperatorMethodDecls->find(name);
    if (iter != OperatorMethodDecls->end()) {
      for (auto item : *iter) {
        auto VD = cast<ValueDecl>(getDecl(item.second));
        results.push_back(VD);
      }
    }
  }
}

OperatorDecl *ModuleFile::lookupOperator(Identifier name, DeclKind fixity) {
  PrettyModuleFileDeserialization stackEntry(*this);

  if (!OperatorDecls)
    return nullptr;

  auto iter = OperatorDecls->find(name);
  if (iter == OperatorDecls->end())
    return nullptr;

  for (auto item : *iter) {
    if (getStableFixity(fixity) == item.first)
      return cast<OperatorDecl>(getDecl(item.second));
  }

  // FIXME: operators re-exported from other modules?

  return nullptr;
}

void ModuleFile::getImportedModules(
    SmallVectorImpl<Module::ImportedModule> &results,
    bool includePrivate) {
  PrettyModuleFileDeserialization stackEntry(*this);

  for (auto &dep : Dependencies) {
    if (!includePrivate && !dep.IsExported)
      continue;
    assert(dep.isLoaded());
    results.push_back(dep.Import);
  }
}

void ModuleFile::getImportDecls(SmallVectorImpl<Decl *> &Results) {
  if (!ComputedImportDecls) {
    ASTContext &Ctx = getContext();
    for (auto &Dep : Dependencies) {
      StringRef ModulePath, ScopePath;
      llvm::tie(ModulePath, ScopePath) = Dep.RawAccessPath.split('\0');

      auto ModuleID = Ctx.getIdentifier(ModulePath);
      assert(!ModuleID.empty() &&
             "invalid module name (submodules not yet supported)");

      if (ModuleID == Ctx.StdlibModuleName)
        continue;

      SmallVector<std::pair<swift::Identifier, swift::SourceLoc>, 1>
          AccessPath;
      AccessPath.push_back({ ModuleID, SourceLoc() });

      auto Kind = ImportKind::Module;
      if (!ScopePath.empty()) {
        auto ScopeID = Ctx.getIdentifier(ScopePath);
        assert(!ScopeID.empty() &&
               "invalid decl name (non-top-level decls not supported)");

        Module *M = Ctx.getModule(AccessPath);
        if (!M) {
          // The dependency module could not be loaded.  Just make a guess
          // about the import kind, we can not do better.
          Kind = ImportKind::Func;
        } else {
          SmallVector<ValueDecl *, 8> Decls;
          M->lookupQualified(ModuleType::get(M), ScopeID,
                             NL_QualifiedDefault, nullptr, Decls);
          Optional<ImportKind> FoundKind = ImportDecl::findBestImportKind(Decls);
          assert(FoundKind.hasValue() &&
                 "deserialized imports should not be ambigous");
          Kind = *FoundKind;
        }

        AccessPath.push_back({ ScopeID, SourceLoc() });
      }

      ImportDecls.push_back(ImportDecl::create(
          Ctx, FileContext, SourceLoc(), Kind, SourceLoc(), Dep.IsExported,
          AccessPath));
    }
    ComputedImportDecls = true;
  }
  Results.append(ImportDecls.begin(), ImportDecls.end());
}

void ModuleFile::lookupVisibleDecls(Module::AccessPathTy accessPath,
                                    VisibleDeclConsumer &consumer,
                                    NLKind lookupKind) {
  PrettyModuleFileDeserialization stackEntry(*this);
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  if (!TopLevelDecls)
    return;

  if (!accessPath.empty()) {
    auto iter = TopLevelDecls->find(accessPath.front().first);
    if (iter == TopLevelDecls->end())
      return;

    for (auto item : *iter)
      consumer.foundDecl(cast<ValueDecl>(getDecl(item.second)),
                         DeclVisibilityKind::VisibleAtTopLevel);
  }

  for (auto entry : make_range(TopLevelDecls->data_begin(),
                               TopLevelDecls->data_end())) {
    for (auto item : entry)
      consumer.foundDecl(cast<ValueDecl>(getDecl(item.second)),
                         DeclVisibilityKind::VisibleAtTopLevel);
  }
}

void ModuleFile::loadExtensions(NominalTypeDecl *nominal) {
  PrettyModuleFileDeserialization stackEntry(*this);
  if (!ExtensionDecls)
    return;

  auto iter = ExtensionDecls->find(nominal->getName());
  if (iter == ExtensionDecls->end())
    return;

  for (auto item : *iter) {
    if (item.first == getKindForTable(nominal))
      (void)getDecl(item.second);
  }
}

void ModuleFile::loadDeclsConformingTo(KnownProtocolKind kind) {
  PrettyModuleFileDeserialization stackEntry(*this);

  auto index = static_cast<unsigned>(kind);
  for (DeclID DID : KnownProtocolAdopters[index]) {
    Decl *D = getDecl(DID);
    getContext().recordConformance(kind, D);
  }
}

void ModuleFile::lookupClassMember(Module::AccessPathTy accessPath,
                                   Identifier name,
                                   SmallVectorImpl<ValueDecl*> &results) {
  PrettyModuleFileDeserialization stackEntry(*this);
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  if (!ClassMembersByName)
    return;
  
  auto iter = ClassMembersByName->find(name);
  if (iter == ClassMembersByName->end())
    return;
  
  if (!accessPath.empty()) {
    for (auto item : *iter) {
      auto vd = cast<ValueDecl>(getDecl(item.second));
      auto dc = vd->getDeclContext();
      while (!dc->getParent()->isModuleScopeContext())
        dc = dc->getParent();
      if (auto nominal = dc->getDeclaredTypeInContext()->getAnyNominal())
        if (nominal->getName() == accessPath.front().first)
          results.push_back(vd);
    }
    return;
  }
  
  for (auto item : *iter) {
    auto vd = cast<ValueDecl>(getDecl(item.second));
    results.push_back(vd);
  }
}

void ModuleFile::lookupClassMembers(Module::AccessPathTy accessPath,
                                    VisibleDeclConsumer &consumer) {
  PrettyModuleFileDeserialization stackEntry(*this);
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  if (!ClassMembersByName)
    return;

  if (!accessPath.empty()) {
    for (const auto &list : make_range(ClassMembersByName->data_begin(),
                                       ClassMembersByName->data_end())) {
      for (auto item : list) {
        auto vd = cast<ValueDecl>(getDecl(item.second));
        auto dc = vd->getDeclContext();
        while (!dc->getParent()->isModuleScopeContext())
          dc = dc->getParent();
        if (auto nominal = dc->getDeclaredTypeInContext()->getAnyNominal())
          if (nominal->getName() == accessPath.front().first)
            consumer.foundDecl(vd, DeclVisibilityKind::DynamicLookup);
      }
    }
    return;
  }

  for (const auto &list : make_range(ClassMembersByName->data_begin(),
                                     ClassMembersByName->data_end())) {
    for (auto item : list)
      consumer.foundDecl(cast<ValueDecl>(getDecl(item.second)),
                         DeclVisibilityKind::DynamicLookup);
  }
}

void
ModuleFile::collectLinkLibraries(Module::LinkLibraryCallback callback) const {
  for (auto &lib : LinkLibraries)
    callback(lib);
}

void ModuleFile::getTopLevelDecls(SmallVectorImpl<Decl *> &results) {
  PrettyModuleFileDeserialization stackEntry(*this);
  if (OperatorDecls) {
    for (auto entry : make_range(OperatorDecls->data_begin(),
                                 OperatorDecls->data_end())) {
      for (auto item : entry)
        results.push_back(getDecl(item.second));
    }
  }

  if (TopLevelDecls) {
    for (auto entry : make_range(TopLevelDecls->data_begin(),
                                 TopLevelDecls->data_end())) {
      for (auto item : entry)
        results.push_back(getDecl(item.second));
    }
  }

  if (ExtensionDecls) {
    for (auto entry : make_range(ExtensionDecls->data_begin(),
                                 ExtensionDecls->data_end())) {
      for (auto item : entry)
        results.push_back(getDecl(item.second));
    }
  }
}

void ModuleFile::getDisplayDecls(SmallVectorImpl<Decl *> &results) {
  if (ShadowedModule)
    ShadowedModule->getDisplayDecls(results);

  PrettyModuleFileDeserialization stackEntry(*this);
  getImportDecls(results);
  getTopLevelDecls(results);
}
