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
//  This file declares the DiagnosticEngine class, which manages any diagnostics
//  emitted by Swift.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_DIAGNOSTICENGINE_H
#define SWIFT_BASIC_DIAGNOSTICENGINE_H

#include "swift/Basic/LLVM.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeLoc.h"
#include "swift/Basic/DiagnosticConsumer.h"
#include "swift/Basic/Optional.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <utility>

namespace swift {
  class Decl;
  class DiagnosticEngine;
  class SourceManager;
  
  enum class PatternKind : uint8_t;
  enum class StaticSpellingKind : uint8_t;
  enum class DescriptiveDeclKind : uint8_t;

  /// \brief Enumeration describing all of possible diagnostics.
  ///
  /// Each of the diagnostics described in Diagnostics.def has an entry in
  /// this enumeration type that uniquely identifies it.
  enum class DiagID : uint32_t;

  /// \brief Describes a diagnostic along with its argument types.
  ///
  /// The diagnostics header introduces instances of this type for each 
  /// diagnostic, which provide both the set of argument types (used to
  /// check/convert the arguments at each call site) and the diagnostic ID
  /// (for other information about the diagnostic).
  template<typename ...ArgTypes>
  struct Diag {
    /// \brief The diagnostic ID corresponding to this diagnostic.
    DiagID ID;
  };

  namespace detail {
    /// \brief Describes how to pass a diagnostic argument of the given type.
    ///
    /// By default, diagnostic arguments are passed by value, because they
    /// tend to be small. Larger diagnostic arguments
    /// need to specialize this class template to pass by reference.
    template<typename T>
    struct PassArgument {
      typedef T type;
    };
  }
    
  /// \brief Describes the kind of diagnostic argument we're storing.
  ///
  enum class DiagnosticArgumentKind {
    String,
    Integer,
    Unsigned,
    Identifier,
    Type,
    TypeRepr,
    PatternKind,
    StaticSpellingKind,
    DescriptiveDeclKind,
  };

  namespace diag {
    enum class RequirementKind : uint8_t;
  }

  /// \brief Variant type that holds a single diagnostic argument of a known
  /// type.
  ///
  /// All diagnostic arguments are converted to an instance of this class.
  class DiagnosticArgument {
    DiagnosticArgumentKind Kind;
    union {
      int IntegerVal;
      unsigned UnsignedVal;
      StringRef StringVal;
      DeclName IdentifierVal;
      Type TypeVal;
      TypeRepr *TyR;
      PatternKind PatternKindVal;
      StaticSpellingKind StaticSpellingKindVal;
      DescriptiveDeclKind DescriptiveDeclKindVal;
    };
    
  public:
    DiagnosticArgument(StringRef S)
      : Kind(DiagnosticArgumentKind::String), StringVal(S) {
    }

    DiagnosticArgument(int I) 
      : Kind(DiagnosticArgumentKind::Integer), IntegerVal(I) {
    }

    DiagnosticArgument(unsigned I) 
      : Kind(DiagnosticArgumentKind::Unsigned), UnsignedVal(I) {
    }

    DiagnosticArgument(DeclName I)
      : Kind(DiagnosticArgumentKind::Identifier), IdentifierVal(I) {
    }
    
    DiagnosticArgument(Identifier I)
      : Kind(DiagnosticArgumentKind::Identifier), IdentifierVal(I) {
    }
    
    DiagnosticArgument(Type T)
      : Kind(DiagnosticArgumentKind::Type), TypeVal(T) {
    }

    DiagnosticArgument(TypeRepr *T)
      : Kind(DiagnosticArgumentKind::TypeRepr), TyR(T) {
    }

    DiagnosticArgument(const TypeLoc &TL) {
      if (TypeRepr *tyR = TL.getTypeRepr()) {
        Kind = DiagnosticArgumentKind::TypeRepr;
        TyR = tyR;
      } else {
        Kind = DiagnosticArgumentKind::Type;
        TypeVal = TL.getType();
      }
    }

    DiagnosticArgument(PatternKind K)
        : Kind(DiagnosticArgumentKind::PatternKind), PatternKindVal(K) {}

    DiagnosticArgument(StaticSpellingKind SSK)
        : Kind(DiagnosticArgumentKind::StaticSpellingKind),
          StaticSpellingKindVal(SSK) {}

    DiagnosticArgument(DescriptiveDeclKind DDK)
        : Kind(DiagnosticArgumentKind::DescriptiveDeclKind),
          DescriptiveDeclKindVal(DDK) {}

