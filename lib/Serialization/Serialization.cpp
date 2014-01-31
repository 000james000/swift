//===--- Serialization.cpp - Read and write Swift modules -----------------===//
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

#include "swift/Subsystems.h"
#include "ModuleFormat.h"
#include "Serialization.h"
#include "SILFormat.h"
#include "swift/AST/AST.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/KnownProtocols.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Serialization/BCRecordLayout.h"

// This is a template-only header; eventually it should move to llvm/Support.
#include "clang/Basic/OnDiskHashTable.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitstreamWriter.h"
#include "llvm/Config/config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace swift::serialization;
using clang::OnDiskChainedHashTableGenerator;

namespace {
  /// Used to serialize the on-disk decl hash table.
  class DeclTableInfo {
  public:
    using key_type = Identifier;
    using key_type_ref = key_type;
    using data_type = Serializer::DeclTableData;
    using data_type_ref = const data_type &;

    uint32_t ComputeHash(key_type_ref key) {
      assert(!key.empty());
      return llvm::HashString(key.str());
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      using namespace clang::io;
      uint32_t keyLength = key.str().size();
      uint32_t dataLength = (sizeof(DeclID) + 1) * data.size();
      Emit16(out, keyLength);
      Emit16(out, dataLength);
      return { keyLength, dataLength };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      out << key.str();
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      static_assert(sizeof(DeclID) <= 32, "DeclID too large");
      using namespace clang::io;
      for (auto entry : data) {
        Emit8(out, entry.first);
        Emit32(out, entry.second);
      }
    }
  };
} // end anonymous namespace

namespace llvm {
  template<> struct DenseMapInfo<Serializer::DeclTypeUnion> {
    using DeclTypeUnion = Serializer::DeclTypeUnion;
    static inline DeclTypeUnion getEmptyKey() { return nullptr; }
    static inline DeclTypeUnion getTombstoneKey() { return swift::Type(); }
    static unsigned getHashValue(const DeclTypeUnion &val) {
      return DenseMapInfo<const void *>::getHashValue(val.getOpaqueValue());
    }
    static bool isEqual(const DeclTypeUnion &lhs, const DeclTypeUnion &rhs) {
      return lhs == rhs;
    }
  };
}

static Module *getModule(ModuleOrSourceFile DC) {
  if (auto M = DC.dyn_cast<Module *>())
    return M;
  return DC.get<SourceFile *>()->getParentModule();
}

static ASTContext &getContext(ModuleOrSourceFile DC) {
  return getModule(DC)->Ctx;
}

static const Decl *getDeclForContext(const DeclContext *DC) {
  switch (DC->getContextKind()) {
  case DeclContextKind::Module:
    // Use a null decl to represent the module.
    return nullptr;
  case DeclContextKind::FileUnit:
    return getDeclForContext(DC->getParent());
  case DeclContextKind::Initializer:
  case DeclContextKind::AbstractClosureExpr:
    // FIXME: What about default functions?
    llvm_unreachable("shouldn't serialize decls from anonymous closures");
  case DeclContextKind::NominalTypeDecl:
    return cast<NominalTypeDecl>(DC);
  case DeclContextKind::ExtensionDecl:
    return cast<ExtensionDecl>(DC);
  case DeclContextKind::TopLevelCodeDecl:
    llvm_unreachable("shouldn't serialize the main module");
  case DeclContextKind::AbstractFunctionDecl:
    return cast<AbstractFunctionDecl>(DC);
  }
}

DeclID Serializer::addDeclRef(const Decl *D) {
  if (!D)
    return 0;

  DeclID &id = DeclIDs[D];
  if (id != 0)
    return id;

  // Record any generic parameters that come from this decl, so that we can use
  // the decl to refer to the parameters later.
  const GenericParamList *paramList = nullptr;
  switch (D->getKind()) {
  case DeclKind::Constructor:
    paramList = cast<ConstructorDecl>(D)->getGenericParams();
    break;
  case DeclKind::Func:
    paramList = cast<FuncDecl>(D)->getGenericParams();
    break;
  case DeclKind::Class:
  case DeclKind::Struct:
  case DeclKind::Enum:
  case DeclKind::Protocol:
    paramList = cast<NominalTypeDecl>(D)->getGenericParams();
    break;
  default:
    break;
  }
  if (paramList)
    GenericContexts[paramList] = D;

  id = ++LastDeclID;
  DeclsAndTypesToWrite.push(D);
  return id;
}

TypeID Serializer::addTypeRef(Type ty) {
  if (!ty)
    return 0;

  TypeID &id = DeclIDs[ty];
  if (id != 0)
    return id;

  id = ++LastTypeID;
  DeclsAndTypesToWrite.push(ty);
  return id;
}

IdentifierID Serializer::addIdentifierRef(Identifier ident) {
  if (ident.empty())
    return 0;

  IdentifierID &id = IdentifierIDs[ident];
  if (id != 0)
    return id;

  id = ++LastIdentifierID;
  IdentifiersToWrite.push_back(ident);
  return id;
}

IdentifierID Serializer::addModuleRef(const Module *M) {
  if (M == this->M->Ctx.TheBuiltinModule)
    return BUILTIN_MODULE_ID;
  if (M == this->M)
    return CURRENT_MODULE_ID;

  assert(!M->Name.empty());
  return addIdentifierRef(M->Name);
}

const Decl *Serializer::getGenericContext(const GenericParamList *paramList) {
  auto contextDecl = GenericContexts.lookup(paramList);
  return contextDecl;
}

/// Record the name of a block.
static void emitBlockID(llvm::BitstreamWriter &out, unsigned ID,
                        StringRef name,
                        SmallVectorImpl<unsigned char> &nameBuffer) {
  SmallVector<unsigned, 1> idBuffer;
  idBuffer.push_back(ID);
  out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETBID, idBuffer);

  // Emit the block name if present.
  if (name.empty())
    return;
  nameBuffer.resize(name.size());
  memcpy(nameBuffer.data(), name.data(), name.size());
  out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_BLOCKNAME, nameBuffer);
}

/// Record the name of a record within a block.
static void emitRecordID(llvm::BitstreamWriter &out, unsigned ID,
                         StringRef name,
                         SmallVectorImpl<unsigned char> &nameBuffer) {
  assert(ID < 256 && "can't fit record ID in next to name");
  nameBuffer.resize(name.size()+1);
  nameBuffer[0] = ID;
  memcpy(nameBuffer.data()+1, name.data(), name.size());
  out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETRECORDNAME, nameBuffer);
}

void Serializer::writeBlockInfoBlock() {
  BCBlockRAII restoreBlock(Out, llvm::bitc::BLOCKINFO_BLOCK_ID, 2);

  SmallVector<unsigned char, 64> nameBuffer;
#define BLOCK(X) emitBlockID(Out, X ## _ID, #X, nameBuffer)
#define BLOCK_RECORD(K, X) emitRecordID(Out, K::X, #X, nameBuffer)

  BLOCK(MODULE_BLOCK);

  BLOCK(CONTROL_BLOCK);
  BLOCK_RECORD(control_block, METADATA);
  BLOCK_RECORD(control_block, MODULE_NAME);

  BLOCK(INPUT_BLOCK);
  BLOCK_RECORD(input_block, SOURCE_FILE);
  BLOCK_RECORD(input_block, IMPORTED_MODULE);
  BLOCK_RECORD(input_block, LINK_LIBRARY);

  BLOCK(DECLS_AND_TYPES_BLOCK);
#define RECORD(X) BLOCK_RECORD(decls_block, X);
#include "DeclTypeRecordNodes.def"

  BLOCK(IDENTIFIER_DATA_BLOCK);
  BLOCK_RECORD(identifier_block, IDENTIFIER_DATA);

  BLOCK(INDEX_BLOCK);
  BLOCK_RECORD(index_block, TYPE_OFFSETS);
  BLOCK_RECORD(index_block, DECL_OFFSETS);
  BLOCK_RECORD(index_block, IDENTIFIER_OFFSETS);
  BLOCK_RECORD(index_block, TOP_LEVEL_DECLS);
  BLOCK_RECORD(index_block, OPERATORS);
  BLOCK_RECORD(index_block, EXTENSIONS);
  BLOCK_RECORD(index_block, CLASS_MEMBERS);
  BLOCK_RECORD(index_block, OPERATOR_METHODS);

  BLOCK(SIL_BLOCK);
  BLOCK_RECORD(sil_block, SIL_FUNCTION);
  BLOCK_RECORD(sil_block, SIL_BASIC_BLOCK);
  BLOCK_RECORD(sil_block, SIL_ONE_VALUE_ONE_OPERAND);
  BLOCK_RECORD(sil_block, SIL_ONE_TYPE);
  BLOCK_RECORD(sil_block, SIL_ONE_OPERAND);
  BLOCK_RECORD(sil_block, SIL_ONE_TYPE_ONE_OPERAND);
  BLOCK_RECORD(sil_block, SIL_ONE_TYPE_VALUES);
  BLOCK_RECORD(sil_block, SIL_TWO_OPERANDS);
  BLOCK_RECORD(sil_block, SIL_INST_APPLY);
  BLOCK_RECORD(sil_block, SIL_INST_NO_OPERAND);
  BLOCK_RECORD(sil_block, SIL_VTABLE);
  BLOCK_RECORD(sil_block, SIL_VTABLE_ENTRY);
  BLOCK_RECORD(sil_block, SIL_GLOBALVAR);
  BLOCK_RECORD(sil_block, SIL_INST_CAST);
  BLOCK_RECORD(sil_block, SIL_INIT_EXISTENTIAL);
  BLOCK_RECORD(sil_block, SIL_WITNESSTABLE);
  BLOCK_RECORD(sil_block, SIL_WITNESS_METHOD_ENTRY);
  // These layouts can exist in both decl blocks and sil blocks.
#define BLOCK_RECORD_WITH_NAMESPACE(K, X) emitRecordID(Out, X, #X, nameBuffer)
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::BOUND_GENERIC_SUBSTITUTION);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::NO_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::NORMAL_PROTOCOL_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::SPECIALIZED_PROTOCOL_CONFORMANCE);
  BLOCK_RECORD_WITH_NAMESPACE(sil_block,
                              decls_block::INHERITED_PROTOCOL_CONFORMANCE);

  BLOCK(SIL_INDEX_BLOCK);
  BLOCK_RECORD(sil_index_block, SIL_FUNC_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_FUNC_OFFSETS);
  BLOCK_RECORD(sil_index_block, SIL_VTABLE_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_VTABLE_OFFSETS);
  BLOCK_RECORD(sil_index_block, SIL_GLOBALVAR_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_GLOBALVAR_OFFSETS);
  BLOCK_RECORD(sil_index_block, SIL_WITNESSTABLE_NAMES);
  BLOCK_RECORD(sil_index_block, SIL_WITNESSTABLE_OFFSETS);

  BLOCK(KNOWN_PROTOCOL_BLOCK);
