//===--- ParsePattern.cpp - Swift Language Parser for Patterns ------------===//
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
// Pattern Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ExprHandle.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SaveAndRestore.h"
using namespace swift;

/// \brief Determine the kind of a default argument given a parsed
/// expression that has not yet been type-checked.
static DefaultArgumentKind getDefaultArgKind(ExprHandle *init) {
  if (!init || !init->getExpr())
    return DefaultArgumentKind::None;

  auto magic = dyn_cast<MagicIdentifierLiteralExpr>(init->getExpr());
  if (!magic)
    return DefaultArgumentKind::Normal;

  switch (magic->getKind()) {
  case MagicIdentifierLiteralExpr::Column:
    return DefaultArgumentKind::Column;
  case MagicIdentifierLiteralExpr::File:
    return DefaultArgumentKind::File;
  case MagicIdentifierLiteralExpr::Line:
    return DefaultArgumentKind::Line;
  case MagicIdentifierLiteralExpr::Function:
    return DefaultArgumentKind::Function;
  }
}

static void recoverFromBadSelectorArgument(Parser &P) {
  while (P.Tok.isNot(tok::eof) && P.Tok.isNot(tok::r_paren) &&
         P.Tok.isNot(tok::l_brace) && P.Tok.isNot(tok::r_brace) &&
         !P.isStartOfStmt() && !P.isStartOfDecl()) {
    P.skipSingle();
  }
  P.consumeIf(tok::r_paren);
}

void Parser::DefaultArgumentInfo::setFunctionContext(DeclContext *DC) {
  assert(DC->isLocalContext());
  for (auto context : ParsedContexts) {
    context->changeFunction(DC);
  }
}

static ParserStatus parseDefaultArgument(Parser &P,
                                   Parser::DefaultArgumentInfo *defaultArgs,
                                   unsigned argIndex,
                                   ExprHandle *&init) {
  SourceLoc equalLoc = P.consumeToken(tok::equal);

  // Enter a fresh default-argument context with a meaningless parent.
  // We'll change the parent to the function later after we've created
  // that declaration.
  auto initDC =
    P.Context.createDefaultArgumentContext(P.CurDeclContext, argIndex);
  Parser::ParseFunctionBody initScope(P, initDC);

  ParserResult<Expr> initR = P.parseExpr(diag::expected_init_value);

  // Give back the default-argument context if we didn't need it.
  if (!initScope.hasClosures()) {
    P.Context.destroyDefaultArgumentContext(initDC);

  // Otherwise, record it if we're supposed to accept default
  // arguments here.
  } else if (defaultArgs) {
    defaultArgs->ParsedContexts.push_back(initDC);
  }

  if (!defaultArgs) {
    auto inFlight = P.diagnose(equalLoc, diag::non_func_decl_pattern_init);
    if (initR.isNonNull())
      inFlight.fixItRemove(SourceRange(equalLoc, initR.get()->getEndLoc()));
  } else {
    defaultArgs->HasDefaultArgument = true;
  }

  if (initR.hasCodeCompletion()) {
    recoverFromBadSelectorArgument(P);
    return makeParserCodeCompletionStatus();
  }
  if (initR.isNull()) {
    recoverFromBadSelectorArgument(P);
    return makeParserError();
  }

  init = ExprHandle::get(P.Context, initR.get());
  return ParserStatus();
}

/// Determine whether we are at the start of a parameter name when
/// parsing a parameter.
static bool startsParameterName(Parser &parser, bool isClosure) {
  // '_' cannot be a type, so it must be a parameter name.
  if (parser.Tok.is(tok::kw__))
    return true;

  // To have a parameter name here, we need a name.
  if (!parser.Tok.is(tok::identifier))
    return false;

  // If the next token is another identifier, '_', or ':', this is a name.
  auto nextToken = parser.peekToken();
  if (nextToken.isIdentifierOrNone() || nextToken.is(tok::colon))
    return true;

  // The identifier could be a name or it could be a type. In a closure, we
  // assume it's a name, because the type can be inferred. Elsewhere, we
  // assume it's a type.
  return isClosure;
}

