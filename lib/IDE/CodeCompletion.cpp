//===- CodeCompletion.cpp - Code completion implementation ----------------===//
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

#include "swift/IDE/CodeCompletion.h"
#include "swift/IDE/CodeCompletionCache.h"
#include "swift/IDE/Utils.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/LLVM.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Sema/CodeCompletionTypeChecking.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include "CodeCompletionResultBuilder.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Module.h"
#include "clang/Index/USRGeneration.h"
#include <algorithm>
#include <string>

using namespace swift;
using namespace ide;

typedef llvm::function_ref<bool(ValueDecl*, DeclVisibilityKind)> DeclFilter;
DeclFilter DefaultFilter = [] (ValueDecl* VD, DeclVisibilityKind Kind) {return true;};

std::string swift::ide::removeCodeCompletionTokens(
    StringRef Input, StringRef TokenName, unsigned *CompletionOffset) {
  assert(TokenName.size() >= 1);

  *CompletionOffset = ~0U;

  std::string CleanFile;
  CleanFile.reserve(Input.size());
  const std::string Token = std::string("#^") + TokenName.str() + "^#";

  for (const char *Ptr = Input.begin(), *End = Input.end();
       Ptr != End; ++Ptr) {
    const char C = *Ptr;
    if (C == '#' && Ptr <= End - Token.size() &&
        StringRef(Ptr, Token.size()) == Token) {
      Ptr += Token.size() - 1;
      *CompletionOffset = CleanFile.size();
      CleanFile += '\0';
      continue;
    }
    if (C == '#' && Ptr <= End - 2 && Ptr[1] == '^') {
      do {
        Ptr++;
      } while(*Ptr != '#');
      continue;
    }
    CleanFile += C;
  }
  return CleanFile;
}

namespace {
class StmtFinder : public ASTWalker {
  SourceManager &SM;
  SourceLoc Loc;
  StmtKind Kind;
  Stmt *Found = nullptr;

public:
  StmtFinder(SourceManager &SM, SourceLoc Loc, StmtKind Kind)
      : SM(SM), Loc(Loc), Kind(Kind) {}

  std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
    if (SM.rangeContainsTokenLoc(S->getSourceRange(), Loc))
      return { true, S };
    else
      return { false, S };
  }

  Stmt *walkToStmtPost(Stmt *S) override {
    if (S->getKind() == Kind) {
      Found = S;
      return nullptr;
    }
    return S;
  }

  Stmt *getFoundStmt() const {
    return Found;
  }
};
} // unnamed namespace

static Stmt *findNearestStmt(const AbstractFunctionDecl *AFD, SourceLoc Loc,
                             StmtKind Kind) {
  auto &SM = AFD->getASTContext().SourceMgr;
  assert(SM.rangeContainsTokenLoc(AFD->getSourceRange(), Loc));
  StmtFinder Finder(SM, Loc, Kind);
  // FIXME(thread-safety): the walker is is mutating the AST.
  const_cast<AbstractFunctionDecl *>(AFD)->walk(Finder);
  return Finder.getFoundStmt();
}

CodeCompletionString::CodeCompletionString(ArrayRef<Chunk> Chunks) {
  Chunk *TailChunks = reinterpret_cast<Chunk *>(this + 1);
  std::copy(Chunks.begin(), Chunks.end(), TailChunks);
  NumChunks = Chunks.size();
}

CodeCompletionString *CodeCompletionString::create(llvm::BumpPtrAllocator &Allocator,
                                                   ArrayRef<Chunk> Chunks) {
  void *CCSMem = Allocator.Allocate(sizeof(CodeCompletionString) +
                                    Chunks.size() * sizeof(CodeCompletionString::Chunk),
                                    llvm::alignOf<CodeCompletionString>());
  return new (CCSMem) CodeCompletionString(Chunks);
}

void CodeCompletionString::print(raw_ostream &OS) const {
  unsigned PrevNestingLevel = 0;
  for (auto C : getChunks()) {
    bool AnnotatedTextChunk = false;
    if (C.getNestingLevel() < PrevNestingLevel) {
      OS << "#}";
    }
    switch (C.getKind()) {
    case Chunk::ChunkKind::AccessControlKeyword:
    case Chunk::ChunkKind::DeclAttrKeyword:
    case Chunk::ChunkKind::DeclAttrParamKeyword:
    case Chunk::ChunkKind::OverrideKeyword:
    case Chunk::ChunkKind::ThrowsKeyword:
    case Chunk::ChunkKind::RethrowsKeyword:
    case Chunk::ChunkKind::DeclIntroducer:
    case Chunk::ChunkKind::Text:
    case Chunk::ChunkKind::LeftParen:
    case Chunk::ChunkKind::RightParen:
    case Chunk::ChunkKind::LeftBracket:
    case Chunk::ChunkKind::RightBracket:
    case Chunk::ChunkKind::LeftAngle:
    case Chunk::ChunkKind::RightAngle:
    case Chunk::ChunkKind::Dot:
    case Chunk::ChunkKind::Ellipsis:
    case Chunk::ChunkKind::Comma:
    case Chunk::ChunkKind::ExclamationMark:
    case Chunk::ChunkKind::QuestionMark:
    case Chunk::ChunkKind::Ampersand:
      AnnotatedTextChunk = C.isAnnotation();
      SWIFT_FALLTHROUGH;
    case Chunk::ChunkKind::CallParameterName:
    case Chunk::ChunkKind::CallParameterInternalName:
    case Chunk::ChunkKind::CallParameterColon:
    case Chunk::ChunkKind::DeclAttrParamEqual:
    case Chunk::ChunkKind::CallParameterType:
    case Chunk::ChunkKind::CallParameterClosureType:
    case CodeCompletionString::Chunk::ChunkKind::GenericParameterName:
      if (AnnotatedTextChunk)
        OS << "['";
      else if (C.getKind() == Chunk::ChunkKind::CallParameterInternalName)
        OS << "(";
      else if (C.getKind() == Chunk::ChunkKind::CallParameterClosureType)
        OS << "##";
      for (char Ch : C.getText()) {
        if (Ch == '\n')
          OS << "\\n";
        else
          OS << Ch;
      }
      if (AnnotatedTextChunk)
        OS << "']";
      else if (C.getKind() == Chunk::ChunkKind::CallParameterInternalName)
        OS << ")";
      break;
    case Chunk::ChunkKind::OptionalBegin:
    case Chunk::ChunkKind::CallParameterBegin:
    case CodeCompletionString::Chunk::ChunkKind::GenericParameterBegin:
      OS << "{#";
      break;
    case Chunk::ChunkKind::DynamicLookupMethodCallTail:
    case Chunk::ChunkKind::OptionalMethodCallTail:
      OS << C.getText();
      break;
    case Chunk::ChunkKind::TypeAnnotation:
      OS << "[#";
      OS << C.getText();
      OS << "#]";
      break;
    case Chunk::ChunkKind::BraceStmtWithCursor:
      OS << " {|}";
      break;
    }
    PrevNestingLevel = C.getNestingLevel();
  }
  while (PrevNestingLevel > 0) {
    OS << "#}";
    PrevNestingLevel--;
  }
}

void CodeCompletionString::dump() const {
  print(llvm::errs());
}

CodeCompletionDeclKind
CodeCompletionResult::getCodeCompletionDeclKind(const Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::IfConfig:
    llvm_unreachable("not expecting such a declaration result");
  case DeclKind::Module:
    return CodeCompletionDeclKind::Module;
  case DeclKind::TypeAlias:
  case DeclKind::AssociatedType:
    return CodeCompletionDeclKind::TypeAlias;
  case DeclKind::GenericTypeParam:
    return CodeCompletionDeclKind::GenericTypeParam;
  case DeclKind::Enum:
    return CodeCompletionDeclKind::Enum;
  case DeclKind::Struct:
    return CodeCompletionDeclKind::Struct;
  case DeclKind::Class:
    return CodeCompletionDeclKind::Class;
  case DeclKind::Protocol:
    return CodeCompletionDeclKind::Protocol;
  case DeclKind::Var:
  case DeclKind::Param: {
    auto DC = D->getDeclContext();
    if (DC->isTypeContext()) {
      if (cast<VarDecl>(D)->isStatic())
        return CodeCompletionDeclKind::StaticVar;
      else
        return CodeCompletionDeclKind::InstanceVar;
    }
    if (DC->isLocalContext())
      return CodeCompletionDeclKind::LocalVar;
    return CodeCompletionDeclKind::GlobalVar;
  }
  case DeclKind::Constructor:
    return CodeCompletionDeclKind::Constructor;
  case DeclKind::Destructor:
    return CodeCompletionDeclKind::Destructor;
  case DeclKind::Func: {
    auto DC = D->getDeclContext();
    auto FD = cast<FuncDecl>(D);
    if (DC->isTypeContext()) {
      if (FD->isStatic())
        return CodeCompletionDeclKind::StaticMethod;
      return CodeCompletionDeclKind::InstanceMethod;
    }
    if (FD->isOperator())
      return CodeCompletionDeclKind::OperatorFunction;
    return CodeCompletionDeclKind::FreeFunction;
  }
  case DeclKind::EnumElement:
    return CodeCompletionDeclKind::EnumElement;
  case DeclKind::Subscript:
    return CodeCompletionDeclKind::Subscript;
  }
  llvm_unreachable("invalid DeclKind");
}

void CodeCompletionResult::print(raw_ostream &OS) const {
  llvm::SmallString<64> Prefix;
  switch (getKind()) {
  case ResultKind::Declaration:
    Prefix.append("Decl");
    switch (getAssociatedDeclKind()) {
    case CodeCompletionDeclKind::Class:
      Prefix.append("[Class]");
      break;
    case CodeCompletionDeclKind::Struct:
      Prefix.append("[Struct]");
      break;
    case CodeCompletionDeclKind::Enum:
      Prefix.append("[Enum]");
      break;
    case CodeCompletionDeclKind::EnumElement:
      Prefix.append("[EnumElement]");
      break;
    case CodeCompletionDeclKind::Protocol:
      Prefix.append("[Protocol]");
      break;
    case CodeCompletionDeclKind::TypeAlias:
      Prefix.append("[TypeAlias]");
      break;
    case CodeCompletionDeclKind::GenericTypeParam:
      Prefix.append("[GenericTypeParam]");
      break;
    case CodeCompletionDeclKind::Constructor:
      Prefix.append("[Constructor]");
      break;
    case CodeCompletionDeclKind::Destructor:
      Prefix.append("[Destructor]");
      break;
    case CodeCompletionDeclKind::Subscript:
      Prefix.append("[Subscript]");
      break;
    case CodeCompletionDeclKind::StaticMethod:
      Prefix.append("[StaticMethod]");
      break;
    case CodeCompletionDeclKind::InstanceMethod:
      Prefix.append("[InstanceMethod]");
      break;
    case CodeCompletionDeclKind::OperatorFunction:
      Prefix.append("[OperatorFunction]");
      break;
    case CodeCompletionDeclKind::FreeFunction:
      Prefix.append("[FreeFunction]");
      break;
    case CodeCompletionDeclKind::StaticVar:
      Prefix.append("[StaticVar]");
      break;
    case CodeCompletionDeclKind::InstanceVar:
      Prefix.append("[InstanceVar]");
      break;
    case CodeCompletionDeclKind::LocalVar:
      Prefix.append("[LocalVar]");
      break;
    case CodeCompletionDeclKind::GlobalVar:
      Prefix.append("[GlobalVar]");
      break;
    case CodeCompletionDeclKind::Module:
      Prefix.append("[Module]");
      break;
    }
    break;
  case ResultKind::Keyword:
    Prefix.append("Keyword");
    break;
  case ResultKind::Pattern:
    Prefix.append("Pattern");
    break;
  }
  Prefix.append("/");
  switch (getSemanticContext()) {
  case SemanticContextKind::None:
    Prefix.append("None");
    break;
  case SemanticContextKind::ExpressionSpecific:
    Prefix.append("ExprSpecific");
    break;
  case SemanticContextKind::Local:
    Prefix.append("Local");
    break;
  case SemanticContextKind::CurrentNominal:
    Prefix.append("CurrNominal");
    break;
  case SemanticContextKind::Super:
    Prefix.append("Super");
    break;
  case SemanticContextKind::OutsideNominal:
    Prefix.append("OutNominal");
    break;
  case SemanticContextKind::CurrentModule:
    Prefix.append("CurrModule");
    break;
  case SemanticContextKind::OtherModule:
    Prefix.append("OtherModule");
    if (!ModuleName.empty())
      Prefix.append((Twine("[") + ModuleName + "]").str());
    break;
  }
  if (NotRecommended)
    Prefix.append("/NotRecommended");
  if (NumBytesToErase != 0) {
    Prefix.append("/Erase[");
    Prefix.append(Twine(NumBytesToErase).str());
    Prefix.append("]");
  }
  switch (TypeDistance) {
    case ExpectedTypeRelation::Invalid:
      Prefix.append("/TypeRelation[Invalid]");
      break;
    case ExpectedTypeRelation::Identical:
      Prefix.append("/TypeRelation[Identical]");
      break;
    case ExpectedTypeRelation::Convertible:
      Prefix.append("/TypeRelation[Convertible]");
      break;
    case ExpectedTypeRelation::Unrelated:
      break;
  }
  Prefix.append(": ");
  while (Prefix.size() < 36) {
    Prefix.append(" ");
  }
  OS << Prefix;
  CompletionString->print(OS);
}

void CodeCompletionResult::dump() const {
  print(llvm::errs());
}

static StringRef copyString(llvm::BumpPtrAllocator &Allocator,
                            StringRef Str) {
  char *Mem = Allocator.Allocate<char>(Str.size());
  std::copy(Str.begin(), Str.end(), Mem);
  return StringRef(Mem, Str.size());
}

static ArrayRef<StringRef> copyStringArray(llvm::BumpPtrAllocator &Allocator,
                                           ArrayRef<StringRef> Arr) {
  StringRef *Buff = Allocator.Allocate<StringRef>(Arr.size());
  std::copy(Arr.begin(), Arr.end(), Buff);
  return llvm::makeArrayRef(Buff, Arr.size());
}

void CodeCompletionResultBuilder::addChunkWithText(
    CodeCompletionString::Chunk::ChunkKind Kind, StringRef Text) {
  addChunkWithTextNoCopy(Kind, copyString(*Sink.Allocator, Text));
}

void CodeCompletionResultBuilder::setAssociatedDecl(const Decl *D) {
  assert(Kind == CodeCompletionResult::ResultKind::Declaration);
  AssociatedDecl = D;

  if (auto *ClangD = D->getClangDecl())
    CurrentModule = ClangD->getImportedOwningModule();
  // FIXME: macros
  // FIXME: imported header module

  if (!CurrentModule)
    CurrentModule = D->getModuleContext();
}

StringRef CodeCompletionContext::copyString(StringRef Str) {
  return ::copyString(*CurrentResults.Allocator, Str);
}

bool shouldCopyAssociatedUSRForDecl(const ValueDecl *VD) {
  // Avoid trying to generate a USR for some declaration types.
  if (isa<AbstractTypeParamDecl>(VD) && !isa<AssociatedTypeDecl>(VD))
    return false;
  if (isa<ParamDecl>(VD))
    return false;
  if (isa<ModuleDecl>(VD))
    return false;
  if (VD->hasClangNode() && !VD->getClangDecl())
    return false;

  return true;
}

template <typename FnTy>
static void walkValueDeclAndOverriddenDecls(const Decl *D, const FnTy &Fn) {
  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    Fn(VD);
    walkOverriddenDecls(VD, Fn);
  }
}

