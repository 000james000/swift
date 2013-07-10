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
#include "swift/Serialization/BCReadingExtras.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace swift;
using namespace swift::serialization;

static ModuleStatus
validateControlBlock(llvm::BitstreamCursor &cursor,
                     llvm::SmallVectorImpl<uint64_t> &scratch) {
  // The control block is malformed until we've at least read a major version
  // number.
  ModuleStatus result = ModuleStatus::Malformed;

  auto next = cursor.advance();
  while (next.Kind != llvm::BitstreamEntry::EndBlock) {
    if (next.Kind == llvm::BitstreamEntry::Error)
      return ModuleStatus::Malformed;

    if (next.Kind == llvm::BitstreamEntry::SubBlock) {
      // Unknown metadata sub-block, possibly for use by a future version of the
      // module format.
      if (cursor.SkipBlock())
        return ModuleStatus::Malformed;
      next = cursor.advance();
      continue;
    }

    scratch.clear();
    StringRef blobData;
    unsigned kind = cursor.readRecord(next.ID, scratch, &blobData);
    switch (kind) {
    case control_block::METADATA: {
      uint16_t versionMajor = scratch[0];
      if (versionMajor > VERSION_MAJOR)
        return ModuleStatus::FormatTooNew;
      result = ModuleStatus::Valid;
      break;
    }
    default:
      // Unknown metadata record, possibly for use by a future version of the
      // module format.
      break;
    }

    next = cursor.advance();
  }

  return result;
}

Pattern *ModuleFile::maybeReadPattern() {
  using namespace decls_block;
  
  SmallVector<uint64_t, 8> scratch;

  auto next = DeclTypeCursor.advance();
  if (next.Kind != llvm::BitstreamEntry::Record)
    return nullptr;

  unsigned kind = DeclTypeCursor.readRecord(next.ID, scratch);
  switch (kind) {
  case decls_block::PAREN_PATTERN: {
    Pattern *subPattern = maybeReadPattern();
    assert(subPattern);

    auto result = new (ModuleContext->Ctx) ParenPattern(SourceLoc(),
                                                        subPattern,
                                                        SourceLoc());
    result->setType(subPattern->getType());
    return result;
  }
  case decls_block::TUPLE_PATTERN: {
    TypeID tupleTypeID;
    unsigned count;

    TuplePatternLayout::readRecord(scratch, tupleTypeID, count);

    SmallVector<TuplePatternElt, 8> elements;
    for ( ; count > 0; --count) {
      scratch.clear();
      next = DeclTypeCursor.advance();
      assert(next.Kind == llvm::BitstreamEntry::Record);

      kind = DeclTypeCursor.readRecord(next.ID, scratch);
      assert(kind == decls_block::TUPLE_PATTERN_ELT);

      TypeID varargsTypeID;
      TuplePatternEltLayout::readRecord(scratch, varargsTypeID);

      Pattern *subPattern = maybeReadPattern();
      assert(subPattern);

      elements.push_back(TuplePatternElt(subPattern));
      if (varargsTypeID) {
        BCOffsetRAII restoreOffset(DeclTypeCursor);
        elements.back().setVarargBaseType(getType(varargsTypeID));
      }
    }

    auto result = TuplePattern::create(ModuleContext->Ctx, SourceLoc(),
                                       elements, SourceLoc());
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      result->setType(getType(tupleTypeID));
    }
    return result;
  }
  case decls_block::NAMED_PATTERN: {

    DeclID varID;
    NamedPatternLayout::readRecord(scratch, varID);

    BCOffsetRAII restoreOffset(DeclTypeCursor);

    auto var = cast<VarDecl>(getDecl(varID));
    auto result = new (ModuleContext->Ctx) NamedPattern(var);
    if (var->hasType())
      result->setType(var->getType());
    return result;
  }
  case decls_block::ANY_PATTERN: {
    TypeID typeID;

    AnyPatternLayout::readRecord(scratch, typeID);
    auto result = new (ModuleContext->Ctx) AnyPattern(SourceLoc());
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      result->setType(getType(typeID));
    }
    return result;
  }
  case decls_block::TYPED_PATTERN: {
    TypeID typeID;

    TypedPatternLayout::readRecord(scratch, typeID);
    Pattern *subPattern = maybeReadPattern();
    assert(subPattern);

    TypeLoc typeInfo = TypeLoc::withoutLoc(getType(typeID));
    auto result = new (ModuleContext->Ctx) TypedPattern(subPattern, typeInfo);
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      result->setType(typeInfo.getType());
    }
    return result;
  }
  default:
    return nullptr;
  }
}

ProtocolConformance *ModuleFile::maybeReadConformance() {
  using namespace decls_block;

  BCOffsetRAII lastRecordOffset(DeclTypeCursor);
  SmallVector<uint64_t, 8> scratch;

  auto next = DeclTypeCursor.advance();
  if (next.Kind != llvm::BitstreamEntry::Record)
    return nullptr;

  unsigned kind = DeclTypeCursor.readRecord(next.ID, scratch);
  if (kind != PROTOCOL_CONFORMANCE)
    return nullptr;

  lastRecordOffset.reset();
  unsigned valueCount, typeCount, inheritedCount;
  ArrayRef<uint64_t> rawIDs;

  ProtocolConformanceLayout::readRecord(scratch, valueCount, typeCount,
                                        inheritedCount, rawIDs);

  ProtocolConformance *conformance =
    ModuleContext->Ctx.Allocate<ProtocolConformance>(1);

  ArrayRef<uint64_t>::iterator rawIDIter = rawIDs.begin();
  {
    BCOffsetRAII restoreOffset(DeclTypeCursor);
    while (valueCount--) {
      auto first = cast<ValueDecl>(getDecl(*rawIDIter++));
      auto second = cast<ValueDecl>(getDecl(*rawIDIter++));
      conformance->Mapping.insert(std::make_pair(first, second));
    }
    while (typeCount--) {
      auto first = getType(*rawIDIter++)->castTo<SubstitutableType>();
      auto second = getType(*rawIDIter++);
      conformance->TypeMapping.insert(std::make_pair(first, second));
    }
  }

  while (inheritedCount--) {
    ProtocolDecl *proto;
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      proto = cast<ProtocolDecl>(getDecl(*rawIDIter++));
    }

    ProtocolConformance *inherited = maybeReadConformance();
    assert(inherited);
    lastRecordOffset.reset();

    conformance->InheritedMapping.insert(std::make_pair(proto, inherited));
  }

  return conformance;
}