ParserStatus
Parser::parseParameterClause(SourceLoc &leftParenLoc,
                             SmallVectorImpl<ParsedParameter> &params,
                             SourceLoc &rightParenLoc,
                             DefaultArgumentInfo *defaultArgs,
                             ParameterContextKind paramContext) {
  assert(params.empty() && leftParenLoc.isInvalid() &&
         rightParenLoc.isInvalid() && "Must start with empty state");

  // Consume the starting '(';
  leftParenLoc = consumeToken(tok::l_paren);

  // Trivial case: empty parameter list.
  if (Tok.is(tok::r_paren)) {
    rightParenLoc = consumeToken(tok::r_paren);
    return ParserStatus();
  }

  // Parse the parameter list.
  bool isClosure = paramContext == ParameterContextKind::Closure;
  return parseList(tok::r_paren, leftParenLoc, rightParenLoc, tok::comma,
                      /*OptionalSep=*/false, /*AllowSepAfterLast=*/false,
                      diag::expected_rparen_parameter,
                      [&]() -> ParserStatus {
    ParsedParameter param;
    ParserStatus status;
    SourceLoc StartLoc = Tok.getLoc();

    unsigned defaultArgIndex = defaultArgs? defaultArgs->NextIndex++ : 0;

    // 'inout'?
    if (Tok.isContextualKeyword("inout"))
      param.InOutLoc = consumeToken();

    // ('let' | 'var')?
    if (Tok.is(tok::kw_let)) {
      param.LetVarLoc = consumeToken();
      param.IsLet = true;
    } else if (Tok.is(tok::kw_var)) {
      param.LetVarLoc = consumeToken();
      param.IsLet = false;
    }

    // '`'?
    if (Tok.is(tok::backtick)) {
      param.BackTickLoc = consumeToken(tok::backtick);
    }

    if (param.BackTickLoc.isValid() || startsParameterName(*this, isClosure)) {
      // identifier-or-none for the first name
      if (Tok.is(tok::identifier)) {
        param.FirstName = Context.getIdentifier(Tok.getText());
        param.FirstNameLoc = consumeToken();

        // Operators can not have API names.
        if (paramContext == ParameterContextKind::Operator &&
            param.BackTickLoc.isValid()) {
          diagnose(param.BackTickLoc, 
                   diag::parameter_operator_keyword_argument)
            .fixItRemove(param.BackTickLoc);
          param.BackTickLoc = SourceLoc();
        }
      } else if (Tok.is(tok::kw__)) {
        // A back-tick cannot precede an empty name marker.
        if (param.BackTickLoc.isValid()) {
          diagnose(Tok, diag::parameter_backtick_empty_name)
            .fixItRemove(param.BackTickLoc);
          param.BackTickLoc = SourceLoc();
        }

        param.FirstNameLoc = consumeToken();
      } else {
        assert(param.BackTickLoc.isValid() && "startsParameterName() lied");
        diagnose(Tok, diag::parameter_backtick_missing_name);
        param.FirstNameLoc = param.BackTickLoc;
        param.BackTickLoc = SourceLoc();
      }

      // identifier-or-none? for the second name
      if (Tok.is(tok::identifier)) {
        param.SecondName = Context.getIdentifier(Tok.getText());
        param.SecondNameLoc = consumeToken();
      } else if (Tok.is(tok::kw__)) {
        param.SecondNameLoc = consumeToken();
      }

      // Operators can not have API names.
      if (paramContext == ParameterContextKind::Operator &&
          param.SecondNameLoc.isValid()) {
        diagnose(param.FirstNameLoc, 
                 diag::parameter_operator_keyword_argument)
          .fixItRemoveChars(param.FirstNameLoc, param.SecondNameLoc);
        param.FirstName = param.SecondName;
        param.FirstNameLoc = param.SecondNameLoc;
        param.SecondName = Identifier();
        param.SecondNameLoc = SourceLoc();
      }

      // Cannot have a back-tick and two names.
      if (param.BackTickLoc.isValid() && param.SecondNameLoc.isValid()) {
        diagnose(param.BackTickLoc, diag::parameter_backtick_two_names)
          .fixItRemove(param.BackTickLoc);
        param.BackTickLoc = SourceLoc();
      }

      // If we have two equivalent names, suggest using the back-tick.
      if (param.FirstNameLoc.isValid() && param.SecondNameLoc.isValid() &&
          param.FirstName == param.SecondName) {
        StringRef name;
        if (param.FirstName.empty())
          name = "_";
        else
          name = param.FirstName.str();

        SourceLoc afterFirst = Lexer::getLocForEndOfToken(Context.SourceMgr,
                                                          param.FirstNameLoc);
        diagnose(param.FirstNameLoc, diag::parameter_two_equivalent_names,
                 name)
          .fixItInsert(param.FirstNameLoc, "`")
          .fixItRemove(SourceRange(afterFirst, param.SecondNameLoc));
      }

      // (':' type)?
      if (Tok.is(tok::colon)) {
        param.ColonLoc = consumeToken();
        auto type = parseType(diag::expected_parameter_type);
        status |= type;
        param.Type = type.getPtrOrNull();
      }
    } else {
      auto type = parseType(diag::expected_parameter_type);
      status |= type;
      param.Type = type.getPtrOrNull();
    }

    // '...'?
    if (Tok.isEllipsis()) {
      param.EllipsisLoc = consumeToken();
    }

    // ('=' expr)?
    if (Tok.is(tok::equal)) {
      param.EqualLoc = Tok.getLoc();
      status |= parseDefaultArgument(*this, defaultArgs, defaultArgIndex,
                                     param.DefaultArg);

      // A default argument implies that the name is API, making the
      // back-tick redundant.
      if (param.BackTickLoc.isValid()) {
        diagnose(param.BackTickLoc, diag::parameter_backtick_default_arg)
          .fixItRemove(param.BackTickLoc);
        param.BackTickLoc = SourceLoc();
      }

      if (param.EllipsisLoc.isValid()) {
        // The range of the complete default argument.
        SourceRange defaultArgRange;
        if (param.DefaultArg) {
          if (auto init = param.DefaultArg->getExpr()) {
            defaultArgRange = SourceRange(param.EllipsisLoc, init->getEndLoc());
          }
        }

        diagnose(param.EqualLoc, diag::parameter_vararg_default)
          .highlight(param.EllipsisLoc)
          .fixItRemove(defaultArgRange);
      }
    }

    // If we haven't made progress, don't add the param.
    if (Tok.getLoc() == StartLoc)
      return status;

    params.push_back(param);
    return status;
  });
}