ArrayRef<StringRef> copyAssociatedUSRs(llvm::BumpPtrAllocator &Allocator,
                                       const Decl *D) {
  llvm::SmallVector<StringRef, 4> USRs;
  walkValueDeclAndOverriddenDecls(D, [&](llvm::PointerUnion<const ValueDecl*,
                                                  const clang::NamedDecl*> OD) {
    llvm::SmallString<128> SS;
    bool Ignored = true;
    if (auto *OVD = OD.dyn_cast<const ValueDecl*>()) {
      if (shouldCopyAssociatedUSRForDecl(OVD)) {
        llvm::raw_svector_ostream OS(SS);
        Ignored = printDeclUSR(OVD, OS);
      }
    } else if (auto *OND = OD.dyn_cast<const clang::NamedDecl*>()) {
      Ignored = clang::index::generateUSRForDecl(OND, SS);
    }
    
    if (!Ignored)
      USRs.push_back(copyString(Allocator, SS));
  });

  if (!USRs.empty())
    return copyStringArray(Allocator, USRs);

  return ArrayRef<StringRef>();
}

static CodeCompletionResult::ExpectedTypeRelation calculateTypeRelation(
                                                                Type Ty,
                                                                Type ExpectedTy,
                                                                DeclContext *DC) {
  if (Ty.isNull() || ExpectedTy.isNull() ||
      Ty->is<ErrorType>() || ExpectedTy->is<ErrorType>())
    return CodeCompletionResult::ExpectedTypeRelation::Unrelated;
  if (Ty.getCanonicalTypeOrNull() == ExpectedTy.getCanonicalTypeOrNull())
    return CodeCompletionResult::ExpectedTypeRelation::Identical;
  if (isConvertibleTo(Ty, ExpectedTy, DC))
    return CodeCompletionResult::ExpectedTypeRelation::Convertible;
  if (auto FT = Ty->getAs<AnyFunctionType>()) {
    if (FT->getResult()->isVoid())
      return CodeCompletionResult::ExpectedTypeRelation::Invalid;
    return std::max(calculateTypeRelation(FT->getResult(), ExpectedTy, DC),
                    CodeCompletionResult::ExpectedTypeRelation::Unrelated);
  }
  return CodeCompletionResult::ExpectedTypeRelation::Unrelated;
}

static CodeCompletionResult::ExpectedTypeRelation calculateTypeRelationForDecl (
                                                            const Decl *D,
                                                            Type ExpectedType) {
  auto VD = dyn_cast<ValueDecl>(D);
  auto DC = D->getDeclContext();
  if (!VD)
    return CodeCompletionResult::ExpectedTypeRelation::Unrelated;
  if (auto FD = dyn_cast<FuncDecl>(VD)) {
    return std::max(calculateTypeRelation(FD->getType(), ExpectedType, DC),
                    calculateTypeRelation(FD->getResultType(), ExpectedType, DC));
  }
  if (auto NTD = dyn_cast<NominalTypeDecl>(VD)) {
    return std::max(calculateTypeRelation(NTD->getType(), ExpectedType, DC),
                    calculateTypeRelation(NTD->getDeclaredType(), ExpectedType, DC));
  }
  return calculateTypeRelation(VD->getType(), ExpectedType, DC);
}

static CodeCompletionResult::ExpectedTypeRelation calculateMaxTypeRelationForDecl (
                                                const Decl *D,
                                                ArrayRef<Type> ExpectedTypes) {
  auto Result = CodeCompletionResult::ExpectedTypeRelation::Unrelated;
  for (auto Type : ExpectedTypes) {
    Result = std::max(Result, calculateTypeRelationForDecl(D, Type));
  }
  return Result;
}

CodeCompletionResult *CodeCompletionResultBuilder::takeResult() {
  auto *CCS = CodeCompletionString::create(*Sink.Allocator, Chunks);

  switch (Kind) {
  case CodeCompletionResult::ResultKind::Declaration: {
    StringRef BriefComment;
    auto MaybeClangNode = AssociatedDecl->getClangNode();
    if (MaybeClangNode) {
      if (auto *D = MaybeClangNode.getAsDecl()) {
        const auto &ClangContext = D->getASTContext();
        if (const clang::RawComment *RC =
                ClangContext.getRawCommentForAnyRedecl(D))
          BriefComment = RC->getBriefText(ClangContext);
      }
    } else {
      BriefComment = AssociatedDecl->getBriefComment();
    }

    StringRef ModuleName;
    if (CurrentModule) {
      if (Sink.LastModule.first == CurrentModule.getOpaqueValue()) {
        ModuleName = Sink.LastModule.second;
      } else {
        if (auto *C = CurrentModule.dyn_cast<const clang::Module *>()) {
          ModuleName = copyString(*Sink.Allocator, C->getFullModuleName());
        } else {
          ModuleName = copyString(
              *Sink.Allocator,
              CurrentModule.get<const swift::Module *>()->getName().str());
        }
        Sink.LastModule.first = CurrentModule.getOpaqueValue();
        Sink.LastModule.second = ModuleName;
      }
    }

    return new (*Sink.Allocator) CodeCompletionResult(
        SemanticContext, NumBytesToErase, CCS, AssociatedDecl, ModuleName,
        /*NotRecommended=*/false, copyString(*Sink.Allocator, BriefComment),
        copyAssociatedUSRs(*Sink.Allocator, AssociatedDecl),
        ExpectedTypes.empty() ?
          CodeCompletionResult::ExpectedTypeRelation::Unrelated :
          calculateMaxTypeRelationForDecl(AssociatedDecl, ExpectedTypes));
  }

  case CodeCompletionResult::ResultKind::Keyword:
  case CodeCompletionResult::ResultKind::Pattern:
    return new (*Sink.Allocator)
        CodeCompletionResult(Kind, SemanticContext, NumBytesToErase, CCS);
  }
}

void CodeCompletionResultBuilder::finishResult() {
  Sink.Results.push_back(takeResult());
}


MutableArrayRef<CodeCompletionResult *> CodeCompletionContext::takeResults() {
  // Copy pointers to the results.
  const size_t Count = CurrentResults.Results.size();
  CodeCompletionResult **Results =
      CurrentResults.Allocator->Allocate<CodeCompletionResult *>(Count);
  std::copy(CurrentResults.Results.begin(), CurrentResults.Results.end(),
            Results);
  CurrentResults.Results.clear();
  return MutableArrayRef<CodeCompletionResult *>(Results, Count);
}

Optional<unsigned> CodeCompletionString::getFirstTextChunkIndex(
    bool includeLeadingPunctuation) const {
  for (auto i : indices(getChunks())) {
    auto &C = getChunks()[i];
    switch (C.getKind()) {
    case CodeCompletionString::Chunk::ChunkKind::Text:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterName:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterInternalName:
    case CodeCompletionString::Chunk::ChunkKind::GenericParameterName:
    case CodeCompletionString::Chunk::ChunkKind::LeftParen:
    case CodeCompletionString::Chunk::ChunkKind::LeftBracket:
    case CodeCompletionString::Chunk::ChunkKind::DeclAttrParamKeyword:
    case CodeCompletionString::Chunk::ChunkKind::DeclAttrKeyword:
      return i;
    case CodeCompletionString::Chunk::ChunkKind::Dot:
    case CodeCompletionString::Chunk::ChunkKind::ExclamationMark:
    case CodeCompletionString::Chunk::ChunkKind::QuestionMark:
      if (includeLeadingPunctuation)
        return i;
      continue;
    case CodeCompletionString::Chunk::ChunkKind::RightParen:
    case CodeCompletionString::Chunk::ChunkKind::RightBracket:
    case CodeCompletionString::Chunk::ChunkKind::LeftAngle:
    case CodeCompletionString::Chunk::ChunkKind::RightAngle:
    case CodeCompletionString::Chunk::ChunkKind::Ellipsis:
    case CodeCompletionString::Chunk::ChunkKind::Comma:
    case CodeCompletionString::Chunk::ChunkKind::Ampersand:
    case CodeCompletionString::Chunk::ChunkKind::AccessControlKeyword:
    case CodeCompletionString::Chunk::ChunkKind::OverrideKeyword:
    case CodeCompletionString::Chunk::ChunkKind::ThrowsKeyword:
    case CodeCompletionString::Chunk::ChunkKind::RethrowsKeyword:
    case CodeCompletionString::Chunk::ChunkKind::DeclIntroducer:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterColon:
    case CodeCompletionString::Chunk::ChunkKind::DeclAttrParamEqual:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterType:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterClosureType:
    case CodeCompletionString::Chunk::ChunkKind::OptionalBegin:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterBegin:
    case CodeCompletionString::Chunk::ChunkKind::GenericParameterBegin:
    case CodeCompletionString::Chunk::ChunkKind::DynamicLookupMethodCallTail:
    case CodeCompletionString::Chunk::ChunkKind::OptionalMethodCallTail:
    case CodeCompletionString::Chunk::ChunkKind::TypeAnnotation:
      continue;

    case CodeCompletionString::Chunk::ChunkKind::BraceStmtWithCursor:
      llvm_unreachable("should have already extracted the text");
    }
  }
  return None;
}

StringRef CodeCompletionString::getFirstTextChunk() const {
  Optional<unsigned> Idx = getFirstTextChunkIndex();
  if (Idx.hasValue())
    return getChunks()[*Idx].getText();
  return StringRef();
}

void CodeCompletionString::getName(raw_ostream &OS) const {
  auto FirstTextChunk = getFirstTextChunkIndex();
  int TextSize = 0;
  if (FirstTextChunk.hasValue()) {
    for (auto C : getChunks().slice(*FirstTextChunk)) {
      using ChunkKind = CodeCompletionString::Chunk::ChunkKind;
      if (C.getKind() == ChunkKind::BraceStmtWithCursor)
        break;

      bool shouldPrint = !C.isAnnotation();
      switch (C.getKind()) {
      case ChunkKind::TypeAnnotation:
      case ChunkKind::CallParameterClosureType:
      case ChunkKind::DeclAttrParamEqual:
        continue;
      case ChunkKind::ThrowsKeyword:
      case ChunkKind::RethrowsKeyword:
        shouldPrint = true; // Even when they're annotations.
        break;
      default:
        break;
      }

      if (C.hasText() && shouldPrint) {
        TextSize += C.getText().size();
        OS << C.getText();
      }
    }
  }
  assert((TextSize > 0) &&
         "code completion string should have non-empty name!");
}

void CodeCompletionContext::sortCompletionResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  struct ResultAndName {
    CodeCompletionResult *result;
    std::string name;
  };

  // Caching the name of each field is important to avoid unnecessary calls to
  // CodeCompletionString::getName().
  std::vector<ResultAndName> nameCache(Results.size());
  for (unsigned i = 0, n = Results.size(); i < n; ++i) {
    auto *result = Results[i];
    nameCache[i].result = result;
    llvm::raw_string_ostream OS(nameCache[i].name);
    result->getCompletionString()->getName(OS);
    OS.flush();
  }

  // Sort nameCache, and then transform Results to return the pointers in order.
  std::sort(nameCache.begin(), nameCache.end(),
            [](const ResultAndName &LHS, const ResultAndName &RHS) {
    int Result = StringRef(LHS.name).compare_lower(RHS.name);
    // If the case insensitive comparison is equal, then secondary sort order
    // should be case sensitive.
    if (Result == 0)
      Result = LHS.name.compare(RHS.name);
    return Result < 0;
  });

  std::transform(nameCache.begin(), nameCache.end(), Results.begin(),
                 [](const ResultAndName &entry) { return entry.result; });
}

namespace {
class CodeCompletionCallbacksImpl : public CodeCompletionCallbacks {
  CodeCompletionContext &CompletionContext;
  std::vector<RequestedCachedModule> RequestedModules;
  CodeCompletionConsumer &Consumer;

  enum class CompletionKind {
    None,
    Import,
    UnresolvedMember,
    DotExpr,
    PostfixExprBeginning,
    PostfixExpr,
    PostfixExprParen,
    SuperExpr,
    SuperExprDot,
    TypeSimpleBeginning,
    TypeIdentifierWithDot,
    TypeIdentifierWithoutDot,
    CaseStmtBeginning,
    CaseStmtDotPrefix,
    NominalMemberBeginning,
    AttributeBegin,
    AttributeDeclParen,
    PoundAvailablePlatform,
    AssignmentRHS,
    CallArg,
  };

  CodeCompletionExpr *CodeCompleteTokenExpr;
  AssignExpr *AssignmentExpr;
  CallExpr *FuncCallExpr;
  UnresolvedMemberExpr *UnresolvedExpr;
  bool UnresolvedExprInReturn;
  std::vector<std::string> TokensBeforeUnresolvedExpr;
  CompletionKind Kind = CompletionKind::None;
  Expr *ParsedExpr = nullptr;
  SourceLoc DotLoc;
  TypeLoc ParsedTypeLoc;
  DeclContext *CurDeclContext = nullptr;
  Decl *CStyleForLoopIterationVariable = nullptr;
  DeclAttrKind AttrKind;
  int AttrParamIndex;
  bool IsInSil;
  Optional<DeclKind> AttTargetDK;

  SmallVector<StringRef, 3> ParsedKeywords;

  void addSuperKeyword(CodeCompletionResultSink &Sink) {
    auto *DC = CurDeclContext->getInnermostTypeContext();
    if (!DC)
      return;
    Type DT = DC->getDeclaredTypeInContext();
    if (DT.isNull() || DT->is<ErrorType>())
      return;
    OwnedResolver TypeResolver(createLazyResolver(CurDeclContext->getASTContext()));
    Type ST = DT->getSuperclass(TypeResolver.get());
    if (ST.isNull() || ST->is<ErrorType>())
      return;
    if (ST->getNominalOrBoundGenericNominal()) {
      CodeCompletionResultBuilder Builder(Sink,
                                          CodeCompletionResult::ResultKind::Keyword,
                                          SemanticContextKind::CurrentNominal,
                                          {});
      Builder.addTextChunk("super");
      ST = ST->getReferenceStorageReferent();
      assert(!ST->isVoid() && "Cannot get type name.");
      Builder.addTypeAnnotation(ST.getString());
    }
  }

  /// \brief Set to true when we have delivered code completion results
  /// to the \c Consumer.
  bool DeliveredResults = false;

