//===--- ParseDecl.cpp - Swift Language Parser for Declarations -----------===//
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
// Declaration Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/Parse/Lexer.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Diagnostics.h"
#include "swift/Basic/Fallthrough.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

/// parseTranslationUnit - Main entrypoint for the parser.
///   translation-unit:
///     stmt-brace-item*
///     decl-sil    [[only in SIL mode]
bool Parser::parseTranslationUnit(TranslationUnit *TU) {
  TU->ASTStage = TranslationUnit::Parsing;

  // Prime the lexer.
  if (Tok.is(tok::NUM_TOKENS))
    consumeToken();

  CurDeclContext = TU;
  
  // Parse the body of the file.
  SmallVector<ExprStmtOrDecl, 128> Items;

  if (Tok.is(tok::r_brace)) {
    diagnose(Tok.getLoc(), diag::extra_rbrace)
      .fixItRemove(SourceRange(Tok.getLoc()));
    consumeToken();
  }

  // If we are in SIL mode, and if the first token is the start of a sil
  // declaration, parse that one SIL function and return to the top level.  This
  // allows type declarations and other things to be parsed, name bound, and
  // type checked in batches, similar to immediate mode.  This also enforces
  // that SIL bodies can only be at the top level.
  if (Tok.is(tok::kw_sil)) {
    assert(isInSILMode() && "'sil' should only be a keyword in SIL mode");
    parseDeclSIL();
  } else {
    parseBraceItems(Items, true,
                    IsMainModule ? BraceItemListKind::TopLevelCode
                                 : BraceItemListKind::Brace);
  }


  // If this is a MainModule, determine if we found code that needs to be
  // executed (this is used by the repl to know whether to compile and run the
  // newly parsed stuff).
  bool FoundTopLevelCodeToExecute = false;
  if (IsMainModule) {
    for (auto V : Items)
      if (isa<TopLevelCodeDecl>(V.get<Decl*>()))
        FoundTopLevelCodeToExecute = true;
  }

  // Add newly parsed decls to the translation unit.
  for (auto Item : Items)
    TU->Decls.push_back(Item.get<Decl*>());

  // Note that the translation unit is fully parsed and verify it.
  TU->ASTStage = TranslationUnit::Parsed;
  verify(TU);

  return FoundTopLevelCodeToExecute;
}

namespace {
#define MAKE_ENUMERATOR(id) id,
  enum class AttrName {
    none,
#define ATTR(X) X,
#include "swift/AST/Attr.def"
  };
}

static AttrName getAttrName(StringRef text) {
  return llvm::StringSwitch<AttrName>(text)
#define ATTR(X) .Case(#X, AttrName::X)
#include "swift/AST/Attr.def"
    .Default(AttrName::none);
}

static Resilience getResilience(AttrName attr) {
  switch (attr) {
  case AttrName::resilient: return Resilience::Resilient;
  case AttrName::fragile: return Resilience::Fragile;
  case AttrName::born_fragile: return Resilience::InherentlyFragile;
  default: llvm_unreachable("bad resilience");
  }
}

/// parseAttribute
///   attribute:
///     'asmname' '=' identifier  (FIXME: This is a temporary hack until we
///                                       can import C modules.)
///     'infix' '=' numeric_constant
///     'infix_left' '=' numeric_constant
///     'infix_right' '=' numeric_constant
///     'unary'
///     'stdlib'
///     'weak'
///     'unowned'
///     'noreturn'
bool Parser::parseAttribute(DeclAttributes &Attributes) {
  if (Tok.is(tok::kw_weak)) {
    if (Attributes.hasOwnership()) {
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    } else {
      Attributes.Weak = true;
    }
    consumeToken(tok::kw_weak);
    return false;
  }

  if (Tok.is(tok::kw_unowned)) {
    if (Attributes.hasOwnership()) {
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    } else {
      Attributes.Unowned = true;
    }
    consumeToken(tok::kw_unowned);
    return false;
  }

  if (!Tok.is(tok::identifier)) {
    diagnose(Tok, diag::expected_attribute_name);
    skipUntil(tok::r_square);
    return true;
  }

  switch (AttrName attr = getAttrName(Tok.getText())) {
  case AttrName::none:
    diagnose(Tok, diag::unknown_attribute, Tok.getText());
    skipUntil(tok::r_square);
    return true;

  // Infix attributes.
  case AttrName::infix: {
    if (Attributes.isInfix())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);
    Attributes.ExplicitInfix = true;

    return false;
  }

  // Resilience attributes.
  case AttrName::resilient:
  case AttrName::fragile:
  case AttrName::born_fragile: {
    if (Attributes.Resilience.isValid())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);

    Resilience resil = getResilience(attr);
    
    // TODO: 'fragile' should allow deployment versioning.
    Attributes.Resilience = ResilienceData(resil);
    return false;
  }

  // 'byref' attribute.
  // FIXME: only permit this in specific contexts.
  case AttrName::byref: {
    SourceLoc TokLoc = Tok.getLoc();
    if (Attributes.Byref)
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);

    Attributes.Byref = true;

    // Permit "qualifiers" on the byref.
    SourceLoc beginLoc = Tok.getLoc();
    if (consumeIfNotAtStartOfLine(tok::l_paren)) {
      diagnose(Tok, diag::byref_attribute_unknown_qualifier);
      SourceLoc endLoc;
      parseMatchingToken(tok::r_paren, endLoc,
                         diag::byref_attribute_expected_rparen,
                         beginLoc);
    }
    
    // Verify that we're not combining this attribute incorrectly.  Cannot be
    // both byref and auto_closure.
    if (Attributes.isAutoClosure()) {
      diagnose(TokLoc, diag::cannot_combine_attribute, "auto_closure");
      Attributes.AutoClosure = false;
    }
    
    return false;
  }
      
  // 'cc' attribute.
  // FIXME: only permit this in type contexts.
  case AttrName::cc: {
    if (Attributes.hasCC())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    
    consumeToken(tok::identifier);
    
    // Parse the cc name in parens.
    SourceLoc beginLoc = Tok.getLoc(), nameLoc, endLoc;
    StringRef name;
    if (consumeIfNotAtStartOfLine(tok::l_paren)) {
      if (Tok.is(tok::identifier)) {
        nameLoc = Tok.getLoc();
        name = Tok.getText();
        consumeToken();
      } else {
        diagnose(Tok, diag::cc_attribute_expected_name);
      }
      if (parseMatchingToken(tok::r_paren, endLoc,
                             diag::cc_attribute_expected_rparen,
                             beginLoc)) {
        // If the name isn't immediately followed by a closing paren, recover
        // by trying to find some closing paren.
        skipUntil(tok::r_paren);
        consumeIf(tok::r_paren);
      }
    } else {
      diagnose(Tok, diag::cc_attribute_expected_lparen);
    }
    
    if (!name.empty()) {
      Attributes.cc = llvm::StringSwitch<Optional<AbstractCC>>(name)
        .Case("freestanding", AbstractCC::Freestanding)
        .Case("method", AbstractCC::Method)
        .Case("cdecl", AbstractCC::C)
        .Case("objc_method", AbstractCC::ObjCMethod)
        .Default(Nothing);
      if (!Attributes.cc) {
        diagnose(nameLoc, diag::cc_attribute_unknown_cc_name, name);
        Attributes.cc = AbstractCC::Freestanding;
      }
    }
    return false;
  }
   
  case AttrName::class_protocol: {
    if (Attributes.isClassProtocol())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    
    consumeToken(tok::identifier);
    
    Attributes.ClassProtocol = true;
    return false;
  }

  // 'objc_block' attribute.
  // FIXME: only permit this in type contexts.
  case AttrName::objc_block: {
    if (Attributes.Byref)
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);
    
    Attributes.ObjCBlock = true;
    
    return false;
  }
      
  // FIXME: Only valid on var and tuple elements, not on func's, typealias, etc.
  case AttrName::auto_closure: {
    SourceLoc TokLoc = Tok.getLoc();
    if (Attributes.isAutoClosure())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);
    
    // Verify that we're not combining this attribute incorrectly.  Cannot be
    // both byref and auto_closure.
    if (Attributes.isByref()) {
      diagnose(TokLoc, diag::cannot_combine_attribute, "byref");
      return false;
    }
    
    Attributes.AutoClosure = true;
    return false;
  }
  case AttrName::thin: {
    if (Attributes.isThin())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);
    
    Attributes.Thin = true;
    return false;
  }
  case AttrName::noreturn: {
    if (Attributes.isNoReturn())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);

    Attributes.NoReturn = true;
    return false;
  }

  case AttrName::assignment: {
    if (Attributes.isAssignment())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);
    
    Attributes.Assignment = true;
    return false;    
  }

  case AttrName::prefix: {
    if (Attributes.isPrefix())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());

    consumeToken(tok::identifier);
    Attributes.ExplicitPrefix = true;
    return false;
  }

  case AttrName::postfix: {
    if (Attributes.isPostfix())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());

    consumeToken(tok::identifier);
    Attributes.ExplicitPostfix = true;
    return false;
  }

  case AttrName::conversion: {
    if (Attributes.isConversion())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);
    
    Attributes.Conversion = true;
    return false;    
  }
      
  case AttrName::force_inline: {
    if (Attributes.isForceInline())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);
    
    Attributes.ForceInline = true;
    return false;
  }

  case AttrName::iboutlet: {
    if (Attributes.isIBOutlet())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());

    consumeToken(tok::identifier);
    Attributes.IBOutlet = true;
    return false;
  }

  case AttrName::ibaction: {
    if (Attributes.isIBAction())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());

    consumeToken(tok::identifier);
    Attributes.IBAction = true;
    return false;
  }

  case AttrName::objc: {
    if (Attributes.isObjC())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());

    consumeToken(tok::identifier);
    Attributes.ObjC = true;
    return false;
  }

  case AttrName::stdlib: {
    if (Attributes.Stdlib)
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);

    Attributes.Stdlib = true;
    return false;
  }

  /// FIXME: This is a temporary hack until we can import C modules.
  case AttrName::asmname: {
    SourceLoc TokLoc = Tok.getLoc();
    if (!Attributes.AsmName.empty())
      diagnose(Tok, diag::duplicate_attribute, Tok.getText());
    consumeToken(tok::identifier);

    if (!consumeIf(tok::equal)) {
      diagnose(TokLoc, diag::asmname_expected_equals);
      return false;
    }

    if (!Tok.is(tok::string_literal)) {
      diagnose(TokLoc, diag::asmname_expected_string_literal);
      return false;
    }

    llvm::SmallVector<Lexer::StringSegment, 1> Segments;
    L->getEncodedStringLiteral(Tok, Context, Segments);
    if (Segments.size() != 1 ||
        Segments.front().Kind == Lexer::StringSegment::Expr) {
      diagnose(TokLoc, diag::asmname_interpolated_string);
    } else {
      Attributes.AsmName = Segments.front().Data;
    }
    consumeToken(tok::string_literal);
    return false;
  }
  }
  llvm_unreachable("bad attribute kind");
}