/// Map parsed parameters to argument and body patterns.
///
/// \returns the pattern describing the parsed parameters.
static Pattern*
mapParsedParameters(Parser &parser,
                    SourceLoc leftParenLoc,
                    MutableArrayRef<Parser::ParsedParameter> params,
                    SourceLoc rightParenLoc,
                    bool isFirstParameterClause,
                    SmallVectorImpl<Identifier> *argNames,
                    Parser::ParameterContextKind paramContext) {
  auto &ctx = parser.Context;

  // Local function to create a pattern for a single parameter.
  auto createParamPattern = [&](SourceLoc &inOutLoc, bool isLet,
                                SourceLoc letVarLoc,
                                Identifier argName, SourceLoc argNameLoc,
                                Identifier paramName, SourceLoc paramNameLoc,
                                TypeRepr *type) -> Pattern * {
    // Create the parameter based on the name.
    Pattern *param;
    if (paramName.empty()) {
      if (paramNameLoc.isInvalid())
        paramNameLoc = letVarLoc;
      param = new (ctx) AnyPattern(paramNameLoc);
    } else {
      // Create a variable to capture this.
      ParamDecl *var = new (ctx) ParamDecl(isLet, argNameLoc, argName,
                                           paramNameLoc, paramName, Type(), 
                                           parser.CurDeclContext);
      param = new (ctx) NamedPattern(var);
    }

    // If a type was provided, create the typed pattern.
    if (type) {
      // If 'inout' was specified, turn the type into an in-out type.
      if (inOutLoc.isValid()) {
        type = new (ctx) InOutTypeRepr(type, inOutLoc);
      }

      param = new (ctx) TypedPattern(param, type);
    } else if (inOutLoc.isValid()) {
      parser.diagnose(inOutLoc, diag::inout_must_have_type);
      inOutLoc = SourceLoc();
    }

    // If 'var' or 'let' was specified explicitly, create a pattern for it.
    if (letVarLoc.isValid()) {
      if (inOutLoc.isValid()) {
        parser.diagnose(inOutLoc, diag::inout_varpattern);
        inOutLoc = SourceLoc();
      } else {
        param = new (ctx) VarPattern(letVarLoc, param);
      }
    }

    return param;
  };

  // Collect the elements of the tuple patterns for argument and body
  // parameters.
  SmallVector<TuplePatternElt, 4> elements;
  SourceLoc ellipsisLoc;
  bool isFirstParameter = true;
  for (auto &param : params) {
    // Whether the provided name is API by default depends on the parameter
    // context.
    bool isKeywordArgumentByDefault;
    switch (paramContext) {
    case Parser::ParameterContextKind::Function:
    case Parser::ParameterContextKind::Closure:
    case Parser::ParameterContextKind::Subscript:
    case Parser::ParameterContextKind::Operator:
      isKeywordArgumentByDefault = false;
      break;

    case Parser::ParameterContextKind::Initializer:
      isKeywordArgumentByDefault = true;
      break;

    case Parser::ParameterContextKind::Method:
      isKeywordArgumentByDefault = !isFirstParameter;
      break;
    }

    // The presence of a default argument implies that this argument
    // is a keyword argument.
    if (param.DefaultArg)
      isKeywordArgumentByDefault = true;

    // Create the pattern.
    Pattern *pattern;
    Identifier argName;
    if (param.SecondNameLoc.isValid()) {
      // Both names were provided, so pass them in directly.
      pattern = createParamPattern(param.InOutLoc,
                                   param.IsLet, param.LetVarLoc,
                                   param.FirstName, param.FirstNameLoc,
                                   param.SecondName, param.SecondNameLoc,
                                   param.Type);

      argName = param.FirstName;

      // If the first name is empty and this parameter would not have been
      // an API name by default, complain.
      if (param.FirstName.empty() && !isKeywordArgumentByDefault) {
        parser.diagnose(param.FirstNameLoc,
                        diag::parameter_extraneous_empty_name,
                        param.SecondName)
          .fixItRemoveChars(param.FirstNameLoc, param.SecondNameLoc);

        param.FirstNameLoc = SourceLoc();
      }
    } else {
      // If it's an API name by default, or there was a back-tick, we have an
      // API name.
      if (isKeywordArgumentByDefault || param.BackTickLoc.isValid()) {
        argName = param.FirstName;

        // If both are true, warn that the back-tick is unnecessary.
        if (isKeywordArgumentByDefault && param.BackTickLoc.isValid()) {
          parser.diagnose(param.BackTickLoc,
                          diag::parameter_extraneous_backtick, argName)
            .fixItRemove(param.BackTickLoc);
        }
      }

      pattern = createParamPattern(param.InOutLoc,
                                   param.IsLet, param.LetVarLoc,
                                   argName, SourceLoc(),
                                   param.FirstName, param.FirstNameLoc,
                                   param.Type);
    }

    // If this parameter had an ellipsis, check whether it's the last parameter.
    if (param.EllipsisLoc.isValid()) {
      if (&param != &params.back()) {
        parser.diagnose(param.EllipsisLoc, diag::parameter_ellipsis_not_at_end)
          .fixItRemove(param.EllipsisLoc);
        param.EllipsisLoc = SourceLoc();
      } else {
        ellipsisLoc = param.EllipsisLoc;
      }
    }

    // Default arguments are only permitted on the first parameter clause.
    if (param.DefaultArg && !isFirstParameterClause) {
      parser.diagnose(param.EqualLoc, diag::non_func_decl_pattern_init)
        .fixItRemove(SourceRange(param.EqualLoc,
                                 param.DefaultArg->getExpr()->getEndLoc()));
    }

    // Create the tuple pattern elements.
    auto defArgKind = getDefaultArgKind(param.DefaultArg);
    elements.push_back(TuplePatternElt(pattern, param.DefaultArg, defArgKind));

    if (argNames)
      argNames->push_back(argName);

    isFirstParameter = false;
  }

  return TuplePattern::createSimple(ctx, leftParenLoc, elements,
                                    rightParenLoc, ellipsisLoc.isValid(),
                                    ellipsisLoc);
}