GenericParamList *ModuleFile::maybeReadGenericParams(DeclContext *DC) {
  using namespace decls_block;

  assert(DC && "need a context for the decls in the list");

  BCOffsetRAII lastRecordOffset(DeclTypeCursor);
  SmallVector<uint64_t, 8> scratch;
  StringRef blobData;

  auto next = DeclTypeCursor.advance();
  if (next.Kind != llvm::BitstreamEntry::Record)
    return nullptr;

  unsigned kind = DeclTypeCursor.readRecord(next.ID, scratch, &blobData);

  if (kind != GENERIC_PARAM_LIST)
    return nullptr;

  ArrayRef<uint64_t> rawArchetypeIDs;
  GenericParamListLayout::readRecord(scratch, rawArchetypeIDs);

  SmallVector<ArchetypeType *, 8> archetypes;
  {
    BCOffsetRAII restoreOffset(DeclTypeCursor);
    for (TypeID next : rawArchetypeIDs)
      archetypes.push_back(getType(next)->castTo<ArchetypeType>());
  }

  SmallVector<GenericParam, 8> params;
  SmallVector<Requirement, 8> requirements;
  while (true) {
    lastRecordOffset.reset();
    bool shouldContinue = true;

    auto entry = DeclTypeCursor.advance();
    if (entry.Kind != llvm::BitstreamEntry::Record)
      break;

    scratch.clear();
    unsigned recordID = DeclTypeCursor.readRecord(entry.ID, scratch,
                                                  &blobData);
    switch (recordID) {
    case GENERIC_PARAM: {
      DeclID paramDeclID;
      GenericParamLayout::readRecord(scratch, paramDeclID);
      {
        BCOffsetRAII restoreInnerOffset(DeclTypeCursor);
        auto typeAlias = cast<TypeAliasDecl>(getDecl(paramDeclID, DC));
        params.push_back(GenericParam(typeAlias));
      }
      break;
    }
    case GENERIC_REQUIREMENT: {
      uint8_t rawKind;
      ArrayRef<uint64_t> rawTypeIDs;
      GenericRequirementLayout::readRecord(scratch, rawKind, rawTypeIDs);

      switch (rawKind) {
      case GenericRequirementKind::Conformance: {
        assert(rawTypeIDs.size() == 2);
        TypeLoc subject, constraint;
        {
          BCOffsetRAII restoreInnerOffset(DeclTypeCursor);
          subject = TypeLoc::withoutLoc(getType(rawTypeIDs[0]));
          constraint = TypeLoc::withoutLoc(getType(rawTypeIDs[1]));
        }

        requirements.push_back(Requirement::getConformance(subject,
                                                           SourceLoc(),
                                                           constraint));
        break;
      }
      case GenericRequirementKind::SameType: {
        assert(rawTypeIDs.size() == 2);
        TypeLoc first, second;
        {
          BCOffsetRAII restoreInnerOffset(DeclTypeCursor);
          first = TypeLoc::withoutLoc(getType(rawTypeIDs[0]));
          second = TypeLoc::withoutLoc(getType(rawTypeIDs[1]));
        }

        requirements.push_back(Requirement::getSameType(first,
                                                        SourceLoc(),
                                                        second));
        break;
      }
      default:
        // Unknown requirement kind. Drop the requirement and continue, but log
        // an error so that we don't actually try to generate code.
        error();
      }

      break;
    }
    default:
      // This record is not part of the GenericParamList.
      shouldContinue = false;
      break;
    }

    if (!shouldContinue)
      break;
  }

  auto paramList = GenericParamList::create(ModuleContext->Ctx, SourceLoc(),
                                            params, SourceLoc(), requirements,
                                            SourceLoc());
  paramList->setAllArchetypes(ModuleContext->Ctx.AllocateCopy(archetypes));
  paramList->setOuterParameters(DC->getGenericParamsOfContext());

  return paramList;
}

MutableArrayRef<TypeLoc> ModuleFile::getTypes(ArrayRef<uint64_t> rawTypeIDs,
                                              Type *classType) {
  ASTContext &ctx = ModuleContext->Ctx;
  auto result =
    MutableArrayRef<TypeLoc>(ctx.Allocate<TypeLoc>(rawTypeIDs.size()),
                             rawTypeIDs.size());
  if (classType)
    *classType = nullptr;

  TypeLoc *nextType = result.data();
  for (TypeID rawID : rawTypeIDs) {
    auto type = getType(rawID);
    if (classType && type->getClassOrBoundGenericClass()) {
      assert(!*classType && "already found a class type");
      *classType = type;
    }
    new (nextType) auto(TypeLoc::withoutLoc(type));
    ++nextType;
  }

  return result;
}

Optional<MutableArrayRef<Decl *>> ModuleFile::readMembers() {
  using namespace decls_block;

  auto entry = DeclTypeCursor.advance();
  if (entry.Kind != llvm::BitstreamEntry::Record)
    return Nothing;

  SmallVector<uint64_t, 16> memberIDBuffer;

  unsigned kind = DeclTypeCursor.readRecord(entry.ID, memberIDBuffer);
  if (kind != DECL_CONTEXT)
    return Nothing;

  ArrayRef<uint64_t> rawMemberIDs;
  decls_block::DeclContextLayout::readRecord(memberIDBuffer, rawMemberIDs);

  if (rawMemberIDs.empty())
    return MutableArrayRef<Decl *>();

  ASTContext &ctx = ModuleContext->Ctx;
  MutableArrayRef<Decl *> members(ctx.Allocate<Decl *>(rawMemberIDs.size()),
                                  rawMemberIDs.size());
  auto nextMember = members.begin();
  for (DeclID rawID : rawMemberIDs) {
    *nextMember = getDecl(rawID);
    assert(*nextMember && "unable to deserialize next member");
    ++nextMember;
  }

  return members;
}

Identifier ModuleFile::getIdentifier(IdentifierID IID) {
  if (IID == 0)
    return Identifier();

  assert(IID <= Identifiers.size() && "invalid identifier ID");
  auto identRecord = Identifiers[IID-1];

  if (identRecord.Offset == 0)
    return identRecord.Ident;

  assert(!IdentifierData.empty() && "no identifier data in module");

  StringRef rawStrPtr = IdentifierData.substr(identRecord.Offset);
  size_t terminatorOffset = rawStrPtr.find('\0');
  assert(terminatorOffset != StringRef::npos &&
         "unterminated identifier string data");

  return ModuleContext->Ctx.getIdentifier(rawStrPtr.slice(0, terminatorOffset));
}

DeclContext *ModuleFile::getDeclContext(DeclID DID) {
  if (DID == 0)
    return ModuleContext;

  Decl *D = getDecl(DID);

  if (auto ND = dyn_cast<NominalTypeDecl>(D))
    return ND;
  if (auto ED = dyn_cast<ExtensionDecl>(D))
    return ED;
  if (auto CD = dyn_cast<ConstructorDecl>(D))
    return CD;
  if (auto DD = dyn_cast<DestructorDecl>(D))
    return DD;
  if (auto FD = dyn_cast<FuncDecl>(D))
    return FD->getBody();

  llvm_unreachable("unknown DeclContext kind");
}