  bool typecheckContextImpl(DeclContext *DC) {
    // Type check the function that contains the expression.
    if (DC->getContextKind() == DeclContextKind::AbstractClosureExpr ||
        DC->getContextKind() == DeclContextKind::AbstractFunctionDecl) {
      SourceLoc EndTypeCheckLoc =
          ParsedExpr ? ParsedExpr->getStartLoc()
                     : P.Context.SourceMgr.getCodeCompletionLoc();
      // Find the nearest containing function or nominal decl.
      DeclContext *DCToTypeCheck = DC;
      while (!DCToTypeCheck->isModuleContext() &&
             !isa<AbstractFunctionDecl>(DCToTypeCheck) &&
             !isa<NominalTypeDecl>(DCToTypeCheck) &&
             !isa<TopLevelCodeDecl>(DCToTypeCheck))
        DCToTypeCheck = DCToTypeCheck->getParent();
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DCToTypeCheck)) {
        // We found a function.  First, type check the nominal decl that
        // contains the function.  Then type check the function itself.
        typecheckContextImpl(DCToTypeCheck->getParent());
        return typeCheckAbstractFunctionBodyUntil(AFD, EndTypeCheckLoc);
      }
      if (isa<NominalTypeDecl>(DCToTypeCheck)) {
        // We found a nominal decl (for example, the closure is used in an
        // initializer of a property).
        return typecheckContextImpl(DCToTypeCheck);
      }
      if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(DCToTypeCheck)) {
        return typeCheckTopLevelCodeDecl(TLCD);
      }
      return false;
    }
    if (auto *NTD = dyn_cast<NominalTypeDecl>(DC)) {
      // First, type check the parent DeclContext.
      typecheckContextImpl(DC->getParent());
      if (NTD->hasType())
        return true;
      return typeCheckCompletionDecl(cast<NominalTypeDecl>(DC));
    }
    if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(DC)) {
      return typeCheckTopLevelCodeDecl(TLCD);
    }
    return true;
  }

  /// \returns true on success, false on failure.
  bool typecheckContext() {
    return typecheckContextImpl(CurDeclContext);
  }

  /// \returns true on success, false on failure.
  bool typecheckDelayedParsedDecl() {
    assert(DelayedParsedDecl && "should have a delayed parsed decl");
    return typeCheckCompletionDecl(DelayedParsedDecl);
  }

  Optional<Type> getTypeOfParsedExpr() {
    assert(ParsedExpr && "should have an expression");
    // If we've already successfully type-checked the expression for some
    // reason, just return the type.
    // FIXME: if it's ErrorType but we've already typechecked we shouldn't
    // typecheck again. rdar://21466394
    if (ParsedExpr->getType() && !ParsedExpr->getType()->is<ErrorType>())
      return ParsedExpr->getType();

    Expr *ModifiedExpr = ParsedExpr;
    if (auto T = getTypeOfCompletionContextExpr(P.Context, CurDeclContext,
                                                ModifiedExpr)) {
      // FIXME: even though we don't apply the solution, the type checker may
      // modify the original expression. We should understand what effect that
      // may have on code completion.
      ParsedExpr = ModifiedExpr;
      return T;
    }
    return None;
  }

  /// \returns true on success, false on failure.
  bool typecheckParsedType() {
    assert(ParsedTypeLoc.getTypeRepr() && "should have a TypeRepr");
    return !performTypeLocChecking(P.Context, ParsedTypeLoc, /*SIL*/ false,
                                   CurDeclContext, false);
  }

public:
  CodeCompletionCallbacksImpl(Parser &P,
                              CodeCompletionContext &CompletionContext,
                              CodeCompletionConsumer &Consumer)
      : CodeCompletionCallbacks(P), CompletionContext(CompletionContext),
        Consumer(Consumer) {
  }

  void completeExpr() override;
  void completeDotExpr(Expr *E, SourceLoc DotLoc) override;
  void completePostfixExprBeginning(CodeCompletionExpr *E) override;
  void completePostfixExpr(Expr *E) override;
  void completePostfixExprParen(Expr *E, Expr *CCE) override;
  void completeExprSuper(SuperRefExpr *SRE) override;
  void completeExprSuperDot(SuperRefExpr *SRE) override;

  void completeTypeSimpleBeginning() override;
  void completeTypeIdentifierWithDot(IdentTypeRepr *ITR) override;
  void completeTypeIdentifierWithoutDot(IdentTypeRepr *ITR) override;

  void completeCaseStmtBeginning() override;
  void completeCaseStmtDotPrefix() override;
  void completeDeclAttrKeyword(Decl *D, bool Sil, bool Param) override;
  void completeDeclAttrParam(DeclAttrKind DK, int Index) override;
  void completeNominalMemberBeginning(
      SmallVectorImpl<StringRef> &Keywords) override;

  void completePoundAvailablePlatform() override;
  void completeImportDecl() override;
  void completeUnresolvedMember(UnresolvedMemberExpr *E,
                                ArrayRef<StringRef> Identifiers,
                                bool HasReturn) override;
  void completeAssignmentRHS(AssignExpr *E) override;
  void completeCallArg(CallExpr *E) override;
  void addKeywords(CodeCompletionResultSink &Sink);

  void doneParsing() override;

  void deliverCompletionResults();
};
} // end unnamed namespace

void CodeCompletionCallbacksImpl::completeExpr() {
  if (DeliveredResults)
    return;

  Parser::ParserPositionRAII RestorePosition(P);
  P.restoreParserPosition(ExprBeginPosition);

  // FIXME: implement fallback code completion.

  deliverCompletionResults();
}

namespace {
class ArchetypeTransformer {
  DeclContext *DC;
  Type BaseTy;
  llvm::DenseMap<TypeBase *, Type> Cache;
  llvm::DenseMap<Identifier, Type> TypeParams;

public:
  ArchetypeTransformer(DeclContext *DC, Type Ty) : DC(DC),
                                                   BaseTy(Ty->getRValueType()){
    if (!BaseTy->getNominalOrBoundGenericNominal())
      return;
    SmallVector<swift::Substitution, 3> Scrach;
    for (auto Sub : BaseTy->getDesugaredType()->gatherAllSubstitutions(
                      DC->getParentModule(), Scrach,
                      DC->getASTContext().getLazyResolver())) {
      if (Sub.getReplacement()->isCanonical())
        TypeParams[Sub.getArchetype()->getName()] = Sub.getReplacement();
    }
  }

  std::function<Type(Type)> getTransformerFunc() {
    return [&](Type Ty) {
      if (Ty->getKind() != TypeKind::Archetype)
        return Ty;
      if (Cache.count(Ty.getPointer()) > 0) {
        return Cache[Ty.getPointer()];
      }
      Type Result = Ty;
      auto *RootArc = cast<ArchetypeType>(Result.getPointer());
      llvm::SmallVector<Identifier, 1> Names;
      bool SelfDerived = false;
      for(auto *AT = RootArc; AT; AT = AT->getParent()) {
        if(!AT->getSelfProtocol())
          Names.insert(Names.begin(), AT->getName());
        else
          SelfDerived = true;
      }
      if (SelfDerived) {
        if (auto MT = checkMemberType(*DC, BaseTy, Names)) {
          if (auto NAT = dyn_cast<NameAliasType>(MT.getPointer())) {
            Result = NAT->getSinglyDesugaredType();
          } else {
            Result =  MT;
          }
        }
      } else if (TypeParams.count(RootArc->getName()) != 0 &&
                 !RootArc->getParent()) {
        Result = TypeParams[RootArc->getName()];
      }

      auto ATT = dyn_cast<ArchetypeType>(Result.getPointer());
      if (ATT && !ATT->getParent()) {
        auto Conformances = ATT->getConformsTo();
        if (Conformances.size() == 1) {
          Result = Conformances[0]->getDeclaredType();
        } else if (!Conformances.empty()) {
          llvm::SmallVector<Type, 3> ConformedTypes;
          for (auto PD : Conformances) {
            ConformedTypes.push_back(PD->getDeclaredType());
          }
          Result = ProtocolCompositionType::get(DC->getASTContext(),
                                                ConformedTypes);
        }
      }
      if (Result->getKind() != TypeKind::Archetype)
        Result = Result.transform(getTransformerFunc());
      Cache[Ty.getPointer()] = Result;
      return Result;
    };
  }
};

/// Build completions by doing visible decl lookup from a context.
class CompletionLookup final : public swift::VisibleDeclConsumer {
  CodeCompletionResultSink &Sink;
  ASTContext &Ctx;
  OwnedResolver TypeResolver;
  const DeclContext *CurrDeclContext;

  enum class LookupKind {
    ValueExpr,
    ValueInDeclContext,
    EnumElement,
    Type,
    TypeInDeclContext,
    ImportFromModule
  };

  LookupKind Kind;

  /// Type of the user-provided expression for LookupKind::ValueExpr
  /// completions.
  Type ExprType;

  /// Whether the expr is of statically inferred metatype.
  bool IsStaticMetatype;

  /// User-provided base type for LookupKind::Type completions.
  Type BaseType;

  /// Expected types of the code completion expression.
  ArrayRef<Type> ExpectedTypes;

  bool HaveDot = false;
  SourceLoc DotLoc;
  bool NeedLeadingDot = false;

  bool NeedOptionalUnwrap = false;
  unsigned NumBytesToEraseForOptionalUnwrap = 0;

  bool HaveLParen = false;
  bool IsSuperRefExpr = false;
  bool IsDynamicLookup = false;

  /// \brief True if we are code completing inside a static method.
  bool InsideStaticMethod = false;

  /// \brief Innermost method that the code completion point is in.
  const AbstractFunctionDecl *CurrentMethod = nullptr;

  /// \brief Declarations that should get ExpressionSpecific semantic context.
  llvm::SmallSet<const Decl *, 4> ExpressionSpecificDecls;

  using DeducedAssociatedTypes =
      llvm::DenseMap<const AssociatedTypeDecl *, Type>;
  std::map<const NominalTypeDecl *, DeducedAssociatedTypes>
      DeducedAssociatedTypeCache;

  Optional<SemanticContextKind> ForcedSemanticContext = None;

  std::unique_ptr<ArchetypeTransformer> TransformerPt = nullptr;

public:
  bool FoundFunctionCalls = false;
  bool FoundFunctionsWithoutFirstKeyword = false;

private:
  void foundFunction(const AbstractFunctionDecl *AFD) {
    FoundFunctionCalls = true;
    DeclName Name = AFD->getFullName();
    auto ArgNames = Name.getArgumentNames();
    if (ArgNames.empty())
      return;
    if (ArgNames[0].empty())
      FoundFunctionsWithoutFirstKeyword = true;
  }

  void foundFunction(const AnyFunctionType *AFT) {
    FoundFunctionCalls = true;
    Type In = AFT->getInput();
    if (!In)
      return;
    if (isa<ParenType>(In.getPointer())) {
      FoundFunctionsWithoutFirstKeyword = true;
      return;
    }
    TupleType *InTuple = In->getAs<TupleType>();
    if (!InTuple)
      return;
    auto Elements = InTuple->getElements();
    if (Elements.empty())
      return;
    if (!Elements[0].hasName())
      FoundFunctionsWithoutFirstKeyword = true;
  }

public:
  struct RequestedResultsTy {
    const Module *TheModule;
    bool OnlyTypes;
    bool NeedLeadingDot;

    static RequestedResultsTy fromModule(const Module *TheModule) {
      return { TheModule, false, false };
    }

    RequestedResultsTy onlyTypes() const {
      return { TheModule, true, NeedLeadingDot };
    }

    RequestedResultsTy needLeadingDot(bool NeedDot) const {
      return { TheModule, OnlyTypes, NeedDot };
    }

    static RequestedResultsTy toplevelResults() {
      return { nullptr, false, false };
    }
  };

  Optional<RequestedResultsTy> RequestedCachedResults;

public:
  CompletionLookup(CodeCompletionResultSink &Sink,
                   ASTContext &Ctx,
                   const DeclContext *CurrDeclContext)
      : Sink(Sink), Ctx(Ctx),
        TypeResolver(createLazyResolver(Ctx)),
        CurrDeclContext(CurrDeclContext) {

    // Determine if we are doing code completion inside a static method.
    if (CurrDeclContext) {
      CurrentMethod = CurrDeclContext->getInnermostMethodContext();
      if (auto *FD = dyn_cast_or_null<FuncDecl>(CurrentMethod))
        InsideStaticMethod = FD->isStatic();
    }
  }

  void discardTypeResolver() {
    TypeResolver.reset();
  }

  void setHaveDot(SourceLoc DotLoc) {
    HaveDot = true;
    this->DotLoc = DotLoc;
  }

  void initializeArchetypeTransformer(DeclContext *DC, Type BaseTy) {
    TransformerPt = llvm::make_unique<ArchetypeTransformer>(DC, BaseTy);
  }

  void setIsStaticMetatype(bool value) {
    IsStaticMetatype = value;
  }

  void setExpectedTypes(ArrayRef<Type> Types) {
    ExpectedTypes = Types;
  }

  bool needDot() const {
    return NeedLeadingDot;
  }

  void setHaveLParen(bool Value) {
    HaveLParen = Value;
  }

  void setIsSuperRefExpr() {
    IsSuperRefExpr = true;
  }

  void setIsDynamicLookup() {
    IsDynamicLookup = true;
  }

  void addExpressionSpecificDecl(const Decl *D) {
    ExpressionSpecificDecls.insert(D);
  }

  void addImportModuleNames() {
    // FIXME: Add user-defined swift modules
    SmallVector<clang::Module*, 20> Modules;
    Ctx.getVisibleTopLevelClangeModules(Modules);
    std::sort(Modules.begin(), Modules.end(),
              [](clang::Module* LHS , clang::Module* RHS) {
                return LHS->getTopLevelModuleName().compare_lower(
                  RHS->getTopLevelModuleName()) < 0;
              });
    for (auto *M : Modules) {
      if (M->isAvailable() &&
          !M->getTopLevelModuleName().startswith("_") &&
          // Name hidden implies not imported yet, exactly what code completion
          // wants.
          M->NameVisibility == clang::Module::NameVisibilityKind::Hidden) {
        auto MD = ModuleDecl::create(Ctx.getIdentifier(M->getTopLevelModuleName()),
                                     Ctx);
        CodeCompletionResultBuilder Builder(Sink,
                                            CodeCompletionResult::ResultKind::
                                              Declaration,
                                            SemanticContextKind::OtherModule,
                                            ExpectedTypes);
        Builder.setAssociatedDecl(MD);
        Builder.addTextChunk(MD->getNameStr());
        Builder.addTypeAnnotation("Module");
      }
    }
  }

  SemanticContextKind getSemanticContext(const Decl *D,
                                         DeclVisibilityKind Reason) {
    if (ForcedSemanticContext)
      return *ForcedSemanticContext;

    switch (Reason) {
    case DeclVisibilityKind::LocalVariable:
    case DeclVisibilityKind::FunctionParameter:
    case DeclVisibilityKind::GenericParameter:
      if (ExpressionSpecificDecls.count(D))
        return SemanticContextKind::ExpressionSpecific;
      return SemanticContextKind::Local;

    case DeclVisibilityKind::MemberOfCurrentNominal:
      if (IsSuperRefExpr &&
          CurrentMethod && CurrentMethod->getOverriddenDecl() == D)
        return SemanticContextKind::ExpressionSpecific;
      return SemanticContextKind::CurrentNominal;

    case DeclVisibilityKind::MemberOfProtocolImplementedByCurrentNominal:
    case DeclVisibilityKind::MemberOfSuper:
      return SemanticContextKind::Super;

    case DeclVisibilityKind::MemberOfOutsideNominal:
      return SemanticContextKind::OutsideNominal;

    case DeclVisibilityKind::VisibleAtTopLevel:
      if (CurrDeclContext &&
          D->getModuleContext() == CurrDeclContext->getParentModule())
        return SemanticContextKind::CurrentModule;
      else
        return SemanticContextKind::OtherModule;

    case DeclVisibilityKind::DynamicLookup:
      // AnyObject results can come from different modules, including the
      // current module, but we always assign them the OtherModule semantic
      // context.  These declarations are uniqued by signature, so it is
      // totally random (determined by the hash function) which of the
      // equivalent declarations (across multiple modules) we will get.
      return SemanticContextKind::OtherModule;
    }
    llvm_unreachable("unhandled kind");
  }

  void addLeadingDot(CodeCompletionResultBuilder &Builder) {
    if (NeedOptionalUnwrap) {
      Builder.setNumBytesToErase(NumBytesToEraseForOptionalUnwrap);
      Builder.addQuestionMark();
      Builder.addLeadingDot();
      return;
    }
    if (needDot())
      Builder.addLeadingDot();
  }

  void addTypeAnnotation(CodeCompletionResultBuilder &Builder, Type T) {
    T = T->getReferenceStorageReferent();
    if (T->isVoid())
      Builder.addTypeAnnotation("Void");
    else
      Builder.addTypeAnnotation(T.getString());
  }

  static bool isBoringBoundGenericType(Type T) {
    BoundGenericType *BGT = T->getAs<BoundGenericType>();
    if (!BGT)
      return false;
    for (Type Arg : BGT->getGenericArgs()) {
      if (!Arg->is<GenericTypeParamType>())
        return false;
    }
    return true;
  }