/// Parse a single parameter-clause.
ParserResult<Pattern> Parser::parseSingleParameterClause(
                                ParameterContextKind paramContext) {
  ParserStatus status;
  SmallVector<ParsedParameter, 4> params;
  SourceLoc leftParenLoc, rightParenLoc;
  
  // Parse the parameter clause.
  status |= parseParameterClause(leftParenLoc, params, rightParenLoc,
                                 /*defaultArgs=*/nullptr, paramContext);
  
  // Turn the parameter clause into argument and body patterns.
  auto pattern = mapParsedParameters(*this, leftParenLoc, params,
                                     rightParenLoc, true, nullptr,
                                     paramContext);

  return makeParserResult(status, pattern);
}

/// Parse function arguments.
///   func-arguments:
///     curried-arguments | selector-arguments
///   curried-arguments:
///     parameter-clause+
///   selector-arguments:
///     '(' selector-element ')' (identifier '(' selector-element ')')+
///   selector-element:
///      identifier '(' pattern-atom (':' type)? ('=' expr)? ')'
///
ParserStatus
Parser::parseFunctionArguments(SmallVectorImpl<Identifier> &NamePieces,
                               SmallVectorImpl<Pattern *> &BodyPatterns,
                               ParameterContextKind paramContext,
                               DefaultArgumentInfo &DefaultArgs) {
  // Parse parameter-clauses.
  ParserStatus status;
  bool isFirstParameterClause = true;
  while (Tok.is(tok::l_paren)) {
    SmallVector<ParsedParameter, 4> params;
    SourceLoc leftParenLoc, rightParenLoc;

    // Parse the parameter clause.
    status |= parseParameterClause(leftParenLoc, params, rightParenLoc,
                                   &DefaultArgs, paramContext);

    // Turn the parameter clause into argument and body patterns.
    auto pattern = mapParsedParameters(*this, leftParenLoc, params,
                                       rightParenLoc, 
                                       isFirstParameterClause,
                                       isFirstParameterClause ? &NamePieces
                                                              : nullptr,
                                       paramContext);
    BodyPatterns.push_back(pattern);
    isFirstParameterClause = false;
  }

  return status;
}

