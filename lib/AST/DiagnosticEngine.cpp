//===- DiagnosticEngine.h - Diagnostic Display Engine -----------*- C++ -*-===//
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
//  This file defines the DiagnosticEngine class, which manages any diagnostics
//  emitted by Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrintOptions.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/Lexer.h" // bad dependency
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;

enum class DiagnosticOptions {
  /// No options.
  none,

  /// The location of this diagnostic points to the beginning of the first
  /// token that the parser considers invalid.  If this token is located at the
  /// beginning of the line, then the location is adjusted to point to the end
  /// of the previous token.
  ///
  /// This behaviour improves experience for "expected token X" diagnostics.
  PointsToFirstBadToken
};

struct StoredDiagnosticInfo {
  /// \brief The kind of diagnostic we're dealing with.
  DiagnosticKind Kind;

  DiagnosticOptions Options;

  // FIXME: Category
  
  /// \brief Text associated with the diagnostic
  const char *Text;
};

static StoredDiagnosticInfo StoredDiagnosticInfos[] = {
#define ERROR(ID,Category,Options,Text,Signature) \
  { DiagnosticKind::Error, DiagnosticOptions::Options, Text },
#define WARNING(ID,Category,Options,Text,Signature) \
  { DiagnosticKind::Warning, DiagnosticOptions::Options, Text },
#define NOTE(ID,Category,Options,Text,Signature) \
  { DiagnosticKind::Note, DiagnosticOptions::Options, Text },
#include "swift/AST/DiagnosticsAll.def"
  { DiagnosticKind::Error, DiagnosticOptions::none, "<not a diagnostic>" }
};

static CharSourceRange toCharSourceRange(SourceManager &SM, SourceRange SR) {
  return CharSourceRange(SM, SR.Start, Lexer::getLocForEndOfToken(SM, SR.End));
}

static CharSourceRange toCharSourceRange(SourceManager &SM, SourceLoc Start,
                                         SourceLoc End) {
  return CharSourceRange(SM, Start, End);
}

InFlightDiagnostic &InFlightDiagnostic::highlight(SourceRange R) {
  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (Engine)
    Engine->getActiveDiagnostic()
        .addRange(toCharSourceRange(Engine->SourceMgr, R));
  return *this;
}

InFlightDiagnostic &InFlightDiagnostic::highlightChars(SourceLoc Start,
                                                       SourceLoc End) {
  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (Engine)
    Engine->getActiveDiagnostic()
        .addRange(toCharSourceRange(Engine->SourceMgr, Start, End));
  return *this;
}

InFlightDiagnostic &InFlightDiagnostic::fixItReplace(SourceRange R,
                                                     StringRef Str) {
  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (Engine)
    Engine->getActiveDiagnostic().addFixIt(
        Diagnostic::FixIt(toCharSourceRange(Engine->SourceMgr, R), Str));
  return *this;
}

InFlightDiagnostic &InFlightDiagnostic::fixItReplaceChars(SourceLoc Start,
                                                          SourceLoc End,
                                                          StringRef Str) {
  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (Engine && Start.isValid())
    Engine->getActiveDiagnostic().addFixIt(Diagnostic::FixIt(
        toCharSourceRange(Engine->SourceMgr, Start, End), Str));
  return *this;
}

void InFlightDiagnostic::flush() {
  if (!IsActive)
    return;
  
  IsActive = false;
  if (Engine)
    Engine->flushActiveDiagnostic();
}

bool DiagnosticEngine::isDiagnosticPointsToFirstBadToken(DiagID ID) const {
  const StoredDiagnosticInfo &StoredInfo =
      StoredDiagnosticInfos[(unsigned) ID];
  return StoredInfo.Options == DiagnosticOptions::PointsToFirstBadToken;
}

