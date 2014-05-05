//===--- Token.h - Token interface ------------------------------*- C++ -*-===//
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
//  This file defines the Token interface.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TOKEN_H
#define SWIFT_TOKEN_H

#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/StringRef.h"

namespace swift {

enum class tok {
  unknown = 0,
  eof,
  code_complete,
  identifier,
  oper_binary,
  oper_postfix,
  oper_prefix,
  dollarident,
  integer_literal,
  floating_literal,
  string_literal,
  character_literal,
  sil_local_name,      // %42 in SIL mode.
  pound_if,
  pound_else,
  pound_elseif,
  pound_endif,
  comment,
  
#define KEYWORD(X) kw_ ## X,
#define PUNCTUATOR(X, Y) X,
#include "swift/Parse/Tokens.def"
  
  NUM_TOKENS
};

/// Token - This structure provides full information about a lexed token.
/// It is not intended to be space efficient, it is intended to return as much
/// information as possible about each returned token.  This is expected to be
/// compressed into a smaller form if memory footprint is important.
///
class Token {
  /// Kind - The actual flavor of token this is.
  ///
  tok Kind;

  /// \brief Whether this token is the first token on the line.
  unsigned AtStartOfLine : 1;

  /// \brief The length of the comment that precedes the token.
  ///
  /// Hopefully 64 Kib is enough.
  unsigned CommentLength : 16;
  
  /// \brief Whether this token is an escaped `identifier` token.
  unsigned EscapedIdentifier : 1;
  
  /// Text - The actual string covered by the token in the source buffer.
  StringRef Text;
  
public:
  Token() : Kind(tok::NUM_TOKENS), AtStartOfLine(false), CommentLength(0),
            EscapedIdentifier(false) {}
  
  tok getKind() const { return Kind; }
  void setKind(tok K) { Kind = K; }
  
  /// is/isNot - Predicates to check if this token is a specific kind, as in
  /// "if (Tok.is(tok::l_brace)) {...}".
  bool is(tok K) const { return Kind == K; }
  bool isNot(tok K) const { return Kind != K; }

  // Predicates to check to see if the token is any of a list of tokens.
  bool isAny(tok K1, tok K2) const {
    return Kind == K1 || Kind == K2;
  }
  bool isAny(tok K1, tok K2, tok K3) const {
    return Kind == K1 || Kind == K2 || Kind == K3;
  }

  // Predicates to check to see if the token is not the same as any of a list.
  bool isNotAny(tok K1, tok K2) const { return !isAny(K1, K2); }
  bool isNotAny(tok K1, tok K2, tok K3) const { return !isAny(K1, K2, K3); }

  bool isAnyOperator() const {
    return Kind == tok::oper_binary ||
           Kind == tok::oper_postfix ||
           Kind == tok::oper_prefix;
  }
  bool isNotAnyOperator() const {
    return !isAnyOperator();
  }

  bool isEllipsis() const {
    return isAnyOperator() && Text == "...";
  }
  bool isNotEllipsis() const {
    return !isEllipsis();
  }

  /// \brief Determine whether this token occurred at the start of a line.
  bool isAtStartOfLine() const { return AtStartOfLine; }

  /// \brief Set whether this token occurred at the start of a line.
  void setAtStartOfLine(bool value) { AtStartOfLine = value; }
  
  /// \brief True if this token is an escaped identifier token.
  bool isEscapedIdentifier() const { return EscapedIdentifier; }
  /// \brief Set whether this token is an escaped identifier token.
  void setEscapedIdentifier(bool value) {
    assert(!value || Kind == tok::identifier &&
           "only identifiers can be escaped identifiers");
    EscapedIdentifier = value;
  }
  
  bool isContextualKeyword(StringRef ContextKW) const {
    return is(tok::identifier) && !isEscapedIdentifier() &&
           Text == ContextKW;
  }
  
  /// Return true if this is a contextual keyword that could be the start of a
  /// decl.
  bool isContextualDeclKeyword() const {
    if (isNot(tok::identifier) || isEscapedIdentifier()) return false;

    return Text == "mutating" || Text == "nonmutating" ||
           Text == "override" || Text == "weak" || Text == "unowned" ||
           Text == "convenience";
  }

  bool isContextualPunctuator(StringRef ContextPunc) const {
    return isAnyOperator() && Text == ContextPunc;
  }

  /// True if the token is an identifier or '_'.
  bool isIdentifierOrNone() const {
    return is(tok::identifier) || is(tok::kw__);
  }

  /// True if the token is an l_paren token that does not start a new line.
  bool isFollowingLParen() const {
    return !isAtStartOfLine() && Kind == tok::l_paren;
  }
  
  /// True if the token is an l_square token that does not start a new line.
  bool isFollowingLSquare() const {
    return !isAtStartOfLine() && Kind == tok::l_square;
  }

  /// True if the token is an l_brace token that does not start a new line.
  bool isFollowingLBrace() const {
    return !isAtStartOfLine() && Kind == tok::l_brace;
  }

  /// True if the token is any keyword.
  bool isKeyword() const {
    switch (Kind) {
#define KEYWORD(X) case tok::kw_##X: return true;
#include "swift/Parse/Tokens.def"
    default: return false;
    }
  }
  
  /// getLoc - Return a source location identifier for the specified
  /// offset in the current file.
  SourceLoc getLoc() const {
    return SourceLoc(llvm::SMLoc::getFromPointer(Text.begin()));
  }

  unsigned getLength() const { return Text.size(); }

  CharSourceRange getRange() const {
    return CharSourceRange(getLoc(), getLength());
  }

  bool hasComment() const {
    return CommentLength != 0;
  }

  CharSourceRange getCommentRange() const {
    return CharSourceRange(
        SourceLoc(llvm::SMLoc::getFromPointer(Text.begin() - CommentLength)),
        CommentLength);
  }

  StringRef getText() const { return Text; }
  void setText(StringRef T) { Text = T; }

  /// \brief Set the token to the specified kind and source range.
  void setToken(tok K, StringRef T, unsigned CommentLength = 0) {
    Kind = K;
    Text = T;
    this->CommentLength = CommentLength;
    EscapedIdentifier = false;
  }
};
  
} // end namespace swift


namespace llvm {
  template <typename T> struct isPodLike;
  template <>
  struct isPodLike<swift::Token> { static const bool value = true; };
}  // end namespace llvm

#endif