/// parsePresentAttributeList - This is the internal implementation of
/// parseAttributeList, which we expect to be inlined to handle the common case
/// of an absent attribute list.
///   attribute-list:
///     /*empty*/
///     '[' ']'
///     '[' attribute (',' attribute)* ']'
bool Parser::parseAttributeListPresent(DeclAttributes &Attributes) {
  Attributes.LSquareLoc = consumeToken(tok::l_square);

  return parseList(tok::r_square, Attributes.LSquareLoc, Attributes.RSquareLoc,
                   tok::comma, /*OptionalSep=*/false,
                   diag::expected_in_attribute_list,
                   [&] () -> bool {
    return parseAttribute(Attributes);
  });
}

bool Parser::isStartOfOperatorDecl(const Token &Tok, const Token &Tok2) {
  return Tok.isContextualKeyword("operator")
    && (Tok2.isContextualKeyword("prefix")
        || Tok2.isContextualKeyword("postfix")
        || Tok2.isContextualKeyword("infix"));
}

/// parseDecl - Parse a single syntactic declaration and return a list of decl
/// ASTs.  This can return multiple results for var decls that bind to multiple
/// values, structs that define a struct decl and a constructor, etc.
///
/// This method returns true on a parser error that requires recovery.
///
///   decl:
///     decl-typealias
///     decl-extension
///     decl-var
///     decl-func
///     decl-oneof
///     decl-struct
///     decl-import
///     decl-operator
///
bool Parser::parseDecl(SmallVectorImpl<Decl*> &Entries, unsigned Flags) {
  // If we see the 'static' keyword, parse it now.
  SourceLoc StaticLoc;
  bool UnhandledStatic = false;
  if (Tok.is(tok::kw_static)) {
    StaticLoc = consumeToken();
    UnhandledStatic = true;
  }

  bool HadParseError = false;
  switch (Tok.getKind()) {
  case tok::kw_import:
    Entries.push_back(parseDeclImport(Flags));
    break;
  case tok::kw_extension:
    Entries.push_back(parseDeclExtension(Flags));
    break;
  case tok::kw_var:
    if (StaticLoc.isValid()) {
      diagnose(Tok, diag::unimplemented_static_var)
        .highlight(SourceRange(StaticLoc));
      UnhandledStatic = false;
    }
    HadParseError = parseDeclVar(Flags, Entries);
    break;
  case tok::kw_typealias:
    Entries.push_back(parseDeclTypeAlias(!(Flags & PD_DisallowTypeAliasDef)));
    break;
  case tok::kw_oneof:
    HadParseError = parseDeclOneOf(Flags, Entries);
    break;
  case tok::kw_case:
    HadParseError = parseDeclOneOfElement(Flags, Entries);
    break;
  case tok::kw_struct:
    HadParseError = parseDeclStruct(Flags, Entries);
    break;
  case tok::kw_class:
    HadParseError = parseDeclClass(Flags, Entries);
    break;
  case tok::kw_constructor:
    Entries.push_back(parseDeclConstructor(Flags & PD_HasContainerType));
    break;
  case tok::kw_destructor:
    Entries.push_back(parseDeclDestructor(Flags));
    break;
  case tok::kw_protocol:
    Entries.push_back(parseDeclProtocol(Flags));
    break;
      
  case tok::kw_func:
    Entries.push_back(parseDeclFunc(StaticLoc, Flags));
    UnhandledStatic = false;
    break;

  case tok::kw_subscript:
    if (StaticLoc.isValid()) {
      diagnose(Tok, diag::subscript_static)
        .fixItRemove(SourceRange(StaticLoc));
      UnhandledStatic = false;
    }
    HadParseError = parseDeclSubscript(Flags & PD_HasContainerType,
                                       !(Flags & PD_DisallowFuncDef),
                                       Entries);
    break;
  
  case tok::identifier:
    if (isStartOfOperatorDecl(Tok, peekToken())) {
      Entries.push_back(parseDeclOperator(Flags & PD_AllowTopLevel));
      break;
    }
    SWIFT_FALLTHROUGH;

  default:
    diagnose(Tok, diag::expected_decl);
    HadParseError = true;
    break;
  }

  if (!HadParseError && Tok.is(tok::semi))
    Entries.back()->TrailingSemiLoc = consumeToken(tok::semi);

  // If we got back a null pointer, then a parse error happened.
  if (Entries.empty())
    HadParseError = true;
  else if (Entries.back() == 0) {
    Entries.pop_back();
    HadParseError = true;
  }

  // If we parsed 'static' but didn't handle it above, complain about it.
  if (!HadParseError && UnhandledStatic) {
    diagnose(Entries.back()->getLoc(), diag::decl_not_static)
      .fixItRemove(SourceRange(StaticLoc));
  }

  return HadParseError;
}


/// parseDeclImport - Parse an 'import' declaration, returning null (and doing
/// no token skipping) on error.
///
///   decl-import:
///      'import' attribute-list any-identifier ('.' any-identifier)*
///
Decl *Parser::parseDeclImport(unsigned Flags) {
  SourceLoc ImportLoc = consumeToken(tok::kw_import);
  
  DeclAttributes Attributes;
  parseAttributeList(Attributes);
  
  SmallVector<std::pair<Identifier, SourceLoc>, 8> ImportPath(1);
  ImportPath.back().second = Tok.getLoc();
  if (parseAnyIdentifier(ImportPath.back().first,
                         diag::decl_expected_module_name))
    return 0;
  
  while (consumeIf(tok::period)) {
    ImportPath.push_back(std::make_pair(Identifier(), Tok.getLoc()));
    if (parseAnyIdentifier(ImportPath.back().first,
                        diag::expected_identifier_in_decl, "import"))
      return 0;
  }

  bool isStdlibImport = Attributes.Stdlib;
  Attributes.Stdlib = false;

  if (!Attributes.empty())
    diagnose(Attributes.LSquareLoc, diag::import_attributes);
  
  if (!(Flags & PD_AllowTopLevel)) {
    diagnose(ImportLoc, diag::decl_inner_scope);
    return 0;
  }

  return ImportDecl::create(Context, CurDeclContext, ImportLoc, ImportPath,
                            isStdlibImport);
}

/// parseInheritance - Parse an inheritance clause.
///
///   inheritance:
///      ':' type-identifier (',' type-identifier)*
bool Parser::parseInheritance(SmallVectorImpl<TypeLoc> &Inherited) {
  consumeToken(tok::colon);
  
  do {
    // Parse the inherited type (which must be a protocol).
    TypeRepr *Ty = parseTypeIdentifier();
    if (!Ty)
      return true;
    
    // Record the type.
    Inherited.push_back(Ty);
    
    // Check for a ',', which indicates that there are more protocols coming.
  } while (consumeIf(tok::comma));
  
  return false;
}

/// parseDeclExtension - Parse an 'extension' declaration.
///   extension:
///    'extension' type-identifier inheritance? '{' decl* '}'
///
Decl *Parser::parseDeclExtension(unsigned Flags) {
  SourceLoc ExtensionLoc = consumeToken(tok::kw_extension);

  TypeRepr *Ty = parseTypeIdentifier();
  if (!Ty)
    return nullptr;
  SourceLoc LBLoc, RBLoc;
  
  // Parse optional inheritance clause.
  SmallVector<TypeLoc, 2> Inherited;
  if (Tok.is(tok::colon))
    parseInheritance(Inherited);

  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_extension))
    return nullptr;

  ExtensionDecl *ED
    = new (Context) ExtensionDecl(ExtensionLoc, Ty,
                                  Context.AllocateCopy(Inherited),
                                  CurDeclContext);
  ContextChange CC(*this, ED);
  Scope S(this, ScopeKind::Extension);

  SmallVector<Decl*, 8> MemberDecls;

  bool Invalid = parseList(tok::r_brace, LBLoc, RBLoc,
                           tok::semi, /*OptionalSep=*/true,
                           diag::expected_rbrace_extension,
                           [&] () -> bool {
    return parseDecl(MemberDecls, PD_HasContainerType|PD_DisallowVar);
  });

  if (Invalid) return nullptr;

  ED->setMembers(Context.AllocateCopy(MemberDecls), { LBLoc, RBLoc });

  if (!(Flags & PD_AllowTopLevel)) {
    diagnose(ExtensionLoc, diag::decl_inner_scope);
    return nullptr;
  }

  return ED;
}