#define PROTOCOL(Id) BLOCK_RECORD(index_block, Id);
#include "swift/AST/KnownProtocols.def"

#undef BLOCK
#undef BLOCK_RECORD
}

void Serializer::writeHeader(const Module *M) {
  {
    BCBlockRAII restoreBlock(Out, CONTROL_BLOCK_ID, 3);
    control_block::ModuleNameLayout ModuleName(Out);
    control_block::MetadataLayout Metadata(Out);

    ModuleName.emit(ScratchRecord, M->Name.str());

    // FIXME: put a real version in here.
#ifdef LLVM_VERSION_INFO
# define EXTRA_VERSION_STRING PACKAGE_STRING LLVM_VERSION_INFO
#else
# define EXTRA_VERSION_STRING PACKAGE_STRING
#endif
    Metadata.emit(ScratchRecord,
                  VERSION_MAJOR, VERSION_MINOR, EXTRA_VERSION_STRING);
#undef EXTRA_VERSION_STRING
  }
}

using ImportPathBlob = llvm::SmallString<64>;
void flattenImportPath(const Module::ImportedModule &import,
                       ImportPathBlob &out) {
  // FIXME: Submodules?
  out.append(import.second->Name.str());

  if (import.first.empty())
    return;

  out.push_back('\0');
  assert(import.first.size() == 1 && "can only handle top-level decl imports");
  auto accessPathElem = import.first.front();
  out.append(accessPathElem.first.str());
}

void Serializer::writeInputFiles(const Module *M,
                                 FilenamesTy inputFiles,
                                 StringRef moduleLinkName) {
  BCBlockRAII restoreBlock(Out, INPUT_BLOCK_ID, 3);
  input_block::SourceFileLayout SourceFile(Out);
  input_block::ImportedModuleLayout ImportedModule(Out);
  input_block::LinkLibraryLayout LinkLibrary(Out);

  for (auto filename : inputFiles) {
    llvm::SmallString<128> path(filename);

    llvm::error_code err;
    err = llvm::sys::fs::make_absolute(path);
    if (err)
      continue;
    
    SourceFile.emit(ScratchRecord, path);
  }

  for (auto file : M->getFiles()) {
    // FIXME: Do some uniquing.
    // FIXME: Clean this up to handle mixed source/AST modules.
    auto SF = dyn_cast<swift::SourceFile>(file);
    if (!SF)
      continue;

    for (auto import : SF->getImports()) {
      if (import.first.second == M->Ctx.TheBuiltinModule)
        continue;

      ImportPathBlob importPath;
      flattenImportPath(import.first, importPath);
      ImportedModule.emit(ScratchRecord, import.second, importPath);
    }
  }

  if (!moduleLinkName.empty()) {
    LinkLibrary.emit(ScratchRecord, serialization::LibraryKind::Library,
                     moduleLinkName);
  }
}

/// Translate AST default argument kind to the Serialization enum values, which
/// are guaranteed to be stable.
static uint8_t getRawStableDefaultArgumentKind(swift::DefaultArgumentKind kind) {
  switch (kind) {
  case swift::DefaultArgumentKind::None:
    return serialization::DefaultArgumentKind::None;
  case swift::DefaultArgumentKind::Normal:
    return serialization::DefaultArgumentKind::Normal;
  case swift::DefaultArgumentKind::Column:
    return serialization::DefaultArgumentKind::Column;
  case swift::DefaultArgumentKind::File:
    return serialization::DefaultArgumentKind::File;
  case swift::DefaultArgumentKind::Line:
    return serialization::DefaultArgumentKind::Line;
  }
}

void Serializer::writePattern(const Pattern *pattern) {
  using namespace decls_block;

  assert(pattern && "null pattern");
  switch (pattern->getKind()) {
  case PatternKind::Paren: {
    unsigned abbrCode = DeclTypeAbbrCodes[ParenPatternLayout::Code];
    ParenPatternLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                   pattern->isImplicit());
    writePattern(cast<ParenPattern>(pattern)->getSubPattern());
    break;
  }
  case PatternKind::Tuple: {
    auto tuple = cast<TuplePattern>(pattern);

    unsigned abbrCode = DeclTypeAbbrCodes[TuplePatternLayout::Code];
    TuplePatternLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                   addTypeRef(tuple->getType()),
                                   tuple->getNumFields(), tuple->hasVararg(),
                                   tuple->isImplicit());

    abbrCode = DeclTypeAbbrCodes[TuplePatternEltLayout::Code];
    for (auto &elt : tuple->getFields()) {
      // FIXME: Default argument expressions?
      TuplePatternEltLayout::emitRecord(
        Out, ScratchRecord, abbrCode,
        getRawStableDefaultArgumentKind(elt.getDefaultArgKind()));
      writePattern(elt.getPattern());
    }
    break;
  }
  case PatternKind::Named: {
    auto named = cast<NamedPattern>(pattern);

    unsigned abbrCode = DeclTypeAbbrCodes[NamedPatternLayout::Code];
    NamedPatternLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                   addDeclRef(named->getDecl()),
                                   named->isImplicit());
    break;
  }
  case PatternKind::Any: {
    unsigned abbrCode = DeclTypeAbbrCodes[AnyPatternLayout::Code];
    AnyPatternLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                 addTypeRef(pattern->getType()),
                                 pattern->isImplicit());
    break;
  }
  case PatternKind::Typed: {
    auto typed = cast<TypedPattern>(pattern);

    unsigned abbrCode = DeclTypeAbbrCodes[TypedPatternLayout::Code];
    TypedPatternLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                   addTypeRef(typed->getType()),
                                   typed->isImplicit());
    writePattern(typed->getSubPattern());
    break;
  }
  case PatternKind::Isa: {
    auto isa = cast<IsaPattern>(pattern);
    
    unsigned abbrCode = DeclTypeAbbrCodes[IsaPatternLayout::Code];
    IsaPatternLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                 addTypeRef(isa->getCastTypeLoc().getType()),
                                 isa->isImplicit());
    break;
  }
  case PatternKind::NominalType: {
    auto nom = cast<NominalTypePattern>(pattern);

    unsigned abbrCode = DeclTypeAbbrCodes[NominalTypePatternLayout::Code];
    auto castTy = nom->getCastTypeLoc().getType();
    NominalTypePatternLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                         addTypeRef(castTy),
                                         nom->getElements().size(),
                                         nom->isImplicit());
    abbrCode = DeclTypeAbbrCodes[NominalTypePatternEltLayout::Code];
    for (auto &elt : nom->getElements()) {
      NominalTypePatternEltLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                              addDeclRef(elt.getProperty()));
      writePattern(elt.getSubPattern());
    }
    break;
  }
  case PatternKind::EnumElement:
  case PatternKind::Expr:
    llvm_unreachable("FIXME: not implemented");

  case PatternKind::Var: {
    auto var = cast<VarPattern>(pattern);
    
    unsigned abbrCode = DeclTypeAbbrCodes[VarPatternLayout::Code];
    VarPatternLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                 var->isImplicit());
    writePattern(var->getSubPattern());
    break;
  }
  }
}

/// Translate from the requirement kind to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableRequirementKind(RequirementKind kind) {
#define CASE(KIND)            \
  case RequirementKind::KIND: \
    return GenericRequirementKind::KIND;

  switch (kind) {
  CASE(Conformance)
  CASE(SameType)
  CASE(WitnessMarker)
  }
#undef CASE
}

void Serializer::writeRequirements(ArrayRef<Requirement> requirements) {
  using namespace decls_block;

  if (requirements.empty())
    return;

  auto reqAbbrCode = DeclTypeAbbrCodes[GenericRequirementLayout::Code];
  for (const auto &req : requirements) { {
    switch (req.getKind()) {
    case RequirementKind::Conformance:
    case RequirementKind::SameType:
      GenericRequirementLayout::emitRecord(
        Out, ScratchRecord, reqAbbrCode,
        getRawStableRequirementKind(req.getKind()),
        addTypeRef(req.getFirstType()),
        addTypeRef(req.getSecondType()));
      break;

    case RequirementKind::WitnessMarker:
      GenericRequirementLayout::emitRecord(
        Out, ScratchRecord, reqAbbrCode,
        getRawStableRequirementKind(req.getKind()),
        llvm::makeArrayRef(addTypeRef(req.getFirstType())));
      break;
    }
  }
  }
}

bool Serializer::writeGenericParams(const GenericParamList *genericParams) {
  using namespace decls_block;

  // Don't write anything if there are no generic params.
  if (!genericParams)
    return true;

  SmallVector<TypeID, 8> archetypeIDs;
  for (auto archetype : genericParams->getAllArchetypes())
    archetypeIDs.push_back(addTypeRef(archetype));

  unsigned abbrCode = DeclTypeAbbrCodes[GenericParamListLayout::Code];
  GenericParamListLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                     archetypeIDs);

  abbrCode = DeclTypeAbbrCodes[GenericParamLayout::Code];
  for (auto next : genericParams->getParams()) {
    GenericParamLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                   addDeclRef(next.getDecl()));
  }

  abbrCode = DeclTypeAbbrCodes[GenericRequirementLayout::Code];
  for (auto next : genericParams->getRequirements()) {
    switch (next.getKind()) {
    case RequirementKind::Conformance:
      GenericRequirementLayout::emitRecord(
                                      Out, ScratchRecord, abbrCode,
                                      GenericRequirementKind::Conformance,
                                      addTypeRef(next.getSubject()),
                                      addTypeRef(next.getConstraint()));
      break;
    case RequirementKind::SameType:
      GenericRequirementLayout::emitRecord(
                                      Out, ScratchRecord, abbrCode,
                                      GenericRequirementKind::SameType,
                                      addTypeRef(next.getFirstType()),
                                      addTypeRef(next.getSecondType()));
      break;
    case RequirementKind::WitnessMarker:
      llvm_unreachable("Can't show up in requirement representations");
      break;
    }
  }

  abbrCode = DeclTypeAbbrCodes[LastGenericRequirementLayout::Code];
  uint8_t dummy = 0;
  LastGenericRequirementLayout::emitRecord(Out, ScratchRecord, abbrCode, dummy);
  return true;
}