/// Parse a function definition signature.
///   func-signature:
///     func-arguments func-signature-result?
///   func-signature-result:
///     '->' type
///
/// Note that this leaves retType as null if unspecified.
ParserStatus
Parser::parseFunctionSignature(Identifier SimpleName,
                               DeclName &FullName,
                               SmallVectorImpl<Pattern *> &bodyPatterns,
                               DefaultArgumentInfo &defaultArgs,
                               TypeRepr *&retType) {
  SmallVector<Identifier, 4> NamePieces;
  NamePieces.push_back(SimpleName);
  FullName = SimpleName;
  
  ParserStatus Status;
  // We force first type of a func declaration to be a tuple for consistency.
  if (Tok.is(tok::l_paren)) {
    ParameterContextKind paramContext;
    if (SimpleName.isOperator())
      paramContext = ParameterContextKind::Operator;
    else if (CurDeclContext->isTypeContext())
      paramContext = ParameterContextKind::Method;
    else
      paramContext = ParameterContextKind::Function;

    Status = parseFunctionArguments(NamePieces, bodyPatterns, paramContext,
                                    defaultArgs);
    FullName = DeclName(Context, SimpleName, 
                        llvm::makeArrayRef(NamePieces.begin() + 1,
                                           NamePieces.end()));

    if (bodyPatterns.empty()) {
      // If we didn't get anything, add a () pattern to avoid breaking
      // invariants.
      assert(Status.hasCodeCompletion() || Status.isError());
      bodyPatterns.push_back(TuplePattern::create(Context, Tok.getLoc(),
                                                  {}, Tok.getLoc()));
    }
  } else {
    diagnose(Tok, diag::func_decl_without_paren);
    Status = makeParserError();

    // Recover by creating a '() -> ?' signature.
    auto *EmptyTuplePattern =
        TuplePattern::create(Context, PreviousLoc, {}, PreviousLoc);
    bodyPatterns.push_back(EmptyTuplePattern);
    FullName = DeclName(Context, SimpleName, { });
  }

  // If there's a trailing arrow, parse the rest as the result type.
  if (Tok.is(tok::arrow) || Tok.is(tok::colon)) {
    if (!consumeIf(tok::arrow)) {
      // FixIt ':' to '->'.
      diagnose(Tok, diag::func_decl_expected_arrow)
          .fixItReplace(SourceRange(Tok.getLoc()), "->");
      consumeToken(tok::colon);
    }

    ParserResult<TypeRepr> ResultType =
      parseType(diag::expected_type_function_result);
    if (ResultType.hasCodeCompletion())
      return ResultType;
    retType = ResultType.getPtrOrNull();
    if (!retType) {
      Status.setIsParseError();
      return Status;
    }
  } else {
    // Otherwise, we leave retType null.
    retType = nullptr;
  }

  return Status;
}

ParserStatus
Parser::parseConstructorArguments(DeclName &FullName, Pattern *&BodyPattern,
                                  DefaultArgumentInfo &DefaultArgs) {
  // If we don't have the leading '(', complain.
  if (!Tok.is(tok::l_paren)) {
    // Complain that we expected '('.
    {
      auto diag = diagnose(Tok, diag::expected_lparen_initializer);
      if (Tok.is(tok::l_brace))
        diag.fixItInsert(Tok.getLoc(), "() ");
    }

    // Create an empty tuple to recover.
    BodyPattern = TuplePattern::createSimple(Context, Tok.getLoc(), {},
                                             Tok.getLoc());
    FullName = DeclName(Context, Context.Id_init, { });
    return makeParserError();
  }

  // Parse the parameter-clause.
  SmallVector<ParsedParameter, 4> params;
  SourceLoc leftParenLoc, rightParenLoc;
  
  // Parse the parameter clause.
  ParserStatus status 
    = parseParameterClause(leftParenLoc, params, rightParenLoc,
                           &DefaultArgs, ParameterContextKind::Initializer);

  // Turn the parameter clause into argument and body patterns.
  llvm::SmallVector<Identifier, 2> namePieces;
  BodyPattern = mapParsedParameters(*this, leftParenLoc, params,
                                    rightParenLoc, 
                                    /*isFirstParameterClause=*/true,
                                    &namePieces,
                                    ParameterContextKind::Initializer);

  FullName = DeclName(Context, Context.Id_init, namePieces);
  return status;
}