/// parseDeclTypeAlias
///   decl-typealias:
///     'typealias' identifier inheritance? '=' type
///
TypeAliasDecl *Parser::parseDeclTypeAlias(bool WantDefinition) {
  SourceLoc TypeAliasLoc = consumeToken(tok::kw_typealias);
  
  Identifier Id;
  TypeRepr *UnderlyingTy = nullptr;
  SourceLoc IdLoc;
  if (parseIdentifier(Id, IdLoc, diag::expected_identifier_in_decl,"typealias"))
    return nullptr;

  // Parse optional inheritance clause.
  SmallVector<TypeLoc, 2> Inherited;
  if (Tok.is(tok::colon))
    parseInheritance(Inherited);

  if (WantDefinition || Tok.is(tok::equal)) {
    if (parseToken(tok::equal, diag::expected_equal_in_typealias) ||
        !(UnderlyingTy = parseType(diag::expected_type_in_typealias)))
      return nullptr;
    
    if (!WantDefinition) {
      diagnose(IdLoc, diag::associated_type_def, Id);
      UnderlyingTy = nullptr;
    }
  }

  TypeAliasDecl *TAD =
    new (Context) TypeAliasDecl(TypeAliasLoc, Id, IdLoc, UnderlyingTy,
                                CurDeclContext,
                                Context.AllocateCopy(Inherited));
  addToScope(TAD);
  return TAD;
}

namespace {
  class AddVarsToScope : public ASTWalker {
  public:
    Parser &TheParser;
    ASTContext &Context;
    DeclContext *CurDeclContext;
    SmallVectorImpl<Decl*> &Decls;
    DeclAttributes &Attributes;
    
    AddVarsToScope(Parser &P,
                   ASTContext &Context,
                   DeclContext *CurDeclContext,
                   SmallVectorImpl<Decl*> &Decls,
                   DeclAttributes &Attributes)
      : TheParser(P),
        Context(Context),
        CurDeclContext(CurDeclContext),
        Decls(Decls),
        Attributes(Attributes)
    {}
    
    Pattern *walkToPatternPost(Pattern *P) override {
      // Handle vars.
      if (auto *Named = dyn_cast<NamedPattern>(P)) {
        VarDecl *VD = Named->getDecl();
        VD->setDeclContext(CurDeclContext);
        if (Attributes.isValid())
          VD->getMutableAttrs() = Attributes;
        
        if (VD->isProperty()) {
          // FIXME: Order of get/set not preserved.
          if (FuncDecl *Get = VD->getGetter()) {
            Get->setDeclContext(CurDeclContext);
            Decls.push_back(Get);
          }
          if (FuncDecl *Set = VD->getSetter()) {
            Set->setDeclContext(CurDeclContext);
            Decls.push_back(Set);
          }
        }
        
        Decls.push_back(VD);
        TheParser.addToScope(VD);
      }
      return P;
    }
  };
}

void Parser::addVarsToScope(Pattern *Pat,
                            SmallVectorImpl<Decl*> &Decls,
                            DeclAttributes &Attributes) {
  Pat->walk(AddVarsToScope(*this, Context, CurDeclContext,
                           Decls, Attributes));
}

/// parseSetGet - Parse a get-set clause, containing a getter and (optionally)
/// a setter.
///
///   get-set:
///      get var-set?
///      set var-get
///
///   get:
///     'get:' stmt-brace-item*
///
///   set:
///     'set' set-name? ':' stmt-brace-item*
///
///   set-name:
///     '(' identifier ')'
bool Parser::parseGetSet(bool HasContainerType, Pattern *Indices,
                         TypeLoc ElementTy, FuncDecl *&Get, FuncDecl *&Set,
                         SourceLoc &LastValidLoc) {
  bool Invalid = false;
  Get = 0;
  Set = 0;
  
  while (Tok.isNot(tok::r_brace)) {
    if (Tok.is(tok::eof)) {
      Invalid = true;
      break;
    }
    if (Tok.isContextualKeyword("get") || !Tok.isContextualKeyword("set")) {
      //   get         ::= 'get' stmt-brace
      
      // Have we already parsed a get clause?
      if (Get) {
        diagnose(Tok.getLoc(), diag::duplicate_getset, false);
        diagnose(Get->getLoc(), diag::previous_getset, false);
        
        // Forget the previous version.
        Get = 0;
      }
      
      SourceLoc GetLoc = Tok.getLoc(), ColonLoc = Tok.getLoc();
      if (Tok.isContextualKeyword("get")) {
        GetLoc = consumeToken();
        if (Tok.isNot(tok::colon)) {
          diagnose(Tok.getLoc(), diag::expected_colon_get);
          Invalid = true;
          break;
        }
        ColonLoc = consumeToken(tok::colon);
      }

      // Set up a function declaration for the getter and parse its body.
      
      // Create the parameter list(s) for the getter.
      llvm::SmallVector<Pattern *, 3> Params;
      
      // Add the implicit 'this' to Params, if needed.
      if (HasContainerType)
        Params.push_back(buildImplicitThisParameter());

      // Add the index clause if necessary.
      if (Indices) {
        Params.push_back(Indices->clone(Context));
      }
      
      // Add a no-parameters clause.
      Params.push_back(TuplePattern::create(Context, SourceLoc(),
                                            ArrayRef<TuplePatternElt>(),
                                            SourceLoc()));

      Scope S(this, ScopeKind::FunctionBody);

      // Start the function.
      FuncExpr *GetFn = actOnFuncExprStart(GetLoc, ElementTy, Params,Params);
      
      // Establish the new context.
      ContextChange CC(*this, GetFn);

      SmallVector<ExprStmtOrDecl, 16> Entries;
      parseBraceItems(Entries, false /*NotTopLevel*/,
                      BraceItemListKind::Property);
      NullablePtr<BraceStmt> Body = BraceStmt::create(Context, ColonLoc,
                                                      Entries, Tok.getLoc());

      if (Body.isNull()) {
        GetLoc = SourceLoc();
        skipUntilDeclRBrace();
        Invalid = true;
        break;
      }
      
      GetFn->setBody(Body.get());

      LastValidLoc = Body.get()->getRBraceLoc();
      
      Get = new (Context) FuncDecl(/*StaticLoc=*/SourceLoc(), GetLoc,
                                   Identifier(), GetLoc, /*generic=*/nullptr,
                                   Type(), GetFn, CurDeclContext);
      GetFn->setDecl(Get);
      continue;
    }

    //   var-set         ::= 'set' var-set-name? stmt-brace
    
    // Have we already parsed a var-set clause?
    if (Set) {
      diagnose(Tok.getLoc(), diag::duplicate_getset, true);
      diagnose(Set->getLoc(), diag::previous_getset, true);

      // Forget the previous setter.
      Set = 0;
    }
    
    SourceLoc SetLoc = consumeToken();
    
    //   var-set-name    ::= '(' identifier ')'
    Identifier SetName;
    SourceLoc SetNameLoc;
    SourceRange SetNameParens;
    if (Tok.is(tok::l_paren)) {
      SourceLoc StartLoc = consumeToken();
      if (Tok.is(tok::identifier)) {
        // We have a name.
        SetName = Context.getIdentifier(Tok.getText());
        SetNameLoc = consumeToken();
        
        // Look for the closing ')'.
        SourceLoc EndLoc;
        if (parseMatchingToken(tok::r_paren, EndLoc,
                               diag::expected_rparen_setname, StartLoc))
          EndLoc = SetNameLoc;
        SetNameParens = SourceRange(StartLoc, EndLoc);
      } else {
        diagnose(Tok.getLoc(), diag::expected_setname);
        skipUntil(tok::r_paren, tok::l_brace);
        if (Tok.is(tok::r_paren))
          consumeToken();
      }
    }
    if (Tok.isNot(tok::colon)) {
      diagnose(Tok.getLoc(), diag::expected_colon_set);
      Invalid = true;
      break;
    }
    SourceLoc ColonLoc = consumeToken(tok::colon);

    // Set up a function declaration for the setter and parse its body.
    
    // Create the parameter list(s) for the setter.
    llvm::SmallVector<Pattern *, 3> Params;
    
    // Add the implicit 'this' to Params, if needed.
    if (HasContainerType)
      Params.push_back(buildImplicitThisParameter());

    // Add the index parameters, if necessary.
    if (Indices) {
      Params.push_back(Indices->clone(Context));
    }
    
    // Add the parameter. If no name was specified, the name defaults to
    // 'value'.
    if (SetName.empty())
      SetName = Context.getIdentifier("value");
    {
      VarDecl *Value = new (Context) VarDecl(SetNameLoc, SetName,
                                             Type(), CurDeclContext);
      
      Pattern *ValuePattern
        = new (Context) TypedPattern(new (Context) NamedPattern(Value),
                                     ElementTy);
      TuplePatternElt ValueElt(ValuePattern);
      Pattern *ValueParamsPattern
        = TuplePattern::create(Context, SetNameParens.Start, ValueElt,
                               SetNameParens.End);
      Params.push_back(ValueParamsPattern);
    }

    Scope S(this, ScopeKind::FunctionBody);

    // Start the function.
    Type SetterRetTy = TupleType::getEmpty(Context);
    FuncExpr *SetFn = actOnFuncExprStart(SetLoc,
                                         TypeLoc::withoutLoc(SetterRetTy),
                                         Params, Params);
    
    // Establish the new context.
    ContextChange CC(*this, SetFn);
    
    // Parse the body.
    SmallVector<ExprStmtOrDecl, 16> Entries;
    parseBraceItems(Entries, false /*NotTopLevel*/,
                    BraceItemListKind::Property);
    NullablePtr<BraceStmt> Body = BraceStmt::create(Context, ColonLoc,
                                                    Entries, Tok.getLoc());

    if (Body.isNull()) {
      skipUntilDeclRBrace();
      Invalid = true;
      break;
    }
    
    SetFn->setBody(Body.get());

    LastValidLoc = Body.get()->getRBraceLoc();
    
    Set = new (Context) FuncDecl(/*StaticLoc=*/SourceLoc(), SetLoc,
                                 Identifier(), SetLoc, /*generic=*/nullptr,
                                 Type(), SetFn, CurDeclContext);
    SetFn->setDecl(Set);
  }
  
  return Invalid;
}