    DiagnosticArgument(diag::RequirementKind kind)
      : DiagnosticArgument(static_cast<uint8_t>(kind)) { }

    DiagnosticArgumentKind getKind() const { return Kind; }

    StringRef getAsString() const {
      assert(Kind == DiagnosticArgumentKind::String);
      return StringVal;
    }

    int getAsInteger() const {
      assert(Kind == DiagnosticArgumentKind::Integer);
      return IntegerVal;
    }

    unsigned getAsUnsigned() const {
      assert(Kind == DiagnosticArgumentKind::Unsigned);
      return UnsignedVal;
    }

    DeclName getAsIdentifier() const {
      assert(Kind == DiagnosticArgumentKind::Identifier);
      return IdentifierVal;
    }

    Type getAsType() const {
      assert(Kind == DiagnosticArgumentKind::Type);
      return TypeVal;
    }

    TypeRepr *getAsTypeRepr() const {
      assert(Kind == DiagnosticArgumentKind::TypeRepr);
      return TyR;
    }
    
    PatternKind getAsPatternKind() const {
      assert(Kind == DiagnosticArgumentKind::PatternKind);
      return PatternKindVal;
    }

    StaticSpellingKind getAsStaticSpellingKind() const {
      assert(Kind == DiagnosticArgumentKind::StaticSpellingKind);
      return StaticSpellingKindVal;
    }

    DescriptiveDeclKind getAsDescriptiveDeclKind() const {
      assert(Kind == DiagnosticArgumentKind::DescriptiveDeclKind);
      return DescriptiveDeclKindVal;
    }
  };
  
  /// Diagnostic - This is a specific instance of a diagnostic along with all of
  /// the DiagnosticArguments that it requires. 
  class Diagnostic {
  public:
    typedef DiagnosticInfo::FixIt FixIt;

  private:
    DiagID ID;
    SmallVector<DiagnosticArgument, 3> Args;
    SmallVector<CharSourceRange, 2> Ranges;
    SmallVector<FixIt, 2> FixIts;

  public:
    // All constructors are intentionally implicit.
    template<typename ...ArgTypes>
    Diagnostic(Diag<ArgTypes...> ID,
               typename detail::PassArgument<ArgTypes>::type... VArgs)
      : ID(ID.ID) {
      DiagnosticArgument DiagArgs[] = { 
        DiagnosticArgument(0), std::move(VArgs)... 
      };
      Args.append(DiagArgs + 1, DiagArgs + 1 + sizeof...(VArgs));
    }

    /*implicit*/Diagnostic(DiagID ID, ArrayRef<DiagnosticArgument> Args)
      : ID(ID), Args(Args.begin(), Args.end()) {}
    
    // Accessors.
    DiagID getID() const { return ID; }
    ArrayRef<DiagnosticArgument> getArgs() const { return Args; }
    ArrayRef<CharSourceRange> getRanges() const { return Ranges; }
    ArrayRef<FixIt> getFixIts() const { return FixIts; }

    /// Returns true if this object represents a particular diagnostic.
    ///
    /// \code
    /// someDiag.is(diag::invalid_diagnostic)
    /// \endcode
    template<typename ...OtherArgTypes>
    bool is(Diag<OtherArgTypes...> Other) const {
      return ID == Other.ID;
    }

    void addRange(CharSourceRange R) {
      Ranges.push_back(R);
    }

    // Avoid copying the fix-it text more than necessary.
    void addFixIt(FixIt &&F) {
      FixIts.push_back(std::move(F));
    }
  };
  
  /// \brief Describes an in-flight diagnostic, which is currently active
  /// within the diagnostic engine and can be augmented within additional
  /// information (source ranges, Fix-Its, etc.).
  ///
  /// Only a single in-flight diagnostic can be active at one time, and all
  /// additional information must be emitted through the active in-flight
  /// diagnostic.
  class InFlightDiagnostic {
    friend class DiagnosticEngine;
    
    DiagnosticEngine *Engine;
    bool IsActive;
    
    /// \brief Create a new in-flight diagnostic. 
    ///
    /// This constructor is only available to the DiagnosticEngine.
    InFlightDiagnostic(DiagnosticEngine &Engine)
      : Engine(&Engine), IsActive(true) { }
    
    InFlightDiagnostic(const InFlightDiagnostic &) = delete;
    InFlightDiagnostic &operator=(const InFlightDiagnostic &) = delete;
    InFlightDiagnostic &operator=(InFlightDiagnostic &&) = delete;