/// Translate from the Serialization assocativity enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static Optional<swift::Associativity> getActualAssociativity(uint8_t assoc) {
  switch (assoc) {
  case serialization::Associativity::LeftAssociative:
    return swift::Associativity::Left;
  case serialization::Associativity::RightAssociative:
    return swift::Associativity::Right;
  case serialization::Associativity::NonAssociative:
    return swift::Associativity::None;
  default:
    return Nothing;
  }
}

Decl *ModuleFile::getDecl(DeclID DID, Optional<DeclContext *> ForcedContext) {
  if (DID == 0)
    return nullptr;

  assert(DID <= Decls.size() && "invalid decl ID");
  auto &declOrOffset = Decls[DID-1];

  if (declOrOffset.is<Decl *>())
    return declOrOffset.get<Decl *>();

  DeclTypeCursor.JumpToBit(declOrOffset.get<BitOffset>());
  auto entry = DeclTypeCursor.advance();

  if (entry.Kind != llvm::BitstreamEntry::Record) {
    // We don't know how to serialize decls represented by sub-blocks.
    error();
    return nullptr;
  }

#ifndef NDEBUG
  assert(declOrOffset.get<BitOffset>() != 0 &&
         "this decl is already being deserialized");
  declOrOffset = BitOffset(0);
#endif

  ASTContext &ctx = ModuleContext->Ctx;

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  unsigned recordID = DeclTypeCursor.readRecord(entry.ID, scratch, &blobData);

  switch (recordID) {
  case decls_block::TYPE_ALIAS_DECL: {
    IdentifierID nameID;
    DeclID contextID;
    TypeID underlyingTypeID;
    bool isGeneric;
    bool isImplicit;
    ArrayRef<uint64_t> inheritedIDs;

    decls_block::TypeAliasLayout::readRecord(scratch, nameID, contextID,
                                             underlyingTypeID,
                                             isGeneric, isImplicit,
                                             inheritedIDs);

    auto inherited =
      MutableArrayRef<TypeLoc>(ctx.Allocate<TypeLoc>(inheritedIDs.size()),
                               inheritedIDs.size());
    TypeLoc underlyingType;
    DeclContext *DC;

    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);

      TypeLoc *nextInheritedType = inherited.data();
      for (TypeID TID : inheritedIDs) {
        auto type = getType(TID);
        new (nextInheritedType) TypeLoc(TypeLoc::withoutLoc(type));
        ++nextInheritedType;
      }

      underlyingType = TypeLoc::withoutLoc(getType(underlyingTypeID));
      DC = ForcedContext ? *ForcedContext : getDeclContext(contextID);
    }

    
    auto alias = new (ctx) TypeAliasDecl(SourceLoc(), getIdentifier(nameID),
                                         SourceLoc(), underlyingType,
                                         DC, inherited);
    declOrOffset = alias;

    if (isImplicit)
      alias->setImplicit();
    if (isGeneric)
      alias->setGenericParameter();

    SmallVector<ProtocolConformance *, 16> conformanceBuf;
    while (ProtocolConformance *conformance = maybeReadConformance())
      conformanceBuf.push_back(conformance);
    alias->setConformances(ctx.AllocateCopy(conformanceBuf));

    break;
  }

  case decls_block::STRUCT_DECL: {
    IdentifierID nameID;
    DeclID contextID;
    bool isImplicit;
    ArrayRef<uint64_t> inheritedIDs;

    decls_block::StructLayout::readRecord(scratch, nameID, contextID,
                                          isImplicit, inheritedIDs);

    MutableArrayRef<TypeLoc> inherited;
    DeclContext *DC;
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      inherited = getTypes(inheritedIDs);
      DC = getDeclContext(contextID);
    }

    auto genericParams = maybeReadGenericParams(DC);

    auto theStruct = new (ctx) StructDecl(SourceLoc(), getIdentifier(nameID),
                                          SourceLoc(), inherited,
                                          genericParams, DC);
    declOrOffset = theStruct;

    if (isImplicit)
      theStruct->setImplicit();
    if (genericParams)
      for (auto &genericParam : *theStruct->getGenericParams())
        genericParam.getAsTypeParam()->setDeclContext(theStruct);

    SmallVector<ProtocolConformance *, 16> conformanceBuf;
    while (ProtocolConformance *conformance = maybeReadConformance())
      conformanceBuf.push_back(conformance);
    theStruct->setConformances(ctx.AllocateCopy(conformanceBuf));

    auto members = readMembers();
    assert(members.hasValue() && "could not read struct members");
    theStruct->setMembers(members.getValue(), SourceRange());

    break;
  }

  case decls_block::CONSTRUCTOR_DECL: {
    DeclID parentID;
    bool isImplicit;
    TypeID signatureID;
    DeclID implicitThisID;

    decls_block::ConstructorLayout::readRecord(scratch, parentID, isImplicit,
                                               signatureID, implicitThisID);
    VarDecl *thisDecl;
    NominalTypeDecl *parent;
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      thisDecl = cast<VarDecl>(getDecl(implicitThisID, nullptr));
      parent = cast<NominalTypeDecl>(getDeclContext(parentID));
    }

    auto genericParams = maybeReadGenericParams(parent);

    auto ctor = new (ctx) ConstructorDecl(ctx.getIdentifier("constructor"),
                                          SourceLoc(), /*args=*/nullptr,
                                          thisDecl, genericParams, parent);
    declOrOffset = ctor;
    thisDecl->setDeclContext(ctor);

    Pattern *args = maybeReadPattern();
    assert(args && "missing arguments for constructor");
    ctor->setArguments(args);

    // This must be set after recording the constructor in the map.
    // A polymorphic constructor type needs to refer to the constructor to get
    // its generic parameters.
    ctor->setType(getType(signatureID));

    if (isImplicit)
      ctor->setImplicit();

    if (genericParams)
      for (auto &genericParam : *ctor->getGenericParams())
        genericParam.getAsTypeParam()->setDeclContext(ctor);

    break;
  }

  case decls_block::VAR_DECL: {
    IdentifierID nameID;
    DeclID contextID;
    bool isImplicit;
    TypeID typeID;
    DeclID getterID, setterID;
    DeclID overriddenID;

    decls_block::VarLayout::readRecord(scratch, nameID, contextID, isImplicit,
                                       typeID, getterID, setterID,
                                       overriddenID);

    auto DC = ForcedContext ? *ForcedContext : getDeclContext(contextID);
    auto var = new (ctx) VarDecl(SourceLoc(), getIdentifier(nameID),
                                 getType(typeID), DC);

    // Explicitly set the getter and setter info /before/ recording the VarDecl
    // in the map. The functions will check this to know if they are getters or
    // setters.
    if (getterID || setterID) {
      var->setProperty(ctx, SourceLoc(),
                       cast_or_null<FuncDecl>(getDecl(getterID)),
                       cast_or_null<FuncDecl>(getDecl(setterID)),
                       SourceLoc());
    }

    declOrOffset = var;

    if (isImplicit)
      var->setImplicit();

    var->setOverriddenDecl(cast_or_null<VarDecl>(getDecl(overriddenID)));
    break;
  }

  case decls_block::FUNC_DECL: {
    IdentifierID nameID;
    DeclID contextID;
    bool isImplicit;
    bool isClassMethod;
    bool isAssignment;
    TypeID signatureID;
    DeclID associatedDeclID;
    DeclID overriddenID;

    decls_block::FuncLayout::readRecord(scratch, nameID, contextID, isImplicit,
                                        isClassMethod, isAssignment,
                                        signatureID, associatedDeclID,
                                        overriddenID);

    DeclContext *DC;
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      DC = getDeclContext(contextID);
    }

    // Read generic params before reading the type, because the type may
    // reference generic parameters, and we want them to have a dummy
    // DeclContext for now.
    GenericParamList *genericParams = maybeReadGenericParams(DC);

    auto fn = new (ctx) FuncDecl(SourceLoc(), SourceLoc(),
                                 getIdentifier(nameID), SourceLoc(),
                                 genericParams, /*type=*/nullptr,
                                 /*body=*/nullptr, DC);
    declOrOffset = fn;

    AnyFunctionType *signature;
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      signature = getType(signatureID)->castTo<AnyFunctionType>();

      // This must be set after recording the constructor in the map.
      // A polymorphic constructor type needs to refer to the constructor to get
      // its generic parameters.
      fn->setType(signature);
    }

    SmallVector<Pattern *, 16> patternBuf;
    while (Pattern *pattern = maybeReadPattern())
      patternBuf.push_back(pattern);

    assert(!patternBuf.empty());
    size_t patternCount = patternBuf.size() / 2;
    assert(patternCount * 2 == patternBuf.size() &&
           "two sets of patterns don't  match up");

    ArrayRef<Pattern *> patterns(patternBuf);
    ArrayRef<Pattern *> argPatterns = patterns.slice(0, patternCount);
    ArrayRef<Pattern *> bodyPatterns = patterns.slice(patternCount);

    auto body = FuncExpr::create(ctx, SourceLoc(),
                                 argPatterns, bodyPatterns,
                                 TypeLoc::withoutLoc(signature->getResult()),
                                 DC);
    fn->setBody(body);

    if (genericParams)
      for (auto &genericParam : *fn->getGenericParams())
        genericParam.getAsTypeParam()->setDeclContext(body);

    fn->setOverriddenDecl(cast_or_null<FuncDecl>(getDecl(overriddenID)));

    fn->setStatic(isClassMethod);
    if (isImplicit)
      fn->setImplicit();
    if (isAssignment)
      fn->getMutableAttrs().Assignment = isAssignment;

    if (Decl *associated = getDecl(associatedDeclID)) {
      if (auto op = dyn_cast<OperatorDecl>(associated)) {
        fn->setOperatorDecl(op);

        DeclAttributes &attrs = fn->getMutableAttrs();
        if (isa<PrefixOperatorDecl>(op))
          attrs.ExplicitPrefix = true;
        else if (isa<PostfixOperatorDecl>(op))
          attrs.ExplicitPostfix = true;
        // Note that an explicit [infix] is not required.
      } else {
        bool isGetter = false;

        if (auto subscript = dyn_cast<SubscriptDecl>(associated)) {
          isGetter = (subscript->getGetter() == fn);
          assert(isGetter || subscript->getSetter() == fn);
        } else if (auto var = dyn_cast<VarDecl>(associated)) {
          isGetter = (var->getGetter() == fn);
          assert(isGetter || var->getSetter() == fn);
        } else {
          llvm_unreachable("unknown associated decl kind");
        }

        if (isGetter)
          fn->makeGetter(associated);
        else
          fn->makeSetter(associated);
      }
    }

    break;
  }

  case decls_block::PATTERN_BINDING_DECL: {
    DeclID contextID;
    bool isImplicit;

    decls_block::PatternBindingLayout::readRecord(scratch, contextID,
                                                  isImplicit);
    Pattern *pattern = maybeReadPattern();
    assert(pattern);

    auto binding = new (ctx) PatternBindingDecl(SourceLoc(), pattern,
                                                /*init=*/nullptr,
                                                getDeclContext(contextID));
    declOrOffset = binding;

    if (isImplicit)
      binding->setImplicit();

    break;
  }

  case decls_block::PROTOCOL_DECL: {
    IdentifierID nameID;
    DeclID contextID;
    bool isImplicit;
    ArrayRef<uint64_t> inheritedIDs;

    decls_block::ProtocolLayout::readRecord(scratch, nameID, contextID,
                                            isImplicit, inheritedIDs);

    MutableArrayRef<TypeLoc> inherited;
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      inherited = getTypes(inheritedIDs);
    }
    auto proto = new (ctx) ProtocolDecl(getDeclContext(contextID), SourceLoc(),
                                        SourceLoc(), getIdentifier(nameID),
                                        inherited);
    declOrOffset = proto;

    if (isImplicit)
      proto->setImplicit();

    SmallVector<ProtocolConformance *, 16> conformanceBuf;
    while (ProtocolConformance *conformance = maybeReadConformance())
      conformanceBuf.push_back(conformance);
    proto->setConformances(ctx.AllocateCopy(conformanceBuf));

    auto members = readMembers();
    assert(members.hasValue() && "could not read struct members");
    proto->setMembers(members.getValue(), SourceRange());

    break;
  }

  case decls_block::PREFIX_OPERATOR_DECL: {
    IdentifierID nameID;
    DeclID contextID;

    decls_block::PrefixOperatorLayout::readRecord(scratch, nameID, contextID);
    declOrOffset = new (ctx) PrefixOperatorDecl(getDeclContext(contextID),
                                                SourceLoc(), SourceLoc(),
                                                getIdentifier(nameID),
                                                SourceLoc(), SourceLoc(),
                                                SourceLoc());
    break;
  }

  case decls_block::POSTFIX_OPERATOR_DECL: {
    IdentifierID nameID;
    DeclID contextID;

    decls_block::PostfixOperatorLayout::readRecord(scratch, nameID, contextID);
    declOrOffset = new (ctx) PostfixOperatorDecl(getDeclContext(contextID),
                                                 SourceLoc(), SourceLoc(),
                                                 getIdentifier(nameID),
                                                 SourceLoc(), SourceLoc(),
                                                 SourceLoc());
    break;
  }

  case decls_block::INFIX_OPERATOR_DECL: {
    IdentifierID nameID;
    DeclID contextID;
    uint8_t rawAssociativity;
    unsigned precedence;

    decls_block::InfixOperatorLayout::readRecord(scratch, nameID, contextID,
                                                 rawAssociativity, precedence);

    auto associativity = getActualAssociativity(rawAssociativity);
    if (!associativity.hasValue()) {
      error();
      return nullptr;
    }

    InfixData infixData(precedence, associativity.getValue());

    declOrOffset = new (ctx) InfixOperatorDecl(getDeclContext(contextID),
                                               SourceLoc(), SourceLoc(),
                                               getIdentifier(nameID),
                                               SourceLoc(), SourceLoc(),
                                               SourceLoc(), SourceLoc(),
                                               SourceLoc(), SourceLoc(),
                                               SourceLoc(), infixData);
    break;
  }
      
  case decls_block::CLASS_DECL: {
    IdentifierID nameID;
    DeclID contextID;
    bool isImplicit;
    ArrayRef<uint64_t> inheritedIDs;

    decls_block::ClassLayout::readRecord(scratch, nameID, contextID,
                                         isImplicit, inheritedIDs);

    MutableArrayRef<TypeLoc> inherited;
    Type baseClassTy;
    DeclContext *DC;
    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);
      inherited = getTypes(inheritedIDs, &baseClassTy);
      DC = getDeclContext(contextID);
    }

    auto genericParams = maybeReadGenericParams(DC);

    auto theClass = new (ctx) ClassDecl(SourceLoc(), getIdentifier(nameID),
                                        SourceLoc(), inherited,
                                        genericParams, DC);
    declOrOffset = theClass;

    if (isImplicit)
      theClass->setImplicit();
    if (baseClassTy)
      theClass->setBaseClassLoc(TypeLoc::withoutLoc(baseClassTy));
    if (genericParams)
      for (auto &genericParam : *theClass->getGenericParams())
        genericParam.getAsTypeParam()->setDeclContext(theClass);

    SmallVector<ProtocolConformance *, 16> conformanceBuf;
    while (ProtocolConformance *conformance = maybeReadConformance())
      conformanceBuf.push_back(conformance);
    theClass->setConformances(ctx.AllocateCopy(conformanceBuf));

    auto members = readMembers();
    assert(members.hasValue() && "could not read struct members");
    theClass->setMembers(members.getValue(), SourceRange());

    break;
  }

  case decls_block::XREF: {
    uint8_t kind;
    TypeID expectedTypeID;
    ArrayRef<uint64_t> rawAccessPath;

    decls_block::XRefLayout::readRecord(scratch, kind, expectedTypeID,
                                        rawAccessPath);

    // First, find the module this reference is referring to.
    Identifier moduleName = getIdentifier(rawAccessPath.front());
    rawAccessPath = rawAccessPath.slice(1);

    Module *M;
    if (moduleName.empty()) {
      M = ctx.TheBuiltinModule;
    } else {
      // FIXME: provide a real source location.
      M = ctx.getModule(std::make_pair(moduleName, SourceLoc()), false);
    }
    assert(M && "missing dependency");

    switch (kind) {
    case XRefKind::SwiftValue: {
      // Start by looking up the top-level decl in the module.
      SmallVector<ValueDecl *, 8> values;
      M->lookupValue(Module::AccessPathTy(),
                     getIdentifier(rawAccessPath.front()),
                     NLKind::QualifiedLookup,
                     values);
      rawAccessPath = rawAccessPath.slice(1);

      // Then, follow the chain of nested ValueDecls until we run out of
      // identifiers in the access path.
      SmallVector<ValueDecl *, 8> baseValues;
      while (!rawAccessPath.empty()) {
        baseValues.swap(values);
        values.clear();
        for (auto base : baseValues) {
          // FIXME: extensions?
          if (auto nominal = dyn_cast<NominalTypeDecl>(base)) {
            Identifier memberName = getIdentifier(rawAccessPath.front());
            auto members = nominal->lookupDirect(memberName);
            values.append(members.begin(), members.end());
          }
        }
        rawAccessPath = rawAccessPath.slice(1);
      }

      // If we have a type to validate against, filter out any ValueDecls that
      // don't match that type.
      CanType expectedTy;
      Type maybeExpectedTy = getType(expectedTypeID);
      if (maybeExpectedTy)
        expectedTy = maybeExpectedTy->getCanonicalType();

      ValueDecl *result = nullptr;
      for (auto value : values) {
        if (!expectedTy || value->getType()->getCanonicalType() == expectedTy) {
          // It's an error if more than one value has the same type.
          // FIXME: Functions and constructors can overload based on parameter
          // names.
          if (result) {
            error();
            return nullptr;
          }
          result = value;
        }
      }

      // It's an error if lookup doesn't actually find anything -- that means
      // the module's out of date.
      if (!result) {
        error();
        return nullptr;
      }

      declOrOffset = result;
      break;
    }
    case XRefKind::SwiftOperator: {
      assert(rawAccessPath.size() == 1 &&
             "can't import operators not at module scope");
      Identifier opName = getIdentifier(rawAccessPath.back());

      switch (expectedTypeID) {
      case OperatorKind::Infix: {
        auto op = M->lookupInfixOperator(opName);
        declOrOffset = op.hasValue() ? op.getValue() : nullptr;
        break;
      }
      case OperatorKind::Prefix: {
        auto op = M->lookupPrefixOperator(opName);
        declOrOffset = op.hasValue() ? op.getValue() : nullptr;
        break;
      }
      case OperatorKind::Postfix: {
        auto op = M->lookupPostfixOperator(opName);
        declOrOffset = op.hasValue() ? op.getValue() : nullptr;
        break;
      }
      default:
        // Unknown operator kind.
        error();
        return nullptr;
      }
      break;
    }
    default:
      // Unknown cross-reference kind.
      error();
      return nullptr;
    }

    break;
  }
      
  default:
    // We don't know how to deserialize this kind of decl.
    error();
    return nullptr;
  }

  return declOrOffset.get<Decl *>();
}