/// parseDeclVarGetSet - Parse the brace-enclosed getter and setter for a variable.
///
///   decl-var:
///      'var' attribute-list identifier : type-annotation { get-set }
void Parser::parseDeclVarGetSet(Pattern &pattern, bool HasContainerType) {
  bool Invalid = false;
    
  // The grammar syntactically requires a simple identifier for the variable
  // name. Complain if that isn't what we got.
  VarDecl *PrimaryVar = 0;
  {
    Pattern *PrimaryPattern = &pattern;
    if (TypedPattern *Typed = dyn_cast<TypedPattern>(PrimaryPattern))
      PrimaryPattern = Typed->getSubPattern();
    if (NamedPattern *Named = dyn_cast<NamedPattern>(PrimaryPattern)) {
      PrimaryVar = Named->getDecl();
    }
  }

  if (!PrimaryVar)
    diagnose(pattern.getLoc(), diag::getset_nontrivial_pattern);

  // The grammar syntactically requires a type annotation. Complain if
  // our pattern does not have one.
  TypeLoc TyLoc;
  if (TypedPattern *TP = dyn_cast<TypedPattern>(&pattern)) {
    TyLoc = TP->getTypeLoc();
  } else {
    if (PrimaryVar)
      diagnose(pattern.getLoc(), diag::getset_missing_type);
    TyLoc = TypeLoc::withoutLoc(ErrorType::get(Context));
  }
  
  SourceLoc LBLoc = consumeToken(tok::l_brace);
    
  // Parse getter and setter.
  FuncDecl *Get = 0;
  FuncDecl *Set = 0;
  SourceLoc LastValidLoc = LBLoc;
  if (parseGetSet(HasContainerType, /*Indices=*/0, TyLoc, Get, Set, LastValidLoc))
    Invalid = true;
  
  // Parse the final '}'.
  SourceLoc RBLoc;
  if (Invalid) {
    skipUntilDeclRBrace();
    RBLoc = LastValidLoc;
  }

  if (parseMatchingToken(tok::r_brace, RBLoc, diag::expected_rbrace_in_getset,
                         LBLoc)) {
    RBLoc = LastValidLoc;
  }
  
  if (Set && !Get) {
    if (!Invalid)
      diagnose(Set->getLoc(), diag::var_set_without_get);
    
    Set = nullptr;
    Invalid = true;
  }

  // If things went well, turn this variable into a property.
  if (!Invalid && PrimaryVar && (Set || Get))
    PrimaryVar->setProperty(Context, LBLoc, Get, Set, RBLoc);
}

/// parseDeclVar - Parse a 'var' declaration, returning true (and doing no
/// token skipping) on error.
///
///   decl-var:
///      'var' attribute-list pattern initializer? (',' pattern initializer? )*
///      'var' attribute-list identifier : type-annotation { get-set }
bool Parser::parseDeclVar(unsigned Flags, SmallVectorImpl<Decl*> &Decls){
  SourceLoc VarLoc = consumeToken(tok::kw_var);

  SmallVector<PatternBindingDecl*, 4> PBDs;
  bool HasGetSet = false;
  bool isInvalid = false;

  unsigned FirstDecl = Decls.size();

  do {
    DeclAttributes Attributes;
    parseAttributeList(Attributes);

    NullablePtr<Pattern> pattern = parsePattern();
    if (pattern.isNull()) return true;

    // If we syntactically match the second decl-var production, with a
    // var-get-set clause, parse the var-get-set clause.
    if (Tok.is(tok::l_brace)) {
      parseDeclVarGetSet(*pattern.get(), Flags & PD_HasContainerType);
      HasGetSet = true;
    }

    NullablePtr<Expr> Init;
    if (Tok.is(tok::equal)) {
      SourceLoc EqualLoc = consumeToken(tok::equal);
      Init = parseExpr(diag::expected_initializer_expr);
      if (Init.isNull()) {
        isInvalid = true;
        break;
      }
    
      if (HasGetSet) {
        diagnose(pattern.get()->getLoc(), diag::getset_init)
          .highlight(Init.get()->getSourceRange());
        Init = nullptr;
      }
      if (Flags & PD_DisallowInit) {
        diagnose(EqualLoc, diag::disallowed_init);
        isInvalid = true;
      }
    }

    addVarsToScope(pattern.get(), Decls, Attributes);

    // In the normal case, just add PatternBindingDecls to our DeclContext.
    PatternBindingDecl *PBD =
      new (Context) PatternBindingDecl(VarLoc, pattern.get(),
                                       Init.getPtrOrNull(), CurDeclContext);
    Decls.push_back(PBD);

    // Propagate back types for simple patterns, like "var A, B : T".
    if (TypedPattern *TP = dyn_cast<TypedPattern>(PBD->getPattern())) {
      if (isa<NamedPattern>(TP->getSubPattern()) && !PBD->hasInit()) {
        for (unsigned i = PBDs.size(); i != 0; --i) {
          PatternBindingDecl *PrevPBD = PBDs[i-1];
          Pattern *PrevPat = PrevPBD->getPattern();
          if (!isa<NamedPattern>(PrevPat) || PrevPBD->hasInit())
            break;
          if (HasGetSet) {
            // FIXME -- offer a fixit to explicitly specify the type
            diagnose(PrevPat->getLoc(), diag::getset_cannot_be_implied);
            isInvalid = true;
          }

          TypedPattern *NewTP = new (Context) TypedPattern(PrevPat,
                                                           TP->getTypeLoc());
          PrevPBD->setPattern(NewTP);
        }
      }
    }
    PBDs.push_back(PBD);
  } while (consumeIf(tok::comma));

  if (HasGetSet) {
    if (PBDs.size() > 1) {
      diagnose(VarLoc, diag::disallowed_property_multiple_getset);
      isInvalid = true;
    }
    if (Flags & PD_DisallowProperty) {
      diagnose(VarLoc, diag::disallowed_property_decl);
      isInvalid = true;
    }
  } else if (Flags & PD_DisallowVar) {
    diagnose(VarLoc, diag::disallowed_var_decl);
    return true;
  }

  if (isInvalid) return true;

  // If this is a var in the top-level of script/repl translation unit, then
  // wrap the PatternBindingDecls in TopLevelCodeDecls, since they represents
  // executable code.
  if (IsMainModule && isa<TranslationUnit>(CurDeclContext)) {
    for (unsigned i = FirstDecl; i != Decls.size(); ++i) {
      auto *PBD = dyn_cast<PatternBindingDecl>(Decls[i]);
      if (PBD == 0) continue;
      auto *Brace = BraceStmt::create(Context, PBD->getStartLoc(),
                                      ExprStmtOrDecl(PBD), PBD->getEndLoc());

      auto *TLCD = new (Context) TopLevelCodeDecl(CurDeclContext, Brace);
      PBD->setDeclContext(TLCD);
      Decls[i] = TLCD;
    }
  }

  return isInvalid;
}

/// addImplicitThisParameter - Add an implicit 'this' parameter to the given
/// set of parameter clauses.
Pattern *Parser::buildImplicitThisParameter() {
  VarDecl *D
    = new (Context) VarDecl(SourceLoc(), Context.getIdentifier("this"),
                            Type(), CurDeclContext);
  Pattern *P = new (Context) NamedPattern(D);
  return new (Context) TypedPattern(P, TypeLoc());
}

Optional<ParserTokenRange>
Parser::consumeFunctionBody(unsigned Flags) {
  auto BeginParserPosition = getParserPosition();

  // Consume the '{', and find the matching '}'.
  consumeToken(tok::l_brace);
  unsigned OpenBraces = 1;
  while (OpenBraces != 0 && Tok.isNot(tok::eof)) {
    if (consumeIf(tok::l_brace)) {
      OpenBraces++;
      continue;
    }
    if (consumeIf(tok::r_brace)) {
      OpenBraces--;
      continue;
    }
    consumeToken();
  }
  if (OpenBraces != 0 && Tok.isNot(tok::code_complete)) {
    assert(Tok.is(tok::eof));
    // We hit EOF, and not every brace has a pair.  Recover by searching
    // for the next decl except variable decls and cutting off before
    // that point.
    backtrackToPosition(BeginParserPosition);
    consumeToken(tok::l_brace);
    while (Tok.isNot(tok::kw_var) && !isStartOfDecl(Tok, peekToken())) {
      consumeToken();
    }
  }

  auto EndLexerState = L->getStateForBeginningOfToken(Tok);

  // When doing code completion, skip function bodies that don't contain a code
  // completion token.
  if (Context.LangOpts.isCodeCompletion() &&
      !L->stateRangeHasCodeCompletionToken(
          BeginParserPosition.LS,
          EndLexerState,
          Context.LangOpts.CodeCompletionOffset))
    return Nothing;

  return ParserTokenRange(
            Context.AllocateObjectCopy(
                FunctionBodyParserState(BeginParserPosition,
                                        ScopeInfo.saveCurrentScope(),
                                        Flags)),
            Context.AllocateObjectCopy(EndLexerState));
}

