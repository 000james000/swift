//===--- SourceLoc.cpp - SourceLoc and SourceRange implementations --------===//
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

#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

SourceLoc SourceManager::getCodeCompletionLoc() const {
  return getLocForBufferStart(CodeCompletionBufferID)
      .getAdvancedLoc(CodeCompletionOffset);
}

size_t SourceManager::addNewSourceBuffer(llvm::MemoryBuffer *Buffer) {
  assert(Buffer);
  auto ID = LLVMSourceMgr.AddNewSourceBuffer(Buffer, llvm::SMLoc());
  BufIdentIDMap[Buffer->getBufferIdentifier()] = ID;
  return ID;
}

size_t SourceManager::addMemBufferCopy(llvm::MemoryBuffer *Buffer) {
  return addMemBufferCopy(Buffer->getBuffer(), Buffer->getBufferIdentifier());
}

size_t SourceManager::addMemBufferCopy(StringRef InputData,
                                       StringRef BufIdentifier) {
  auto Buffer = llvm::MemoryBuffer::getMemBufferCopy(InputData, BufIdentifier);
  return addNewSourceBuffer(Buffer);
}

Optional<unsigned> SourceManager::getIDForBufferIdentifier(
    StringRef BufIdentifier) {
  auto It = BufIdentIDMap.find(BufIdentifier);
  if (It == BufIdentIDMap.end())
    return Nothing;
  return It->second;
}

SourceLoc SourceManager::getLocForBufferStart(unsigned BufferID) const {
  auto *Buffer = LLVMSourceMgr.getMemoryBuffer(BufferID);
  return SourceLoc(llvm::SMLoc::getFromPointer(Buffer->getBufferStart()));
}

unsigned SourceManager::getLocOffsetInBuffer(SourceLoc Loc,
                                             unsigned BufferID) const {
  assert(Loc.isValid() && "location should be valid");
  auto *Buffer = LLVMSourceMgr.getMemoryBuffer(BufferID);
  assert(Loc.Value.getPointer() >= Buffer->getBuffer().begin() &&
         Loc.Value.getPointer() <= Buffer->getBuffer().end() &&
         "Location is not from the specified buffer");
  return Loc.Value.getPointer() - Buffer->getBuffer().begin();
}

unsigned SourceManager::getByteDistance(SourceLoc Start, SourceLoc End) const {
  assert(Start.isValid() && "start location should be valid");
  assert(End.isValid() && "end location should be valid");
#ifndef NDEBUG
  unsigned BufferID = findBufferContainingLoc(Start);
  auto *Buffer = LLVMSourceMgr.getMemoryBuffer(BufferID);
  assert(End.Value.getPointer() >= Buffer->getBuffer().begin() &&
         End.Value.getPointer() <= Buffer->getBuffer().end() &&
         "End location is not from the same buffer");
#endif
  // When we have a rope buffer, could be implemented in terms of
  // getLocOffsetInBuffer().
  return End.Value.getPointer() - Start.Value.getPointer();
}

StringRef SourceManager::extractText(CharSourceRange Range) const {
  assert(Range.isValid() && "range should be valid");

  unsigned BufferID = findBufferContainingLoc(Range.getStart());
  StringRef Buffer = LLVMSourceMgr.getMemoryBuffer(BufferID)->getBuffer();
  return Buffer.substr(getLocOffsetInBuffer(Range.getStart(), BufferID),
                       Range.getByteLength());
}

void SourceLoc::printLineAndColumn(raw_ostream &OS,
                                   const SourceManager &SM) const {
  if (isInvalid()) {
    OS << "<invalid loc>";
    return;
  }

  auto LineAndCol = SM.getLineAndColumn(*this);
  OS << "line:" << LineAndCol.first << ':' << LineAndCol.second;
}

void SourceLoc::print(raw_ostream &OS, const SourceManager &SM,
                      unsigned &LastBufferID) const {
  if (isInvalid()) {
    OS << "<invalid loc>";
    return;
  }

  unsigned BufferID = SM.findBufferContainingLoc(*this);
  if (BufferID != LastBufferID) {
    OS << SM->getMemoryBuffer(BufferID)->getBufferIdentifier();
    LastBufferID = BufferID;
  } else {
    OS << "line";
  }

  auto LineAndCol = SM.getLineAndColumn(*this, BufferID);
  OS << ':' << LineAndCol.first << ':' << LineAndCol.second;
}

void SourceLoc::dump(const SourceManager &SM) const {
  print(llvm::errs(), SM);
}

void SourceRange::print(raw_ostream &OS, const SourceManager &SM,
                        unsigned &LastBufferID, bool PrintText) const {
  OS << '[';
  Start.print(OS, SM, LastBufferID);
  OS << " - ";
  End.print(OS, SM, LastBufferID);
  OS << ']';
  
  if (Start.isInvalid() || End.isInvalid())
    return;
  
  if (PrintText) {
    OS << " RangeText=\""
       << StringRef(Start.Value.getPointer(),
                    End.Value.getPointer() - Start.Value.getPointer()+1)
       << '"';
  }
}

void SourceRange::dump(const SourceManager &SM) const {
  print(llvm::errs(), SM);
}

CharSourceRange::CharSourceRange(const SourceManager &SM, SourceLoc Start,
                                 SourceLoc End)
    : Start(Start) {
  assert(Start.isValid() == End.isValid() &&
         "Start and end should either both be valid or both be invalid!");
  if (Start.isValid())
    ByteLength = SM.getByteDistance(Start, End);
}