/// Translate from the Serialization calling convention enum values to the AST
/// strongly-typed enum.
///
/// The former is guaranteed to be stable, but may not reflect this version of
/// the AST.
static Optional<swift::AbstractCC> getActualCC(uint8_t cc) {
  switch (cc) {
#define CASE(THE_CC) \
  case serialization::AbstractCC::THE_CC: \
    return swift::AbstractCC::THE_CC;
  CASE(C)
  CASE(ObjCMethod)
  CASE(Freestanding)
  CASE(Method)
#undef CASE
  default:
    return Nothing;
  }
}

/// Translate from the serialization Ownership enumerators, which are
/// guaranteed to be stable, to the AST ones.
static
Optional<swift::Ownership> getActualOwnership(serialization::Ownership raw) {
  switch (raw) {
  case serialization::Ownership::Strong:  return swift::Ownership::Strong;
  case serialization::Ownership::Unowned: return swift::Ownership::Unowned;
  case serialization::Ownership::Weak:    return swift::Ownership::Weak;
  }
  return Nothing;
}

Type ModuleFile::getType(TypeID TID) {
  if (TID == 0)
    return Type();

  assert(TID <= Types.size() && "invalid decl ID");
  auto &typeOrOffset = Types[TID-1];

  if (typeOrOffset.is<Type>())
    return typeOrOffset.get<Type>();

  DeclTypeCursor.JumpToBit(typeOrOffset.get<BitOffset>());
  auto entry = DeclTypeCursor.advance();

  if (entry.Kind != llvm::BitstreamEntry::Record) {
    // We don't know how to serialize types represented by sub-blocks.
    error();
    return nullptr;
  }

#ifndef NDEBUG
  assert(typeOrOffset.get<BitOffset>() != 0 &&
         "this type is already being deserialized");
  typeOrOffset = BitOffset(0);
#endif

  ASTContext &ctx = ModuleContext->Ctx;

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  unsigned recordID = DeclTypeCursor.readRecord(entry.ID, scratch, &blobData);

  switch (recordID) {
  case decls_block::NAME_ALIAS_TYPE: {
    DeclID underlyingID;
    decls_block::NameAliasTypeLayout::readRecord(scratch, underlyingID);
    auto alias = dyn_cast_or_null<TypeAliasDecl>(getDecl(underlyingID));
    if (!alias) {
      error();
      return nullptr;
    }
    typeOrOffset = alias->getDeclaredType();
    break;
  }

  case decls_block::NOMINAL_TYPE: {
    DeclID declID;
    TypeID parentID;
    decls_block::NominalTypeLayout::readRecord(scratch, declID, parentID);
    typeOrOffset = NominalType::get(cast<NominalTypeDecl>(getDecl(declID)),
                                    getType(parentID), ctx);
    break;
  }

  case decls_block::PAREN_TYPE: {
    TypeID underlyingID;
    decls_block::ParenTypeLayout::readRecord(scratch, underlyingID);
    typeOrOffset = ParenType::get(ctx, getType(underlyingID));
    break;
  }

  case decls_block::TUPLE_TYPE: {
    // The tuple record itself is empty. Read all trailing elements.
    SmallVector<TupleTypeElt, 8> elements;
    while (true) {
      auto entry = DeclTypeCursor.advance();
      if (entry.Kind != llvm::BitstreamEntry::Record)
        break;

      scratch.clear();
      unsigned recordID = DeclTypeCursor.readRecord(entry.ID, scratch,
                                                    &blobData);
      if (recordID != decls_block::TUPLE_TYPE_ELT)
        break;

      IdentifierID nameID;
      TypeID typeID;
      TypeID varargBaseID;
      decls_block::TupleTypeEltLayout::readRecord(scratch, nameID, typeID,
                                                  varargBaseID);

      {
        BCOffsetRAII restoreOffset(DeclTypeCursor);
        elements.push_back({getType(typeID), getIdentifier(nameID),
                            /*initializer=*/nullptr, getType(varargBaseID)});
      }
    }

    typeOrOffset = TupleType::get(elements, ctx);
    break;
  }

  case decls_block::IDENTIFIER_TYPE: {
    TypeID mappedID;
    decls_block::IdentifierTypeLayout::readRecord(scratch, mappedID);
    // FIXME: Actually recreate the IdentifierType instead of just aliasing the
    // underlying mapped type.
    typeOrOffset = getType(mappedID);
    break;
  }

  case decls_block::FUNCTION_TYPE: {
    TypeID inputID;
    TypeID resultID;
    uint8_t rawCallingConvention;
    bool autoClosure;
    bool thin;
    bool blockCompatible;

    decls_block::FunctionTypeLayout::readRecord(scratch, inputID, resultID,
                                                rawCallingConvention,
                                                autoClosure, thin,
                                                blockCompatible);
    auto callingConvention = getActualCC(rawCallingConvention);
    if (!callingConvention.hasValue()) {
      error();
      return nullptr;
    }

    typeOrOffset = FunctionType::get(getType(inputID), getType(resultID),
                                     autoClosure, blockCompatible, thin,
                                     callingConvention.getValue(), ctx);
    break;
  }

  case decls_block::METATYPE_TYPE: {
    TypeID instanceID;
    decls_block::MetaTypeTypeLayout::readRecord(scratch, instanceID);
    typeOrOffset = MetaTypeType::get(getType(instanceID), ctx);
    break;
  }

  case decls_block::LVALUE_TYPE: {
    TypeID objectTypeID;
    bool isImplicit, isNonSettable;
    decls_block::LValueTypeLayout::readRecord(scratch, objectTypeID,
                                              isImplicit, isNonSettable);
    LValueType::Qual quals;
    if (isImplicit)
      quals |= LValueType::Qual::Implicit;
    if (isNonSettable)
      quals |= LValueType::Qual::NonSettable;

    typeOrOffset = LValueType::get(getType(objectTypeID), quals, ctx);
    break;
  }

  case decls_block::REFERENCE_STORAGE_TYPE: {
    uint8_t rawOwnership;
    TypeID referentTypeID;
    decls_block::ReferenceStorageTypeLayout::readRecord(scratch, rawOwnership,
                                                        referentTypeID);

    auto ownership =
      getActualOwnership((serialization::Ownership) rawOwnership);
    if (!ownership.hasValue()) {
      error();
      break;
    }

    typeOrOffset = ReferenceStorageType::get(getType(referentTypeID),
                                             ownership.getValue(), ctx);
    break;
  }

  case decls_block::ARCHETYPE_TYPE: {
    IdentifierID nameID;
    bool isPrimary;
    TypeID parentOrIndex;
    TypeID superclassID;
    ArrayRef<uint64_t> rawConformanceIDs;

    decls_block::ArchetypeTypeLayout::readRecord(scratch, nameID, isPrimary,
                                                 parentOrIndex, superclassID,
                                                 rawConformanceIDs);

    ArchetypeType *parent = nullptr;
    Type superclass;
    Optional<unsigned> index;
    SmallVector<ProtocolDecl *, 4> conformances;

    {
      BCOffsetRAII restoreOffset(DeclTypeCursor);

      if (isPrimary)
        index = parentOrIndex;
      else
        parent = getType(parentOrIndex)->castTo<ArchetypeType>();

      superclass = getType(superclassID);

      for (DeclID protoID : rawConformanceIDs)
        conformances.push_back(cast<ProtocolDecl>(getDecl(protoID)));
    }

    auto archetype = ArchetypeType::getNew(ctx, parent, getIdentifier(nameID),
                                           conformances, superclass, index);
    typeOrOffset = archetype;

    auto entry = DeclTypeCursor.advance();
    if (entry.Kind != llvm::BitstreamEntry::Record) {
      error();
      break;
    }

    scratch.clear();
    unsigned kind = DeclTypeCursor.readRecord(entry.ID, scratch);
    if (kind != decls_block::ARCHETYPE_NESTED_TYPES) {
      error();
      break;
    }

    ArrayRef<uint64_t> rawTypeIDs;
    decls_block::ArchetypeNestedTypesLayout::readRecord(scratch, rawTypeIDs);

    SmallVector<std::pair<Identifier, ArchetypeType *>, 4> nestedTypes;
    for (TypeID nestedID : rawTypeIDs) {
      if (nestedID == TID) {
        nestedTypes.push_back(std::make_pair(ctx.getIdentifier("This"),
                                             archetype));
      } else {
        auto nested = getType(nestedID)->castTo<ArchetypeType>();
        nestedTypes.push_back(std::make_pair(nested->getName(),
                                             nested));
      }
    }
    archetype->setNestedTypes(ctx, nestedTypes);

    break;
  }

  case decls_block::PROTOCOL_COMPOSITION_TYPE: {
    ArrayRef<uint64_t> rawProtocolIDs;

    decls_block::ProtocolCompositionTypeLayout::readRecord(scratch,
                                                           rawProtocolIDs);
    SmallVector<Type, 4> protocols;
    for (TypeID protoID : rawProtocolIDs)
      protocols.push_back(getType(protoID));

    auto composition = ProtocolCompositionType::get(ctx, protocols);
    typeOrOffset = composition;
    break;
  }

  case decls_block::SUBSTITUTED_TYPE: {
    TypeID originalID, replacementID;

    decls_block::SubstitutedTypeLayout::readRecord(scratch, originalID,
                                                   replacementID);
    typeOrOffset = SubstitutedType::get(getType(originalID),
                                        getType(replacementID),
                                        ctx);
    break;
  }

  case decls_block::BOUND_GENERIC_TYPE: {
    DeclID declID;
    TypeID parentID;
    ArrayRef<uint64_t> rawArgumentIDs;

    decls_block::BoundGenericTypeLayout::readRecord(scratch, declID, parentID,
                                                    rawArgumentIDs);
    SmallVector<Type, 8> genericArgs;
    for (TypeID type : rawArgumentIDs)
      genericArgs.push_back(getType(type));

    auto boundTy = BoundGenericType::get(cast<NominalTypeDecl>(getDecl(declID)),
                                         getType(parentID), genericArgs);
    typeOrOffset = boundTy;

    // BoundGenericTypes get uniqued in the ASTContext, so it's possible this
    // type already has its substitutions. In that case, ignore the module's.
    if (boundTy->hasSubstitutions())
      break;

    SmallVector<Substitution, 8> substitutions;
    while (true) {
      auto entry = DeclTypeCursor.advance();
      if (entry.Kind != llvm::BitstreamEntry::Record)
        break;

      scratch.clear();
      unsigned recordID = DeclTypeCursor.readRecord(entry.ID, scratch,
                                                    &blobData);
      if (recordID != decls_block::BOUND_GENERIC_SUBSTITUTION)
        break;

      TypeID archetypeID, replacementID;
      decls_block::BoundGenericSubstitutionLayout::readRecord(scratch,
                                                              archetypeID,
                                                              replacementID);

      SmallVector<ProtocolConformance *, 16> conformanceBuf;
      while (ProtocolConformance *conformance = maybeReadConformance())
        conformanceBuf.push_back(conformance);

      {
        BCOffsetRAII restoreOffset(DeclTypeCursor);
        substitutions.push_back({getType(archetypeID)->castTo<ArchetypeType>(),
                                 getType(replacementID),
                                 ctx.AllocateCopy(conformanceBuf)});
      }
    }

    boundTy->setSubstitutions(substitutions);
    break;
  }

  case decls_block::POLYMORPHIC_FUNCTION_TYPE: {
    TypeID inputID;
    TypeID resultID;
    DeclID genericContextID;
    uint8_t rawCallingConvention;
    bool thin;

    decls_block::PolymorphicFunctionTypeLayout::readRecord(scratch,
                                                           inputID,
                                                           resultID,
                                                           genericContextID,
                                                           rawCallingConvention,
                                                           thin);
    auto callingConvention = getActualCC(rawCallingConvention);
    if (!callingConvention.hasValue()) {
      error();
      return nullptr;
    }

    Decl *genericContext = getDecl(genericContextID);
    assert(genericContext && "loading PolymorphicFunctionType before its decl");

    GenericParamList *paramList = nullptr;
    switch (genericContext->getKind()) {
    case DeclKind::Constructor:
      paramList = cast<ConstructorDecl>(genericContext)->getGenericParams();
      break;
    case DeclKind::Func:
      paramList = cast<FuncDecl>(genericContext)->getGenericParams();
      break;
    case DeclKind::Class:
    case DeclKind::Struct:
    case DeclKind::OneOf:
      paramList = cast<NominalTypeDecl>(genericContext)->getGenericParams();
      break;
    default:
      break;
    }
    assert(paramList && "missing generic params for polymorphic function");

    typeOrOffset = PolymorphicFunctionType::get(getType(inputID),
                                                getType(resultID),
                                                paramList,
                                                thin,
                                                callingConvention.getValue(),
                                                ctx);
    break;
  }

  case decls_block::ARRAY_SLICE_TYPE: {
    TypeID baseID, implID;
    decls_block::ArraySliceTypeLayout::readRecord(scratch, baseID, implID);

    auto sliceTy = ArraySliceType::get(getType(baseID), ctx);
    typeOrOffset = sliceTy;

    // Slice types are uniqued by the ASTContext, so they may already have
    // type information. If so, ignore the information in the module.
    if (!sliceTy->hasImplementationType())
      sliceTy->setImplementationType(getType(implID));
    break;
  }

  case decls_block::ARRAY_TYPE: {
    TypeID baseID;
    uint64_t size;
    decls_block::ArrayTypeLayout::readRecord(scratch, baseID, size);

    typeOrOffset = ArrayType::get(getType(baseID), size, ctx);
    break;
  }

  default:
    // We don't know how to deserialize this kind of type.
    error();
    return nullptr;
  }
  
  return typeOrOffset.get<Type>();
}