/// Parse a pattern.
///   pattern ::= pattern-atom
///   pattern ::= pattern-atom ':' type
///   pattern ::= 'var' pattern
///   pattern ::= 'let' pattern
ParserResult<Pattern> Parser::parsePattern(bool isLet) {
  // If this is a let or var pattern parse it.
  if (Tok.is(tok::kw_let) || Tok.is(tok::kw_var))
    return parsePatternVarOrLet();
  
  // First, parse the pattern atom.
  ParserResult<Pattern> Result = parsePatternAtom(isLet);

  // Now parse an optional type annotation.
  if (consumeIf(tok::colon)) {
    if (Result.isNull()) {
      // Recover by creating AnyPattern.
      Result = makeParserErrorResult(new (Context) AnyPattern(PreviousLoc));
    }

    ParserResult<TypeRepr> Ty = parseType();
    if (Ty.hasCodeCompletion())
      return makeParserCodeCompletionResult<Pattern>();

    if (Ty.isNull())
      Ty = makeParserResult(new (Context) ErrorTypeRepr(PreviousLoc));

    Result = makeParserResult(Result,
        new (Context) TypedPattern(Result.get(), Ty.get()));
  }

  return Result;
}

ParserResult<Pattern> Parser::parsePatternVarOrLet() {
  assert((Tok.is(tok::kw_let) || Tok.is(tok::kw_var)) && "expects let or var");
  bool isLet = Tok.is(tok::kw_let);
  SourceLoc varLoc = consumeToken();

  // 'var' and 'let' patterns shouldn't nest.
  if (InVarOrLetPattern)
    diagnose(varLoc, diag::var_pattern_in_var, unsigned(isLet));

  // In our recursive parse, remember that we're in a var/let pattern.
  llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
    T(InVarOrLetPattern, isLet ? IVOLP_InLet : IVOLP_InVar);

  ParserResult<Pattern> subPattern = parsePattern(isLet);
  if (subPattern.hasCodeCompletion())
    return makeParserCodeCompletionResult<Pattern>();
  if (subPattern.isNull())
    return nullptr;
  return makeParserResult(new (Context) VarPattern(varLoc, subPattern.get()));
}

/// \brief Determine whether this token can start a binding name, whether an
/// identifier or the special discard-value binding '_'.
bool Parser::isAtStartOfBindingName() {
  return Tok.is(tok::kw__) || (Tok.is(tok::identifier) && !isStartOfDecl());
}

Pattern *Parser::createBindingFromPattern(SourceLoc loc, Identifier name,
                                          bool isLet) {
  VarDecl *var;
  if (ArgumentIsParameter) {
    var = new (Context) ParamDecl(isLet, loc, name, loc, name, Type(),
                                  CurDeclContext);
  } else {
    var = new (Context) VarDecl(/*static*/ false, /*IsLet*/ isLet,
                                loc, name, Type(), CurDeclContext);
  }
  return new (Context) NamedPattern(var);
}

/// Parse an identifier as a pattern.
ParserResult<Pattern> Parser::parsePatternIdentifier(bool isLet) {
  SourceLoc loc = Tok.getLoc();
  if (consumeIf(tok::kw__)) {
    return makeParserResult(new (Context) AnyPattern(loc));
  }
  
  StringRef text = Tok.getText();
  if (consumeIf(tok::identifier)) {
    Identifier ident = Context.getIdentifier(text);
    return makeParserResult(createBindingFromPattern(loc, ident, isLet));
  }

  return nullptr;
}

/// Parse a pattern "atom", meaning the part that precedes the
/// optional type annotation.
///
///   pattern-atom ::= identifier
///   pattern-atom ::= '_'
///   pattern-atom ::= pattern-tuple
ParserResult<Pattern> Parser::parsePatternAtom(bool isLet) {
  switch (Tok.getKind()) {
  case tok::l_paren:
    return parsePatternTuple(isLet, /*IsArgList*/false);

  case tok::identifier:
  case tok::kw__:
    return parsePatternIdentifier(isLet);

  case tok::code_complete:
    // Just eat the token and return an error status, *not* the code completion
    // status.  We can not code complete anything here -- we expect an
    // identifier.
    consumeToken(tok::code_complete);
    return nullptr;

  default:
    if (Tok.isKeyword() &&
        (peekToken().is(tok::colon) || peekToken().is(tok::equal))) {
      diagnose(Tok, diag::expected_pattern_is_keyword, Tok.getText());
      SourceLoc Loc = Tok.getLoc();
      consumeToken();
      return makeParserErrorResult(new (Context) AnyPattern(Loc));
    }
    diagnose(Tok, diag::expected_pattern);
    return nullptr;
  }
}