bool
Serializer::encodeUnderlyingConformance(const ProtocolConformance *conformance,
                                        DeclID &typeID,
                                        IdentifierID &moduleID) {
  bool append = !isa<NormalProtocolConformance>(conformance);
  if (append) {
    // Encode the type in typeID. Set moduleID to BUILTIN_MODULE_ID to indicate
    // that the underlying conformance will follow. This is safe because there
    // should never be any conformances in the Builtin module.
    typeID = addTypeRef(conformance->getType());
    moduleID = serialization::BUILTIN_MODULE_ID;
  } else {
    typeID = addDeclRef(conformance->getType()->getAnyNominal());
    assert(typeID && "Missing nominal type for specialized conformance");

    // BUILTIN_MODULE_ID is a sentinel for a trailing underlying conformance
    // record.
    moduleID = addModuleRef(conformance->getDeclContext()->getParentModule());
    assert(moduleID != serialization::BUILTIN_MODULE_ID);
  }

  return append;
}

void
Serializer::writeConformance(const ProtocolDecl *protocol,
                             const ProtocolConformance *conformance,
                             const Decl *associatedDecl,
                             const std::array<unsigned, 256> &abbrCodes) {
  using namespace decls_block;

  if (!conformance) {
    unsigned abbrCode = abbrCodes[NoConformanceLayout::Code];
    NoConformanceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                    addDeclRef(protocol));
    return;
  }

  if (associatedDecl) {
    if (auto protoKind = protocol->getKnownProtocolKind()) {
      auto index = static_cast<unsigned>(protoKind.getValue());
      KnownProtocolAdopters[index].push_back(addDeclRef(associatedDecl));
    }
  }

  switch (conformance->getKind()) {
  case ProtocolConformanceKind::Normal: {
    auto conf = cast<NormalProtocolConformance>(conformance);

    SmallVector<DeclID, 16> data;
    unsigned numValueWitnesses = 0;
    unsigned numTypeWitnesses = 0;
    unsigned numDefaultedDefinitions = 0;
    conformance->forEachValueWitness(nullptr, 
                                     [&](ValueDecl *req,
                                         ConcreteDeclRef witness) {
      data.push_back(addDeclRef(req));
      data.push_back(addDeclRef(witness.getDecl()));
      // The substitution records are serialized later.
      data.push_back(witness.getSubstitutions().size());
      ++numValueWitnesses;
    });

    conformance->forEachTypeWitness(/*resolver=*/nullptr,
                                    [&](AssociatedTypeDecl *assocType,
                                        const Substitution &witness) {
       data.push_back(addDeclRef(assocType));
       // The substitution record is serialized later.
       ++numTypeWitnesses;
      return false;
    });

    for (auto defaulted : conf->getDefaultedDefinitions()) {
      data.push_back(addDeclRef(defaulted));
      ++numDefaultedDefinitions;
    }

    unsigned numInheritedConformances = conf->getInheritedConformances().size();
    unsigned abbrCode
      = abbrCodes[NormalProtocolConformanceLayout::Code];
    NormalProtocolConformanceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                                addDeclRef(protocol),
                                                numValueWitnesses,
                                                numTypeWitnesses,
                                                numInheritedConformances,
                                                numDefaultedDefinitions,
                                                data);

    // FIXME: Unfortunate to have to copy these.
    SmallVector<ProtocolDecl *, 8> inheritedProtos;
    SmallVector<ProtocolConformance *, 8> inheritedConformance;
    for (auto inheritedMapping : conf->getInheritedConformances()) {
      inheritedProtos.push_back(inheritedMapping.first);
      inheritedConformance.push_back(inheritedMapping.second);
    }
    writeConformances(inheritedProtos, inheritedConformance, associatedDecl,
                      abbrCodes);
    conformance->forEachValueWitness(nullptr,
                                     [&](ValueDecl *req,
                                         ConcreteDeclRef witness) {
      writeSubstitutions(witness.getSubstitutions(), abbrCodes);
    });
    conformance->forEachTypeWitness(/*resolver=*/nullptr,
                                    [&](AssociatedTypeDecl *assocType,
                                        const Substitution &witness) {
      writeSubstitutions(witness, abbrCodes);
      return false;
    });

    break;
  }

  case ProtocolConformanceKind::Specialized: {
    auto conf = cast<SpecializedProtocolConformance>(conformance);
    auto substitutions = conf->getGenericSubstitutions();
    unsigned abbrCode
      = abbrCodes[SpecializedProtocolConformanceLayout::Code];
    DeclID typeID;
    IdentifierID moduleID;

    bool appendGenericConformance
      = encodeUnderlyingConformance(conf->getGenericConformance(),
                                    typeID, moduleID);

    SpecializedProtocolConformanceLayout::emitRecord(Out, ScratchRecord,
                                                     abbrCode,
                                                     addDeclRef(protocol),
                                                     typeID,
                                                     moduleID,
                                                     substitutions.size());
    writeSubstitutions(substitutions, abbrCodes);

    if (appendGenericConformance) {
      writeConformance(protocol, conf->getGenericConformance(), nullptr,
                       abbrCodes);
    }
    break;
  }

  case ProtocolConformanceKind::Inherited: {
    auto conf = cast<InheritedProtocolConformance>(conformance);
    unsigned abbrCode
      = abbrCodes[InheritedProtocolConformanceLayout::Code];
    DeclID typeID;
    IdentifierID moduleID;

    bool appendInheritedConformance
      = encodeUnderlyingConformance(conf->getInheritedConformance(),
                                    typeID, moduleID);

    InheritedProtocolConformanceLayout::emitRecord(Out, ScratchRecord,
                                                   abbrCode,
                                                   addDeclRef(protocol),
                                                   typeID,
                                                   moduleID);
    if (appendInheritedConformance) {
      writeConformance(protocol, conf->getInheritedConformance(), nullptr,
                       abbrCodes);
    }
    break;
  }
  }
}

void
Serializer::writeConformances(ArrayRef<ProtocolDecl *> protocols,
                              ArrayRef<ProtocolConformance *> conformances,
                              const Decl *associatedDecl,
                              const std::array<unsigned, 256> &abbrCodes) {
  using namespace decls_block;

  for_each(protocols, conformances,
           [&](const ProtocolDecl *proto, const ProtocolConformance *conf) {
    writeConformance(proto, conf, associatedDecl, abbrCodes);
  });
}

/// Writes a list of generic substitutions.
void Serializer::writeSubstitutions(ArrayRef<Substitution> substitutions,
                     const std::array<unsigned, 256> &abbrCodes) {
  using namespace decls_block;
  auto abbrCode = abbrCodes[BoundGenericSubstitutionLayout::Code];
  for (auto &sub : substitutions) {
    BoundGenericSubstitutionLayout::emitRecord(
      Out, ScratchRecord, abbrCode,
      addTypeRef(sub.Archetype),
      addTypeRef(sub.Replacement),
      sub.Archetype->getConformsTo().size());
    // For archetypes the conformance information is context dependent,
    // the conformance array is either empty or full of nulls and can be
    // ignored. We use an array of null pointers for conformances to satisfy
    // the requirement in writeConformances: the first and second arguments
    // have the same size.
    SmallVector<ProtocolConformance *, 4> conformances(
      sub.Archetype->getConformsTo().size(), nullptr);
    writeConformances(sub.Archetype->getConformsTo(), conformances, nullptr,
                      abbrCodes);
  }
}

static bool shouldSerializeMember(Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::TopLevelCode:
  case DeclKind::Extension:
    llvm_unreachable("decl should never be a member");
  
  case DeclKind::EnumCase:
    return false;

  case DeclKind::EnumElement:
  case DeclKind::Protocol:
  case DeclKind::Destructor:
  case DeclKind::PatternBinding:
  case DeclKind::Subscript:
  case DeclKind::TypeAlias:
  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType:
  case DeclKind::Enum:
  case DeclKind::Struct:
  case DeclKind::Class:
  case DeclKind::Var:
  case DeclKind::Func:
  case DeclKind::Constructor:
    return true;
  }
}

void Serializer::writeMembers(ArrayRef<Decl*> members, bool isClass) {
  using namespace decls_block;
  
  unsigned abbrCode = DeclTypeAbbrCodes[DeclContextLayout::Code];
  SmallVector<DeclID, 16> memberIDs;
  for (auto member : members) {
    if (!shouldSerializeMember(member))
      continue;
    
    DeclID memberID = addDeclRef(member);
    memberIDs.push_back(memberID);

    if (isClass) {
      if (auto VD = dyn_cast<ValueDecl>(member)) {
        if (VD->canBeAccessedByDynamicLookup()) {
          auto &list = ClassMembersByName[VD->getName()];
          list.push_back({getKindForTable(VD), memberID});
        }
      }
    }
  }
  DeclContextLayout::emitRecord(Out, ScratchRecord, abbrCode, memberIDs);
}