ModuleFile::ModuleFile(llvm::OwningPtr<llvm::MemoryBuffer> &&input)
  : ModuleContext(nullptr),
    InputFile(std::move(input)),
    InputReader(reinterpret_cast<const uint8_t *>(InputFile->getBufferStart()),
                reinterpret_cast<const uint8_t *>(InputFile->getBufferEnd())),
    Status(ModuleStatus::Valid) {
  llvm::BitstreamCursor cursor{InputReader};

  for (unsigned char byte : SIGNATURE) {
    if (cursor.AtEndOfStream() || cursor.Read(8) != byte)
      return error();
  }

  // Future-proofing: make sure we validate the control block before we try to
  // read any other blocks.
  bool hasValidControlBlock = false;
  SmallVector<uint64_t, 64> scratch;

  auto topLevelEntry = cursor.advance();
  while (topLevelEntry.Kind == llvm::BitstreamEntry::SubBlock) {
    switch (topLevelEntry.ID) {
    case llvm::bitc::BLOCKINFO_BLOCK_ID:
      if (cursor.ReadBlockInfoBlock())
        return error();
      break;

    case CONTROL_BLOCK_ID: {
      cursor.EnterSubBlock(CONTROL_BLOCK_ID);

      ModuleStatus err = validateControlBlock(cursor, scratch);
      if (err != ModuleStatus::Valid)
        return error(err);

      hasValidControlBlock = true;
      break;
    }

    case INPUT_BLOCK_ID: {
      if (!hasValidControlBlock)
        return error();

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
        case input_block::IMPORTED_MODULE:
          assert(scratch.empty());
          Dependencies.push_back(blobData);
          break;
        default:
          // Unknown input kind, possibly for use by a future version of the
          // module format.
          // FIXME: Should we warn about this?
          break;
        }

        next = cursor.advance();
      }

      if (next.Kind != llvm::BitstreamEntry::EndBlock)
        return error();

      break;
    }

    case DECLS_AND_TYPES_BLOCK_ID: {
      if (!hasValidControlBlock)
        return error();

      // The decls-and-types block is lazily loaded. Save the cursor and load
      // any abbrev records at the start of the block.
      DeclTypeCursor = cursor;
      DeclTypeCursor.EnterSubBlock(DECLS_AND_TYPES_BLOCK_ID);
      if (DeclTypeCursor.advance().Kind == llvm::BitstreamEntry::Error)
        return error();

      // With the main cursor, skip over the block and continue.
      if (cursor.SkipBlock())
        return error();
      break;
    }

    case IDENTIFIER_DATA_BLOCK_ID: {
      if (!hasValidControlBlock)
        return error();

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

      if (next.Kind != llvm::BitstreamEntry::EndBlock)
        return error();

      break;
    }

    case INDEX_BLOCK_ID: {
      if (!hasValidControlBlock)
        return error();

      cursor.EnterSubBlock(INDEX_BLOCK_ID);

      auto next = cursor.advanceSkippingSubblocks();
      while (next.Kind == llvm::BitstreamEntry::Record) {
        scratch.clear();
        StringRef blobData;
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
          assert(blobData.empty());
          RawTopLevelIDs.assign(scratch.begin(), scratch.end());
          break;
        case index_block::OPERATORS:
          assert(blobData.empty());
          RawOperatorIDs.assign(scratch.begin(), scratch.end());
          break;
        default:
          // Unknown index kind, which this version of the compiler won't use.
          break;
        }

        next = cursor.advanceSkippingSubblocks();
      }

      if (next.Kind != llvm::BitstreamEntry::EndBlock)
        return error();

      break;
    }

    case FALL_BACK_TO_TRANSLATION_UNIT_ID:
      // This is a bring-up hack and will eventually go away.
      Status = ModuleStatus::FallBackToTranslationUnit;
      break;

    default:
      // Unknown top-level block, possibly for use by a future version of the
      // module format.
      if (cursor.SkipBlock())
        return error();
      break;
    }
    
    topLevelEntry = cursor.advance(llvm::BitstreamCursor::AF_DontPopBlockAtEnd);
  }
  
  if (topLevelEntry.Kind != llvm::BitstreamEntry::EndBlock)
    return error();
}