std::pair<ParserStatus, Optional<TuplePatternElt>>
Parser::parsePatternTupleElement(bool isLet, bool isArgumentList) {
  
  // Function argument lists can have "inout" applied to TypedPatterns in their
  // arguments.
  SourceLoc InOutLoc;
  if (isArgumentList && Tok.isContextualKeyword("inout"))
    InOutLoc = consumeToken(tok::identifier);
  
  // Parse the pattern.
  ParserResult<Pattern> pattern;

  // Parse the pattern.
  pattern = parsePattern(isLet);
  if (pattern.hasCodeCompletion())
    return std::make_pair(makeParserCodeCompletionStatus(), Nothing);
  if (pattern.isNull())
    return std::make_pair(makeParserError(), Nothing);

  // We don't accept initializers here, but parse one if it's there
  // for recovery purposes.
  ExprHandle *init = nullptr;
  if (Tok.is(tok::equal))
    parseDefaultArgument(*this, nullptr, 0, init);

  // If this is an inout function argument, validate that the sub-pattern is
  // a TypedPattern.
  if (InOutLoc.isValid()) {
    if (auto *TP = dyn_cast<TypedPattern>(pattern.get())) {
      // Change the TypeRep of the underlying typed pattern to be an inout
      // typerep.
      TypeLoc &LocInfo = TP->getTypeLoc();
      LocInfo = TypeLoc(new (Context) InOutTypeRepr(LocInfo.getTypeRepr(),
                                                    InOutLoc));
    } else if (isa<VarPattern>(pattern.get())) {
      diagnose(InOutLoc, diag::inout_varpattern);
    } else {
      diagnose(InOutLoc, diag::inout_must_have_type);
    }
  }
  
  return std::make_pair(
      makeParserSuccess(),
        TuplePatternElt(pattern.get(), nullptr, DefaultArgumentKind::None));
}

ParserResult<Pattern> Parser::parsePatternTuple(bool isLet,
                                                bool isArgumentList) {
  StructureMarkerRAII ParsingPatternTuple(*this, Tok);
  SourceLoc LPLoc = consumeToken(tok::l_paren);
  return parsePatternTupleAfterLP(isLet, isArgumentList, LPLoc);
}