/// parseDeclFunc - Parse a 'func' declaration, returning null on error.  The
/// caller handles this case and does recovery as appropriate.
///
///   decl-func:
///     'static'? 'func' attribute-list any-identifier generic-params?
///               func-signature stmt-brace?
///
/// NOTE: The caller of this method must ensure that the token sequence is
/// either 'func' or 'static' 'func'.
///
FuncDecl *Parser::parseDeclFunc(SourceLoc StaticLoc, unsigned Flags) {
  bool HasContainerType = Flags & PD_HasContainerType;

  // Reject 'static' functions at global scope.
  if (StaticLoc.isValid() && !HasContainerType) {
    diagnose(Tok, diag::static_func_decl_global_scope)
      .fixItRemove(Diagnostic::Range(StaticLoc, Tok.getLoc()));
    StaticLoc = SourceLoc();
  }
  
  SourceLoc FuncLoc = consumeToken(tok::kw_func);

  DeclAttributes Attributes;
  // FIXME: Implicitly add immutable attribute.
  parseAttributeList(Attributes);
  
  Identifier Name;
  SourceLoc NameLoc = Tok.getLoc();
  if (!(Flags & PD_AllowTopLevel) && !(Flags & PD_DisallowFuncDef) &&
      Tok.isAnyOperator()) {
    diagnose(Tok, diag::func_decl_nonglobal_operator);
    return 0;
  }
  if (parseAnyIdentifier(Name, diag::expected_identifier_in_decl, "func"))
    return 0;
  
  // Parse the generic-params, if present.
  Optional<Scope> GenericsScope;
  GenericsScope.emplace(this, ScopeKind::Generics);
  GenericParamList *GenericParams;

  // If the name is an operator token that ends in '<' and the following token
  // is an identifier, split the '<' off as a separate token. This allows things
  // like 'func ==<T>(x:T, y:T) {}' to parse as '==' with generic type variable
  // '<T>' as expected.
  if (Name.str().size() > 1 && Name.str().back() == '<'
      && Tok.is(tok::identifier)) {
    Name = Context.getIdentifier(Name.str().slice(0, Name.str().size() - 1));
    SourceLoc LAngleLoc = NameLoc.getAdvancedLoc(Name.str().size());
    GenericParams = parseGenericParameters(LAngleLoc);
  } else {
    GenericParams = maybeParseGenericParams();
  }

  // We force first type of a func declaration to be a tuple for consistency.
  //
  if (!Tok.is(tok::l_paren)) {
    diagnose(Tok, diag::func_decl_without_paren);
    return 0;
  }

  SmallVector<Pattern*, 8> ArgParams;
  SmallVector<Pattern*, 8> BodyParams;
  
  // If we're within a container, add an implicit first pattern to match the
  // container type as an element named 'this'.
  //
  // This turns an instance function "(int)->int" on FooTy into
  // "(this: [byref] FooTy)->(int)->int", and a static function
  // "(int)->int" on FooTy into "(this: [byref] FooTy.metatype)->(int)->int".
  // Note that we can't actually compute the type here until Sema.
  if (HasContainerType) {
    Pattern *thisPattern = buildImplicitThisParameter();
    ArgParams.push_back(thisPattern);
    BodyParams.push_back(thisPattern);
  }

  TypeRepr *FuncRetTy;
  if (parseFunctionSignature(ArgParams, BodyParams, FuncRetTy))
    return 0;

  // Enter the arguments for the function into a new function-body scope.  We
  // need this even if there is no function body to detect argument name
  // duplication.
  FuncExpr *FE = 0;
  {
    Scope S(this, ScopeKind::FunctionBody);
    
    FE = actOnFuncExprStart(FuncLoc, FuncRetTy, ArgParams, BodyParams);

    // Now that we have a context, update the generic parameters with that
    // context.
    if (GenericParams) {
      for (auto Param : *GenericParams) {
        Param.setDeclContext(FE);
      }
    }
    
    // Establish the new context.
    ContextChange CC(*this, FE);
    
    // Then parse the expression.
    NullablePtr<Stmt> Body;
    
    // Check to see if we have a "{" to start a brace statement.
    if (Flags & PD_DisallowFuncDef) {
      if (Tok.is(tok::l_brace)) {
        diagnose(Tok.getLoc(), diag::disallowed_func_def);
        consumeToken();
        skipUntil(tok::r_brace);
        consumeToken();
        return 0;
      }
    } else if (Attributes.AsmName.empty() || Tok.is(tok::l_brace)) {
      if ((!Context.LangOpts.DelayFunctionBodyParsing &&
           !Context.LangOpts.isCodeCompletion()) ||
          IsDelayedParsingSecondPass) {
        NullablePtr<BraceStmt> Body =
            parseBraceItemList(diag::func_decl_without_brace);
        if (Body.isNull()) {
          // FIXME: Should do some sort of error recovery here?
        } else {
          FE->setBody(Body.get());
        }
      } else {
        Optional<ParserTokenRange> TR = consumeFunctionBody(Flags);
        if (TR.hasValue())
          FE->setBodyTokenRange(TR.getValue());
        else
          FE->setBodySkipped();
      }
    }
  }

  // Exit the scope introduced for the generic parameters.
  GenericsScope.reset();

  // Create the decl for the func and add it to the parent scope.
  FuncDecl *FD = new (Context) FuncDecl(StaticLoc, FuncLoc, Name, NameLoc,
                                        GenericParams, Type(), FE,
                                        CurDeclContext);
  if (FE)
    FE->setDecl(FD);
  if (Attributes.isValid()) FD->getMutableAttrs() = Attributes;
  addToScope(FD);
  return FD;
}

bool Parser::parseDeclFuncBodyDelayed(FuncDecl *FD) {
  auto FE = FD->getBody();
  assert(!FE->getBody() && "function should not have a parsed body");
  assert(FE->getBodyKind() == FuncExpr::BodyKind::Unparsed &&
         "function body should be delayed");

  auto TokenRange = FE->getBodyTokenRange();
  auto FunctionParserState =
      TokenRange.getBeginParserState<FunctionBodyParserState>();
  if (!FunctionParserState)
    return false;

  auto EndLexerState = TokenRange.getEndLexerState<Lexer::State>();

  // Ensure that we restore the parser state at exit.
  ParserPositionRAII PPR(*this);

  // Create a lexer that can not go past the end state.
  Lexer LocalLex(*L, FunctionParserState->BeginParserPosition.LS,
                 *EndLexerState, SourceMgr, &Diags, nullptr /*not SIL*/);

  // Temporarily swap out the parser's current lexer with our new one.
  llvm::SaveAndRestore<Lexer *> T(L, &LocalLex);

  // Rewind to '{' of the function body.
  restoreParserPosition(FunctionParserState->BeginParserPosition);

  // Re-enter the lexical scope.
  Scope S(this, FunctionParserState->takeScope());
  ContextChange CC(*this, FE);

  NullablePtr<BraceStmt> Body =
      parseBraceItemList(diag::func_decl_without_brace);
  if (Body.isNull()) {
    // FIXME: Should do some sort of error recovery here?
    return true;
  } else {
    FE->setBody(Body.get());
  }

  return false;
}

/// parseDeclOneOf - Parse a 'oneof' declaration, returning true (and doing no
/// token skipping) on error.
///
///   decl-oneof:
///      'oneof' attribute-list identifier generic-params? inheritance?
///          '{' decl-oneof-body '}'
///   decl-oneof-body:
///      decl*
///
bool Parser::parseDeclOneOf(unsigned Flags, SmallVectorImpl<Decl*> &Decls) {
  SourceLoc OneOfLoc = consumeToken(tok::kw_oneof);

  DeclAttributes Attributes;
  parseAttributeList(Attributes);
  
  Identifier OneOfName;
  SourceLoc OneOfNameLoc;
  if (parseIdentifier(OneOfName, OneOfNameLoc,
                      diag::expected_identifier_in_decl, "oneof"))
    return true;

  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    GenericParams = maybeParseGenericParams();
  }

  OneOfDecl *OOD = new (Context) OneOfDecl(OneOfLoc,
                                           /*isEnum*/ false,
                                           OneOfName, OneOfNameLoc,
                                           { },
                                           GenericParams, CurDeclContext);
  Decls.push_back(OOD);

  // Now that we have a context, update the generic parameters with that
  // context.
  if (GenericParams)
    for (auto Param : *GenericParams)
      Param.setDeclContext(OOD);

  if (Attributes.isValid()) OOD->getMutableAttrs() = Attributes;

  // Parse optional inheritance clause.
  if (Tok.is(tok::colon)) {
    ContextChange CC(*this, OOD);
    SmallVector<TypeLoc, 2> Inherited;
    parseInheritance(Inherited);
    OOD->setInherited(Context.AllocateCopy(Inherited));
  }

  SourceLoc LBLoc, RBLoc;
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_oneof_type))
    return true;

  bool Invalid = false;
  // Parse the body.
  SmallVector<Decl*, 8> MemberDecls;
  {
    ContextChange CC(*this, OOD);
    Scope S(this, ScopeKind::ClassBody);
    Invalid |= parseNominalDeclMembers(MemberDecls, LBLoc, RBLoc,
                                       diag::expected_rbrace_oneof_type,
                                       PD_HasContainerType
                                         | PD_AllowOneOfElement
                                         | PD_DisallowVar);
  }
  
  OOD->setMembers(Context.AllocateCopy(MemberDecls), {LBLoc, RBLoc});
  addToScope(OOD);
  
  if (Flags & PD_DisallowNominalTypes) {
    diagnose(OneOfLoc, diag::disallowed_type);
    return true;
  }
  
  return Invalid;
}