bool ModuleFile::associateWithModule(Module *module) {
  assert(!ModuleContext && "already associated with an AST module");
  assert(Status == ModuleStatus::Valid && "invalid module file");

  ASTContext &ctx = module->Ctx;
  bool missingDependency = false;
  for (auto &dependency : Dependencies) {
    assert(!dependency.Mod && "already loaded?");
    Identifier ID = ctx.getIdentifier(dependency.Name);
    // FIXME: Provide a proper source location.
    dependency.Mod = ctx.getModule(std::make_pair(ID, SourceLoc()), false);
    if (!dependency.Mod)
      missingDependency = true;
  }

  if (missingDependency) {
    error(ModuleStatus::MissingDependency);
    return false;
  }

  ModuleContext = module;
  return true;
}

void ModuleFile::buildTopLevelDeclMap() {
  // FIXME: be more lazy about deserialization by encoding this some other way.
  for (DeclID ID : RawTopLevelIDs) {
    auto value = cast<ValueDecl>(getDecl(ID));
    TopLevelIDs[value->getName()] = ID;
  }

  RawTopLevelIDs.clear();
}

void ModuleFile::lookupValue(Identifier name,
                             SmallVectorImpl<ValueDecl*> &results) {
  if (!RawTopLevelIDs.empty())
    buildTopLevelDeclMap();

  if (DeclID ID = TopLevelIDs.lookup(name))
    results.push_back(cast<ValueDecl>(getDecl(ID)));
}

OperatorKind getOperatorKind(DeclKind kind) {
  switch (kind) {
  case DeclKind::PrefixOperator:
    return Prefix;
  case DeclKind::PostfixOperator:
    return Postfix;
  case DeclKind::InfixOperator:
    return Infix;
  default:
    llvm_unreachable("unknown operator fixity");
  }
}

OperatorDecl *ModuleFile::lookupOperator(Identifier name, DeclKind fixity) {
  if (!RawOperatorIDs.empty()) {
    for (DeclID ID : RawOperatorIDs) {
      auto op = cast<OperatorDecl>(getDecl(ID));
      OperatorKey key(op->getName(), getOperatorKind(op->getKind()));
      Operators[key] = op;
    }

    RawOperatorIDs = {};
  }

  return Operators.lookup(OperatorKey(name, getOperatorKind(fixity)));
}