  Type getTypeOfMember(const ValueDecl *VD) {
    if (ExprType) {
      Type ContextTy = VD->getDeclContext()->getDeclaredTypeOfContext();
      if (ContextTy) {
        Type MaybeNominalType = ExprType->getRValueInstanceType();
        if (ContextTy->getAnyNominal() == MaybeNominalType->getAnyNominal() &&
            !isBoringBoundGenericType(MaybeNominalType)) {
          if (Type T = MaybeNominalType->getTypeOfMember(
              CurrDeclContext->getParentModule(), VD, TypeResolver.get()))
            return TransformerPt ? T.transform(TransformerPt->getTransformerFunc()) :
                                   T;
        }
      }
    }
    return TransformerPt ? VD->getType().transform(TransformerPt->getTransformerFunc()) :
                           VD->getType();
  }

  const DeducedAssociatedTypes &
  getAssociatedTypeMap(const NominalTypeDecl *NTD) {
    {
      auto It = DeducedAssociatedTypeCache.find(NTD);
      if (It != DeducedAssociatedTypeCache.end())
        return It->second;
    }

    DeducedAssociatedTypes Types;
    for (auto Conformance : NTD->getAllConformances()) {
      if (!Conformance->isComplete())
        continue;
      Conformance->forEachTypeWitness(TypeResolver.get(),
                                      [&](const AssociatedTypeDecl *ATD,
                                          const Substitution &Subst,
                                          TypeDecl *TD) -> bool {
        Types[ATD] = Subst.getReplacement();
        return false;
      });
    }

    auto ItAndInserted = DeducedAssociatedTypeCache.insert({ NTD, Types });
    assert(ItAndInserted.second == true && "should not be in the map");
    return ItAndInserted.first->second;
  }

  Type getAssociatedTypeType(const AssociatedTypeDecl *ATD) {
    Type BaseTy = BaseType;
    if (!BaseTy)
      BaseTy = ExprType;
    if (!BaseTy && CurrDeclContext)
      BaseTy = CurrDeclContext->getInnermostTypeContext()
                   ->getDeclaredTypeInContext();
    if (BaseTy) {
      BaseTy = BaseTy->getRValueInstanceType();
      if (auto NTD = BaseTy->getAnyNominal()) {
        auto &Types = getAssociatedTypeMap(NTD);
        if (Type T = Types.lookup(ATD))
          return T;
      }
    }
    return Type();
  }

  void addVarDeclRef(const VarDecl *VD, DeclVisibilityKind Reason) {
    if (!VD->hasName())
      return;
    if (!VD->isUserAccessible())
      return;

    StringRef Name = VD->getName().get();
    assert(!Name.empty() && "name should not be empty");

    assert(VD->isStatic() ||
           !(InsideStaticMethod &&
            VD->getDeclContext() == CurrentMethod->getDeclContext()) &&
           "name lookup bug -- can not see an instance variable "
           "in a static function");

    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(VD, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(VD);
    addLeadingDot(Builder);
    Builder.addTextChunk(Name);

    // Add a type annotation.
    Type VarType = getTypeOfMember(VD);
    if (VD->getName() == Ctx.Id_self) {
      // Strip inout from 'self'.  It is useful to show inout for function
      // parameters.  But for 'self' it is just noise.
      VarType = VarType->getInOutObjectType();
    }
    if (IsDynamicLookup || VD->getAttrs().hasAttribute<OptionalAttr>()) {
      // Values of properties that were found on a AnyObject have
      // Optional<T> type.  Same applies to optional members.
      VarType = OptionalType::get(VarType);
    }
    addTypeAnnotation(Builder, VarType);
  }

  void addPatternParameters(CodeCompletionResultBuilder &Builder,
                            const Pattern *P) {
    if (auto *TP = dyn_cast<TuplePattern>(P)) {
      bool NeedComma = false;
      for (unsigned i = 0, end = TP->getNumElements(); i < end; ++i) {
        TuplePatternElt TupleElt = TP->getElement(i);
        if (NeedComma)
          Builder.addComma();
        NeedComma = true;

        bool HasEllipsis = TupleElt.hasEllipsis();
        Type EltT = TupleElt.getPattern()->getType();
        if (HasEllipsis)
          EltT = TupleTypeElt::getVarargBaseTy(EltT);

        Builder.addCallParameter(TupleElt.getPattern()->getBoundName(),
                                 EltT, HasEllipsis);
      }
      return;
    }

    Type PType = P->getType();
    if (auto Parens = dyn_cast<ParenType>(PType.getPointer()))
      PType = Parens->getUnderlyingType();
    Builder.addCallParameter(P->getBoundName(), PType, /*IsVarArg*/false);
  }

  void addPatternFromTypeImpl(CodeCompletionResultBuilder &Builder, Type T,
                              Identifier Label, bool IsTopLevel, bool IsVarArg) {
    if (auto *TT = T->getAs<TupleType>()) {
      if (!Label.empty()) {
        Builder.addTextChunk(Label.str());
        Builder.addTextChunk(": ");
      }
      if (!IsTopLevel || !HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();
      bool NeedComma = false;
      for (auto TupleElt : TT->getElements()) {
        if (NeedComma)
          Builder.addComma();
        Type EltT = TupleElt.isVararg() ? TupleElt.getVarargBaseTy()
                                        : TupleElt.getType();
        addPatternFromTypeImpl(Builder, EltT, TupleElt.getName(), false,
                               TupleElt.isVararg());
        NeedComma = true;
      }
      Builder.addRightParen();
      return;
    }
    if (auto *PT = dyn_cast<ParenType>(T.getPointer())) {
      if (IsTopLevel && !HaveLParen)
        Builder.addLeftParen();
      else if (IsTopLevel)
        Builder.addAnnotatedLeftParen();
      Builder.addCallParameter(Identifier(), PT->getUnderlyingType(),
                               /*IsVarArg*/false);
      if (IsTopLevel)
        Builder.addRightParen();
      return;
    }

    if (IsTopLevel && !HaveLParen)
      Builder.addLeftParen();
    else if (IsTopLevel)
      Builder.addAnnotatedLeftParen();

    Builder.addCallParameter(Label, T, IsVarArg);
    if (IsTopLevel)
      Builder.addRightParen();
  }

  void addPatternFromType(CodeCompletionResultBuilder &Builder, Type T) {
    addPatternFromTypeImpl(Builder, T, Identifier(), true, /*isVarArg*/false);
  }

  static bool hasInterestingDefaultValues(const AnyFunctionType *AFT) {
    if (auto *TT = dyn_cast<TupleType>(AFT->getInput().getPointer())) {
      for (const TupleTypeElt &EltT : TT->getElements()) {
        switch (EltT.getDefaultArgKind()) {
        case DefaultArgumentKind::Normal:
        case DefaultArgumentKind::Inherited: // FIXME: include this?
          return true;
        default:
          break;
        }
      }
    }
    return false;
  }

  void addParamPatternFromFunction(CodeCompletionResultBuilder &Builder,
                                   const AnyFunctionType *AFT,
                                   const AbstractFunctionDecl *AFD,
                                   bool includeDefaultArgs = true) {

    const TuplePattern *BodyTuple = nullptr;
    if (AFD) {
      auto BodyPatterns = AFD->getBodyParamPatterns();
      // Skip over the implicit 'self'.
      if (AFD->getImplicitSelfDecl()) {
        BodyPatterns = BodyPatterns.slice(1);
      }

      if (!BodyPatterns.empty())
        BodyTuple = dyn_cast<TuplePattern>(BodyPatterns.front());
    }

    // Do not desugar AFT->getInput(), as we want to treat (_: (a,b)) distinctly
    // from (a,b) for code-completion.
    if (auto *TT = dyn_cast<TupleType>(AFT->getInput().getPointer())) {
      bool NeedComma = false;
      // Iterate over the tuple type fields, corresponding to each parameter.
      for (unsigned i = 0, e = TT->getNumElements(); i != e; ++i) {
        const auto &TupleElt = TT->getElement(i);
        switch (TupleElt.getDefaultArgKind()) {
        case DefaultArgumentKind::None:
          break;

        case DefaultArgumentKind::Normal:
        case DefaultArgumentKind::Inherited:
          if (includeDefaultArgs)
            break;
          continue;

        case DefaultArgumentKind::File:
        case DefaultArgumentKind::Line:
        case DefaultArgumentKind::Column:
        case DefaultArgumentKind::Function:
        case DefaultArgumentKind::DSOHandle:
          // Skip parameters that are defaulted to source location or other
          // caller context information.  Users typically don't want to specify
          // these parameters.
          continue;
        }
        auto ParamType = TupleElt.isVararg() ? TupleElt.getVarargBaseTy()
                                             : TupleElt.getType();
        auto Name = TupleElt.getName();

        if (NeedComma)
          Builder.addComma();
        if (BodyTuple) {
          // If we have a local name for the parameter, pass in that as well.
          auto ParamPat = BodyTuple->getElement(i).getPattern();
          Builder.addCallParameter(Name, ParamPat->getBodyName(), ParamType,
                                   TupleElt.isVararg());
        } else {
          Builder.addCallParameter(Name, ParamType, TupleElt.isVararg());
        }
        NeedComma = true;
      }
    } else {
      // If it's not a tuple, it could be a unary function.
      Type T = AFT->getInput();
      if (auto *PT = dyn_cast<ParenType>(T.getPointer())) {
        // Only unwrap the paren sugar, if it exists.
        T = PT->getUnderlyingType();
      }
      if (BodyTuple) {
        auto ParamPat = BodyTuple->getElement(0).getPattern();
        Builder.addCallParameter(Identifier(), ParamPat->getBodyName(), T,
                                 /*IsVarArg*/false);
      } else
        Builder.addCallParameter(Identifier(), T, /*IsVarArg*/false);
    }
  }

  static void addThrows(CodeCompletionResultBuilder &Builder,
                        const AnyFunctionType *AFT,
                        const AbstractFunctionDecl *AFD) {
    if (AFD && AFD->getAttrs().hasAttribute<RethrowsAttr>())
      Builder.addAnnotatedRethrows();
    else if (AFT->throws())
      Builder.addAnnotatedThrows();
  }

  void addFunctionCallPattern(const AnyFunctionType *AFT,
                              const AbstractFunctionDecl *AFD = nullptr) {
    foundFunction(AFT);

    // Add the pattern, possibly including any default arguments.
    auto addPattern = [&](bool includeDefaultArgs = true) {
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Pattern,
          SemanticContextKind::ExpressionSpecific, ExpectedTypes);
      if (!HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();

      addParamPatternFromFunction(Builder, AFT, AFD, includeDefaultArgs);

      Builder.addRightParen();
      addThrows(Builder, AFT, AFD);

      addTypeAnnotation(Builder, AFT->getResult());
    };

    if (hasInterestingDefaultValues(AFT))
      addPattern(/*includeDefaultArgs*/ false);
    addPattern();
  }

  void addMethodCall(const FuncDecl *FD, DeclVisibilityKind Reason) {
    if (FD->getName().empty())
      return;
    foundFunction(FD);
    bool IsImplicitlyCurriedInstanceMethod;
    switch (Kind) {
    case LookupKind::ValueExpr:
      IsImplicitlyCurriedInstanceMethod = ExprType->is<AnyMetatypeType>() &&
                                          !FD->isStatic();
      break;
    case LookupKind::ValueInDeclContext:
      IsImplicitlyCurriedInstanceMethod =
          InsideStaticMethod &&
          FD->getDeclContext() == CurrentMethod->getDeclContext() &&
          !FD->isStatic();
      if (!IsImplicitlyCurriedInstanceMethod) {
        if (auto Init = dyn_cast<Initializer>(CurrDeclContext)) {
          IsImplicitlyCurriedInstanceMethod =
              FD->getDeclContext() == Init->getParent() &&
              !FD->isStatic();
        }
      }
      break;
    case LookupKind::EnumElement:
    case LookupKind::Type:
    case LookupKind::TypeInDeclContext:
      llvm_unreachable("can not have a method call while doing a "
                       "type completion");
    case LookupKind::ImportFromModule:
      IsImplicitlyCurriedInstanceMethod = false;
      break;
    }

    StringRef Name = FD->getName().get();
    assert(!Name.empty() && "name should not be empty");

    unsigned FirstIndex = 0;
    if (!IsImplicitlyCurriedInstanceMethod && FD->getImplicitSelfDecl())
      FirstIndex = 1;
    Type FunctionType = getTypeOfMember(FD);
    assert(FunctionType);
    if (FirstIndex != 0 && !FunctionType->is<ErrorType>())
      FunctionType = FunctionType->castTo<AnyFunctionType>()->getResult();

    // Add the method, possibly including any default arguments.
    auto addMethodImpl = [&](bool includeDefaultArgs = true) {
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Declaration,
          getSemanticContext(FD, Reason), ExpectedTypes);
      Builder.setAssociatedDecl(FD);
      addLeadingDot(Builder);
      Builder.addTextChunk(Name);
      if (IsDynamicLookup)
        Builder.addDynamicLookupMethodCallTail();
      else if (FD->getAttrs().hasAttribute<OptionalAttr>())
        Builder.addOptionalMethodCallTail();

      llvm::SmallString<32> TypeStr;

      if (FunctionType->is<ErrorType>()) {
        llvm::raw_svector_ostream OS(TypeStr);
        FunctionType.print(OS);
        Builder.addTypeAnnotation(OS.str());
        return;
      }

      Type FirstInputType = FunctionType->castTo<AnyFunctionType>()->getInput();

      if (IsImplicitlyCurriedInstanceMethod) {
        if (auto PT = dyn_cast<ParenType>(FirstInputType.getPointer()))
          FirstInputType = PT->getUnderlyingType();

        Builder.addLeftParen();
        Builder.addCallParameter(Ctx.Id_self, FirstInputType,
                                 /*IsVarArg*/ false);
        Builder.addRightParen();
      } else {
        Builder.addLeftParen();
        auto AFT = FunctionType->castTo<AnyFunctionType>();
        addParamPatternFromFunction(Builder, AFT, FD, includeDefaultArgs);
        Builder.addRightParen();
        addThrows(Builder, AFT, FD);
      }

      Type ResultType = FunctionType->castTo<AnyFunctionType>()->getResult();

      // Build type annotation.
      {
        llvm::raw_svector_ostream OS(TypeStr);
        for (unsigned i = FirstIndex + 1, e = FD->getBodyParamPatterns().size();
             i != e; ++i) {
          ResultType->castTo<AnyFunctionType>()->getInput()->print(OS);
          ResultType = ResultType->castTo<AnyFunctionType>()->getResult();
          OS << " -> ";
        }
        // What's left is the result type.
        if (ResultType->isVoid())
          OS << "Void";
        else
          ResultType.print(OS);
      }
      Builder.addTypeAnnotation(TypeStr);
    };

    if (!FunctionType->is<ErrorType>() &&
        hasInterestingDefaultValues(FunctionType->castTo<AnyFunctionType>())) {
      addMethodImpl(/*includeDefaultArgs*/ false);
    }
    addMethodImpl();
  }

  void addConstructorCall(const ConstructorDecl *CD, DeclVisibilityKind Reason,
                          Optional<Type> Result,
                          Identifier addName = Identifier()) {
    foundFunction(CD);
    Type MemberType = getTypeOfMember(CD);
    AnyFunctionType *ConstructorType = nullptr;
    if (!MemberType->is<ErrorType>())
      ConstructorType = MemberType->castTo<AnyFunctionType>()
                            ->getResult()
                            ->castTo<AnyFunctionType>();

    // Add the constructor, possibly including any default arguments.
    auto addConstructorImpl = [&](bool includeDefaultArgs = true) {
      CodeCompletionResultBuilder Builder(
          Sink, CodeCompletionResult::ResultKind::Declaration,
          getSemanticContext(CD, Reason), ExpectedTypes);
      Builder.setAssociatedDecl(CD);
      if (IsSuperRefExpr) {
        assert(addName.empty());
        assert(isa<ConstructorDecl>(CurrDeclContext) &&
               "can call super.init only inside a constructor");
        addLeadingDot(Builder);
        Builder.addTextChunk("init");
      } else if (!addName.empty()) {
        Builder.addTextChunk(addName.str());
      } else if (HaveDot &&
                 Reason == DeclVisibilityKind::MemberOfCurrentNominal) {

        // This case is querying the init function as member
        assert(addName.empty());
        Builder.addTextChunk("init");
      }

      if (MemberType->is<ErrorType>()) {
        addTypeAnnotation(Builder, MemberType);
        return;
      }
      assert(ConstructorType);

      if (!HaveLParen)
        Builder.addLeftParen();
      else
        Builder.addAnnotatedLeftParen();

      addParamPatternFromFunction(Builder, ConstructorType, CD,
                                  includeDefaultArgs);

      Builder.addRightParen();
      addThrows(Builder, ConstructorType, CD);

      addTypeAnnotation(Builder,
                        Result.hasValue() ? Result.getValue() :
                                            ConstructorType->getResult());
    };

    if (ConstructorType && hasInterestingDefaultValues(ConstructorType))
      addConstructorImpl(/*includeDefaultArgs*/ false);
    addConstructorImpl();
  }

  void addConstructorCallsForType(Type type, Identifier name,
                                  DeclVisibilityKind Reason) {
    if (!Ctx.LangOpts.CodeCompleteInitsInPostfixExpr)
      return;

    assert(CurrDeclContext);
    SmallVector<ValueDecl *, 16> initializers;
    if (CurrDeclContext->lookupQualified(type, Ctx.Id_init, NL_QualifiedDefault,
                                         TypeResolver.get(), initializers)) {
      for (auto *init : initializers) {
        if (init->isPrivateStdlibDecl(/*whitelistProtocols*/ false) ||
            AvailableAttr::isUnavailable(init))
          continue;
        addConstructorCall(cast<ConstructorDecl>(init), Reason, None, name);
      }
    }
  }

  void addSubscriptCall(const SubscriptDecl *SD, DeclVisibilityKind Reason) {
    assert(!HaveDot && "can not add a subscript after a dot");
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(SD, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(SD);
    Builder.addLeftBracket();
    addPatternParameters(Builder, SD->getIndices());
    Builder.addRightBracket();

    // Add a type annotation.
    Type T = SD->getElementType();
    if (IsDynamicLookup) {
      // Values of properties that were found on a AnyObject have
      // Optional<T> type.
      T = OptionalType::get(T);
    }
    addTypeAnnotation(Builder, T);
  }

  void addNominalTypeRef(const NominalTypeDecl *NTD,
                         DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(NTD, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(NTD);
    addLeadingDot(Builder);
    Builder.addTextChunk(NTD->getName().str());
    addTypeAnnotation(Builder, NTD->getDeclaredType());
  }

  void addTypeAliasRef(const TypeAliasDecl *TAD, DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(TAD, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(TAD);
    addLeadingDot(Builder);
    Builder.addTextChunk(TAD->getName().str());
    if (TAD->hasUnderlyingType() && !TAD->getUnderlyingType()->is<ErrorType>())
      addTypeAnnotation(Builder, TAD->getUnderlyingType());
    else {
      addTypeAnnotation(Builder, TAD->getDeclaredType());
    }
  }

  void addGenericTypeParamRef(const GenericTypeParamDecl *GP,
                              DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(GP, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(GP);
    addLeadingDot(Builder);
    Builder.addTextChunk(GP->getName().str());
    addTypeAnnotation(Builder, GP->getDeclaredType());
  }

  void addAssociatedTypeRef(const AssociatedTypeDecl *AT,
                            DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(AT, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(AT);
    addLeadingDot(Builder);
    Builder.addTextChunk(AT->getName().str());
    if (Type T = getAssociatedTypeType(AT))
      addTypeAnnotation(Builder, T);
  }

  void addEnumElementRef(const EnumElementDecl *EED,
                         DeclVisibilityKind Reason,
                         bool HasTypeContext) {
    if (!EED->hasName())
      return;

    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        HasTypeContext ? SemanticContextKind::ExpressionSpecific
                       : getSemanticContext(EED, Reason), ExpectedTypes);
    Builder.setAssociatedDecl(EED);
    addLeadingDot(Builder);
    Builder.addTextChunk(EED->getName().str());
    if (EED->hasArgumentType())
      addPatternFromType(Builder, EED->getArgumentType());
    Type EnumType = EED->getType();

    // Enum element is of function type such as EnumName.type -> Int ->
    // EnumName; however we should show Int -> EnumName as the type
    if (auto FuncType = EED->getType()->getAs<AnyFunctionType>()) {
      EnumType = FuncType->getResult();
    }
    addTypeAnnotation(Builder, EnumType);
  }

  void addKeyword(StringRef Name, Type TypeAnnotation,
                  SemanticContextKind SK = SemanticContextKind::None) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword, SK, ExpectedTypes);
    addLeadingDot(Builder);
    Builder.addTextChunk(Name);
    if (!TypeAnnotation.isNull())
      addTypeAnnotation(Builder, TypeAnnotation);
  }

  void addKeyword(StringRef Name, StringRef TypeAnnotation) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, ExpectedTypes);
    addLeadingDot(Builder);
    Builder.addTextChunk(Name);
    if (!TypeAnnotation.empty())
      Builder.addTypeAnnotation(TypeAnnotation);
  }

  void addDeclAttrParamKeyword(StringRef Name, StringRef Annotation,
                             bool NeedSpecify) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, ExpectedTypes);
    Builder.addDeclAttrParamKeyword(Name, Annotation, NeedSpecify);
  }