/// Parse a tuple pattern.  The leading left paren has already been consumed and
/// we are looking at the next token.  LPLoc specifies its location.
///
///   pattern-tuple:
///     '(' pattern-tuple-body? ')'
///   pattern-tuple-body:
///     pattern-tuple-element (',' pattern-tuple-body)*
ParserResult<Pattern>
Parser::parsePatternTupleAfterLP(bool isLet, bool isArgumentList,
                                 SourceLoc LPLoc) {
  SourceLoc RPLoc, EllipsisLoc;

  auto diagToUse = isArgumentList ? diag::expected_rparen_parameter
                                  : diag::expected_rparen_tuple_pattern_list;
  
  // Parse all the elements.
  SmallVector<TuplePatternElt, 8> elts;
  ParserStatus ListStatus =
    parseList(tok::r_paren, LPLoc, RPLoc, tok::comma, /*OptionalSep=*/false,
              /*AllowSepAfterLast=*/false, diagToUse, [&] () -> ParserStatus {
    // Parse the pattern tuple element.
    ParserStatus EltStatus;
    Optional<TuplePatternElt> elt;
    std::tie(EltStatus, elt) = parsePatternTupleElement(isLet, isArgumentList);
    if (EltStatus.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
    if (!elt)
      return makeParserError();

    // Add this element to the list.
    elts.push_back(*elt);

    // If there is no ellipsis, we're done with the element.
    if (Tok.isNotEllipsis())
      return makeParserSuccess();
    SourceLoc ellLoc = consumeToken();

    // An ellipsis element shall have a specified element type.
    TypedPattern *typedPattern = dyn_cast<TypedPattern>(elt->getPattern());
    if (!typedPattern) {
      diagnose(ellLoc, diag::untyped_pattern_ellipsis)
        .highlight(elt->getPattern()->getSourceRange());
      // Return success so that the caller does not attempt recovery -- it
      // should have already happened when we were parsing the tuple element.
      return makeParserSuccess();
    }

    // Variadic elements must come last.
    if (Tok.is(tok::r_paren)) {
      EllipsisLoc = ellLoc;
    } else {
      diagnose(ellLoc, diag::ellipsis_pattern_not_at_end);
    }

    return makeParserSuccess();
  });

  return makeParserResult(ListStatus, TuplePattern::createSimple(
                                          Context, LPLoc, elts, RPLoc,
                                          EllipsisLoc.isValid(), EllipsisLoc));
}

ParserResult<Pattern> Parser::parseMatchingPattern() {
  // TODO: Since we expect a pattern in this position, we should optimistically
  // parse pattern nodes for productions shared by pattern and expression
  // grammar. For short-term ease of initial implementation, we always go
  // through the expr parser for ambiguious productions.

  // Parse productions that can only be patterns.
  // matching-pattern ::= matching-pattern-var
  if (Tok.is(tok::kw_var) || Tok.is(tok::kw_let))
    return parseMatchingPatternVarOrVal();

  // matching-pattern ::= 'is' type
  if (Tok.is(tok::kw_is))
    return parseMatchingPatternIs();

  // matching-pattern ::= expr
  // Fall back to expression parsing for ambiguous forms. Name lookup will
  // disambiguate.
  ParserResult<Expr> subExpr = parseExpr(diag::expected_pattern);
  if (subExpr.hasCodeCompletion())
    return makeParserCodeCompletionStatus();
  if (subExpr.isNull())
    return nullptr;
  
  return makeParserResult(new (Context) ExprPattern(subExpr.get()));
}

ParserResult<Pattern> Parser::parseMatchingPatternVarOrVal() {
  assert((Tok.is(tok::kw_let) || Tok.is(tok::kw_var)) && "expects val or var");
  bool isVal = Tok.is(tok::kw_let);
  SourceLoc varLoc = consumeToken();

  // 'var' and 'let' patterns shouldn't nest.
  if (InVarOrLetPattern)
    diagnose(varLoc, diag::var_pattern_in_var, unsigned(isVal));

  // In our recursive parse, remember that we're in a var/let pattern.
  llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
    T(InVarOrLetPattern, isVal ? IVOLP_InLet : IVOLP_InVar);

  ParserResult<Pattern> subPattern = parseMatchingPattern();
  if (subPattern.isNull())
    return nullptr;
  return makeParserResult(new (Context) VarPattern(varLoc, subPattern.get()));
}

// matching-pattern ::= 'is' type
ParserResult<Pattern> Parser::parseMatchingPatternIs() {
  SourceLoc isLoc = consumeToken(tok::kw_is);
  ParserResult<TypeRepr> castType = parseType();
  if (castType.isNull() || castType.hasCodeCompletion())
    return nullptr;
  return makeParserResult(new (Context) IsaPattern(isLoc, castType.get(),
                                                   nullptr));
}

bool Parser::isOnlyStartOfMatchingPattern() {
  return Tok.is(tok::kw_var) || Tok.is(tok::kw_let) || Tok.is(tok::kw_is);
}

bool Parser::canParsePattern() {
  switch (Tok.getKind()) {
  case tok::kw_let:      ///   pattern ::= 'let' pattern
  case tok::kw_var:      ///   pattern ::= 'var' pattern
    consumeToken();
    return canParsePattern();
  default:
    ///   pattern ::= pattern-atom
    ///   pattern ::= pattern-atom ':' type
    if (!canParsePatternAtom())
      return false;

    if (!consumeIf(tok::colon))
      return true;
    return canParseType();
  }
}

bool Parser::canParsePatternAtom() {
  switch (Tok.getKind()) {
  case tok::l_paren: return canParsePatternTuple();
  case tok::identifier:
  case tok::kw__:
    consumeToken();
    return true;
  default:
    return false;
  }
}


bool Parser::canParsePatternTuple() {
  if (!consumeIf(tok::l_paren)) return false;

  if (Tok.isNot(tok::r_paren)) {
    do {
      // The contextual inout marker is part of argument lists.
      if (Tok.isContextualKeyword("inout"))
        consumeToken(tok::identifier);

      if (!canParsePattern()) return false;

      // Parse default values. This aren't actually allowed, but we recover
      // better if we skip over them.
      if (consumeIf(tok::equal)) {
        while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_paren) &&
               Tok.isNot(tok::r_brace) && Tok.isNotEllipsis() &&
               Tok.isNot(tok::comma) &&
               !isStartOfDecl()) {
          skipSingle();
        }
      }

    } while (consumeIf(tok::comma));
  }

  if (Tok.isEllipsis())
    consumeToken();

  return consumeIf(tok::r_paren);
}
