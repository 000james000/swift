//===--- Utils.cpp - Misc utilities ---------------------------------------===//
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

#include "swift/IDE/Utils.h"
#include "swift/Basic/SourceManager.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/Parser.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/ModuleFile.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace swift;
using namespace ide;

static const char *skipStringInCode(const char *p, const char *End);

static const char *skipParenExpression(const char *p, const char *End) {
  const char *e = p;
  if (*e == '(') {
    uint32_t ParenCount = 1;
    bool done = false;
    for (++e; e < End; ++e) {
      switch (*e) {
      case ')':
        done = --ParenCount == 0;
        break;
                  
      case '(':
        ++ParenCount;
        break;
              
      case '"':
        e = skipStringInCode (e, End);
        break;
              
      default:
        break;
      }
      // If "done" is true make sure we don't increment "e"
      if (done)
        break;
    }
  }
  if (e >= End)
    return End;
  return e;
}

static const char *skipStringInCode(const char *p, const char *End) {
  const char *e = p;
  if (*e == '"') {
    bool done = false;
    for (++e; e < End; ++e) {
      switch (*e) {
      case '"':
        done = true;
        break;
                  
      case '\\':
        ++e;
        if (e >= End)
          done = true;
        else if (*e == '(')
          e = skipParenExpression (e, End);
        break;
              
      default:
        break;
      }
      // If "done" is true make sure we don't increment "e"
      if (done)
          break;
    }
  }
  if (e >= End)
    return End;
  return e;
}

SourceCompleteResult
ide::isSourceInputComplete(std::unique_ptr<llvm::MemoryBuffer> MemBuf) {
  SourceManager SM;
  auto BufferID = SM.addNewSourceBuffer(std::move(MemBuf));
  ParserUnit Parse(SM, BufferID);
  Parser &P = Parse.getParser();

  bool Done;
  do {
    P.parseTopLevel();
    Done = P.Tok.is(tok::eof);
  } while (!Done);

  SourceCompleteResult SCR;
  SCR.IsComplete = !P.isInputIncomplete();
    
  // Use the same code that was in the REPL code to track the indent level 
  // for now. In the future we should get this from the Parser if possible.

  CharSourceRange entireRange = SM.getRangeForBuffer(BufferID);
  StringRef Buffer = SM.extractText(entireRange);
  const char *SourceStart = Buffer.data();
  const char *SourceEnd = Buffer.data() + Buffer.size();
  const char *LineStart = SourceStart;
  const char *LineSourceStart = NULL;
  uint32_t LineIndent = 0;
  struct IndentInfo {
    StringRef Prefix;
    uint32_t Indent;
    IndentInfo(const char *s, size_t n, uint32_t i) :
      Prefix(s, n),
      Indent(i) {}
  };
  SmallVector<IndentInfo, 4> IndentInfos;
  for (const char *p = SourceStart; p<SourceEnd; ++p) {
    switch (*p) {
    case '\r':
    case '\n':
      LineIndent = 0;
      LineSourceStart = NULL;
      LineStart = p + 1;
      break;

    case '"':
      p = skipStringInCode (p, SourceEnd);
      break;

    case '{':
    case '(':
    case '[':
      ++LineIndent;
      if (LineSourceStart == NULL)
        IndentInfos.push_back(IndentInfo(LineStart,
                                         p - LineStart,
                                         LineIndent));
      else
        IndentInfos.push_back(IndentInfo(LineStart,
                                         LineSourceStart - LineStart,
                                         LineIndent));
      break;

    case '}':
    case ')':
    case ']':
      if (LineIndent > 0)
        --LineIndent;
      if (!IndentInfos.empty())
        IndentInfos.pop_back();
      break;
  
    default:
      if (LineSourceStart == NULL && !isspace(*p))
        LineSourceStart = p;
      break;
    }
    if (*p == '\0')
      break;
  }
  if (!IndentInfos.empty()) {
    SCR.IndentPrefix = std::move(IndentInfos.back().Prefix.str());
    // Trim off anything that follows a non-space character
    const size_t pos = SCR.IndentPrefix.find_first_not_of(" \t");
    if (pos != std::string::npos)
        SCR.IndentPrefix.erase(pos);
    SCR.IndentLevel = IndentInfos.back().Indent;
  }
  return SCR;
}

SourceCompleteResult ide::isSourceInputComplete(StringRef Text) {
  std::unique_ptr<llvm::MemoryBuffer> InputBuf;
  InputBuf.reset(llvm::MemoryBuffer::getMemBufferCopy(Text));
  return ide::isSourceInputComplete(std::move(InputBuf));
}

const clang::Module *ide::findUnderlyingClangModule(const Module *M) {
  const ClangModuleUnit *CMU = nullptr;
  for (auto *FU : M->getFiles()) {
    if ((CMU = dyn_cast<ClangModuleUnit>(FU)))
      break;
    if (auto *AST = dyn_cast<SerializedASTFile>(FU)) {
      if (auto *ShadowedModule = AST->getFile().getShadowedModule())
        if (auto *Result = findUnderlyingClangModule(ShadowedModule))
          return Result;
    }
  }
  if (!CMU)
    return nullptr;
  return CMU->getClangModule();
}

template <typename FnTy>
static void walkOverriddenClangDecls(const clang::NamedDecl *D, const FnTy &Fn){
  SmallVector<const clang::NamedDecl *, 8> OverDecls;
  D->getASTContext().getOverriddenMethods(D, OverDecls);
  for (auto Over : OverDecls)
    Fn(Over);
  for (auto Over : OverDecls)
    walkOverriddenClangDecls(Over, Fn);
}

void
ide::walkOverriddenDecls(const ValueDecl *VD,
                         std::function<void(llvm::PointerUnion<
                             const ValueDecl*, const clang::NamedDecl*>)> Fn) {
  for (auto CurrOver = VD; CurrOver; CurrOver = CurrOver->getOverriddenDecl()) {
    if (CurrOver != VD)
      Fn(CurrOver);
    if (auto ClangD =
        dyn_cast_or_null<clang::NamedDecl>(CurrOver->getClangDecl())) {
      walkOverriddenClangDecls(ClangD, Fn);
      return;
    }
    for (auto Conf: const_cast<ValueDecl*>(CurrOver)->getConformances())
      Fn(Conf);
  }
}