void Serializer::writeCrossReference(const DeclContext *DC, uint32_t pathLen) {
  using namespace decls_block;

  unsigned abbrCode;

  switch (DC->getContextKind()) {
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::Initializer:
  case DeclContextKind::TopLevelCodeDecl:
    llvm_unreachable("cannot cross-reference this context");

  case DeclContextKind::FileUnit:
    DC = cast<FileUnit>(DC)->getParentModule();
    SWIFT_FALLTHROUGH;

  case DeclContextKind::Module:
    abbrCode = DeclTypeAbbrCodes[XRefLayout::Code];
    XRefLayout::emitRecord(Out, ScratchRecord, abbrCode,
                           addModuleRef(cast<Module>(DC)), pathLen);
    break;

  case DeclContextKind::NominalTypeDecl: {
    writeCrossReference(DC->getParent(), pathLen + 1);

    auto nominal = cast<NominalTypeDecl>(DC);
    abbrCode = DeclTypeAbbrCodes[XRefTypePathPieceLayout::Code];
    XRefTypePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                        addIdentifierRef(nominal->getName()));
    break;
  }

  case DeclContextKind::ExtensionDecl: {
    Type baseTy = cast<ExtensionDecl>(DC)->getExtendedType();
    writeCrossReference(baseTy->getAnyNominal(), pathLen + 1);

    abbrCode = DeclTypeAbbrCodes[XRefExtensionPathPieceLayout::Code];
    XRefExtensionPathPieceLayout::emitRecord(
        Out, ScratchRecord, abbrCode, addModuleRef(DC->getParentModule()));
    break;
  }

  case DeclContextKind::AbstractFunctionDecl: {
    auto fn = cast<AbstractFunctionDecl>(DC);
    writeCrossReference(DC->getParent(), pathLen + 1 + fn->isOperator());

    Type ty = fn->getInterfaceType()->getCanonicalType();
    abbrCode = DeclTypeAbbrCodes[XRefValuePathPieceLayout::Code];
    XRefValuePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                         addTypeRef(ty),
                                         addIdentifierRef(fn->getName()));

    if (fn->isOperator()) {
      // Encode the fixity as a filter on the func decls, to distinguish prefix
      // and postfix operators.
      auto op = cast<FuncDecl>(fn)->getOperatorDecl();
      assert(op);
      abbrCode = DeclTypeAbbrCodes[XRefOperatorPathPieceLayout::Code];
      XRefOperatorPathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                              addIdentifierRef(Identifier()),
                                              getStableFixity(op->getKind()));
    }
    break;
  }
  }
}

void Serializer::writeCrossReference(const Decl *D) {
  using namespace decls_block;

  unsigned abbrCode;

  if (auto op = dyn_cast<OperatorDecl>(D)) {
    writeCrossReference(op->getModuleContext());

    abbrCode = DeclTypeAbbrCodes[XRefOperatorPathPieceLayout::Code];
    XRefOperatorPathPieceLayout::emitRecord(Out, ScratchRecord,abbrCode,
                                            addIdentifierRef(op->getName()),
                                            getStableFixity(op->getKind()));
    return;
  }

  if (auto fn = dyn_cast<AbstractFunctionDecl>(D)) {
    // Functions are special because they might be operators.
    writeCrossReference(fn, 0);
    return;
  }

  writeCrossReference(D->getDeclContext());

  if (auto genericParam = dyn_cast<GenericTypeParamDecl>(D)) {
    abbrCode = DeclTypeAbbrCodes[XRefGenericParamPathPieceLayout::Code];
    XRefGenericParamPathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                                genericParam->getIndex());
    return;
  }

  if (auto type = dyn_cast<TypeDecl>(D)) {
    abbrCode = DeclTypeAbbrCodes[XRefTypePathPieceLayout::Code];
    XRefTypePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                        addIdentifierRef(type->getName()));
    return;
  }

  auto val = cast<ValueDecl>(D);
  auto ty = val->getInterfaceType()->getCanonicalType();
  abbrCode = DeclTypeAbbrCodes[XRefValuePathPieceLayout::Code];
  XRefValuePathPieceLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                       addTypeRef(ty),
                                       addIdentifierRef(val->getName()));
}

/// Translate from the AST associativity enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableAssociativity(swift::Associativity assoc) {
  switch (assoc) {
  case swift::Associativity::Left:
    return serialization::Associativity::LeftAssociative;
  case swift::Associativity::Right:
    return serialization::Associativity::RightAssociative;
  case swift::Associativity::None:
    return serialization::Associativity::NonAssociative;
  }
}

/// Asserts if the declaration has any attributes other than the ones
/// specified in the template parameters.
///
/// This is a no-op in release builds.
template <AttrKind ...KINDS>
static void checkAllowedAttributes(const Decl *D) {
#ifndef NDEBUG
  DeclAttributes attrs = D->getAttrs();
  for (AttrKind AK : { KINDS... })
    attrs.clearAttribute(AK);

  if (!attrs.empty()) {
    llvm::errs() << "Serialization: unhandled attributes ";
    attrs.print(llvm::errs());
    llvm::errs() << "\n";
    llvm_unreachable("TODO: handle the above attributes");
  }
#endif
}

template<>
void checkAllowedAttributes(const Decl *D) {
  assert(D->getAttrs().empty());
}