/// parseDeclOneOfElement - Parse a 'case' of a oneof, returning true on error.
///
///   decl-oneof-element:
///      'case' identifier type-tuple? ('->' type)?
bool Parser::parseDeclOneOfElement(unsigned Flags,
                                   SmallVectorImpl<Decl*> &Decls) {
  SourceLoc CaseLoc = consumeToken(tok::kw_case);
  
  // TODO: Accept attributes here?
  
  Identifier Name;
  SourceLoc NameLoc;
  // For recovery, see if the user typed something resembling a switch "case"
  // label.
  if (!Tok.is(tok::identifier)) {
    NullablePtr<Pattern> pattern = parseMatchingPattern();
    if (pattern.isNull())
      return true;
    diagnose(CaseLoc, diag::case_outside_of_switch, "case");
    skipUntil(tok::colon);
    consumeIf(tok::colon);
    return true;
  }
  
  if (parseIdentifier(Name, NameLoc,
                      diag::expected_identifier_in_decl, "oneof case"))
    return true;
  
  // See if there's a following argument type.
  TypeRepr *ArgType = nullptr;
  if (Tok.isFollowingLParen()) {
    if (!(ArgType = parseTypeTupleBody()))
      return true;
  }
  
  // See if there's a result type.
  SourceLoc ArrowLoc;
  TypeRepr *ResultType = nullptr;
  if (Tok.is(tok::arrow)) {
    ArrowLoc = consumeToken();
    if (!(ResultType = parseType(diag::expected_type_oneof_element_result)))
      return true;
  }
  
  // For recovery, again make sure the the user didn't try to spell a switch
  // case label:
  // 'case Identifier:',
  // 'case Identifier, ...:', or
  // 'case Identifier where ...:'
  if (Tok.is(tok::colon) || Tok.is(tok::comma) || Tok.is(tok::kw_where)) {
    diagnose(CaseLoc, diag::case_outside_of_switch, "case");
    skipUntil(tok::colon);
    consumeIf(tok::colon);
    return true;
  }
  
  // Add the element.
  auto *result = new (Context) OneOfElementDecl(CaseLoc, NameLoc, Name,
                                                ArgType, ArrowLoc, ResultType,
                                                CurDeclContext);
  Decls.push_back(result);
  
  if (!(Flags & PD_AllowOneOfElement)) {
    diagnose(CaseLoc, diag::disallowed_oneof_element);
    return true;
  }
  
  return false;
}

/// \brief Parse the members in a struct/class/protocol definition.
///
///    decl*
bool Parser::parseNominalDeclMembers(SmallVectorImpl<Decl *> &memberDecls,
                                     SourceLoc LBLoc, SourceLoc &RBLoc,
                                     Diag<> ErrorDiag, unsigned flags) {
  bool previousHadSemi = true;
  return parseList(tok::r_brace, LBLoc, RBLoc, tok::semi, /*OptionalSep=*/true,
                   ErrorDiag, [&] () -> bool {
    // If the previous declaration didn't have a semicolon and this new
    // declaration doesn't start a line, complain.
    if (!previousHadSemi && !Tok.isAtStartOfLine()) {
      SourceLoc endOfPrevious = getEndOfPreviousLoc();
      diagnose(endOfPrevious, diag::declaration_same_line_without_semi)
        .fixItInsert(endOfPrevious, ";");
      // FIXME: Add semicolon to the AST?
    }

    previousHadSemi = false;
    if (parseDecl(memberDecls, flags))
      return true;

    // Check whether the previous declaration had a semicolon after it.
    if (!memberDecls.empty() && memberDecls.back()->TrailingSemiLoc.isValid())
      previousHadSemi = true;

    return false;
  });
}

/// parseDeclStruct - Parse a 'struct' declaration, returning true (and doing no
/// token skipping) on error.
///
///   decl-struct:
///      'struct' attribute-list identifier generic-params? inheritance?
///          '{' decl-struct-body '}
///   decl-struct-body:
///      decl*
///
bool Parser::parseDeclStruct(unsigned Flags, SmallVectorImpl<Decl*> &Decls) {
  SourceLoc StructLoc = consumeToken(tok::kw_struct);
  
  DeclAttributes Attributes;
  parseAttributeList(Attributes);

  Identifier StructName;
  SourceLoc StructNameLoc;
  if (parseIdentifier(StructName, StructNameLoc,
                      diag::expected_identifier_in_decl, "struct"))
    return true;

  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    GenericParams = maybeParseGenericParams();
  }

  StructDecl *SD = new (Context) StructDecl(StructLoc, StructName,
                                            StructNameLoc,
                                            { },
                                            GenericParams,
                                            CurDeclContext);
  Decls.push_back(SD);

  if (Attributes.isValid()) SD->getMutableAttrs() = Attributes;

  // Now that we have a context, update the generic parameters with that
  // context.
  if (GenericParams) {
    for (auto Param : *GenericParams) {
      Param.setDeclContext(SD);
    }
  }

  // Parse optional inheritance clause.
  if (Tok.is(tok::colon)) {
    ContextChange CC(*this, SD);
    SmallVector<TypeLoc, 2> Inherited;
    parseInheritance(Inherited);
    SD->setInherited(Context.AllocateCopy(Inherited));
  }

  SourceLoc LBLoc, RBLoc;
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_struct))
    return true;

  bool Invalid = false;

  // Parse the body.
  SmallVector<Decl*, 8> MemberDecls;
  {
    ContextChange CC(*this, SD);
    Scope S(this, ScopeKind::StructBody);
    Invalid |= parseNominalDeclMembers(MemberDecls, LBLoc, RBLoc,
                                       diag::expected_rbrace_struct,
                                       PD_HasContainerType);
  }

  SD->setMembers(Context.AllocateCopy(MemberDecls), { LBLoc, RBLoc });
  addToScope(SD);

  if (Flags & PD_DisallowNominalTypes) {
    diagnose(StructLoc, diag::disallowed_type);
    return true;
  }

  return Invalid;
}

/// parseDeclClass - Parse a 'class' declaration, returning true (and doing no
/// token skipping) on error.  
///
///   decl-class:
///      'class' attribute-list identifier generic-params? inheritance?
///          '{' decl-class-body '}
///   decl-class-body:
///      decl*
///
bool Parser::parseDeclClass(unsigned Flags, SmallVectorImpl<Decl*> &Decls) {
  SourceLoc ClassLoc = consumeToken(tok::kw_class);
  
  DeclAttributes Attributes;
  parseAttributeList(Attributes);

  Identifier ClassName;
  SourceLoc ClassNameLoc;
  SourceLoc LBLoc, RBLoc;
  if (parseIdentifier(ClassName, ClassNameLoc,
                      diag::expected_identifier_in_decl, "class"))
    return true;

  // Parse the generic-params, if present.
  GenericParamList *GenericParams = nullptr;
  {
    Scope S(this, ScopeKind::Generics);
    GenericParams = maybeParseGenericParams();
  }

  // Create the class.
  ClassDecl *CD = new (Context) ClassDecl(ClassLoc, ClassName, ClassNameLoc,
                                          { }, GenericParams, CurDeclContext);
  Decls.push_back(CD);

  // Now that we have a context, update the generic parameters with that
  // context.
  if (GenericParams) {
    for (auto Param : *GenericParams) {
      Param.setDeclContext(CD);
    }
  }

  // Attach attributes.
  if (Attributes.isValid()) CD->getMutableAttrs() = Attributes;

  // Parse optional inheritance clause within the context of the class.
  SmallVector<TypeLoc, 2> Inherited;
  if (Tok.is(tok::colon)) {
    ContextChange CC(*this, CD);    
    parseInheritance(Inherited);
    CD->setInherited(Context.AllocateCopy(Inherited));
  }

  // Parse the class body.
  if (parseToken(tok::l_brace, LBLoc, diag::expected_lbrace_class))
    return true;

  bool Invalid = false;

  // Parse the body.
  SmallVector<Decl*, 8> MemberDecls;
  {
    ContextChange CC(*this, CD);
    Scope S(this, ScopeKind::ClassBody);
    Invalid |= parseNominalDeclMembers(MemberDecls, LBLoc, RBLoc,
                                       diag::expected_rbrace_class,
                                       PD_HasContainerType|PD_AllowDestructor);
  }

  bool hasConstructor = false;
  for (Decl *Member : MemberDecls) {
    if (isa<ConstructorDecl>(Member))
      hasConstructor = true;
  }

  if (!hasConstructor) {
    VarDecl *ThisDecl
      = new (Context) VarDecl(SourceLoc(), Context.getIdentifier("this"),
                              Type(), CD);
    Pattern *Arguments = TuplePattern::create(Context, SourceLoc(),
                                              ArrayRef<TuplePatternElt>(),
                                              SourceLoc());
    ConstructorDecl *Constructor =
        new (Context) ConstructorDecl(Context.getIdentifier("constructor"),
                                     SourceLoc(), Arguments, ThisDecl,
                                     nullptr, CD);
    Constructor->setImplicit();
    ThisDecl->setDeclContext(Constructor);
    MemberDecls.push_back(Constructor);
  }

  CD->setMembers(Context.AllocateCopy(MemberDecls), { LBLoc, RBLoc });
  addToScope(CD);

  if (Flags & PD_DisallowNominalTypes) {
    diagnose(ClassLoc, diag::disallowed_type);
    return true;
  }

  return Invalid;
}