  public:
    /// \brief Create an active but unattached in-flight diagnostic.
    /// 
    /// The resulting diagnostic can be used as a dummy, accepting the
    /// syntax to add additional information to a diagnostic without
    /// actually emitting a diagnostic.
    InFlightDiagnostic() : Engine(0), IsActive(true) { }
    
    /// \brief Transfer an in-flight diagnostic to a new object, which is
    /// typically used when returning in-flight diagnostics.
    InFlightDiagnostic(InFlightDiagnostic &&Other)
      : Engine(Other.Engine), IsActive(Other.IsActive) {
      Other.IsActive = false;
    }
    
    ~InFlightDiagnostic() {
      if (IsActive)
        flush();
    }
    
    /// \brief Flush the active diagnostic to the diagnostic output engine.
    void flush();

    /// \brief Add a token-based range to the currently-active diagnostic.
    InFlightDiagnostic &highlight(SourceRange R);

    /// \brief Add a character-based range to the currently-active diagnostic.
    InFlightDiagnostic &highlightChars(SourceLoc Start, SourceLoc End);

    /// \brief Add a token-based replacement fix-it to the currently-active
    /// diagnostic.
    InFlightDiagnostic &fixItReplace(SourceRange R, StringRef Str);

    /// \brief Add a character-based replacement fix-it to the currently-active
    /// diagnostic.
    InFlightDiagnostic &fixItReplaceChars(SourceLoc Start, SourceLoc End,
                                          StringRef Str);

    /// \brief Add an insertion fix-it to the currently-active diagnostic.
    InFlightDiagnostic &fixItInsert(SourceLoc L, StringRef Str) {
      return fixItReplaceChars(L, L, Str);
    }

    /// \brief Add a token-based removal fix-it to the currently-active
    /// diagnostic.
    InFlightDiagnostic &fixItRemove(SourceRange R) {
      return fixItReplace(R, {});
    }

    /// \brief Add a token-based removal fix-it to the currently-active
    /// diagnostic.
    InFlightDiagnostic &fixItRemoveChars(SourceLoc Start, SourceLoc End) {
      return fixItReplaceChars(Start, End, {});
    }
  };
    
  /// \brief Class responsible for formatting diagnostics and presenting them
  /// to the user.
  class DiagnosticEngine {
    /// \brief The source manager used to interpret source locations and
    /// display diagnostics.
    SourceManager &SourceMgr;

    /// \brief The diagnostic consumer(s) that will be responsible for actually
    /// emitting diagnostics.
    SmallVector<DiagnosticConsumer *, 2> Consumers;

    /// HadAnyError - True if any error diagnostics have been emitted.
    bool HadAnyError;

    /// \brief The declaration of the currently active diagnostic, if there is
    /// one.
    Decl *ActiveDiagnosticDecl = nullptr;

    /// \brief The source location of the currently active diagnostic, if there
    /// is one.
    SourceLoc ActiveDiagnosticLoc;
    
    /// \brief The currently active diagnostic, if there is one.
    Optional<Diagnostic> ActiveDiagnostic;

    /// \brief The set of declarations for which we have pretty-printed
    /// results that we can point to on the command line.
    llvm::DenseMap<const Decl *, SourceLoc> PrettyPrintedDeclarations;

    friend class InFlightDiagnostic;
    
  public:
    explicit DiagnosticEngine(SourceManager &SourceMgr)
      : SourceMgr(SourceMgr), HadAnyError(false), ActiveDiagnostic() {
    }

    /// hadAnyError - return true if any *error* diagnostics have been emitted.
    bool hadAnyError() const {
      return HadAnyError;
    }

    void resetHadAnyError() {
      HadAnyError = false;
    }

    /// \brief Add an additional DiagnosticConsumer to receive diagnostics.
    void addConsumer(DiagnosticConsumer &Consumer) {
      Consumers.push_back(&Consumer);
    }

    /// \brief Remove and return all \c DiagnosticConsumers.
    std::vector<DiagnosticConsumer *> takeConsumers() {
      auto Result = std::vector<DiagnosticConsumer*>(Consumers.begin(),
                                                     Consumers.end());
      Consumers.clear();
      return Result;
    }