void Serializer::writeDecl(const Decl *D) {
  using namespace decls_block;

  assert(!D->isInvalid() && "cannot create a module with an invalid decl");
  const DeclContext *topLevel = D->getDeclContext()->getModuleScopeContext();
  if (SF ? (topLevel != SF) : (topLevel->getParentModule() != M)) {
    writeCrossReference(D);
    return;
  }

  assert(!D->hasClangNode() && "imported decls should use cross-references");

  switch (D->getKind()) {
  case DeclKind::Import:
    llvm_unreachable("import decls should not be serialized");

  case DeclKind::Extension: {
    auto extension = cast<ExtensionDecl>(D);

    // @transparent on extensions is propagated down to the methods and
    // constructors during serialization.
    checkAllowedAttributes<AK_transparent>(extension);

    const Decl *DC = getDeclForContext(extension->getDeclContext());
    Type baseTy = extension->getExtendedType();

    unsigned abbrCode = DeclTypeAbbrCodes[ExtensionLayout::Code];
    ExtensionLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                addTypeRef(baseTy),
                                addDeclRef(DC),
                                extension->isImplicit());

    writeConformances(extension->getProtocols(), extension->getConformances(),
                      extension, DeclTypeAbbrCodes);

    bool isClassExtension = false;
    if (auto baseNominal = baseTy->getAnyNominal()) {
      isClassExtension = isa<ClassDecl>(baseNominal) ||
                         isa<ProtocolDecl>(baseNominal);
    }
    writeMembers(extension->getMembers(), isClassExtension);

    break;
  }

  case DeclKind::EnumCase:
    llvm_unreachable("enum case decls should not be serialized");
      
  case DeclKind::PatternBinding: {
    auto binding = cast<PatternBindingDecl>(D);
    checkAllowedAttributes<>(binding);

    const Decl *DC = getDeclForContext(binding->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[PatternBindingLayout::Code];
    PatternBindingLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                     addDeclRef(DC), binding->isImplicit(),
                                     binding->isStatic(),
                                     binding->hasStorage());

    writePattern(binding->getPattern());
    // Ignore initializer; external clients don't need to know about it.

    break;
  }

  case DeclKind::TopLevelCode:
    // Top-level code is ignored; external clients don't need to know about it.
    break;

  case DeclKind::InfixOperator: {
    auto op = cast<InfixOperatorDecl>(D);
    checkAllowedAttributes<>(op);

    const Decl *DC = getDeclForContext(op->getDeclContext());
    auto associativity = getRawStableAssociativity(op->getAssociativity());

    unsigned abbrCode = DeclTypeAbbrCodes[InfixOperatorLayout::Code];
    InfixOperatorLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                    addIdentifierRef(op->getName()),
                                    addDeclRef(DC),
                                    associativity,
                                    op->getPrecedence());
    break;
  }
      
  case DeclKind::PrefixOperator: {
    auto op = cast<PrefixOperatorDecl>(D);
    checkAllowedAttributes<>(op);

    const Decl *DC = getDeclForContext(op->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[PrefixOperatorLayout::Code];
    PrefixOperatorLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                     addIdentifierRef(op->getName()),
                                     addDeclRef(DC));
    break;
  }
    
  case DeclKind::PostfixOperator: {
    auto op = cast<PostfixOperatorDecl>(D);
    checkAllowedAttributes<>(op);

    const Decl *DC = getDeclForContext(op->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[PostfixOperatorLayout::Code];
    PostfixOperatorLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                      addIdentifierRef(op->getName()),
                                      addDeclRef(DC));
    break;
  }

  case DeclKind::TypeAlias: {
    auto typeAlias = cast<TypeAliasDecl>(D);
    assert(!typeAlias->isObjC() && "ObjC typealias is not meaningful");
    assert(typeAlias->getProtocols().empty() &&
           "concrete typealiases cannot have protocols");
    checkAllowedAttributes<>(typeAlias);

    const Decl *DC = getDeclForContext(typeAlias->getDeclContext());

    Type underlying;
    if (typeAlias->hasUnderlyingType())
      underlying = typeAlias->getUnderlyingType();

    unsigned abbrCode = DeclTypeAbbrCodes[TypeAliasLayout::Code];
    TypeAliasLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                addIdentifierRef(typeAlias->getName()),
                                addDeclRef(DC),
                                addTypeRef(underlying),
                                addTypeRef(typeAlias->getInterfaceType()),
                                typeAlias->isImplicit());
    break;
  }

  case DeclKind::GenericTypeParam: {
    auto genericParam = cast<GenericTypeParamDecl>(D);
    checkAllowedAttributes<>(genericParam);

    const Decl *DC = getDeclForContext(genericParam->getDeclContext());

    SmallVector<DeclID, 4> protocols;
    for (auto proto : genericParam->getProtocols())
      protocols.push_back(addDeclRef(proto));

    unsigned abbrCode = DeclTypeAbbrCodes[GenericTypeParamDeclLayout::Code];
    GenericTypeParamDeclLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                addIdentifierRef(genericParam->getName()),
                                addDeclRef(DC),
                                genericParam->isImplicit(),
                                genericParam->getDepth(),
                                genericParam->getIndex(),
                                addTypeRef(genericParam->getSuperclass()),
                                addTypeRef(genericParam->getArchetype()),
                                protocols);
    break;
  }

  case DeclKind::AssociatedType: {
    auto assocType = cast<AssociatedTypeDecl>(D);
    checkAllowedAttributes<>(assocType);

    const Decl *DC = getDeclForContext(assocType->getDeclContext());

    SmallVector<DeclID, 4> protocols;
    for (auto proto : assocType->getProtocols())
      protocols.push_back(addDeclRef(proto));

    unsigned abbrCode = DeclTypeAbbrCodes[AssociatedTypeDeclLayout::Code];
    AssociatedTypeDeclLayout::emitRecord(
      Out, ScratchRecord, abbrCode,
      addIdentifierRef(assocType->getName()),
      addDeclRef(DC),
      addTypeRef(assocType->getSuperclass()),
      addTypeRef(assocType->getArchetype()),
      addTypeRef(assocType->getDefaultDefinitionType()),
      assocType->isImplicit(),
      protocols);
    break;
  }

  case DeclKind::Struct: {
    auto theStruct = cast<StructDecl>(D);
    checkAllowedAttributes<>(theStruct);

    const Decl *DC = getDeclForContext(theStruct->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[StructLayout::Code];
    StructLayout::emitRecord(Out, ScratchRecord, abbrCode,
                             addIdentifierRef(theStruct->getName()),
                             addDeclRef(DC),
                             theStruct->isImplicit());

    writeGenericParams(theStruct->getGenericParams());
    writeRequirements(theStruct->getGenericRequirements());
    writeConformances(theStruct->getProtocols(), theStruct->getConformances(),
                      theStruct, DeclTypeAbbrCodes);
    writeMembers(theStruct->getMembers(), false);
    break;
  }

  case DeclKind::Enum: {
    auto theEnum = cast<EnumDecl>(D);
    checkAllowedAttributes<>(theEnum);

    const Decl *DC = getDeclForContext(theEnum->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[EnumLayout::Code];
    EnumLayout::emitRecord(Out, ScratchRecord, abbrCode,
                            addIdentifierRef(theEnum->getName()),
                            addDeclRef(DC),
                            theEnum->isImplicit(),
                            addTypeRef(theEnum->getRawType()));

    writeGenericParams(theEnum->getGenericParams());
    writeRequirements(theEnum->getGenericRequirements());
    writeConformances(theEnum->getProtocols(), theEnum->getConformances(),
                      theEnum, DeclTypeAbbrCodes);
    writeMembers(theEnum->getMembers(), false);
    break;
  }

  case DeclKind::Class: {
    auto theClass = cast<ClassDecl>(D);
    checkAllowedAttributes<
      AK_IBLiveView, AK_objc, AK_resilient, AK_fragile,
      AK_born_fragile, AK_requires_stored_property_inits>(theClass);

    const Decl *DC = getDeclForContext(theClass->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[ClassLayout::Code];
    ClassLayout::emitRecord(Out, ScratchRecord, abbrCode,
                            addIdentifierRef(theClass->getName()),
                            addDeclRef(DC),
                            theClass->isImplicit(),
                            theClass->isObjC(),
                            theClass->getAttrs().isIBLiveView(),
                            (unsigned)theClass->getAttrs().getResilienceKind(),
                            theClass->getAttrs().requiresStoredPropertyInits(),
                            theClass->requiresStoredPropertyInits(),
                            addTypeRef(theClass->getSuperclass()));

    writeGenericParams(theClass->getGenericParams());
    writeRequirements(theClass->getGenericRequirements());
    writeConformances(theClass->getProtocols(), theClass->getConformances(),
                      theClass, DeclTypeAbbrCodes);
    writeMembers(theClass->getMembers(), true);
    break;
  }


  case DeclKind::Protocol: {
    auto proto = cast<ProtocolDecl>(D);
    checkAllowedAttributes<AK_class_protocol, AK_objc>(proto);

    const Decl *DC = getDeclForContext(proto->getDeclContext());

    SmallVector<DeclID, 4> protocols;
    for (auto proto : proto->getProtocols())
      protocols.push_back(addDeclRef(proto));

    unsigned abbrCode = DeclTypeAbbrCodes[ProtocolLayout::Code];
    ProtocolLayout::emitRecord(Out, ScratchRecord, abbrCode,
                               addIdentifierRef(proto->getName()),
                               addDeclRef(DC),
                               proto->isImplicit(),
                               proto->getAttrs().isClassProtocol(),
                               proto->isObjC(),
                               protocols);

    writeGenericParams(proto->getGenericParams());
    writeRequirements(proto->getGenericRequirements());
    writeMembers(proto->getMembers(), true);
    break;
  }

  case DeclKind::Var: {
    auto var = cast<VarDecl>(D);
    checkAllowedAttributes<
      AK_IBOutlet, AK_objc, AK_optional, AK_unowned, AK_weak, AK_transparent
    >(var);

    const Decl *DC = getDeclForContext(var->getDeclContext());
    Type type = var->hasType() ? var->getType() : nullptr;

    unsigned abbrCode = DeclTypeAbbrCodes[VarLayout::Code];
    VarLayout::emitRecord(Out, ScratchRecord, abbrCode,
                          addIdentifierRef(var->getName()),
                          addDeclRef(DC),
                          var->isImplicit(),
                          var->isObjC(),
                          var->getAttrs().isIBOutlet(),
                          var->getAttrs().isOptional(),
                          var->isStatic(),
                          var->isLet(),
                          addTypeRef(type),
                          addTypeRef(var->getInterfaceType()),
                          addDeclRef(var->getGetter()),
                          addDeclRef(var->getSetter()),
                          addDeclRef(var->getOverriddenDecl()));
    break;
  }

  case DeclKind::Func: {
    auto fn = cast<FuncDecl>(D);
    checkAllowedAttributes<
      AK_asmname, AK_assignment, AK_conversion, AK_IBAction, AK_infix,
      AK_noreturn, AK_objc, AK_optional, AK_postfix, AK_prefix, AK_transparent,
      AK_mutating
    >(fn);

    const Decl *DC = getDeclForContext(fn->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[FuncLayout::Code];
    FuncLayout::emitRecord(Out, ScratchRecord, abbrCode,
                           addIdentifierRef(fn->getName()),
                           addDeclRef(DC),
                           fn->isImplicit(),
                           fn->hasSelectorStyleSignature(),
                           fn->isStatic(),
                           fn->getAttrs().isAssignment() ||
                             fn->getAttrs().isConversion(),
                           fn->isObjC(),
                           fn->getAttrs().isIBAction(),
                           fn->isTransparent(),
                           fn->isMutating(),
                           fn->hasDynamicSelf(),
                           fn->getAttrs().isOptional(),
                           fn->getArgParamPatterns().size(),
                           addTypeRef(fn->getType()),
                           addTypeRef(fn->getInterfaceType()),
                           addDeclRef(fn->getOperatorDecl()),
                           addDeclRef(fn->getOverriddenDecl()),
                           fn->getAttrs().AsmName);

    writeGenericParams(fn->getGenericParams());

    // Write both argument and body parameters. This is important for proper
    // error messages with selector-style declarations.
    for (auto pattern : fn->getArgParamPatterns())
      writePattern(pattern);
    for (auto pattern : fn->getBodyParamPatterns())
      writePattern(pattern);

    break;
  }

  case DeclKind::EnumElement: {
    auto elem = cast<EnumElementDecl>(D);
    checkAllowedAttributes<>(elem);

    const Decl *DC = getDeclForContext(elem->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[EnumElementLayout::Code];
    EnumElementLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                  addIdentifierRef(elem->getName()),
                                  addDeclRef(DC),
                                  addTypeRef(elem->getArgumentType()),
                                  addTypeRef(elem->getType()),
                                  addTypeRef(elem->getInterfaceType()),
                                  elem->isImplicit());
    break;
  }

  case DeclKind::Subscript: {
    auto subscript = cast<SubscriptDecl>(D);
    checkAllowedAttributes<AK_objc, AK_optional>(subscript);

    const Decl *DC = getDeclForContext(subscript->getDeclContext());

    unsigned abbrCode = DeclTypeAbbrCodes[SubscriptLayout::Code];
    SubscriptLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                addDeclRef(DC),
                                subscript->isImplicit(),
                                subscript->isObjC(),
                                subscript->getAttrs().isOptional(),
                                addTypeRef(subscript->getType()),
                                addTypeRef(subscript->getElementType()),
                                addTypeRef(subscript->getInterfaceType()),
                                addDeclRef(subscript->getGetter()),
                                addDeclRef(subscript->getSetter()),
                                addDeclRef(subscript->getOverriddenDecl()));

    writePattern(subscript->getIndices());
    break;
  }


  case DeclKind::Constructor: {
    auto ctor = cast<ConstructorDecl>(D);
    checkAllowedAttributes<AK_objc, AK_transparent>(ctor);

    const Decl *DC = getDeclForContext(ctor->getDeclContext());
    auto implicitSelf = ctor->getImplicitSelfDecl();

    unsigned abbrCode = DeclTypeAbbrCodes[ConstructorLayout::Code];
    ConstructorLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                  addDeclRef(DC),
                                  ctor->isImplicit(),
                                  ctor->hasSelectorStyleSignature(),
                                  ctor->isObjC(),
                                  ctor->isTransparent(),
                                  addTypeRef(ctor->getType()),
                                  addTypeRef(ctor->getInterfaceType()),
                                  addDeclRef(implicitSelf));

    writeGenericParams(ctor->getGenericParams());
    writePattern(ctor->getArgParams());
    writePattern(ctor->getBodyParams());
    break;
  }

  case DeclKind::Destructor: {
    auto dtor = cast<DestructorDecl>(D);
    checkAllowedAttributes<AK_objc>(dtor);

    const Decl *DC = getDeclForContext(dtor->getDeclContext());
    auto implicitSelf = dtor->getImplicitSelfDecl();

    unsigned abbrCode = DeclTypeAbbrCodes[DestructorLayout::Code];
    DestructorLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                 addDeclRef(DC),
                                 dtor->isImplicit(),
                                 dtor->isObjC(),
                                 addTypeRef(dtor->getType()),
                                 addDeclRef(implicitSelf));
    
    break;
  }
  }
}

#define SIMPLE_CASE(TYPENAME, VALUE) \
  case swift::TYPENAME::VALUE: return uint8_t(serialization::TYPENAME::VALUE);

/// Translate from the AST calling convention enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableCC(swift::AbstractCC cc) {
  switch (cc) {
  SIMPLE_CASE(AbstractCC, C)
  SIMPLE_CASE(AbstractCC, ObjCMethod)
  SIMPLE_CASE(AbstractCC, Freestanding)
  SIMPLE_CASE(AbstractCC, Method)
  SIMPLE_CASE(AbstractCC, WitnessMethod)
  }
  llvm_unreachable("bad calling convention");
}