/// parseDeclProtocol - Parse a 'protocol' declaration, returning null (and
/// doing no token skipping) on error.
///
///   decl-protocol:
///      protocol-head '{' protocol-member* '}'
///
///   protocol-head:
///     'protocol' attribute-list identifier inheritance? 
///
///   protocol-member:
///      decl-func
///      decl-var-simple
///      decl-typealias
///
Decl *Parser::parseDeclProtocol(unsigned Flags) {
  SourceLoc ProtocolLoc = consumeToken(tok::kw_protocol);
  
  DeclAttributes Attributes;
  parseAttributeList(Attributes);
  
  SourceLoc NameLoc;
  Identifier ProtocolName;
  if (parseIdentifier(ProtocolName, NameLoc,
                      diag::expected_identifier_in_decl, "protocol"))
    return 0;
  
  SmallVector<TypeLoc, 4> InheritedProtocols;
  if (Tok.is(tok::colon))
    parseInheritance(InheritedProtocols);

  ProtocolDecl *Proto
    = new (Context) ProtocolDecl(CurDeclContext, ProtocolLoc, NameLoc,
                                 ProtocolName,
                                 Context.AllocateCopy(InheritedProtocols));

  if (Attributes.isValid()) Proto->getMutableAttrs() = Attributes;

  ContextChange CC(*this, Proto);
  Scope ProtocolBodyScope(this, ScopeKind::ProtocolBody);

  {
    // Parse the body.
    SourceLoc LBraceLoc = Tok.getLoc();
    if (parseToken(tok::l_brace, diag::expected_lbrace_protocol_type))
      return nullptr;
    
    // Parse the list of protocol elements.
    SmallVector<Decl*, 8> Members;

    // Add the implicit 'This' associated type.
    // FIXME: Mark as 'implicit'.
    Members.push_back(new (Context) TypeAliasDecl(ProtocolLoc,
                                                  Context.getIdentifier("This"),
                                                  ProtocolLoc, TypeLoc(),
                                                  CurDeclContext,
                                                  MutableArrayRef<TypeLoc>()));

    SourceLoc RBraceLoc;
    // Parse the members.
    if (parseNominalDeclMembers(Members, LBraceLoc, RBraceLoc,
                                diag::expected_rbrace_protocol,
                                PD_HasContainerType|PD_DisallowProperty|
                                PD_DisallowFuncDef|PD_DisallowNominalTypes|
                                PD_DisallowInit|PD_DisallowTypeAliasDef))
      return nullptr;

    // Install the protocol elements.
    Proto->setMembers(Context.AllocateCopy(Members),
                      SourceRange(LBraceLoc, RBraceLoc));
  }
  
  if (Flags & PD_DisallowNominalTypes) {
    diagnose(ProtocolLoc, diag::disallowed_type);
    return nullptr;
  } else if (!(Flags & PD_AllowTopLevel)) {
    diagnose(ProtocolLoc, diag::decl_inner_scope);
    return nullptr;
  }

  return Proto;
}

namespace {
  /// Recursively walks a pattern and sets all variables' decl contexts to the
  /// given context.
  class SetVarContext : public ASTWalker {
    DeclContext *CurDeclContext;

  public:
    SetVarContext(DeclContext *Context) : CurDeclContext(Context) {}

    Pattern *walkToPatternPost(Pattern *P) override {
      // Handle vars.
      if (auto *Named = dyn_cast<NamedPattern>(P))
        Named->getDecl()->setDeclContext(CurDeclContext);
      return P;
    }
  };
}

/// parseDeclSubscript - Parse a 'subscript' declaration, returning true
/// on error.
///
///   decl-subscript:
///     subscript-head get-set
///   subscript-head
///     'subscript' attribute-list pattern-tuple '->' type
///
bool Parser::parseDeclSubscript(bool HasContainerType,
                                bool NeedDefinition,
                                SmallVectorImpl<Decl *> &Decls) {
  bool Invalid = false;
  SourceLoc SubscriptLoc = consumeToken(tok::kw_subscript);

  // attribute-list
  DeclAttributes Attributes;
  parseAttributeList(Attributes);
  
  // pattern-tuple
  if (Tok.isNot(tok::l_paren)) {
    diagnose(Tok.getLoc(), diag::expected_lparen_subscript);
    return true;
  }

  NullablePtr<Pattern> Indices = parsePatternTuple(/*AllowInitExpr=*/false);
  if (Indices.isNull())
    return true;
  Indices.get()->walk(SetVarContext(CurDeclContext));

  // '->'
  if (!Tok.is(tok::arrow)) {
    diagnose(Tok.getLoc(), diag::expected_arrow_subscript);
    return true;
  }
  SourceLoc ArrowLoc = consumeToken();
  
  // type
  TypeRepr *ElementTy = parseTypeAnnotation(diag::expected_type_subscript);
  if (!ElementTy)
    return true;
  
  if (!NeedDefinition) {
    SubscriptDecl *Subscript
      = new (Context) SubscriptDecl(Context.getIdentifier("__subscript"),
                                    SubscriptLoc, Indices.get(), ArrowLoc,
                                    ElementTy, SourceRange(),
                                    0, 0, CurDeclContext);
    Decls.push_back(Subscript);
    return false;
  }
  
  // '{'
  if (!Tok.is(tok::l_brace)) {
    diagnose(Tok.getLoc(), diag::expected_lbrace_subscript);
    return true;
  }
  SourceLoc LBLoc = consumeToken();
  
  // Parse getter and setter.
  FuncDecl *Get = 0;
  FuncDecl *Set = 0;
  SourceLoc LastValidLoc = LBLoc;
  if (parseGetSet(HasContainerType, Indices.get(), ElementTy,
                  Get, Set, LastValidLoc))
    Invalid = true;

  // Parse the final '}'.
  SourceLoc RBLoc;
  if (Invalid) {
    skipUntilDeclRBrace();
    RBLoc = LastValidLoc;
  }

  if (parseMatchingToken(tok::r_brace, RBLoc, diag::expected_rbrace_in_getset,
                         LBLoc)) {
    RBLoc = LastValidLoc;
  }

  if (!Get) {
    if (!Invalid)
      diagnose(SubscriptLoc, diag::subscript_without_get);
    Invalid = true;
  }

  // Reject 'subscript' functions outside of type decls
  if (!HasContainerType) {
    diagnose(SubscriptLoc, diag::subscript_decl_wrong_scope);
    Invalid = true;
  }

  if (!Invalid) {
    // FIXME: We should build the declarations even if they are invalid.

    // Build an AST for the subscript declaration.
    SubscriptDecl *Subscript
      = new (Context) SubscriptDecl(Context.getIdentifier("__subscript"),
                                    SubscriptLoc, Indices.get(), ArrowLoc,
                                    ElementTy, SourceRange(LBLoc, RBLoc),
                                    Get, Set, CurDeclContext);

    // FIXME: Order of get/set not preserved.
    if (Set) {
      Set->setDeclContext(CurDeclContext);
      Set->makeSetter(Subscript);
      Decls.push_back(Set);
    }

    if (Get) {
      Get->setDeclContext(CurDeclContext);
      Get->makeGetter(Subscript);
      Decls.push_back(Get);
    }

    Decls.push_back(Subscript);
  }
  return Invalid;
}

static void AddConstructorArgumentsToScope(const Pattern *pat,
                                           ConstructorDecl *CD,
                                           Parser &P) {
  switch (pat->getKind()) {
  case PatternKind::Named: {
    // Reparent the decl and add it to the scope.
    VarDecl *var = cast<NamedPattern>(pat)->getDecl();
    var->setDeclContext(CD);
    P.addToScope(var);
    return;
  }

  case PatternKind::Any:
    return;

  case PatternKind::Paren:
    AddConstructorArgumentsToScope(cast<ParenPattern>(pat)->getSubPattern(),
                                   CD, P);
    return;

  case PatternKind::Typed:
    AddConstructorArgumentsToScope(cast<TypedPattern>(pat)->getSubPattern(),
                                   CD, P);
    return;

  case PatternKind::Tuple:
    for (const TuplePatternElt &field : cast<TuplePattern>(pat)->getFields())
      AddConstructorArgumentsToScope(field.getPattern(), CD, P);
    return;

#define PATTERN(Id, Parent)
#define REFUTABLE_PATTERN(Id, Parent) case PatternKind::Id:
#include "swift/AST/PatternNodes.def"
    llvm_unreachable("pattern can't appear as a constructor argument!");
  }
  llvm_unreachable("bad pattern kind!");
}