  void addDeclAttrKeyword(StringRef Name, StringRef Annotation) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, ExpectedTypes);
    Builder.addDeclAttrKeyword(Name, Annotation);
  }

  // Implement swift::VisibleDeclConsumer.
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason) override {
    // Hide private stdlib declarations.
    if (D->isPrivateStdlibDecl(/*whitelistProtocols*/false))
      return;
    if (AvailableAttr::isUnavailable(D))
      return;

    // Hide editor placeholders.
    if (D->getName().isEditorPlaceholder())
      return;

    if (!D->hasType())
      TypeResolver->resolveDeclSignature(D);
    else if (isa<TypeAliasDecl>(D)) {
      // A TypeAliasDecl might have type set, but not the underlying type.
      TypeResolver->resolveDeclSignature(D);
    }

    switch (Kind) {
    case LookupKind::ValueExpr:
      if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
        if (auto MT = ExprType->getRValueType()->getAs<AnyMetatypeType>()) {
          if (HaveDot) {
            Type Ty;
            for (Ty = MT; Ty && Ty->is<AnyMetatypeType>();
                 Ty = Ty->getAs<AnyMetatypeType>()->getInstanceType());
            assert(Ty && "Cannot find instance type.");

            // Add init() as member of the metatype.
            if (Reason == DeclVisibilityKind::MemberOfCurrentNominal) {
              if (IsStaticMetatype || CD->isRequired() ||
                  !Ty->is<ClassType>())
                addConstructorCall(CD, Reason, None);
            }
            return;
          }
        }

        if (auto MT = ExprType->getAs<AnyMetatypeType>()) {
          if (HaveDot)
            return;

          // If instance type is type alias, showing users that the contructed
          // type is the typealias instead of the underlying type of the alias.
          Optional<Type> Result = None;
          if (auto AT = MT->getInstanceType()) {
            if (AT->getKind() == TypeKind::NameAlias &&
                AT->getDesugaredType() == CD->getResultType().getPointer())
              Result = AT;
          }
          addConstructorCall(CD, Reason, Result);
        }
        if (IsSuperRefExpr) {
          if (!isa<ConstructorDecl>(CurrDeclContext))
            return;
          addConstructorCall(CD, Reason, None);
        }
        return;
      }

      if (HaveLParen)
        return;

      if (auto *VD = dyn_cast<VarDecl>(D)) {
        addVarDeclRef(VD, Reason);
        return;
      }

      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        // We can not call operators with a postfix parenthesis syntax.
        if (FD->isBinaryOperator() || FD->isUnaryOperator())
          return;

        // We can not call accessors.  We use VarDecls and SubscriptDecls to
        // produce completions that refer to getters and setters.
        if (FD->isAccessor())
          return;

        addMethodCall(FD, Reason);
        return;
      }

      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason);
        addConstructorCallsForType(NTD->getType(), NTD->getName(), Reason);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason);
        addConstructorCallsForType(TAD->getUnderlyingType(), TAD->getName(),
                                   Reason);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason);
        for (auto *protocol : GP->getConformingProtocols(nullptr))
          addConstructorCallsForType(protocol->getType(), GP->getName(),
                                     Reason);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason);
        return;
      }

      if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
        addEnumElementRef(EED, Reason, /*HasTypeContext=*/false);
      }

      if (HaveDot)
        return;

      if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
        if (ExprType->is<AnyMetatypeType>())
          return;
        addSubscriptCall(SD, Reason);
        return;
      }
      return;

    case LookupKind::ValueInDeclContext:
    case LookupKind::ImportFromModule:
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        addVarDeclRef(VD, Reason);
        return;
      }

      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        // We can not call operators with a postfix parenthesis syntax.
        if (FD->isBinaryOperator() || FD->isUnaryOperator())
          return;

        // We can not call accessors.  We use VarDecls and SubscriptDecls to
        // produce completions that refer to getters and setters.
        if (FD->isAccessor())
          return;

        addMethodCall(FD, Reason);
        return;
      }

      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason);
        addConstructorCallsForType(NTD->getType(), NTD->getName(), Reason);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason);
        addConstructorCallsForType(TAD->getUnderlyingType(), TAD->getName(),
                                   Reason);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason);
        for (auto *protocol : GP->getConformingProtocols(nullptr))
          addConstructorCallsForType(protocol->getType(), GP->getName(),
                                     Reason);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason);
        return;
      }

      return;

    case LookupKind::EnumElement:
      handleEnumElement(D, Reason);
      return;

    case LookupKind::Type:
    case LookupKind::TypeInDeclContext:
      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason);
        return;
      }

      return;
    }
  }

  bool handleEnumElement(Decl *D, DeclVisibilityKind Reason) {
    if (auto *EED = dyn_cast<EnumElementDecl>(D)) {
      addEnumElementRef(EED, Reason, /*HasTypeContext=*/true);
      return true;
    } else if (auto *ED = dyn_cast<EnumDecl>(D)) {
      llvm::DenseSet<EnumElementDecl *> Elements;
      ED->getAllElements(Elements);
      for (auto *Ele : Elements) {
        addEnumElementRef(Ele, Reason, /*HasTypeContext=*/true);
      }
      return true;
    }
    return false;
  }

  void handleOptionSetType(Decl *D, DeclVisibilityKind Reason) {
    if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
      if (isOptionSetTypeDecl(NTD)) {
        for (auto M : NTD->getMembers()) {
          if (auto *VD = dyn_cast<VarDecl>(M)) {
            if (isOptionSetType(VD->getType()) && VD->isStatic()) {
              addVarDeclRef(VD, Reason);
            }
          }
        }
      }
    }
  }

  bool isOptionSetTypeDecl(NominalTypeDecl *D) {
    auto optionSetType = dyn_cast<ProtocolDecl>(Ctx.getOptionSetTypeDecl());
    if (!optionSetType)
      return false;

    SmallVector<ProtocolConformance *, 1> conformances;
    return D->lookupConformance(CurrDeclContext->getParentModule(),
                                optionSetType, conformances);
  }

  bool isOptionSetType(Type Ty) {
    return Ty &&
           Ty->getNominalOrBoundGenericNominal() &&
           isOptionSetTypeDecl(Ty->getNominalOrBoundGenericNominal());
  }

  void getTupleExprCompletions(TupleType *ExprType) {
    unsigned Index = 0;
    for (auto TupleElt : ExprType->getElements()) {
      CodeCompletionResultBuilder Builder(
          Sink,
          CodeCompletionResult::ResultKind::Pattern,
          SemanticContextKind::CurrentNominal, ExpectedTypes);
      addLeadingDot(Builder);
      if (TupleElt.hasName()) {
        Builder.addTextChunk(TupleElt.getName().str());
      } else {
        llvm::SmallString<4> IndexStr;
        {
          llvm::raw_svector_ostream OS(IndexStr);
          OS << Index;
        }
        Builder.addTextChunk(IndexStr.str());
      }
      addTypeAnnotation(Builder, TupleElt.getType());
      Index++;
    }
  }

  bool tryFunctionCallCompletions(Type ExprType, const ValueDecl *VD) {
    ExprType = ExprType->getRValueType();
    if (auto AFT = ExprType->getAs<AnyFunctionType>()) {
      if (auto *AFD = dyn_cast_or_null<AbstractFunctionDecl>(VD)) {
        addFunctionCallPattern(AFT, AFD);
      } else {
        addFunctionCallPattern(AFT);
      }
      return true;
    }
    return false;
  }

  bool tryStdlibOptionalCompletions(Type ExprType) {
    // FIXME: consider types convertible to T?.

    ExprType = ExprType->getRValueType();
    if (Type Unwrapped = ExprType->getOptionalObjectType()) {
      llvm::SaveAndRestore<bool> ChangeNeedOptionalUnwrap(NeedOptionalUnwrap,
                                                          true);
      if (DotLoc.isValid()) {
        NumBytesToEraseForOptionalUnwrap = Ctx.SourceMgr.getByteDistance(
            DotLoc, Ctx.SourceMgr.getCodeCompletionLoc());
      } else {
        NumBytesToEraseForOptionalUnwrap = 0;
      }
      if (NumBytesToEraseForOptionalUnwrap <=
          CodeCompletionResult::MaxNumBytesToErase)
        lookupVisibleMemberDecls(*this, Unwrapped, CurrDeclContext,
                                 TypeResolver.get());
    } else if (Type Unwrapped = ExprType->getImplicitlyUnwrappedOptionalObjectType()) {
      lookupVisibleMemberDecls(*this, Unwrapped, CurrDeclContext,
                               TypeResolver.get());
    } else {
      return false;
    }

    // Ignore the internal members of Optional, like getLogicValue() and
    // _getMirror().
    // These are not commonly used and cause noise and confusion when showing
    // among the the members of the underlying type. If someone really wants to
    // use them they can write them directly.

    return true;
  }

  void getValueExprCompletions(Type ExprType, ValueDecl *VD = nullptr) {
    Kind = LookupKind::ValueExpr;
    NeedLeadingDot = !HaveDot;
    this->ExprType = ExprType;
    bool Done = false;
    if (tryFunctionCallCompletions(ExprType, VD))
      Done = true;
    if (auto MT = ExprType->getAs<ModuleType>()) {
      Module *M = MT->getModule();
      if (CurrDeclContext->getParentModule() != M) {
        // Only use the cache if it is not the current module.
        RequestedCachedResults = RequestedResultsTy::fromModule(M)
                                                     .needLeadingDot(needDot());
        Done = true;
      }
    }
    if (auto *TT = ExprType->getRValueType()->getAs<TupleType>()) {
      getTupleExprCompletions(TT);
      Done = true;
    }
    tryStdlibOptionalCompletions(ExprType);
    if (!Done) {
      lookupVisibleMemberDecls(*this, ExprType, CurrDeclContext,
                               TypeResolver.get());
    }
  }

  struct FilteredDeclConsumer : public swift::VisibleDeclConsumer {
    swift::VisibleDeclConsumer &Consumer;
    DeclFilter Filter;
    FilteredDeclConsumer(swift::VisibleDeclConsumer &Consumer,
                         DeclFilter Filter) : Consumer(Consumer), Filter(Filter) {}
    void foundDecl(ValueDecl *VD, DeclVisibilityKind Kind) override {
      if (Filter(VD, Kind))
        Consumer.foundDecl(VD, Kind);
    }
  };

  void getValueCompletionsInDeclContext(SourceLoc Loc,
                                        DeclFilter Filter = DefaultFilter,
                                        bool IncludeTopLevel = false,
                                        bool RequestCache = true) {
    Kind = LookupKind::ValueInDeclContext;
    NeedLeadingDot = false;
    FilteredDeclConsumer Consumer(*this, Filter);
    lookupVisibleDecls(Consumer, CurrDeclContext, TypeResolver.get(),
                       /*IncludeTopLevel=*/IncludeTopLevel, Loc);
    if (RequestCache)
      RequestedCachedResults = RequestedResultsTy::toplevelResults();
  }

  struct LookupByName : public swift::VisibleDeclConsumer {
    CompletionLookup &Lookup;
    std::vector<std::string> &SortedNames;
    llvm::SmallPtrSet<Decl*, 3> HandledDecls;

    bool isNameHit(StringRef Name) {
      return std::binary_search(SortedNames.begin(), SortedNames.end(), Name);
    }

    void collectEnumElementTypes(EnumElementDecl *EED) {
      if (isNameHit(EED->getNameStr()) && EED->getType()) {
        unboxType(EED->getType());
      }
    }

    void unboxType(Type T) {
      if (T->getKind() == TypeKind::Paren) {
        unboxType(T->getDesugaredType());
      } else if (T->getKind() == TypeKind::Tuple) {
        for (auto Ele : T->getAs<TupleType>()->getElements()) {
          unboxType(Ele.getType());
        }
      } else if (auto FT = T->getAs<FunctionType>()) {
        unboxType(FT->getInput());
        unboxType(FT->getResult());
      } else if (auto NTD = T->getNominalOrBoundGenericNominal()){
        if (HandledDecls.count(NTD) == 0) {
          auto Reason = DeclVisibilityKind::MemberOfCurrentNominal;
          if (!Lookup.handleEnumElement(NTD, Reason)) {
            Lookup.handleOptionSetType(NTD, Reason);
          }
          HandledDecls.insert(NTD);
        }
      }
    }

    LookupByName(CompletionLookup &Lookup, std::vector<std::string> &SortedNames) :
                   Lookup(Lookup), SortedNames(SortedNames) {
      std::sort(SortedNames.begin(), SortedNames.end());
    }

    void handleDeclRange(const DeclRange &Members,
                         DeclVisibilityKind Reason) {
      for (auto M : Members) {
        if (auto VD = dyn_cast<ValueDecl>(M)) {
          foundDecl(VD, Reason);
        }
      }
    }

    void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason) override {
      if (auto NTD = dyn_cast<NominalTypeDecl>(VD)) {
        if (isNameHit(NTD->getNameStr())) {
          unboxType(NTD->getDeclaredType());
        }
        handleDeclRange(NTD->getMembers(), Reason);
        for (auto Ex : NTD->getExtensions()) {
          handleDeclRange(Ex->getMembers(), Reason);
        }
      } else if (isNameHit(VD->getNameStr())) {
        unboxType(VD->getType());
      }
    }
  };

  void getUnresolvedMemberCompletions(SourceLoc Loc, SmallVectorImpl<Type> &Types) {
    NeedLeadingDot = !HaveDot;
    for (auto T : Types) {
      if (T && T->getNominalOrBoundGenericNominal()) {
        auto Reason = DeclVisibilityKind::MemberOfCurrentNominal;
        if (!handleEnumElement(T->getNominalOrBoundGenericNominal(), Reason)) {
          handleOptionSetType(T->getNominalOrBoundGenericNominal(), Reason);
        }
      }
    }
  }

  void getUnresolvedMemberCompletions(SourceLoc Loc,
                                      std::vector<std::string> &FuncNames,
                                      bool HasReturn) {
    NeedLeadingDot = !HaveDot;
    LookupByName Lookup(*this, FuncNames);
    lookupVisibleDecls(Lookup, CurrDeclContext, TypeResolver.get(), true);
    if (!HasReturn)
      return;
    if (auto FD = dyn_cast_or_null<AbstractFunctionDecl>(
        CurrDeclContext->getInnermostMethodContext())) {
      Lookup.unboxType(FD->getType());
    }
  }

  static bool getPositionInTupleExpr(DeclContext &DC, Expr *Target,
                                     TupleExpr *Tuple, unsigned &Pos,
                                     bool &HasName,
                                     llvm::SmallVectorImpl<Type> &TupleEleTypes) {
    auto &SM = DC.getASTContext().SourceMgr;
    Pos = 0;
    for (auto E : Tuple->getElements()) {
      if (SM.isBeforeInBuffer(E->getEndLoc(), Target->getStartLoc())) {
        TupleEleTypes.push_back(E->getType());
        Pos ++;
        continue;
      }
      HasName = !Tuple->getElementName(Pos).empty();
      return true;
    }
    return false;
  }

  void addArgNameCompletionResults(ArrayRef<StringRef> Names) {
    for (auto Name : Names) {
      CodeCompletionResultBuilder Builder(Sink,
                                          CodeCompletionResult::ResultKind::Keyword,
                                          SemanticContextKind::ExpressionSpecific, {});
      Builder.addTextChunk(Name);
      Builder.addCallParameterColon();
      Builder.addTypeAnnotation("Argument name");
    }
  }

  static void collectArgumentExpection(unsigned Position, bool HasName,
                                       ArrayRef<Type> Types, SourceLoc Loc,
                                       std::vector<Type> &ExpectedTypes,
                                       std::vector<StringRef> &ExpectedNames) {
    for (auto Type : Types) {
      if (auto TT = Type->getAs<TupleType>()) {
        if (Position >= TT->getElements().size()) {
          continue;
        }
        auto Ele = TT->getElement(Position);
        if (Ele.hasName() && !HasName) {
          ExpectedNames.push_back(Ele.getName().str());
        } else {
          ExpectedTypes.push_back(Ele.getType());
        }
      }
    }
  }

  bool lookupArgCompletionsAtPosition(unsigned Position, bool HasName,
                                      ArrayRef<Type> Types, SourceLoc Loc) {
    std::vector<Type> ExpectedTypes;
    std::vector<StringRef> ExpectedNames;
    collectArgumentExpection(Position, HasName, Types, Loc, ExpectedTypes,
                             ExpectedNames);
    addArgNameCompletionResults(ExpectedNames);
    if (!ExpectedTypes.empty()) {
      setExpectedTypes(ExpectedTypes);
      getValueCompletionsInDeclContext(Loc, DefaultFilter,
                                       /*IncludeTopLevel*/true,
                                       /*RequestCache*/false);
    }
    return true;
  }

  static bool isPotentialSignatureMatch(ArrayRef<Type> TupleEles,
                                        ArrayRef<Type> ExprTypes,
                                        DeclContext *DC) {
    // Not likely to be a mactch if users provide more arguments than expected.
    if (ExprTypes.size() >= TupleEles.size())
      return false;
    for (unsigned I = 0; I < ExprTypes.size(); ++ I) {
      auto Ty = ExprTypes[I];
      if (Ty && !Ty->is<ErrorType>()) {
        if (!isConvertibleTo(Ty, TupleEles[I], DC)) {
          return false;
        }
      }
    }
    return true;
  }

  static void removeUnlikelyOverloads(SmallVectorImpl<Type> &PossibleArgTypes,
                                      ArrayRef<Type> TupleEleTypes,
                                      DeclContext *DC) {
    for (auto It = PossibleArgTypes.begin(); It != PossibleArgTypes.end(); ) {
      llvm::SmallVector<Type, 3> ExpectedTypes;
      if ((*It)->getKind() == TypeKind::Tuple) {
        auto Elements = (*It)->getAs<TupleType>()->getElements();
        for (auto Ele : Elements)
          ExpectedTypes.push_back(Ele.getType());
      } else {
        ExpectedTypes.push_back(*It);
      }
      if (isPotentialSignatureMatch(ExpectedTypes, TupleEleTypes, DC)) {
        ++ It;
      } else {
        PossibleArgTypes.erase(It);
      }
    }
  }

  static bool collectPossibleArgTypes(DeclContext &DC, CallExpr *CallE, Expr *CCExpr,
                                      SmallVectorImpl<Type> &PossibleTypes,
                                      unsigned &Position, bool &HasName,
                                      bool RemoveUnlikelyOverloads) {
    if (auto Ty = CallE->getFn()->getType()) {
      if (auto FT = Ty->getAs<FunctionType>()) {
        PossibleTypes.push_back(FT->getInput());
      }
    }
    auto TAG = dyn_cast<TupleExpr>(CallE->getArg());
    if (!TAG)
      return false;
    llvm::SmallVector<Type, 3> TupleEleTypesBeforeTarget;
    if (!getPositionInTupleExpr(DC, CCExpr, TAG, Position, HasName,
                                TupleEleTypesBeforeTarget))
      return false;
    if (PossibleTypes.empty() &&
        !typeCheckUnresolvedExpr(DC, CallE->getArg(), CallE, PossibleTypes))
      return false;
    if (RemoveUnlikelyOverloads)
      removeUnlikelyOverloads(PossibleTypes, TupleEleTypesBeforeTarget, &DC);
    return true;
  }

  static bool collectArgumentExpectatation(DeclContext &DC, CallExpr *CallE,
                                           Expr *CCExpr,
                                           std::vector<Type> &ExpectedTypes) {
    SmallVector<Type, 2> PossibleTypes;
    unsigned Position;
    bool HasName;
    std::vector<StringRef> ExpectedNames;
    if (collectPossibleArgTypes(DC, CallE, CCExpr, PossibleTypes, Position,
                                HasName, true)) {
      collectArgumentExpection(Position, HasName, PossibleTypes,
                               CCExpr->getStartLoc(), ExpectedTypes, ExpectedNames);
      return !ExpectedTypes.empty();
    }
    return false;
  }

  bool getCallArgCompletions(DeclContext &DC, CallExpr *CallE, Expr *CCExpr) {
    SmallVector<Type, 2> PossibleTypes;
    unsigned Position;
    bool HasName;
    return collectPossibleArgTypes(DC, CallE, CCExpr, PossibleTypes, Position,
                                   HasName, true) &&
           lookupArgCompletionsAtPosition(Position, HasName, PossibleTypes,
                                          CCExpr->getStartLoc());
  }

  void getTypeContextEnumElementCompletions(SourceLoc Loc) {
    llvm::SaveAndRestore<LookupKind> ChangeLookupKind(
        Kind, LookupKind::EnumElement);
    NeedLeadingDot = !HaveDot;

    const DeclContext *FunctionDC = CurrDeclContext;
    const AbstractFunctionDecl *CurrentFunction = nullptr;
    while (FunctionDC->isLocalContext()) {
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(FunctionDC)) {
        CurrentFunction = AFD;
        break;
      }
      FunctionDC = FunctionDC->getParent();
    }
    if (!CurrentFunction)
      return;

    auto *Switch = cast_or_null<SwitchStmt>(
        findNearestStmt(CurrentFunction, Loc, StmtKind::Switch));
    if (!Switch)
      return;
    auto Ty = Switch->getSubjectExpr()->getType();
    if (!Ty)
      return;
    auto *TheEnumDecl = dyn_cast_or_null<EnumDecl>(Ty->getAnyNominal());
    if (!TheEnumDecl)
      return;
    for (auto Element : TheEnumDecl->getAllElements()) {
      foundDecl(Element, DeclVisibilityKind::MemberOfCurrentNominal);
    }
  }

  void getTypeCompletions(Type BaseType) {
    Kind = LookupKind::Type;
    this->BaseType = BaseType;
    NeedLeadingDot = !HaveDot;
    Type MetaBase = MetatypeType::get(BaseType);
    lookupVisibleMemberDecls(*this, MetaBase,
                             CurrDeclContext, TypeResolver.get());
    addKeyword("Type", MetaBase);
    addKeyword("self", BaseType, SemanticContextKind::CurrentNominal);
  }

  void getAttributeDeclCompletions(bool IsInSil, Optional<DeclKind> DK) {
    // FIXME: also include user-defined attribute keywords
    StringRef TargetName = "Declaration";
    if (DK.hasValue()) {
      switch (DK.getValue()) {
#define DECL(Id, ...)                                                         \
      case DeclKind::Id:                                                      \
        TargetName = #Id;                                                     \
        break;
#include "swift/AST/DeclNodes.def"
      }
    }
    std::string Description = TargetName.str() + " Attribute";
#define DECL_ATTR(KEYWORD, NAME, ...)                                         \
    if (!DeclAttribute::isUserInaccessible(DAK_##NAME) &&                     \
        !DeclAttribute::isDeclModifier(DAK_##NAME) &&                         \
        !DeclAttribute::shouldBeRejectedByParser(DAK_##NAME) &&               \
        (!DeclAttribute::isSilOnly(DAK_##NAME) || IsInSil)) {                 \
          if (!DK.hasValue() || DeclAttribute::canAttributeAppearOnDeclKind   \
            (DAK_##NAME, DK.getValue()))                                      \
              addDeclAttrKeyword(#KEYWORD, Description);                      \
    }
#include "swift/AST/Attr.def"
  }

  void getAttributeDeclParamCompletions(DeclAttrKind AttrKind, int ParamIndex) {
    if (AttrKind == DAK_Available) {
      if (ParamIndex == 0) {
        addDeclAttrParamKeyword("*", "Platform", false);
#define AVAILABILITY_PLATFORM(X, PrettyName)                                  \
        addDeclAttrParamKeyword(#X, "Platform", false);
#include "swift/AST/PlatformKinds.def"
      } else {
        addDeclAttrParamKeyword("unavailable", "", false);
        addDeclAttrParamKeyword("message", "Specify message", true);
        addDeclAttrParamKeyword("renamed", "Specify replacing name", true);
        addDeclAttrParamKeyword("introduced", "Specify version number", true);
        addDeclAttrParamKeyword("deprecated", "Specify version number", true);
      }
    }
  }

  void getPoundAvailablePlatformCompletions() {

    // The platform names should be identical to those in @available.
    getAttributeDeclParamCompletions(DAK_Available, 0);
  }

  void getTypeCompletionsInDeclContext(SourceLoc Loc) {
    Kind = LookupKind::TypeInDeclContext;
    lookupVisibleDecls(*this, CurrDeclContext, TypeResolver.get(),
                       /*IncludeTopLevel=*/false, Loc);

    RequestedCachedResults =
        RequestedResultsTy::toplevelResults().onlyTypes();
  }

  void getToplevelCompletions(bool OnlyTypes) {
    Kind = OnlyTypes ? LookupKind::TypeInDeclContext
                     : LookupKind::ValueInDeclContext;
    NeedLeadingDot = false;
    Module *M = CurrDeclContext->getParentModule();
    AccessFilteringDeclConsumer FilteringConsumer(CurrDeclContext, *this,
                                                  TypeResolver.get());
    M->lookupVisibleDecls({}, FilteringConsumer, NLKind::UnqualifiedLookup);
  }

  void getVisibleDeclsOfModule(const Module *TheModule,
                               ArrayRef<std::string> AccessPath,
                               bool ResultsHaveLeadingDot) {
    Kind = LookupKind::ImportFromModule;
    NeedLeadingDot = ResultsHaveLeadingDot;

    llvm::SmallVector<std::pair<Identifier, SourceLoc>, 1> LookupAccessPath;
    for (auto Piece : AccessPath) {
      LookupAccessPath.push_back(
          std::make_pair(Ctx.getIdentifier(Piece), SourceLoc()));
    }
    AccessFilteringDeclConsumer FilteringConsumer(CurrDeclContext, *this,
                                                  TypeResolver.get());
    TheModule->lookupVisibleDecls(LookupAccessPath, FilteringConsumer,
                                  NLKind::UnqualifiedLookup);
  }
};

class CompletionOverrideLookup : public swift::VisibleDeclConsumer {
  CodeCompletionResultSink &Sink;
  OwnedResolver TypeResolver;
  const DeclContext *CurrDeclContext;
  SmallVectorImpl<StringRef> &ParsedKeywords;

public:
  CompletionOverrideLookup(CodeCompletionResultSink &Sink,
                           ASTContext &Ctx,
                           const DeclContext *CurrDeclContext,
                           SmallVectorImpl<StringRef> &ParsedKeywords)
      : Sink(Sink), TypeResolver(createLazyResolver(Ctx)),
        CurrDeclContext(CurrDeclContext),
        ParsedKeywords(ParsedKeywords){}

  bool isKeywordSpecified(StringRef Word) {
    return std::find(ParsedKeywords.begin(), ParsedKeywords.end(), Word)
      != ParsedKeywords.end();
  }

  void addMethodOverride(const FuncDecl *FD, DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setAssociatedDecl(FD);

    class DeclNameOffsetLocatorPrinter : public StreamPrinter {
    public:
      using StreamPrinter::StreamPrinter;

      Optional<unsigned> NameOffset;

      void printDeclLoc(const Decl *D) override {
        if (!NameOffset.hasValue())
          NameOffset = OS.tell();
      }
    };

    llvm::SmallString<256> DeclStr;
    unsigned NameOffset = 0;
    {
      llvm::raw_svector_ostream OS(DeclStr);
      DeclNameOffsetLocatorPrinter Printer(OS);
      PrintOptions Options;
      Options.PrintDefaultParameterPlaceholder = false;
      Options.PrintImplicitAttrs = false;
      Options.ExclusiveAttrList.push_back(DAK_NoReturn);
      Options.PrintOverrideKeyword = false;
      FD->print(Printer, Options);
      NameOffset = Printer.NameOffset.getValue();
    }

    Accessibility AccessibilityOfContext;
    if (auto *NTD = dyn_cast<NominalTypeDecl>(CurrDeclContext))
      AccessibilityOfContext = NTD->getFormalAccess();
    else
      AccessibilityOfContext = cast<ExtensionDecl>(CurrDeclContext)
                                   ->getExtendedType()
                                   ->getAnyNominal()
                                   ->getFormalAccess();
    // If the developer has not input "func", we need to add necessary keywords
    if (!isKeywordSpecified("func")) {
      if (!isKeywordSpecified("private") &&
          !isKeywordSpecified("public") &&
          !isKeywordSpecified("internal"))
        Builder.addAccessControlKeyword(std::min(FD->getFormalAccess(),
                                                 AccessibilityOfContext));

      if (Reason == DeclVisibilityKind::MemberOfSuper &&
          !isKeywordSpecified("override"))
        Builder.addOverrideKeyword();
      Builder.addDeclIntroducer(DeclStr.str().substr(0, NameOffset));
    }
    Builder.addTextChunk(DeclStr.str().substr(NameOffset));
    Builder.addBraceStmtWithCursor();
  }

  void addConstructor(const ConstructorDecl *CD) {
    CodeCompletionResultBuilder Builder(
        Sink,
        CodeCompletionResult::ResultKind::Declaration,
        SemanticContextKind::Super, {});
    Builder.setAssociatedDecl(CD);

    llvm::SmallString<256> DeclStr;
    {
      llvm::raw_svector_ostream OS(DeclStr);
      PrintOptions Options;
      Options.PrintImplicitAttrs = false;
      Options.ExclusiveAttrList.push_back(DAK_NoReturn);
      Options.PrintDefaultParameterPlaceholder = false;
      CD->print(OS, Options);
    }
    Builder.addTextChunk(DeclStr);
    Builder.addBraceStmtWithCursor();
  }

  // Implement swift::VisibleDeclConsumer.
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason) override {
    if (Reason == DeclVisibilityKind::MemberOfCurrentNominal)
      return;

    if (D->getAttrs().hasAttribute<FinalAttr>())
      return;

    if (!D->hasType())
      TypeResolver->resolveDeclSignature(D);

    if (auto *FD = dyn_cast<FuncDecl>(D)) {
      // We can override operators as members.
      if (FD->isBinaryOperator() || FD->isUnaryOperator())
        return;

      // We can not override individual accessors.
      if (FD->isAccessor())
        return;

      addMethodOverride(FD, Reason);
      return;
    }

    if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
      if (!isa<ProtocolDecl>(CD->getDeclContext()))
        return;
      if (CD->isRequired() || CD->isDesignatedInit())
        addConstructor(CD);
      return;
    }
  }

  void addDesignatedInitializers(Type CurrTy) {
    if (!CurrTy)
      return;
    const auto *NTD = CurrTy->getAnyNominal();
    if (!NTD)
      return;
    const auto *CD = dyn_cast<ClassDecl>(NTD);
    if (!CD)
      return;
    if (!CD->getSuperclass())
      return;
    CD = CD->getSuperclass()->getClassOrBoundGenericClass();
    for (const auto *Member : CD->getMembers()) {
      const auto *Constructor = dyn_cast<ConstructorDecl>(Member);
      if (!Constructor)
        continue;
      if (Constructor->hasStubImplementation())
        continue;
      if (Constructor->isDesignatedInit())
        addConstructor(Constructor);
    }
  }

  void getOverrideCompletions(SourceLoc Loc) {
    if (auto TypeContext = CurrDeclContext->getInnermostTypeContext()){
      if (Type CurrTy = TypeContext->getDeclaredTypeInContext()) {
        lookupVisibleMemberDecls(*this, CurrTy, CurrDeclContext,
                                 TypeResolver.get());
        addDesignatedInitializers(CurrTy);
      }
    }
  }
};

} // end unnamed namespace

void CodeCompletionCallbacksImpl::completeDotExpr(Expr *E, SourceLoc DotLoc) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::DotExpr;
  ParsedExpr = E;
  this->DotLoc = DotLoc;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExprBeginning(CodeCompletionExpr *E) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprBeginning;
  CurDeclContext = P.CurDeclContext;
  CStyleForLoopIterationVariable =
      CodeCompletionCallbacks::CStyleForLoopIterationVariable;
  CodeCompleteTokenExpr = E;
}

void CodeCompletionCallbacksImpl::completePostfixExpr(Expr *E) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExpr;
  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExprParen(Expr *E, Expr *CCE) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::PostfixExprParen;
  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
  CodeCompleteTokenExpr = static_cast<CodeCompletionExpr*>(CCE);
}

void CodeCompletionCallbacksImpl::completeExprSuper(SuperRefExpr *SRE) {
  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::SuperExpr;
  ParsedExpr = SRE;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeExprSuperDot(SuperRefExpr *SRE) {
  // Don't produce any results in an enum element.
  if (InEnumElementRawValue)
    return;

  Kind = CompletionKind::SuperExprDot;
  ParsedExpr = SRE;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePoundAvailablePlatform() {
  Kind = CompletionKind::PoundAvailablePlatform;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeSimpleBeginning() {
  Kind = CompletionKind::TypeSimpleBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeDeclAttrParam(DeclAttrKind DK,
                                                        int Index) {
  Kind = CompletionKind::AttributeDeclParen;
  AttrKind = DK;
  AttrParamIndex = Index;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeDeclAttrKeyword(Decl *D,
                                                          bool Sil,
                                                          bool Param) {
  Kind = CompletionKind::AttributeBegin;
  IsInSil = Sil;
  if (Param) {
    AttTargetDK = DeclKind::Param;
  } else if (D) {
    AttTargetDK = D->getKind();
  }
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithDot(
    IdentTypeRepr *ITR) {
  if (!ITR) {
    completeTypeSimpleBeginning();
    return;
  }
  Kind = CompletionKind::TypeIdentifierWithDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithoutDot(
    IdentTypeRepr *ITR) {
  assert(ITR);
  Kind = CompletionKind::TypeIdentifierWithoutDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtBeginning() {
  assert(!InEnumElementRawValue);

  Kind = CompletionKind::CaseStmtBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeCaseStmtDotPrefix() {
  assert(!InEnumElementRawValue);

  Kind = CompletionKind::CaseStmtDotPrefix;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeImportDecl() {
  Kind = CompletionKind::Import;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeUnresolvedMember(UnresolvedMemberExpr *E,
    ArrayRef<StringRef> Identifiers, bool HasReturn) {
  Kind = CompletionKind::UnresolvedMember;
  CurDeclContext = P.CurDeclContext;
  UnresolvedExpr = E;
  UnresolvedExprInReturn = HasReturn;
  for (auto Id : Identifiers) {
    TokensBeforeUnresolvedExpr.push_back(Id);
  }
}

void CodeCompletionCallbacksImpl::completeAssignmentRHS(AssignExpr *E) {
  AssignmentExpr = E;
  ParsedExpr = E->getDest();
  CurDeclContext = P.CurDeclContext;
  Kind = CompletionKind::AssignmentRHS;
}

void CodeCompletionCallbacksImpl::completeCallArg(CallExpr *E) {
  if (Kind == CompletionKind::PostfixExprBeginning ||
      Kind == CompletionKind::None) {
    CurDeclContext = P.CurDeclContext;
    Kind = CompletionKind::CallArg;
    FuncCallExpr = E;
    ParsedExpr = E;
  }
}

void CodeCompletionCallbacksImpl::completeNominalMemberBeginning(
    SmallVectorImpl<StringRef> &Keywords) {
  assert(!InEnumElementRawValue);
  ParsedKeywords.clear();
  ParsedKeywords.append(Keywords.begin(), Keywords.end());
  Kind = CompletionKind::NominalMemberBeginning;
  CurDeclContext = P.CurDeclContext;
}

static bool isDynamicLookup(Type T) {
  if (auto *PT = T->getRValueType()->getAs<ProtocolType>())
    return PT->getDecl()->isSpecificProtocol(KnownProtocolKind::AnyObject);
  return false;
}

static bool isClangSubModule(Module *TheModule) {
  if (auto ClangMod = TheModule->findUnderlyingClangModule())
    return ClangMod->isSubModule();
  return false;
}

static void addDeclKeywords(CodeCompletionResultSink &Sink) {
  auto AddKeyword = [&](StringRef Name) {
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, {});
    Builder.addTextChunk(Name);
  };

#define DECL_KEYWORD(kw) AddKeyword(#kw);
#include "swift/Parse/Tokens.def"
  // Context-sensitive keywords.
  AddKeyword("weak");
  AddKeyword("unowned");
  AddKeyword("optional");
  AddKeyword("required");
  AddKeyword("lazy");
  AddKeyword("final");
  AddKeyword("dynamic");
  AddKeyword("prefix");
  AddKeyword("postfix");
  AddKeyword("infix");
  AddKeyword("override");
  AddKeyword("mutating");
  AddKeyword("nonmutating");
  AddKeyword("convenience");
}

static void addStmtKeywords(CodeCompletionResultSink &Sink) {
  auto AddKeyword = [&](StringRef Name, StringRef TypeAnnotation) {
    CodeCompletionResultBuilder Builder(
        Sink, CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None, {});
    Builder.addTextChunk(Name);
    if (!TypeAnnotation.empty())
      Builder.addTypeAnnotation(TypeAnnotation);
  };

#define STMT_KEYWORD(kw) AddKeyword(#kw, StringRef());
#include "swift/Parse/Tokens.def"

  // Expr keywords.
  AddKeyword("throw", StringRef());
  AddKeyword("try", StringRef());
  AddKeyword("try!", StringRef());
  AddKeyword("try?", StringRef());
  // FIXME: The pedantically correct way to find the type is to resolve the
  // Swift.StringLiteralType type.
  AddKeyword("__FUNCTION__", "String");
  AddKeyword("__FILE__", "String");
  // Same: Swift.IntegerLiteralType.
  AddKeyword("__LINE__", "Int");
  AddKeyword("__COLUMN__", "Int");
  // Same: Swift.BooleanLiteralType.
  AddKeyword("false", "Bool");
  AddKeyword("true", "Bool");

  AddKeyword("__DSO_HANDLE__", "UnsafeMutablePointer<Void>");

  CodeCompletionResultBuilder Builder(Sink,
                                      CodeCompletionResult::ResultKind::Keyword,
                                      SemanticContextKind::CurrentModule, {});
  Builder.addTextChunk("nil");

}

void CodeCompletionCallbacksImpl::addKeywords(CodeCompletionResultSink &Sink) {
  switch (Kind) {
  case CompletionKind::None:
  case CompletionKind::DotExpr:
  case CompletionKind::AttributeDeclParen:
  case CompletionKind::AttributeBegin:
  case CompletionKind::PoundAvailablePlatform:
  case CompletionKind::Import:
  case CompletionKind::UnresolvedMember:
  case CompletionKind::AssignmentRHS:
  case CompletionKind::CallArg:
    break;

  case CompletionKind::PostfixExprBeginning:
    addSuperKeyword(Sink);
    addDeclKeywords(Sink);
    addStmtKeywords(Sink);
    break;

  case CompletionKind::PostfixExpr:
  case CompletionKind::PostfixExprParen:
  case CompletionKind::SuperExpr:
  case CompletionKind::SuperExprDot:
  case CompletionKind::TypeSimpleBeginning:
  case CompletionKind::TypeIdentifierWithDot:
  case CompletionKind::TypeIdentifierWithoutDot:
  case CompletionKind::CaseStmtBeginning:
  case CompletionKind::CaseStmtDotPrefix:
    break;

  case CompletionKind::NominalMemberBeginning:
    addDeclKeywords(Sink);
    break;
  }
}

namespace  {
  class ExprParentFinder : public ASTWalker {
    SourceManager &SM;
    Expr *ChildExpr;
    llvm::function_ref<bool(Expr*)> Predicate;

  public:
    llvm::SmallVector<Expr*, 5> Ancestors;
    Expr *ParentExprClosest = nullptr;
    Expr *ParentExprFarthest = nullptr;
    ExprParentFinder(SourceManager &SM,
                     Expr* ChildExpr,
                     llvm::function_ref<bool(Expr*)> Predicate) :
                       SM(SM), ChildExpr(ChildExpr), Predicate(Predicate){}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (E == ChildExpr) {
        if (!Ancestors.empty()) {
          ParentExprClosest = Ancestors.back();
          ParentExprFarthest = Ancestors.front();
        }
      }
      if (Predicate(E))
        Ancestors.push_back(E);
      return { true, E };
    }

    Expr *walkToExprPost(Expr *E) override {
      if (Predicate(E))
        Ancestors.pop_back();
      return E;
    }
  };
}

class DotExpressionTypeContextAnalyzer {
  DeclContext *DC;
  Expr *ParsedExpr;
  ExprParentFinder Finder;

public:
  DotExpressionTypeContextAnalyzer(DeclContext *DC, Expr *ParsedExpr) : DC(DC),
    ParsedExpr(ParsedExpr),
    Finder(DC->getASTContext().SourceMgr, ParsedExpr, [](Expr *E) {
      switch(E->getKind()) {
        case ExprKind::Call:
          return true;
        default:
          return false;
      }
  }) {}

  bool Analyze(llvm::SmallVectorImpl<Type> &PossibleTypes) {
    DC->walkContext(Finder);
    auto Parent = Finder.ParentExprClosest;
    std::vector<Type> PotentialTypes;
    if (!Parent)
      return false;
    switch (Parent->getKind()) {
      case ExprKind::Call: {
        CompletionLookup::collectArgumentExpectatation(*DC, cast<CallExpr>(Parent),
                                                      ParsedExpr, PotentialTypes);
        break;
      }
      default:
        llvm_unreachable("Unhandled expression kinds.");
    }
    for (auto Ty : PotentialTypes) {
      if (Ty && Ty->getKind() != TypeKind::Error) {
        PossibleTypes.push_back(Ty->getRValueType());
      }
    }
    return !PossibleTypes.empty();
  }
};

void CodeCompletionCallbacksImpl::doneParsing() {
  if (Kind == CompletionKind::None) {
    return;
  }

  // Add keywords even if type checking fails completely.
  addKeywords(CompletionContext.getResultSink());

  if (!typecheckContext())
    return;

  if (DelayedParsedDecl && !typecheckDelayedParsedDecl())
    return;

  if (auto *AFD = dyn_cast_or_null<AbstractFunctionDecl>(DelayedParsedDecl))
    CurDeclContext = AFD;

  Optional<Type> ExprType;
  if (ParsedExpr) {
    ExprType = getTypeOfParsedExpr();
    if (!ExprType && Kind != CompletionKind::PostfixExprParen &&
        Kind != CompletionKind::CallArg)
      return;
  }

  if (!ParsedTypeLoc.isNull() && !typecheckParsedType())
    return;

  CompletionLookup Lookup(CompletionContext.getResultSink(), P.Context,
                          CurDeclContext);
  if (ExprType) {
    Lookup.setIsStaticMetatype(ParsedExpr->isStaticallyDerivedMetatype());
  }

  auto DoPostfixExprBeginning = [&] (){
    if (CStyleForLoopIterationVariable)
      Lookup.addExpressionSpecificDecl(CStyleForLoopIterationVariable);
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getValueCompletionsInDeclContext(Loc);
  };

  switch (Kind) {
  case CompletionKind::None:
    llvm_unreachable("should be already handled");
    return;

  case CompletionKind::DotExpr: {
    Lookup.setHaveDot(DotLoc);
    Type OriginalType = *ExprType;
    Type ExprType = OriginalType;

    // If there is no nominal type in the expr, try to find nominal types
    // in the ancestors of the expr.
    if (!OriginalType->getAnyNominal()) {
      ExprParentFinder Walker(CurDeclContext->getASTContext().SourceMgr,
                              ParsedExpr, [&](Expr* E) {
        return E->getType() && E->getType()->getAnyNominal();
      });
      CurDeclContext->walkContext(Walker);
      ExprType = Walker.ParentExprClosest ? Walker.ParentExprClosest->getType():
        OriginalType;
    }

    if (isDynamicLookup(ExprType))
      Lookup.setIsDynamicLookup();
    Lookup.initializeArchetypeTransformer(CurDeclContext, ExprType);

    DotExpressionTypeContextAnalyzer TypeAnalyzer(CurDeclContext, ParsedExpr);
    llvm::SmallVector<Type, 2> PossibleTypes;
    if (TypeAnalyzer.Analyze(PossibleTypes)) {
      Lookup.setExpectedTypes(PossibleTypes);
    }
    Lookup.getValueExprCompletions(ExprType);
    break;
  }

  case CompletionKind::PostfixExprBeginning: {
    DoPostfixExprBeginning();
    break;
  }

  case CompletionKind::PostfixExpr: {
    if (isDynamicLookup(*ExprType))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(*ExprType);
    break;
  }

  case CompletionKind::PostfixExprParen: {
    Lookup.setHaveLParen(true);
    ValueDecl *VD = nullptr;
    if (auto *AE = dyn_cast<ApplyExpr>(ParsedExpr)) {
      if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getFn()))
        VD = DRE->getDecl();
    }
    DotExpressionTypeContextAnalyzer TypeAnalyzer(CurDeclContext,
                                                  CodeCompleteTokenExpr);
    llvm::SmallVector<Type, 2> PossibleTypes;
    if (TypeAnalyzer.Analyze(PossibleTypes)) {
      Lookup.setExpectedTypes(PossibleTypes);
    }
    if (ExprType)
      Lookup.getValueExprCompletions(*ExprType, VD);
    if (!Lookup.FoundFunctionCalls ||
        (Lookup.FoundFunctionCalls &&
         Lookup.FoundFunctionsWithoutFirstKeyword)) {
      Lookup.setHaveLParen(false);
      DoPostfixExprBeginning();
    }
    break;
  }

  case CompletionKind::SuperExpr: {
    Lookup.setIsSuperRefExpr();
    Lookup.getValueExprCompletions(*ExprType);
    break;
  }

  case CompletionKind::SuperExprDot: {
    Lookup.setIsSuperRefExpr();
    Lookup.setHaveDot(SourceLoc());
    Lookup.getValueExprCompletions(*ExprType);
    break;
  }

  case CompletionKind::TypeSimpleBeginning: {
    Lookup.getTypeCompletionsInDeclContext(
        P.Context.SourceMgr.getCodeCompletionLoc());
    break;
  }

  case CompletionKind::TypeIdentifierWithDot: {
    Lookup.setHaveDot(SourceLoc());
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::TypeIdentifierWithoutDot: {
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::CaseStmtBeginning: {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getValueCompletionsInDeclContext(Loc);
    Lookup.getTypeContextEnumElementCompletions(Loc);
    break;
  }

  case CompletionKind::CaseStmtDotPrefix: {
    Lookup.setHaveDot(SourceLoc());
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.getTypeContextEnumElementCompletions(Loc);
    break;
  }

  case CompletionKind::NominalMemberBeginning: {
    Lookup.discardTypeResolver();
    CompletionOverrideLookup OverrideLookup(CompletionContext.getResultSink(),
                                            P.Context, CurDeclContext,
                                            ParsedKeywords);
    OverrideLookup.getOverrideCompletions(SourceLoc());
    break;
  }
  case CompletionKind::AttributeBegin: {
    Lookup.getAttributeDeclCompletions(IsInSil, AttTargetDK);
    break;
  }
  case CompletionKind::AttributeDeclParen: {
    Lookup.getAttributeDeclParamCompletions(AttrKind, AttrParamIndex);
    break;
  }
  case CompletionKind::PoundAvailablePlatform: {
    Lookup.getPoundAvailablePlatformCompletions();
    break;
  }
  case CompletionKind::Import: {
    Lookup.addImportModuleNames();
    break;
  }
  case CompletionKind::UnresolvedMember : {
    Lookup.setHaveDot(SourceLoc());
    SmallVector<Type, 1> PossibleTypes;
    ExprParentFinder Walker(CurDeclContext->getASTContext().SourceMgr,
                            UnresolvedExpr, [&](Expr* E) { return true; });
    CurDeclContext->walkContext(Walker);
    bool Success = false;
    if(Walker.ParentExprFarthest) {
      Success = typeCheckUnresolvedExpr(*CurDeclContext, UnresolvedExpr,
                                        Walker.ParentExprFarthest,
                                        PossibleTypes);
      Lookup.getUnresolvedMemberCompletions(
        P.Context.SourceMgr.getCodeCompletionLoc(), PossibleTypes);
    }
    if (!Success) {
      Lookup.getUnresolvedMemberCompletions(
        P.Context.SourceMgr.getCodeCompletionLoc(),
        TokensBeforeUnresolvedExpr,
        UnresolvedExprInReturn);
    }
    break;
  }
  case CompletionKind::AssignmentRHS : {
    SourceLoc Loc = P.Context.SourceMgr.getCodeCompletionLoc();
    Lookup.setExpectedTypes(AssignmentExpr->getDest()->getType()->getRValueType());
    Lookup.getValueCompletionsInDeclContext(Loc, DefaultFilter,
                                            /*IncludeTopLevel*/true,
                                            /*RequestCache*/false);
    break;
  }
  case CompletionKind::CallArg : {
    if (!CodeCompleteTokenExpr || !Lookup.getCallArgCompletions(*CurDeclContext,
                                                                FuncCallExpr,
                                                                CodeCompleteTokenExpr))
      DoPostfixExprBeginning();
    break;
  }
  }

  if (Lookup.RequestedCachedResults) {
    // Use the current SourceFile as the DeclContext so that we can use it to
    // perform qualified lookup, and to get the correct visibility for
    // @testable imports.
    const SourceFile &SF = P.SF;

    auto &Request = Lookup.RequestedCachedResults.getValue();

    llvm::DenseSet<CodeCompletionCache::Key> ImportsSeen;
    auto handleImport = [&](Module::ImportedModule Import) {
      Module *TheModule = Import.second;
      Module::AccessPathTy Path = Import.first;
      if (TheModule->getFiles().empty())
        return;

      // Clang submodules are ignored and there's no lookup cost involved,
      // so just ignore them and don't put the empty results in the cache
      // because putting a lot of objects in the cache will push out
      // other lookups.
      if (isClangSubModule(TheModule))
        return;

      std::vector<std::string> AccessPath;
      for (auto Piece : Path) {
        AccessPath.push_back(Piece.first.str());
      }

      StringRef ModuleFilename = TheModule->getModuleFilename();
      // ModuleFilename can be empty if something strange happened during
      // module loading, for example, the module file is corrupted.
      if (!ModuleFilename.empty()) {
        CodeCompletionCache::Key K{ModuleFilename, TheModule->getName().str(),
                                   AccessPath, Request.NeedLeadingDot,
                                   SF.hasTestableImport(TheModule)};
        std::pair<decltype(ImportsSeen)::iterator, bool>
        Result = ImportsSeen.insert(K);
        if (!Result.second)
          return; // already handled.

        RequestedModules.push_back(
            {std::move(K), TheModule, Request.OnlyTypes});
      }
    };

    if (Request.TheModule) {
      Lookup.discardTypeResolver();

      // FIXME: actually check imports.
      const_cast<Module*>(Request.TheModule)
          ->forAllVisibleModules({}, handleImport);
    } else {
      // Add results from current module.
      Lookup.getToplevelCompletions(Request.OnlyTypes);
      Lookup.discardTypeResolver();

      // Add results for all imported modules.
      SmallVector<Module::ImportedModule, 4> Imports;
      auto *SF = CurDeclContext->getParentSourceFile();
      SF->getImportedModules(Imports, Module::ImportFilter::All);

      for (auto Imported : Imports) {
        Module *TheModule = Imported.second;
        Module::AccessPathTy AccessPath = Imported.first;
        TheModule->forAllVisibleModules(AccessPath, handleImport);
      }
    }
    Lookup.RequestedCachedResults.reset();
  }

  deliverCompletionResults();
}

void CodeCompletionCallbacksImpl::deliverCompletionResults() {
  // Use the current SourceFile as the DeclContext so that we can use it to
  // perform qualified lookup, and to get the correct visibility for
  // @testable imports.
  DeclContext *DCForModules = &P.SF;

  Consumer.handleResultsAndModules(CompletionContext, RequestedModules,
                                   DCForModules);
  RequestedModules.clear();
  DeliveredResults = true;
}

void PrintingCodeCompletionConsumer::handleResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  unsigned NumResults = 0;
  for (auto Result : Results) {
    if (!IncludeKeywords && Result->getKind() == CodeCompletionResult::Keyword)
      continue;
    NumResults++;
  }
  if (NumResults == 0)
    return;

  OS << "Begin completions, " << NumResults << " items\n";
  for (auto Result : Results) {
    if (!IncludeKeywords && Result->getKind() == CodeCompletionResult::Keyword)
      continue;
    Result->print(OS);

    llvm::SmallString<64> Name;
    llvm::raw_svector_ostream NameOs(Name);
    Result->getCompletionString()->getName(NameOs);
    OS << "; name=" << Name;

    OS << "\n";
  }
  OS << "End completions\n";
}

namespace {
class CodeCompletionCallbacksFactoryImpl
    : public CodeCompletionCallbacksFactory {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;

public:
  CodeCompletionCallbacksFactoryImpl(CodeCompletionContext &CompletionContext,
                                     CodeCompletionConsumer &Consumer)
      : CompletionContext(CompletionContext), Consumer(Consumer) {}

  CodeCompletionCallbacks *createCodeCompletionCallbacks(Parser &P) override {
    return new CodeCompletionCallbacksImpl(P, CompletionContext, Consumer);
  }
};
} // end unnamed namespace

CodeCompletionCallbacksFactory *
swift::ide::makeCodeCompletionCallbacksFactory(
    CodeCompletionContext &CompletionContext,
    CodeCompletionConsumer &Consumer) {
  return new CodeCompletionCallbacksFactoryImpl(CompletionContext, Consumer);
}

void swift::ide::lookupCodeCompletionResultsFromModule(
    CodeCompletionResultSink &targetSink, const Module *module,
    ArrayRef<std::string> accessPath, bool needLeadingDot,
    const DeclContext *currDeclContext) {
  CompletionLookup Lookup(targetSink, module->getASTContext(), currDeclContext);
  Lookup.getVisibleDeclsOfModule(module, accessPath, needLeadingDot);
}

void swift::ide::copyCodeCompletionResults(CodeCompletionResultSink &targetSink,
                                           CodeCompletionResultSink &sourceSink,
                                           bool onlyTypes) {

  // We will be adding foreign results (from another sink) into TargetSink.
  // TargetSink should have an owning pointer to the allocator that keeps the
  // results alive.
  targetSink.ForeignAllocators.push_back(sourceSink.Allocator);

  if (onlyTypes) {
    std::copy_if(sourceSink.Results.begin(), sourceSink.Results.end(),
                 std::back_inserter(targetSink.Results),
                 [](CodeCompletionResult *R) -> bool {
      if (R->getKind() != CodeCompletionResult::Declaration)
        return false;
      switch(R->getAssociatedDeclKind()) {
      case CodeCompletionDeclKind::Module:
      case CodeCompletionDeclKind::Class:
      case CodeCompletionDeclKind::Struct:
      case CodeCompletionDeclKind::Enum:
      case CodeCompletionDeclKind::Protocol:
      case CodeCompletionDeclKind::TypeAlias:
      case CodeCompletionDeclKind::GenericTypeParam:
        return true;
      case CodeCompletionDeclKind::EnumElement:
      case CodeCompletionDeclKind::Constructor:
      case CodeCompletionDeclKind::Destructor:
      case CodeCompletionDeclKind::Subscript:
      case CodeCompletionDeclKind::StaticMethod:
      case CodeCompletionDeclKind::InstanceMethod:
      case CodeCompletionDeclKind::OperatorFunction:
      case CodeCompletionDeclKind::FreeFunction:
      case CodeCompletionDeclKind::StaticVar:
      case CodeCompletionDeclKind::InstanceVar:
      case CodeCompletionDeclKind::LocalVar:
      case CodeCompletionDeclKind::GlobalVar:
        return false;
      }
    });
  } else {
    targetSink.Results.insert(targetSink.Results.end(),
                              sourceSink.Results.begin(),
                              sourceSink.Results.end());
  }
}

void SimpleCachingCodeCompletionConsumer::handleResultsAndModules(
    CodeCompletionContext &context,
    ArrayRef<RequestedCachedModule> requestedModules,
    DeclContext *DCForModules) {
  for (auto &R : requestedModules) {
    // FIXME(thread-safety): lock the whole AST context.  We might load a
    // module.
    llvm::Optional<CodeCompletionCache::ValueRefCntPtr> V =
        context.Cache.get(R.Key);
    if (!V.hasValue()) {
      // No cached results found. Fill the cache.
      V = context.Cache.createValue();
      lookupCodeCompletionResultsFromModule(
          (*V)->Sink, R.TheModule, R.Key.AccessPath,
          R.Key.ResultsHaveLeadingDot, DCForModules);
      context.Cache.set(R.Key, *V);
    }
    assert(V.hasValue());
    copyCodeCompletionResults(context.getResultSink(), (*V)->Sink, R.OnlyTypes);
  }

  handleResults(context.takeResults());
}