/// Translate from the AST ownership enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableOwnership(swift::Ownership ownership) {
  switch (ownership) {
  SIMPLE_CASE(Ownership, Strong)
  SIMPLE_CASE(Ownership, Weak)
  SIMPLE_CASE(Ownership, Unowned)
  }
  llvm_unreachable("bad ownership kind");
}

/// Translate from the AST ParameterConvention enum to the
/// Serialization enum values, which are guaranteed to be stable.
static uint8_t getRawStableParameterConvention(swift::ParameterConvention pc) {
  switch (pc) {
  SIMPLE_CASE(ParameterConvention, Indirect_In)
  SIMPLE_CASE(ParameterConvention, Indirect_Out)
  SIMPLE_CASE(ParameterConvention, Indirect_Inout)
  SIMPLE_CASE(ParameterConvention, Direct_Owned)
  SIMPLE_CASE(ParameterConvention, Direct_Unowned)
  SIMPLE_CASE(ParameterConvention, Direct_Guaranteed)
  }
  llvm_unreachable("bad parameter convention kind");
}

/// Translate from the AST ResultConvention enum to the
/// Serialization enum values, which are guaranteed to be stable.
static uint8_t getRawStableResultConvention(swift::ResultConvention rc) {
  switch (rc) {
  SIMPLE_CASE(ResultConvention, Owned)
  SIMPLE_CASE(ResultConvention, Unowned)
  SIMPLE_CASE(ResultConvention, Autoreleased)
  }
  llvm_unreachable("bad result convention kind");
}

#undef SIMPLE_CASE

/// Find the typealias given a builtin type.
static TypeAliasDecl *findTypeAliasForBuiltin(ASTContext &Ctx,
                                              BuiltinType *Bt) {
  /// Get the type name by chopping off "Builtin.".
  llvm::SmallString<32> FullName;
  llvm::raw_svector_ostream OS(FullName);
  Bt->print(OS);
  OS.flush();
  assert(FullName.startswith("Builtin."));
  StringRef TypeName = FullName.substr(8);

  SmallVector<ValueDecl*, 4> CurModuleResults;
  Ctx.TheBuiltinModule->lookupValue(Module::AccessPathTy(),
                                    Ctx.getIdentifier(TypeName),
                                    NLKind::QualifiedLookup,
                                    CurModuleResults);
  assert(CurModuleResults.size() == 1);
  return cast<TypeAliasDecl>(CurModuleResults[0]);
}

void Serializer::writeType(Type ty) {
  using namespace decls_block;

  switch (ty.getPointer()->getKind()) {
  case TypeKind::Error:
    llvm_unreachable("should not serialize an error type");

  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinVector: {
    TypeAliasDecl *typeAlias =
      findTypeAliasForBuiltin(M->Ctx, ty->castTo<BuiltinType>());

    unsigned abbrCode = DeclTypeAbbrCodes[NameAliasTypeLayout::Code];
    NameAliasTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                    addDeclRef(typeAlias));
    break;
  }
  case TypeKind::NameAlias: {
    auto nameAlias = cast<NameAliasType>(ty.getPointer());
    const TypeAliasDecl *typeAlias = nameAlias->getDecl();

    unsigned abbrCode = DeclTypeAbbrCodes[NameAliasTypeLayout::Code];
    NameAliasTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                    addDeclRef(typeAlias));
    break;
  }

  case TypeKind::Paren: {
    auto parenTy = cast<ParenType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[ParenTypeLayout::Code];
    ParenTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                addTypeRef(parenTy->getUnderlyingType()));
    break;
  }

  case TypeKind::Tuple: {
    auto tupleTy = cast<TupleType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[TupleTypeLayout::Code];
    TupleTypeLayout::emitRecord(Out, ScratchRecord, abbrCode);

    abbrCode = DeclTypeAbbrCodes[TupleTypeEltLayout::Code];
    for (auto &elt : tupleTy->getFields()) {
      uint8_t rawDefaultArg
        = getRawStableDefaultArgumentKind(elt.getDefaultArgKind());
      TupleTypeEltLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                     addIdentifierRef(elt.getName()),
                                     addTypeRef(elt.getType()),
                                     rawDefaultArg,
                                     elt.isVararg());
    }

    break;
  }

  case TypeKind::Struct:
  case TypeKind::Enum:
  case TypeKind::Class:
  case TypeKind::Protocol: {
    auto nominalTy = cast<NominalType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[NominalTypeLayout::Code];
    NominalTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                  addDeclRef(nominalTy->getDecl()),
                                  addTypeRef(nominalTy->getParent()));
    break;
  }

  case TypeKind::Metatype: {
    auto metatypeTy = cast<MetatypeType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[MetatypeTypeLayout::Code];
    bool hasThin = metatypeTy->hasThin();
    bool isThin = hasThin ? metatypeTy->isThin() : false;
    MetatypeTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                   addTypeRef(metatypeTy->getInstanceType()),
                                   hasThin, isThin);
    break;
  }

  case TypeKind::Module:
    llvm_unreachable("modules are currently not first-class values");

  case TypeKind::DynamicSelf: {
    auto dynamicSelfTy = cast<DynamicSelfType>(ty.getPointer());
    unsigned abbrCode = DeclTypeAbbrCodes[DynamicSelfTypeLayout::Code];
    DynamicSelfTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                      addTypeRef(dynamicSelfTy->getSelfType()));
    break;
  }

  case TypeKind::Archetype: {
    auto archetypeTy = cast<ArchetypeType>(ty.getPointer());

    TypeID indexOrParentID;
    if (archetypeTy->isPrimary())
      indexOrParentID = archetypeTy->getPrimaryIndex();
    else
      indexOrParentID = addTypeRef(archetypeTy->getParent());

    SmallVector<DeclID, 4> conformances;
    for (auto proto : archetypeTy->getConformsTo())
      conformances.push_back(addDeclRef(proto));

    DeclID assocTypeOrProtoID;
    if (auto assocType = archetypeTy->getAssocType())
      assocTypeOrProtoID = addDeclRef(assocType);
    else
      assocTypeOrProtoID = addDeclRef(archetypeTy->getSelfProtocol());

    unsigned abbrCode = DeclTypeAbbrCodes[ArchetypeTypeLayout::Code];
    ArchetypeTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                    addIdentifierRef(archetypeTy->getName()),
                                    archetypeTy->isPrimary(),
                                    indexOrParentID,
                                    assocTypeOrProtoID,
                                    addTypeRef(archetypeTy->getSuperclass()),
                                    conformances);

    SmallVector<IdentifierID, 4> nestedTypeNames;
    SmallVector<TypeID, 4> nestedTypes;
    for (auto next : archetypeTy->getNestedTypes()) {
      nestedTypeNames.push_back(addIdentifierRef(next.first));
      nestedTypes.push_back(addTypeRef(next.second));
    }

    abbrCode = DeclTypeAbbrCodes[ArchetypeNestedTypeNamesLayout::Code];
    ArchetypeNestedTypeNamesLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                               nestedTypeNames);

    abbrCode = DeclTypeAbbrCodes[ArchetypeNestedTypesLayout::Code];
    ArchetypeNestedTypesLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                           nestedTypes);

    break;
  }

  case TypeKind::GenericTypeParam: {
    auto genericParam = cast<GenericTypeParamType>(ty.getPointer());
    unsigned abbrCode = DeclTypeAbbrCodes[GenericTypeParamTypeLayout::Code];
    DeclID declIDOrDepth;
    unsigned indexPlusOne;
    if (genericParam->getDecl()) {
      declIDOrDepth = addDeclRef(genericParam->getDecl());
      indexPlusOne = 0;
    } else {
      declIDOrDepth = genericParam->getDepth();
      indexPlusOne = genericParam->getIndex() + 1;
    }
    GenericTypeParamTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                           declIDOrDepth, indexPlusOne);
    break;
  }

  case TypeKind::AssociatedType: {
    auto assocType = cast<AssociatedTypeType>(ty.getPointer());
    unsigned abbrCode = DeclTypeAbbrCodes[AssociatedTypeTypeLayout::Code];
    AssociatedTypeTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                         addDeclRef(assocType->getDecl()));
    break;
  }

  case TypeKind::Substituted: {
    auto subTy = cast<SubstitutedType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[SubstitutedTypeLayout::Code];
    SubstitutedTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                      addTypeRef(subTy->getOriginal()),
                                      addTypeRef(subTy->getReplacementType()));
    break;
  }

  case TypeKind::DependentMember: {
    auto dependent = cast<DependentMemberType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[DependentMemberTypeLayout::Code];
    assert(dependent->getAssocType() && "Unchecked dependent member type");
    DependentMemberTypeLayout::emitRecord(
      Out, ScratchRecord, abbrCode,
      addTypeRef(dependent->getBase()),
      addDeclRef(dependent->getAssocType()));
    break;
  }

  case TypeKind::Function: {
    auto fnTy = cast<FunctionType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[FunctionTypeLayout::Code];
    FunctionTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                   addTypeRef(fnTy->getInput()),
                                   addTypeRef(fnTy->getResult()),
                                   getRawStableCC(fnTy->getAbstractCC()),
                                   fnTy->isAutoClosure(),
                                   fnTy->isThin(),
                                   fnTy->isNoReturn(),
                                   fnTy->isBlock());
    break;
  }

  case TypeKind::PolymorphicFunction: {
    auto fnTy = cast<PolymorphicFunctionType>(ty.getPointer());
    const Decl *genericContext = getGenericContext(&fnTy->getGenericParams());
    auto callingConvention = fnTy->getAbstractCC();
    DeclID dID = genericContext ? addDeclRef(genericContext) : DeclID(0);

    unsigned abbrCode = DeclTypeAbbrCodes[PolymorphicFunctionTypeLayout::Code];
    PolymorphicFunctionTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                              addTypeRef(fnTy->getInput()),
                                              addTypeRef(fnTy->getResult()),
                                              dID,
                                              getRawStableCC(callingConvention),
                                              fnTy->isThin(),
                                              fnTy->isNoReturn());
    if (!genericContext)
      writeGenericParams(&fnTy->getGenericParams());
    break;
  }

  case TypeKind::GenericFunction: {
    auto fnTy = cast<GenericFunctionType>(ty.getPointer());
    unsigned abbrCode = DeclTypeAbbrCodes[GenericFunctionTypeLayout::Code];
    auto callingConvention = fnTy->getAbstractCC();
    SmallVector<TypeID, 4> genericParams;
    for (auto param : fnTy->getGenericParams())
      genericParams.push_back(addTypeRef(param));
    GenericFunctionTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                          addTypeRef(fnTy->getInput()),
                                          addTypeRef(fnTy->getResult()),
                                          getRawStableCC(callingConvention),
                                          fnTy->isThin(),
                                          fnTy->isNoReturn(),
                                          genericParams);

    // Write requirements.
    writeRequirements(fnTy->getRequirements());
    break;
  }

  case TypeKind::SILFunction: {
    auto fnTy = cast<SILFunctionType>(ty.getPointer());

    auto genericParams = fnTy->getGenericParams();

    auto callingConvention = fnTy->getAbstractCC();
    auto result = fnTy->getResult();
    auto interfaceResult = fnTy->getInterfaceResult();
    auto stableResultConvention =
      getRawStableResultConvention(result.getConvention());
    auto stableInterfaceResultConvention =
      getRawStableResultConvention(interfaceResult.getConvention());

    SmallVector<TypeID, 8> paramTypes;
    for (auto param : fnTy->getParameters()) {
      paramTypes.push_back(addTypeRef(param.getType()));
      unsigned conv = getRawStableParameterConvention(param.getConvention());
      paramTypes.push_back(TypeID(conv));
    }
    for (auto param : fnTy->getInterfaceParameters()) {
      paramTypes.push_back(addTypeRef(param.getType()));
      unsigned conv = getRawStableParameterConvention(param.getConvention());
      paramTypes.push_back(TypeID(conv));
    }
    
    auto sig = fnTy->getGenericSignature();
    if (sig) {
      for (auto param : sig->getGenericParams())
        paramTypes.push_back(addTypeRef(param));
    }

    auto stableCalleeConvention =
      getRawStableParameterConvention(fnTy->getCalleeConvention());

    unsigned abbrCode = DeclTypeAbbrCodes[SILFunctionTypeLayout::Code];
    SILFunctionTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                      addTypeRef(result.getType()),
                                      stableResultConvention,
                                      addTypeRef(interfaceResult.getType()),
                                      stableInterfaceResultConvention,
                                      // FIXME: Always serialize a new
                                      // GenericParamList for now.
                                      // Interface types will kill this soon.
                                      DeclID(0),
                                      stableCalleeConvention,
                                      getRawStableCC(callingConvention),
                                      fnTy->isThin(),
                                      fnTy->isNoReturn(),
                                      sig ? sig->getGenericParams().size() : 0,
                                      paramTypes);
    if (sig)
      writeRequirements(sig->getRequirements());
    else
      writeRequirements({});
    if (genericParams)
      writeGenericParams(genericParams);
    break;
  }

  case TypeKind::Array: {
    auto arrayTy = cast<ArrayType>(ty.getPointer());

    Type base = arrayTy->getBaseType();

    unsigned abbrCode = DeclTypeAbbrCodes[ArrayTypeLayout::Code];
    ArrayTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                     addTypeRef(base), arrayTy->getSize());
    break;
  }

  case TypeKind::ArraySlice: {
    auto sliceTy = cast<ArraySliceType>(ty.getPointer());

    Type base = sliceTy->getBaseType();

    unsigned abbrCode = DeclTypeAbbrCodes[ArraySliceTypeLayout::Code];
    ArraySliceTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                     addTypeRef(base));
    break;
  }

  case TypeKind::Optional: {
    auto sliceTy = cast<OptionalType>(ty.getPointer());

    Type base = sliceTy->getBaseType();

    unsigned abbrCode = DeclTypeAbbrCodes[OptionalTypeLayout::Code];
    OptionalTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                   addTypeRef(base));
    break;
  }

  case TypeKind::UncheckedOptional: {
    auto sliceTy = cast<UncheckedOptionalType>(ty.getPointer());

    Type base = sliceTy->getBaseType();

    unsigned abbrCode = DeclTypeAbbrCodes[UncheckedOptionalTypeLayout::Code];
    UncheckedOptionalTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                            addTypeRef(base));
    break;
  }

  case TypeKind::ProtocolComposition: {
    auto composition = cast<ProtocolCompositionType>(ty.getPointer());

    SmallVector<TypeID, 4> protocols;
    for (auto proto : composition->getProtocols())
      protocols.push_back(addTypeRef(proto));

    unsigned abbrCode = DeclTypeAbbrCodes[ProtocolCompositionTypeLayout::Code];
    ProtocolCompositionTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                              protocols);
    break;
  }

  case TypeKind::LValue: {
    auto lValueTy = cast<LValueType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[LValueTypeLayout::Code];
    LValueTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                 addTypeRef(lValueTy->getObjectType()));
    break;
  }
  case TypeKind::InOut: {
    auto iotTy = cast<InOutType>(ty.getPointer());
    
    unsigned abbrCode = DeclTypeAbbrCodes[InOutTypeLayout::Code];
    InOutTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                addTypeRef(iotTy->getObjectType()));
    break;
  }

  case TypeKind::UnownedStorage:
  case TypeKind::WeakStorage: {
    auto refTy = cast<ReferenceStorageType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[ReferenceStorageTypeLayout::Code];
    auto stableOwnership = getRawStableOwnership(refTy->getOwnership());
    ReferenceStorageTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                           stableOwnership,
                                  addTypeRef(refTy->getReferentType()));
    break;
  }

  case TypeKind::UnboundGeneric: {
    auto generic = cast<UnboundGenericType>(ty.getPointer());

    unsigned abbrCode = DeclTypeAbbrCodes[UnboundGenericTypeLayout::Code];
    UnboundGenericTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                         addDeclRef(generic->getDecl()),
                                         addTypeRef(generic->getParent()));
    break;
  }

  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct: {
    auto generic = cast<BoundGenericType>(ty.getPointer());

    SmallVector<TypeID, 8> genericArgIDs;
    for (auto next : generic->getGenericArgs())
      genericArgIDs.push_back(addTypeRef(next));

    unsigned abbrCode = DeclTypeAbbrCodes[BoundGenericTypeLayout::Code];
    BoundGenericTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                       addDeclRef(generic->getDecl()),
                                       addTypeRef(generic->getParent()),
                                       genericArgIDs);
    break;
  }

  case TypeKind::TypeVariable:
    llvm_unreachable("type variables should not escape the type checker");
  }
}