    /// \brief Emit a diagnostic using a preformatted array of diagnostic
    /// arguments.
    ///
    /// \param Loc The location to which the diagnostic refers in the source
    /// code.
    ///
    /// \param ID The diagnostic ID.
    ///
    /// \param Args The preformatted set of diagnostic arguments. The caller
    /// must ensure that the diagnostic arguments have the appropriate type.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(SourceLoc Loc, DiagID ID, 
                                ArrayRef<DiagnosticArgument> Args) {
      assert(!ActiveDiagnostic && "Already have an active diagnostic");
      ActiveDiagnosticLoc = Loc;
      ActiveDiagnosticDecl = nullptr;
      ActiveDiagnostic = Diagnostic(ID, Args);
      return InFlightDiagnostic(*this);
    }

    /// \brief Emit an already-constructed diagnostic at the given location.
    ///
    /// \param Loc The location to which the diagnostic refers in the source
    /// code.
    ///
    /// \param D The diagnostic.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(SourceLoc Loc, const Diagnostic &D) {
      assert(!ActiveDiagnostic && "Already have an active diagnostic");
      ActiveDiagnosticLoc = Loc;
      ActiveDiagnosticDecl = nullptr;
      ActiveDiagnostic = D;
      return InFlightDiagnostic(*this);
    }
    
    /// \brief Emit a diagnostic with the given set of diagnostic arguments.
    ///
    /// \param Loc The location to which the diagnostic refers in the source
    /// code.
    ///
    /// \param ID The diagnostic to be emitted.
    ///
    /// \param Args The diagnostic arguments, which will be converted to
    /// the types expected by the diagnostic \p ID.
    template<typename ...ArgTypes>
    InFlightDiagnostic 
    diagnose(SourceLoc Loc, Diag<ArgTypes...> ID,
             typename detail::PassArgument<ArgTypes>::type... Args) {
      assert(!ActiveDiagnostic && "Already have an active diagnostic");
      ActiveDiagnosticLoc = Loc;
      ActiveDiagnosticDecl = nullptr;
      ActiveDiagnostic = Diagnostic(ID, std::move(Args)...);
      return InFlightDiagnostic(*this);
    }

    /// \brief Emit a diagnostic using a preformatted array of diagnostic
    /// arguments.
    ///
    /// \param decl The declaration to which this diagnostic refers, which
    /// may or may not have associated source-location information.
    ///
    /// \param id The diagnostic ID.
    ///
    /// \param args The preformatted set of diagnostic arguments. The caller
    /// must ensure that the diagnostic arguments have the appropriate type.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(Decl *decl, DiagID id,
                                ArrayRef<DiagnosticArgument> args) {
      assert(!ActiveDiagnostic && "Already have an active diagnostic");
      ActiveDiagnosticLoc = SourceLoc();
      ActiveDiagnosticDecl = decl;
      ActiveDiagnostic = Diagnostic(id, args);
      return InFlightDiagnostic(*this);
    }

    /// \brief Emit an already-constructed diagnostic referencing the given
    /// declaration.
    ///
    /// \param decl The declaration to which this diagnostic refers, which
    /// may or may not have associated source-location information.
    ///
    /// \param diag The diagnostic.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(Decl *decl, const Diagnostic &diag) {
      assert(!ActiveDiagnostic && "Already have an active diagnostic");
      ActiveDiagnosticLoc = SourceLoc();
      ActiveDiagnosticDecl = decl;
      ActiveDiagnostic = diag;
      return InFlightDiagnostic(*this);
    }

    /// \brief Emit a diagnostic with the given set of diagnostic arguments.
    ///
    /// \param decl The declaration to which this diagnostic refers, which
    /// may or may not have associated source-location information.
    ///
    /// \param id The diagnostic to be emitted.
    ///
    /// \param args The diagnostic arguments, which will be converted to
    /// the types expected by the diagnostic \p ID.
    template<typename ...ArgTypes>
    InFlightDiagnostic
    diagnose(Decl *decl, Diag<ArgTypes...> id,
             typename detail::PassArgument<ArgTypes>::type... args) {
      ActiveDiagnosticLoc = SourceLoc();
      ActiveDiagnosticDecl = decl;
      ActiveDiagnostic = Diagnostic(id, std::move(args)...);
      return InFlightDiagnostic(*this);
    }

    /// \returns true if diagnostic is marked with PointsToFirstBadToken
    /// option.
    bool isDiagnosticPointsToFirstBadToken(DiagID ID) const;

  private:
    /// \brief Flush the active diagnostic.
    void flushActiveDiagnostic();
    
    /// \brief Retrieve the active diagnostic.
    Diagnostic &getActiveDiagnostic() { return *ActiveDiagnostic; }
  };
} // end namespace swift

#endif