/// \brief Skip forward to one of the given delimiters.
///
/// \param Text The text to search through, which will be updated to point
/// just after the delimiter.
///
/// \param Delim1 The first character delimiter to search for.
///
/// \param Delim2 The second character delimiter to search for.
///
/// \returns The string leading up to the delimiter, or the empty string
/// if no delimiter is found.
static StringRef 
skipToDelimiter(StringRef &Text, char Delim1, char Delim2 = 0) {
  unsigned Depth = 0;

  unsigned I = 0;
  for (unsigned N = Text.size(); I != N; ++I) {
    if (Depth == 0 && Text[I] == '{') {
      ++Depth;
      continue;
    }
    if (Depth > 0 && Text[I] == '}') {
      --Depth;
      continue;
    }
    
    if (Text[I] == Delim1 || Text[I] == Delim2)
      break;
  }

  assert(Depth == 0 && "Unbalanced {} set in diagnostic text");
  StringRef Result = Text.substr(0, I);
  Text = Text.substr(I + 1);
  return Result;
}

static void formatDiagnosticText(StringRef InText, 
                                 ArrayRef<DiagnosticArgument> Args,
                                 llvm::raw_ostream &Out);

/// \brief Format a selection argument and write it to the given stream.
static void formatSelectionArgument(StringRef ModifierArguments,
                                    ArrayRef<DiagnosticArgument> Args,
                                    unsigned SelectedIndex,
                                    llvm::raw_ostream &Out) {
  do {
    StringRef Text = skipToDelimiter(ModifierArguments, '|');
    if (SelectedIndex == 0) {
      formatDiagnosticText(Text, Args, Out);
      break;
    }
    --SelectedIndex;
  } while (true);
  
}

/// \brief Format a single diagnostic argument and write it to the given
/// stream.
static void formatDiagnosticArgument(StringRef Modifier, 
                                     StringRef ModifierArguments,
                                     ArrayRef<DiagnosticArgument> Args,
                                     unsigned ArgIndex,
                                     llvm::raw_ostream &Out) {
  const DiagnosticArgument &Arg = Args[ArgIndex];
  switch (Arg.getKind()) {
  case DiagnosticArgumentKind::Integer:
    if (Modifier == "select") {
      assert(Arg.getAsInteger() >= 0 && "Negative selection index");
      formatSelectionArgument(ModifierArguments, Args, Arg.getAsInteger(), 
                              Out);
    } else {
      assert(Modifier.empty() && "Improper modifier for integer argument");
      Out << Arg.getAsInteger();
    }
    break;

  case DiagnosticArgumentKind::Unsigned:
    if (Modifier == "select") {
      formatSelectionArgument(ModifierArguments, Args, Arg.getAsUnsigned(), 
                              Out);
    } else {
      assert(Modifier.empty() && "Improper modifier for unsigned argument");
      Out << Arg.getAsUnsigned();
    }
    break;

  case DiagnosticArgumentKind::String:
    assert(Modifier.empty() && "Improper modifier for string argument");
    Out << Arg.getAsString();
    break;

  case DiagnosticArgumentKind::Identifier:
    assert(Modifier.empty() && "Improper modifier for identifier argument");
    Out << '\'' << Arg.getAsIdentifier() << '\'';
    break;
  case DiagnosticArgumentKind::Type:
    assert(Modifier.empty() && "Improper modifier for Type argument");
    Out << '\'' << Arg.getAsType() << '\'';
    break;
  case DiagnosticArgumentKind::TypeRepr:
    assert(Modifier.empty() && "Improper modifier for TypeRepr argument");
    Out << '\'' << Arg.getAsTypeRepr() << '\'';
    break;
  case DiagnosticArgumentKind::PatternKind:
    assert(Modifier.empty() && "Improper modifier for PatternKind argument");
    Out << Arg.getAsPatternKind();
    break;
  case DiagnosticArgumentKind::StaticSpellingKind:
    if (Modifier == "select") {
      formatSelectionArgument(ModifierArguments, Args,
                              unsigned(Arg.getAsStaticSpellingKind()), Out);
    } else {
      assert(Modifier.empty() &&
             "Improper modifier for StaticSpellingKind argument");
      Out << Arg.getAsStaticSpellingKind();
    }
    break;
  }
}