void Serializer::writeAllDeclsAndTypes() {
  BCBlockRAII restoreBlock(Out, DECLS_AND_TYPES_BLOCK_ID, 8);

  {
    using namespace decls_block;
    registerDeclTypeAbbr<NameAliasTypeLayout>();
    registerDeclTypeAbbr<GenericTypeParamDeclLayout>();
    registerDeclTypeAbbr<AssociatedTypeDeclLayout>();
    registerDeclTypeAbbr<NominalTypeLayout>();
    registerDeclTypeAbbr<ParenTypeLayout>();
    registerDeclTypeAbbr<TupleTypeLayout>();
    registerDeclTypeAbbr<TupleTypeEltLayout>();
    registerDeclTypeAbbr<FunctionTypeLayout>();
    registerDeclTypeAbbr<MetatypeTypeLayout>();
    registerDeclTypeAbbr<LValueTypeLayout>();
    registerDeclTypeAbbr<InOutTypeLayout>();
    registerDeclTypeAbbr<ArchetypeTypeLayout>();
    registerDeclTypeAbbr<ArchetypeNestedTypeNamesLayout>();
    registerDeclTypeAbbr<ArchetypeNestedTypesLayout>();
    registerDeclTypeAbbr<ProtocolCompositionTypeLayout>();
    registerDeclTypeAbbr<SubstitutedTypeLayout>();
    registerDeclTypeAbbr<BoundGenericTypeLayout>();
    registerDeclTypeAbbr<BoundGenericSubstitutionLayout>();
    registerDeclTypeAbbr<PolymorphicFunctionTypeLayout>();
    registerDeclTypeAbbr<GenericFunctionTypeLayout>();
    registerDeclTypeAbbr<SILFunctionTypeLayout>();
    registerDeclTypeAbbr<ArraySliceTypeLayout>();
    registerDeclTypeAbbr<ArrayTypeLayout>();
    registerDeclTypeAbbr<ReferenceStorageTypeLayout>();
    registerDeclTypeAbbr<UnboundGenericTypeLayout>();
    registerDeclTypeAbbr<OptionalTypeLayout>();
    registerDeclTypeAbbr<UncheckedOptionalTypeLayout>();

    registerDeclTypeAbbr<TypeAliasLayout>();
    registerDeclTypeAbbr<GenericTypeParamTypeLayout>();
    registerDeclTypeAbbr<AssociatedTypeTypeLayout>();
    registerDeclTypeAbbr<DependentMemberTypeLayout>();
    registerDeclTypeAbbr<StructLayout>();
    registerDeclTypeAbbr<ConstructorLayout>();
    registerDeclTypeAbbr<VarLayout>();
    registerDeclTypeAbbr<FuncLayout>();
    registerDeclTypeAbbr<PatternBindingLayout>();
    registerDeclTypeAbbr<ProtocolLayout>();
    registerDeclTypeAbbr<PrefixOperatorLayout>();
    registerDeclTypeAbbr<PostfixOperatorLayout>();
    registerDeclTypeAbbr<InfixOperatorLayout>();
    registerDeclTypeAbbr<ClassLayout>();
    registerDeclTypeAbbr<EnumLayout>();
    registerDeclTypeAbbr<EnumElementLayout>();
    registerDeclTypeAbbr<SubscriptLayout>();
    registerDeclTypeAbbr<ExtensionLayout>();
    registerDeclTypeAbbr<DestructorLayout>();

    registerDeclTypeAbbr<ParenPatternLayout>();
    registerDeclTypeAbbr<TuplePatternLayout>();
    registerDeclTypeAbbr<TuplePatternEltLayout>();
    registerDeclTypeAbbr<NamedPatternLayout>();
    registerDeclTypeAbbr<VarPatternLayout>();
    registerDeclTypeAbbr<AnyPatternLayout>();
    registerDeclTypeAbbr<TypedPatternLayout>();

    registerDeclTypeAbbr<GenericParamListLayout>();
    registerDeclTypeAbbr<GenericParamLayout>();
    registerDeclTypeAbbr<GenericRequirementLayout>();
    registerDeclTypeAbbr<LastGenericRequirementLayout>();

    registerDeclTypeAbbr<XRefTypePathPieceLayout>();
    registerDeclTypeAbbr<XRefValuePathPieceLayout>();
    registerDeclTypeAbbr<XRefExtensionPathPieceLayout>();
    registerDeclTypeAbbr<XRefOperatorPathPieceLayout>();
    registerDeclTypeAbbr<XRefGenericParamPathPieceLayout>();

    registerDeclTypeAbbr<NoConformanceLayout>();
    registerDeclTypeAbbr<NormalProtocolConformanceLayout>();
    registerDeclTypeAbbr<SpecializedProtocolConformanceLayout>();
    registerDeclTypeAbbr<InheritedProtocolConformanceLayout>();
    registerDeclTypeAbbr<DeclContextLayout>();
    registerDeclTypeAbbr<XRefLayout>();
  }

  while (!DeclsAndTypesToWrite.empty()) {
    DeclTypeUnion next = DeclsAndTypesToWrite.front();
    DeclsAndTypesToWrite.pop();

    DeclID id = DeclIDs[next];
    assert(id != 0 && "decl or type not referenced properly");
    (void)id;

    auto &offsets = next.isDecl() ? DeclOffsets : TypeOffsets;
    assert((id - 1) == offsets.size());
    
    offsets.push_back(Out.GetCurrentBitNo());

    if (next.isDecl())
      writeDecl(next.getDecl());
    else
      writeType(next.getType());
  }
}