ConstructorDecl *Parser::parseDeclConstructor(bool HasContainerType) {
  SourceLoc ConstructorLoc = consumeToken(tok::kw_constructor);
  
  // Reject 'constructor' functions outside of types
  if (!HasContainerType) {
    diagnose(Tok, diag::constructor_decl_wrong_scope);
    return nullptr;
  }

  // attribute-list
  DeclAttributes Attributes;
  parseAttributeList(Attributes);

  // Parse the generic-params, if present.
  Scope S(this, ScopeKind::Generics);
  GenericParamList *GenericParams = maybeParseGenericParams();

  // pattern-tuple
  if (!Tok.is(tok::l_paren)) {
    diagnose(Tok.getLoc(), diag::expected_lparen_constructor);
    return nullptr;
  }

  NullablePtr<Pattern> Arguments = parsePatternTuple(/*AllowInitExpr=*/true);
  if (Arguments.isNull())
    return nullptr;

  // '{'
  if (!Tok.is(tok::l_brace)) {
    diagnose(Tok.getLoc(), diag::expected_lbrace_constructor);
    return nullptr;
  }

  VarDecl *ThisDecl
    = new (Context) VarDecl(SourceLoc(), Context.getIdentifier("this"),
                            Type(), CurDeclContext);

  Scope S2(this, ScopeKind::ConstructorBody);
  ConstructorDecl *CD =
      new (Context) ConstructorDecl(Context.getIdentifier("constructor"),
                                    ConstructorLoc, Arguments.get(), ThisDecl,
                                    GenericParams, CurDeclContext);
  ThisDecl->setDeclContext(CD);
  if (GenericParams) {
    for (auto Param : *GenericParams)
      Param.setDeclContext(CD);
  }
  AddConstructorArgumentsToScope(Arguments.get(), CD, *this);
  addToScope(ThisDecl);
  ContextChange CC(*this, CD);

  NullablePtr<BraceStmt> Body = parseBraceItemList(diag::invalid_diagnostic);

  if (!Body.isNull())
    CD->setBody(Body.get());

  if (Attributes.isValid()) CD->getMutableAttrs() = Attributes;

  return CD;
}


DestructorDecl *Parser::parseDeclDestructor(unsigned Flags) {
  SourceLoc DestructorLoc = consumeToken(tok::kw_destructor);

  // attribute-list
  DeclAttributes Attributes;
  parseAttributeList(Attributes);

  // '{'
  if (!Tok.is(tok::l_brace)) {
    diagnose(Tok.getLoc(), diag::expected_lbrace_destructor);
    return nullptr;
  }

  VarDecl *ThisDecl
    = new (Context) VarDecl(SourceLoc(), Context.getIdentifier("this"),
                            Type(), CurDeclContext);

  Scope S(this, ScopeKind::DestructorBody);
  DestructorDecl *DD =
      new (Context) DestructorDecl(Context.getIdentifier("destructor"),
                                   DestructorLoc, ThisDecl, CurDeclContext);
  ThisDecl->setDeclContext(DD);
  addToScope(ThisDecl);
  ContextChange CC(*this, DD);

  NullablePtr<BraceStmt> Body = parseBraceItemList(diag::invalid_diagnostic);

  if (!Body.isNull())
    DD->setBody(Body.get());

  if (Attributes.isValid()) DD->getMutableAttrs() = Attributes;

  // Reject 'destructor' functions outside of classes
  if (!(Flags & PD_AllowDestructor)) {
    diagnose(DestructorLoc, diag::destructor_decl_outside_class);
    return nullptr;
  }

  return DD;
}

OperatorDecl *Parser::parseDeclOperator(bool AllowTopLevel) {
  assert(Tok.isContextualKeyword("operator") &&
         "no 'operator' at start of operator decl?!");
  

  SourceLoc OperatorLoc = consumeToken(tok::identifier);

  auto kind = llvm::StringSwitch<Optional<DeclKind>>(Tok.getText())
    .Case("prefix", DeclKind::PrefixOperator)
    .Case("postfix", DeclKind::PostfixOperator)
    .Case("infix", DeclKind::InfixOperator)
    .Default(Nothing);
  
  assert(kind && "no fixity after 'operator'?!");

  SourceLoc KindLoc = consumeToken(tok::identifier);
  
  if (!Tok.isAnyOperator()) {
    diagnose(Tok, diag::expected_operator_name_after_operator);
    return nullptr;
  }
  
  Identifier Name = Context.getIdentifier(Tok.getText());
  SourceLoc NameLoc = consumeToken();
  
  if (!Tok.is(tok::l_brace)) {
    diagnose(Tok, diag::expected_lbrace_after_operator);
    return nullptr;
  }
  
  OperatorDecl *result;
  
  switch (*kind) {
  case DeclKind::PrefixOperator:
    result = parseDeclPrefixOperator(OperatorLoc, KindLoc, Name, NameLoc);
    break;
  case DeclKind::PostfixOperator:
    result = parseDeclPostfixOperator(OperatorLoc, KindLoc, Name, NameLoc);
    break;
  case DeclKind::InfixOperator:
    result = parseDeclInfixOperator(OperatorLoc, KindLoc, Name, NameLoc);
    break;
  default:
    llvm_unreachable("impossible");
  }
  
  if (Tok.is(tok::r_brace))
    consumeToken();
  
  if (!AllowTopLevel) {
    diagnose(OperatorLoc, diag::operator_decl_inner_scope);
    return nullptr;
  }
  
  return result;
}

OperatorDecl *Parser::parseDeclPrefixOperator(SourceLoc OperatorLoc,
                                              SourceLoc PrefixLoc,
                                              Identifier Name,
                                              SourceLoc NameLoc) {
  SourceLoc LBraceLoc = consumeToken(tok::l_brace);
  
  while (!Tok.is(tok::r_brace)) {
    // Currently there are no operator attributes for prefix operators.
    if (Tok.is(tok::identifier))
      diagnose(Tok, diag::unknown_prefix_operator_attribute, Tok.getText());
    else
      diagnose(Tok, diag::expected_operator_attribute);
    skipUntilDeclRBrace();
    return nullptr;
  }
  
  SourceLoc RBraceLoc = Tok.getLoc();

  return new (Context) PrefixOperatorDecl(CurDeclContext,
                                          OperatorLoc,
                                          PrefixLoc,
                                          Name, NameLoc,
                                          LBraceLoc, RBraceLoc);
}

OperatorDecl *Parser::parseDeclPostfixOperator(SourceLoc OperatorLoc,
                                              SourceLoc PostfixLoc,
                                              Identifier Name,
                                              SourceLoc NameLoc) {
  SourceLoc LBraceLoc = consumeToken(tok::l_brace);
  
  while (!Tok.is(tok::r_brace)) {
    // Currently there are no operator attributes for postfix operators.
    if (Tok.is(tok::identifier))
      diagnose(Tok, diag::unknown_postfix_operator_attribute, Tok.getText());
    else
      diagnose(Tok, diag::expected_operator_attribute);
    skipUntilDeclRBrace();
    return nullptr;
  }
  
  SourceLoc RBraceLoc = Tok.getLoc();
  
  return new (Context) PostfixOperatorDecl(CurDeclContext,
                                          OperatorLoc,
                                          PostfixLoc,
                                          Name, NameLoc,
                                          LBraceLoc, RBraceLoc);
}

OperatorDecl *Parser::parseDeclInfixOperator(SourceLoc OperatorLoc,
                                             SourceLoc InfixLoc,
                                             Identifier Name,
                                             SourceLoc NameLoc) {
  SourceLoc LBraceLoc = consumeToken(tok::l_brace);

  // Initialize InfixData with default attributes:
  // precedence 100, associativity none
  unsigned precedence = 100;
  Associativity associativity = Associativity::None;
  
  SourceLoc AssociativityLoc, AssociativityValueLoc,
    PrecedenceLoc, PrecedenceValueLoc;
  
  while (!Tok.is(tok::r_brace)) {
    if (!Tok.is(tok::identifier)) {
      diagnose(Tok, diag::expected_operator_attribute);
      skipUntilDeclRBrace();
      return nullptr;
    }
    
    if (Tok.getText().equals("associativity")) {
      if (AssociativityLoc.isValid()) {
        diagnose(Tok, diag::operator_associativity_redeclared);
        skipUntilDeclRBrace();
        return nullptr;
      }
      AssociativityLoc = consumeToken();
      if (!Tok.is(tok::identifier)) {
        diagnose(Tok, diag::expected_infix_operator_associativity);
        skipUntilDeclRBrace();
        return nullptr;
      }
      auto parsedAssociativity
        = llvm::StringSwitch<Optional<Associativity>>(Tok.getText())
          .Case("none", Associativity::None)
          .Case("left", Associativity::Left)
          .Case("right", Associativity::Right)
          .Default(Nothing);
      if (!parsedAssociativity) {
        diagnose(Tok, diag::unknown_infix_operator_associativity, Tok.getText());
        skipUntilDeclRBrace();
        return nullptr;
      }
      associativity = *parsedAssociativity;

      AssociativityValueLoc = consumeToken();
      continue;
    }
    
    if (Tok.getText().equals("precedence")) {
      if (PrecedenceLoc.isValid()) {
        diagnose(Tok, diag::operator_precedence_redeclared);
        skipUntilDeclRBrace();
        return nullptr;
      }
      PrecedenceLoc = consumeToken();
      if (!Tok.is(tok::integer_literal)) {
        diagnose(Tok, diag::expected_infix_operator_precedence);
        skipUntilDeclRBrace();
        return nullptr;
      }
      bool error = Tok.getText().getAsInteger(0, precedence);
      assert(!error && "integer literal precedence did not parse as integer?!");
      (void)error;
      
      PrecedenceValueLoc = consumeToken();
      continue;
    }
    
    diagnose(Tok, diag::unknown_infix_operator_attribute, Tok.getText());
    skipUntilDeclRBrace();
    return nullptr;
  }
  
  SourceLoc RBraceLoc = Tok.getLoc();
  
  return new (Context) InfixOperatorDecl(CurDeclContext,
                                         OperatorLoc, InfixLoc,
                                         Name, NameLoc,
                                         LBraceLoc,
                                         AssociativityLoc, AssociativityValueLoc,
                                         PrecedenceLoc, PrecedenceValueLoc,
                                         RBraceLoc,
                                         InfixData(precedence, associativity));
}