/// \brief Format the given diagnostic text and place the result in the given
/// buffer.
static void formatDiagnosticText(StringRef InText, 
                                 ArrayRef<DiagnosticArgument> Args,
                                 llvm::raw_ostream &Out) {
  while (!InText.empty()) {
    size_t Percent = InText.find('%');
    if (Percent == StringRef::npos) {
      // Write the rest of the string; we're done.
      Out.write(InText.data(), InText.size());
      break;
    }
    
    // Write the string up to (but not including) the %, then drop that text
    // (including the %).
    Out.write(InText.data(), Percent);
    InText = InText.substr(Percent + 1);
    
    // '%%' -> '%'.
    if (InText[0] == '%') {
      Out.write('%');
      InText = InText.substr(1);
      continue;
    }

    // Parse an optional modifier.
    StringRef Modifier;
    {
      unsigned Length = 0;
      while (isalpha(InText[Length]))
        ++Length;
      Modifier = InText.substr(0, Length);
      InText = InText.substr(Length);
    }
    
    // Parse the optional argument list for a modifier, which is brace-enclosed.
    StringRef ModifierArguments;
    if (InText[0] == '{') {
      InText = InText.substr(1);
      ModifierArguments = skipToDelimiter(InText, '}');
    }
    
    // Find the digit sequence.
    unsigned Length = 0;
    for (size_t N = InText.size(); Length != N; ++Length) {
      if (!isdigit(InText[Length]))
        break;
    }
      
    // Parse the digit sequence into an argument index.
    unsigned ArgIndex;      
    bool Result = InText.substr(0, Length).getAsInteger(10, ArgIndex);
    assert(!Result && "Unparseable argument index value?");
    (void)Result;
    assert(ArgIndex < Args.size() && "Out-of-range argument index");
    InText = InText.substr(Length);
    
    // Convert the argument to a string.
    formatDiagnosticArgument(Modifier, ModifierArguments, Args, ArgIndex, Out);
  }
}