void Serializer::writeAllIdentifiers() {
  BCBlockRAII restoreBlock(Out, IDENTIFIER_DATA_BLOCK_ID, 3);
  identifier_block::IdentifierDataLayout IdentifierData(Out);

  llvm::SmallString<4096> stringData;

  // Make sure no identifier has an offset of 0.
  stringData.push_back('\0');

  for (Identifier ident : IdentifiersToWrite) {
    IdentifierOffsets.push_back(stringData.size());
    stringData.append(ident.get());
    stringData.push_back('\0');
  }

  IdentifierData.emit(ScratchRecord, stringData.str());
}

void Serializer::writeOffsets(const index_block::OffsetsLayout &Offsets,
                              const std::vector<BitOffset> &values) {
  Offsets.emit(ScratchRecord, getOffsetRecordCode(values), values);
}

/// Writes an in-memory decl table to an on-disk representation, using the
/// given layout.
static void writeDeclTable(const index_block::DeclListLayout &DeclList,
                           index_block::RecordKind kind,
                           const Serializer::DeclTable &table) {
  if (table.empty())
    return;

  SmallVector<uint64_t, 8> scratch;
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    OnDiskChainedHashTableGenerator<DeclTableInfo> generator;
    for (auto &entry : table)
      generator.insert(entry.first, entry.second);

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0
    clang::io::Emit32(blobStream, 0);
    tableOffset = generator.Emit(blobStream);
  }

  DeclList.emit(scratch, kind, tableOffset, hashTableBlob);
}

/// Translate from the AST known protocol enum to the Serialization enum
/// values, which are guaranteed to be stable.
static uint8_t getRawStableKnownProtocolKind(KnownProtocolKind kind) {
  switch (kind) {
#define PROTOCOL(Id) \
  case KnownProtocolKind::Id: return index_block::Id;
#include "swift/AST/KnownProtocols.def"
  }
}

/// Writes a list of decls known to conform to the given compiler-known
/// protocol.
static void
writeKnownProtocolList(const index_block::KnownProtocolLayout &AdopterList,
                       KnownProtocolKind kind, ArrayRef<DeclID> adopters) {
  if (adopters.empty())
    return;
                       
  SmallVector<uint32_t, 32> scratch;
  AdopterList.emit(scratch, getRawStableKnownProtocolKind(kind), adopters);
}

/// Add operator methods to the given declaration type.
static void addOperatorMethodDecls(Serializer &S, ArrayRef<Decl *> members, 
                                   Serializer::DeclTable &operatorMethodDecls) {
  for (auto member : members) {
    // Add operator methods.
    if (auto func = dyn_cast<FuncDecl>(member)) {
      if (!func->getName().empty() && func->getName().isOperator())
        operatorMethodDecls[func->getName()]
          .push_back({0, S.addDeclRef(func)});
      continue;
    }

    // Recurse into nested types.
    if (auto nominal = dyn_cast<NominalTypeDecl>(member)) {
      addOperatorMethodDecls(S, nominal->getMembers(), operatorMethodDecls);
    }
  }
}

void Serializer::writeModule(ModuleOrSourceFile DC, const SILModule *SILMod) {
  assert(!this->M && "already serializing a module");
  this->M = getModule(DC);
  this->SF = DC.dyn_cast<SourceFile *>();

  writeSILFunctions(SILMod);

  DeclTable topLevelDecls, extensionDecls, operatorDecls, operatorMethodDecls;
  ArrayRef<const FileUnit *> files = SF ? SF : M->getFiles();
  for (auto nextFile : files) {
    // FIXME: Switch to a visitor interface?
    SmallVector<Decl *, 32> fileDecls;
    nextFile->getTopLevelDecls(fileDecls);

    for (auto D : fileDecls) {
      if (isa<ImportDecl>(D))
        continue;
      else if (auto VD = dyn_cast<ValueDecl>(D)) {
        if (VD->getName().empty())
          continue;
        topLevelDecls[VD->getName()]
          .push_back({ getKindForTable(D), addDeclRef(D) });

        // Add operator methods from nominal types.
        if (auto nominal = dyn_cast<NominalTypeDecl>(VD)) {
          addOperatorMethodDecls(*this, nominal->getMembers(), 
                                 operatorMethodDecls);
        }
      } else if (auto ED = dyn_cast<ExtensionDecl>(D)) {
        Type extendedTy = ED->getExtendedType();
        const NominalTypeDecl *extendedNominal = extendedTy->getAnyNominal();
        extensionDecls[extendedNominal->getName()]
          .push_back({ getKindForTable(extendedNominal), addDeclRef(D) });

        // Add operator methods from extensions.
        addOperatorMethodDecls(*this, ED->getMembers(), operatorMethodDecls);
      } else if (auto OD = dyn_cast<OperatorDecl>(D)) {
        operatorDecls[OD->getName()]
          .push_back({ getStableFixity(OD->getKind()), addDeclRef(D) });
      }
    }
  }

  writeAllDeclsAndTypes();
  writeAllIdentifiers();

  {
    BCBlockRAII restoreBlock(Out, INDEX_BLOCK_ID, 4);

    index_block::OffsetsLayout Offsets(Out);
    writeOffsets(Offsets, DeclOffsets);
    writeOffsets(Offsets, TypeOffsets);
    writeOffsets(Offsets, IdentifierOffsets);

    index_block::DeclListLayout DeclList(Out);
    writeDeclTable(DeclList, index_block::TOP_LEVEL_DECLS, topLevelDecls);
    writeDeclTable(DeclList, index_block::OPERATORS, operatorDecls);
    writeDeclTable(DeclList, index_block::EXTENSIONS, extensionDecls);
    writeDeclTable(DeclList, index_block::CLASS_MEMBERS, ClassMembersByName);
    writeDeclTable(DeclList, index_block::OPERATOR_METHODS, operatorMethodDecls);

    {
      BCBlockRAII subBlock(Out, KNOWN_PROTOCOL_BLOCK_ID, 3);
      index_block::KnownProtocolLayout AdopterList(Out);

      for (unsigned i = 0; i < NumKnownProtocols; ++i) {
        writeKnownProtocolList(AdopterList, static_cast<KnownProtocolKind>(i),
                               KnownProtocolAdopters[i]);
      }
    }
  }

#ifndef NDEBUG
  this->M = nullptr;
#endif
}

namespace {
  template <size_t N>
  union AsBytes {
    uint64_t data;
    char bytes[N];

    AsBytes(uint64_t data) : data(data) {}

    static_assert(N <= sizeof(data), "size is too big");
  };

  template <size_t N>
  static raw_ostream &operator<<(raw_ostream &os, const AsBytes<N> &wrapper) {
    os.write(wrapper.bytes, N);
    return os;
  }
}

void Serializer::writeToStream(raw_ostream &os, ModuleOrSourceFile DC,
                               const SILModule *SILMod,
                               FilenamesTy inputFiles,
                               StringRef moduleLinkName) {
  // Write the signature through the BitstreamWriter for alignment purposes.
  for (unsigned char byte : SIGNATURE)
    Out.Emit(byte, 8);

  // FIXME: This is only really needed for debugging. We don't actually use it.
  writeBlockInfoBlock();

  const Module *mod = getModule(DC);

  {
    BCBlockRAII moduleBlock(Out, MODULE_BLOCK_ID, 2);
    writeHeader(mod);
    writeInputFiles(mod, inputFiles, moduleLinkName);
    writeModule(DC, SILMod);
    Out.FlushToWord();
  }

  os.write(Buffer.data(), Buffer.size());
  os.flush();
  Buffer.clear();
}

void swift::serialize(ModuleOrSourceFile DC, const SILModule *M,
                      const char *outputPath, FilenamesTy inputFiles,
                      StringRef moduleLinkName) {
  std::string errorInfo;
  llvm::raw_fd_ostream out(outputPath, errorInfo,
                           llvm::sys::fs::F_Binary);

  if (out.has_error() || !errorInfo.empty()) {
    getContext(DC).Diags.diagnose(SourceLoc(), diag::error_opening_output,
                                  outputPath, errorInfo);
    out.clear_error();
    return;
  }

  serializeToStream(DC, out, M, inputFiles, moduleLinkName);
}

void swift::serializeToStream(ModuleOrSourceFile DC, raw_ostream &out,
                              const SILModule *M, FilenamesTy inputFiles,
                              StringRef moduleLinkName) {
  Serializer S;
  S.writeToStream(out, DC, M, inputFiles, moduleLinkName);
}