void DiagnosticEngine::flushActiveDiagnostic() {
  assert(ActiveDiagnostic && "No active diagnostic to flush");
  const StoredDiagnosticInfo &StoredInfo
    = StoredDiagnosticInfos[(unsigned)ActiveDiagnostic->getID()];

  // Check whether this is an error.
  switch (StoredInfo.Kind) {
  case DiagnosticKind::Error:
    HadAnyError = true;
    break;
    
  case DiagnosticKind::Note:
  case DiagnosticKind::Warning:
    break;
  }

  // Figure out the source location.
  SourceLoc loc = ActiveDiagnosticLoc;
  if (loc.isInvalid() && ActiveDiagnosticDecl) {
    // If a declaration was provided instead of a location, and that declaration
    // has a location we can point to, use that location.
    if (!ActiveDiagnosticDecl->hasClangNode() &&
        ActiveDiagnosticDecl->getLoc().isValid()) {
      loc = ActiveDiagnosticDecl->getLoc();
    } else {
      // There is no location we can point to. Pretty-print the declaration
      // so we can point to it.
      SourceLoc ppLoc = PrettyPrintedDeclarations[ActiveDiagnosticDecl];
      if (ppLoc.isInvalid()) {
        class TrackingPrinter : public StreamPrinter {
          SmallVectorImpl<std::pair<const Decl *, uint64_t>> &Entries;

        public:
          TrackingPrinter(
              SmallVectorImpl<std::pair<const Decl *, uint64_t>> &Entries,
              raw_ostream &OS) :
            StreamPrinter(OS), Entries(Entries) {}

          void printDeclLoc(const Decl *D) override {
            Entries.push_back({ D, OS.tell() });
          }
        };
        SmallVector<std::pair<const Decl *, uint64_t>, 8> entries;
        llvm::SmallString<128> buffer;
        llvm::SmallString<128> bufferName;
        {
          // Figure out which declaration to print. It's the top-most
          // declaration (not a module).
          Decl *ppDecl = ActiveDiagnosticDecl;
          auto dc = ActiveDiagnosticDecl->getDeclContext();

          // FIXME: Horrible, horrible hackaround. We're not getting a
          // DeclContext everywhere we should.
          if (!dc) {
            // Reset the active diagnostic.
            ActiveDiagnostic.reset();
            return;
          }

          while (!dc->isModuleContext()) {
            switch (dc->getContextKind()) {
            case DeclContextKind::Module:
            case DeclContextKind::TopLevelCodeDecl:
              llvm_unreachable("Not in a module context!");
              break;

            case DeclContextKind::FileUnit:
              break;

            case DeclContextKind::ExtensionDecl:
              ppDecl = cast<ExtensionDecl>(dc);
              break;

            case DeclContextKind::NominalTypeDecl:
              ppDecl = cast<NominalTypeDecl>(dc);
              break;

            case DeclContextKind::Initializer:
            case DeclContextKind::AbstractClosureExpr:
            case DeclContextKind::AbstractFunctionDecl:
              break;
            }

            dc = dc->getParent();
          }

          // Build the module name path (in reverse), which we use to
          // build the name of the buffer.
          SmallVector<StringRef, 4> nameComponents;
          while (dc) {
            nameComponents.push_back(cast<Module>(dc)->Name.str());
            dc = dc->getParent();
          }

          for (unsigned i = nameComponents.size(); i; --i) {
            bufferName += nameComponents[i-1];
            bufferName += '.';
          }

          if (auto value = dyn_cast<ValueDecl>(ppDecl)) {
            bufferName += value->getName().str();
          } else if (auto ext = dyn_cast<ExtensionDecl>(ppDecl)) {
            bufferName += ext->getExtendedType().getString();
          }

          // Don't print bodies if we're looking at a top-level decl.
          PrintOptions options;
          if (ActiveDiagnosticDecl->getDeclContext()->isModuleScopeContext())
            options = PrintOptions();
          else
            options = PrintOptions::printEverything();

          // Pretty-print the declaration we've picked.
          llvm::raw_svector_ostream out(buffer);
          TrackingPrinter printer(entries, out);
          ppDecl->print(printer, options);
        }

        // Build a buffer with the pretty-printed declaration.
        auto memBuffer = llvm::MemoryBuffer::getMemBufferCopy(buffer,
                                                              bufferName);
        SourceMgr.addNewSourceBuffer(memBuffer);

        // Go through all of the pretty-printed entries and record their
        // locations.
        for (auto entry : entries) {
          PrettyPrintedDeclarations[entry.first]
            = SourceLoc(llvm::SMLoc::getFromPointer(memBuffer->getBufferStart()
                                                    + entry.second));
        }

        // Grab the pretty-printed location.
        ppLoc = PrettyPrintedDeclarations[ActiveDiagnosticDecl];
      }

      loc = ppLoc;
    }
  }

  // Actually substitute the diagnostic arguments into the diagnostic text.
  llvm::SmallString<256> Text;
  {
    llvm::raw_svector_ostream Out(Text);
    formatDiagnosticText(StoredInfo.Text, ActiveDiagnostic->getArgs(), Out);
  }

  // Pass the diagnostic off to the consumer.
  DiagnosticInfo Info;
  Info.Ranges = ActiveDiagnostic->getRanges();
  Info.FixIts = ActiveDiagnostic->getFixIts();
  for (auto &Consumer : Consumers)
    Consumer->handleDiagnostic(SourceMgr, loc, StoredInfo.Kind, Text, Info);
  
  // Reset the active diagnostic.
  ActiveDiagnostic.reset();
}

