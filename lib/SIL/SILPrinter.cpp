//===--- SILPrinter.cpp - Pretty-printing of SIL Code ---------------------===//
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
// This file defines the logic to pretty-print SIL, Instructions, etc.
//
//===----------------------------------------------------------------------===//

#include "swift/Strings.h"
#include "swift/Basic/Demangle.h"
#include "swift/Basic/QuotedString.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/SILVTable.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/PrintOptions.h"
#include "swift/AST/Types.h"
#include "swift/Basic/STLExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormattedStream.h"

using namespace swift;
using namespace Demangle;

struct ID {
  enum ID_Kind {
    SILBasicBlock, SILUndef, SSAValue
  } Kind;
  unsigned Number;
  int ResultNumber;

  // A stable ordering of ID objects.
  bool operator<(ID Other) const {
    if (unsigned(Kind) < unsigned(Other.Kind))
      return true;
    if (Number < Other.Number)
      return true;
    if (ResultNumber < Other.ResultNumber)
      return true;
    return false;
  }
};

enum SILColorKind {
  SC_Type,
};

namespace {
/// RAII based coloring of SIL output.
class SILColor {
  raw_ostream &OS;
  enum raw_ostream::Colors Color;
public:
#define DEF_COL(NAME, RAW) case NAME: Color = raw_ostream::RAW; break;

  explicit SILColor(raw_ostream &OS, SILColorKind K) : OS(OS) {
    if (!OS.has_colors())
      return;
    switch (K) {
      DEF_COL(SC_Type, YELLOW)
    }
    OS.resetColor();
    OS.changeColor(Color);
  }

  explicit SILColor(raw_ostream &OS, ID::ID_Kind K) : OS(OS) {
    if (!OS.has_colors())
      return;
    switch (K) {
      DEF_COL(ID::SILUndef, RED)
      DEF_COL(ID::SILBasicBlock, GREEN)
      DEF_COL(ID::SSAValue, MAGENTA)
    }
    OS.resetColor();
    OS.changeColor(Color);
  }
  
  ~SILColor() {
    if (!OS.has_colors())
      return;
    // FIXME: instead of resetColor(), we can look into
    // capturing the current active color and restoring it.
    OS.resetColor();
  }
#undef DEF_COL
};
}

static raw_ostream &operator<<(raw_ostream &OS, ID i) {
  SILColor C(OS, i.Kind);
  switch (i.Kind) {
  case ID::SILUndef: OS << "undef"; return OS;
  case ID::SILBasicBlock: OS << "bb"; break;
  case ID::SSAValue: OS << '%'; break;
  }
  OS << i.Number;

  if (i.ResultNumber != -1)
    OS << '#' << i.ResultNumber;
  return OS;
}

/// IDAndType - Used when a client wants to print something like "%0 : $Int".
struct IDAndType {
  ID id;
  SILType Ty;
};

static raw_ostream &operator<<(raw_ostream &OS, IDAndType i) {
  SILColor C(OS, SC_Type);
  return OS << i.id << " : " << i.Ty;
}

/// Return the fully qualified dotted path for DeclContext.
static void printFullContext(const DeclContext *Context, raw_ostream &Buffer) {
  if (!Context)
    return;
  switch (Context->getContextKind()) {
  case DeclContextKind::Module:
    if (Context == cast<Module>(Context)->Ctx.TheBuiltinModule)
      Buffer << cast<Module>(Context)->Name << ".";
    return;

  case DeclContextKind::FileUnit:
    // Ignore the file; just print the module.
    printFullContext(Context->getParent(), Buffer);
    return;

  case DeclContextKind::Initializer:
    // FIXME
    Buffer << "<initializer>";
    return;

  case DeclContextKind::AbstractClosureExpr:
    // FIXME
    Buffer << "<anonymous function>";
    return;

  case DeclContextKind::NominalTypeDecl: {
    const NominalTypeDecl *Nominal = cast<NominalTypeDecl>(Context);
    printFullContext(Nominal->getDeclContext(), Buffer);
    Buffer << Nominal->getName() << ".";
    return;
  }

  case DeclContextKind::ExtensionDecl: {
    Type Ty = cast<ExtensionDecl>(Context)->getExtendedType();
    TypeBase *Base = Ty->getCanonicalType().getPointer();
    const NominalTypeDecl *ExtNominal = 0;
    switch (Base->getKind()) {
      default:
        llvm_unreachable("unhandled context kind in SILPrint!");
      case TypeKind::Enum:
        ExtNominal = cast<EnumType>(Base)->getDecl();
        break;
      case TypeKind::Struct:
        ExtNominal = cast<StructType>(Base)->getDecl();
        break;
      case TypeKind::Class:
        ExtNominal = cast<ClassType>(Base)->getDecl();
        break;
      case TypeKind::BoundGenericEnum:
        ExtNominal = cast<BoundGenericEnumType>(Base)->getDecl();
        break;
      case TypeKind::BoundGenericStruct:
        ExtNominal = cast<BoundGenericStructType>(Base)->getDecl();
        break;
      case TypeKind::BoundGenericClass:
        ExtNominal = cast<BoundGenericClassType>(Base)->getDecl();
        break;
    }
    printFullContext(ExtNominal->getDeclContext(), Buffer);
    Buffer << ExtNominal->getName() << ".";
    return;
  }

  case DeclContextKind::TopLevelCodeDecl:
    llvm_unreachable("unhandled context kind in SILPrint!");
  case DeclContextKind::AbstractFunctionDecl:
    // FIXME
    Buffer << "<abstract function>";
    return;
  }
  llvm_unreachable("bad decl context");
}

/// SILDeclRef uses sigil "#" and prints the fully qualified dotted path.
void SILDeclRef::print(raw_ostream &OS) const {
  OS << "#";
  if (isNull()) {
    OS << "<null>";
    return;
  }

  bool isDot = true;
  if (!hasDecl()) {
    OS << "<anonymous function>";
  } else if (kind == SILDeclRef::Kind::Func) {
    auto *FD = cast<FuncDecl>(getDecl());
    ValueDecl *decl = FD;
    const char *Suffix;
    switch (FD->getAccessorKind()) {
    case AccessorKind::IsWillSet:
    case AccessorKind::IsDidSet:  assert(0 && "Shouldn't reach here");
    case AccessorKind::NotAccessor:
      Suffix = "";
      isDot = false;
      break;
    case AccessorKind::IsGetter:
      Suffix = "!getter";
      decl = FD->getAccessorStorageDecl();
      break;
    case AccessorKind::IsSetter:
      Suffix = "!setter";
      decl = FD->getAccessorStorageDecl();
      break;
    }

    printFullContext(decl->getDeclContext(), OS);
    assert(!decl->getName().empty());

    OS << decl->getName() << Suffix;
  } else {
    printFullContext(getDecl()->getDeclContext(), OS);
    OS << getDecl()->getName();
  }
  switch (kind) {
  case SILDeclRef::Kind::Func:
    break;
  case SILDeclRef::Kind::Allocator:
    OS << "!allocator";
    break;
  case SILDeclRef::Kind::Initializer:
    OS << "!initializer";
    break;
  case SILDeclRef::Kind::EnumElement:
    OS << "!enumelt";
    break;
  case SILDeclRef::Kind::Destroyer:
    OS << "!destroyer";
    break;
  case SILDeclRef::Kind::Deallocator:
    OS << "!deallocator";
    break;
  case SILDeclRef::Kind::IVarInitializer:
    OS << "!ivarinitializer";
    break;
  case SILDeclRef::Kind::IVarDestroyer:
    OS << "!ivardestroyer";
    break;
  case SILDeclRef::Kind::GlobalAccessor:
    OS << "!globalaccessor";
    break;
  case SILDeclRef::Kind::DefaultArgGenerator:
    OS << "!defaultarg" << "." << defaultArgIndex;
    break;
  }
  if (uncurryLevel != 0)
    OS << (isDot ? '.' : '!')  << uncurryLevel;

  if (isForeign)
    OS << ((isDot || uncurryLevel != 0) ? '.' : '!')  << "foreign";
}

void SILDeclRef::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

static void print(raw_ostream &OS, SILValueCategory category) {
  switch (category) {
  case SILValueCategory::Object: return;
  case SILValueCategory::Address: OS << '*'; return;
  case SILValueCategory::LocalStorage: OS << "*@local_storage "; return;
  }
  llvm_unreachable("bad value category!");
}
      
void SILType::print(raw_ostream &OS) const {
  SILColor C(OS, SC_Type);
  OS << '$';

  // Potentially add a leading sigil for the value category.
  ::print(OS, getCategory());

  // For the Self archetype of a protocol, print @sil_self protocol.
  if (auto archetypeTy = getSwiftRValueType()->getAs<ArchetypeType>()) {
    if (auto proto = archetypeTy->getSelfProtocol()) {
      OS << "@sil_self ";
      proto->getDeclaredType()->print(OS);
      return;
    }
  }

  // Print other types as their Swift representation.
  getSwiftRValueType().print(OS);
}

void SILType::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

namespace {
  
/// SILPrinter class - This holds the internal implementation details of
/// printing SIL structures.
class SILPrinter : public SILVisitor<SILPrinter> {
  llvm::formatted_raw_ostream OS;
  SILValue subjectValue;
  bool Verbose;

  llvm::DenseMap<const SILBasicBlock *, unsigned> BlocksToIDMap;

  llvm::DenseMap<const ValueBase*, unsigned> ValueToIDMap;
public:
  SILPrinter(raw_ostream &OS, bool V = false) : OS(OS), Verbose(V) {
  }

  ID getID(const SILBasicBlock *B);
  ID getID(SILValue V);
  IDAndType getIDAndType(SILValue V) {
    return { getID(V), V.getType() };
  }

  //===--------------------------------------------------------------------===//
  // Big entrypoints.
  void print(const SILFunction *F) {
    interleave(*F,
               [&](const SILBasicBlock &B) { print(&B); },
               [&] { OS << '\n'; });
  }

  void print(const SILBasicBlock *BB) {
    OS << getID(BB);

    if (!BB->bbarg_empty()) {
      OS << '(';
      for (auto I = BB->bbarg_begin(), E = BB->bbarg_end(); I != E; ++I) {
        if (I != BB->bbarg_begin()) OS << ", ";
        OS << getIDAndType(*I);
      }
      OS << ')';
    }

    OS << ":";

    if (!BB->pred_empty()) {
      OS.PadToColumn(50);
      
      OS << "// Preds:";

      // Display the predecessors ids sorted to give a stable use order in the
      // printer's output. This makes diffing large sections of SIL
      // significantly easier.
      llvm::SmallVector<ID, 32> PredIDs;
      for (auto *BBI : BB->getPreds())
        PredIDs.push_back(getID(BBI));
      std::sort(PredIDs.begin(), PredIDs.end());
      for (auto Id : PredIDs)
        OS << ' ' << Id;
    }
    OS << '\n';

    for (const SILInstruction &I : *BB)
      print(&I);
  }

  //===--------------------------------------------------------------------===//
  // SILInstruction Printing Logic

  void print(SILValue V) {
    if (auto *FRI = dyn_cast<FunctionRefInst>(V))
      OS << "  // function_ref "
         << demangleSymbolAsString(FRI->getReferencedFunction()->getName())
         << "\n";

    OS << "  ";

    // Print result.
    if (V->hasValue()) {
      ID Name = getID(V);
      Name.ResultNumber = -1;  // Don't print subresult number.
      OS << Name << " = ";
    }

    // Print the value.
    visit(V);

    // Print users, or id for valueless instructions.
    bool printedSlashes = false;

    if (!V->hasValue()) {
      OS.PadToColumn(50);
      OS << "// id: " << getID(V);
      printedSlashes = true;
    } else if (!V->use_empty()) {
      OS.PadToColumn(50);
      OS << "// user";
      if (std::next(V->use_begin()) != V->use_end())
        OS << 's';
      OS << ": ";

      // Display the user ids sorted to give a stable use order in the printer's
      // output. This makes diffing large sections of SIL significantly easier.
      llvm::SmallVector<ID, 32> UserIDs;
      for (auto *Op : V->getUses())
        UserIDs.push_back(getID(Op->getUser()));
      std::sort(UserIDs.begin(), UserIDs.end());

      interleave(UserIDs.begin(), UserIDs.end(),
                 [&] (ID id) { OS << id; },
                 [&] { OS << ", "; });
      printedSlashes = true;
    }

    // Print SIL location.
    if (Verbose) {
      if (SILInstruction *I = dyn_cast<SILInstruction>(V.getDef())) {
        SILLocation L = I->getLoc();
        SILModule &M = I->getModule();
        if (!L.isNull()) {
          if (!printedSlashes) {
            OS.PadToColumn(50);
            OS << "//";
          }
          OS << " ";

          // To minimize output, only print the line and column number for
          // everything but the first instruction.
          L.getSourceLoc().printLineAndColumn(OS, M.getASTContext().SourceMgr);

          // Print the type of location.
          switch (L.getKind()) {
          case SILLocation::NoneKind :
            assert(L.isAutoGenerated() && "This kind shouldn't be printed.");
            break;
          case SILLocation::RegularKind :
            break;
          case SILLocation::ReturnKind :
            OS << ":return"; break;
          case SILLocation::ImplicitReturnKind :
            OS << ":imp_return"; break;
          case SILLocation::InlinedKind :
            OS << ":inlined"; break;
          case SILLocation::MandatoryInlinedKind :
            OS << ":minlined"; break;
          case SILLocation::CleanupKind :
            OS << ":cleanup"; break;
          case SILLocation::ArtificialUnreachableKind :
            OS << ":art_unreach"; break;
          case SILLocation::SILFileKind :
            OS << ":sil"; break;
          }
          if (L.isAutoGenerated())
            OS << ":auto_gen";
          if (L.isInPrologue())
            OS << ":in_prologue";
        }
        if (L.isNull()) {
          if (!printedSlashes) {
            OS.PadToColumn(50);
            OS << "//";
          }
          if (L.isInTopLevel())
            OS << " top_level";
          else if (L.isAutoGenerated())
            OS << " auto_gen";
          else
            OS << " no_loc";
        }

      }
    }
    
    OS << '\n';
  }
  
  void printInContext(SILValue V) {
    subjectValue = V;
    
    auto sortByID = [&](SILValue a, SILValue b) {
      return getID(a).Number < getID(b).Number;
    };

    
    if (auto *I = dyn_cast<SILInstruction>(V)) {
      auto operands = map<SmallVector<SILValue,4>>(I->getAllOperands(),
                                                   [](Operand const &o) {
                                                     return o.get();
                                                   });
      std::sort(operands.begin(), operands.end(), sortByID);
      for (auto &operand : operands) {
        OS << "   ";
        print(operand);
      }
    }
    
    OS << "-> ";
    print(V);
    
    auto users = map<SmallVector<SILValue,4>>(V->getUses(),
                                              [](Operand *o) {
                                                return o->getUser();
                                              });
    std::sort(users.begin(), users.end(), sortByID);
    for (auto &user : users) {
      OS << "   ";
      print(user);
    }
  }

  void visitSILArgument(SILArgument *A) {
    // This should really only happen during debugging.
    OS << "argument of " << getID(A->getParent()) << " : ";
    A->getType().print(OS);
  }

  void visitSILUndef(SILUndef *A) {
    // This should really only happen during debugging.
    OS << "undef<";
    A->getType().print(OS);
    OS << ">";
  }


  void visitAllocStackInst(AllocStackInst *AVI) {
    OS << "alloc_stack " << AVI->getElementType();
    if (VarDecl *vd = AVI->getDecl())
      OS << "  // " << (vd->isVal() ? "val " : "var ") << vd->getName();
  }

  void visitAllocRefInst(AllocRefInst *ARI) {
    OS << "alloc_ref ";
    if (ARI->isObjC())
      OS << "[objc] ";
    OS << ARI->getType();
  }
  
  void visitAllocBoxInst(AllocBoxInst *ABI) {
    OS << "alloc_box " << ABI->getElementType();
    if (VarDecl *vd = ABI->getDecl())
      OS << "  // " << (vd->isVal() ? "val " : "var ") << vd->getName();
  }

  void visitAllocArrayInst(AllocArrayInst *AAI) {
    OS << "alloc_array " << AAI->getElementType()
       << ", " << getIDAndType(AAI->getNumElements());
  }
  
  void printSubstitutions(ArrayRef<Substitution> Subs) {
    if (Subs.empty())
      return;
    
    OS << '<';
    interleave(Subs,
               [&](const Substitution &s) {
                 s.Archetype->print(OS);
                 OS << " = ";
                 s.Replacement->print(OS);
               },
               [&] { OS << ", "; });
    OS << '>';
  }
  
  void visitApplyInst(ApplyInst *AI) {
    OS << "apply ";
    if (AI->isTransparent())
      OS << "[transparent] ";
    OS << getID(AI->getCallee());
    printSubstitutions(AI->getSubstitutions());
    OS << '(';
    interleave(AI->getArguments(),
               [&](const SILValue &arg) { OS << getID(arg); },
               [&] { OS << ", "; });
    OS << ") : " << AI->getCallee().getType();
  }
  
  void visitPartialApplyInst(PartialApplyInst *CI) {
    OS << "partial_apply ";
    OS << getID(CI->getCallee());
    printSubstitutions(CI->getSubstitutions());
    OS << '(';
    interleave(CI->getArguments(),
               [&](const SILValue &arg) { OS << getID(arg); },
               [&] { OS << ", "; });
    OS << ") : " << CI->getCallee().getType();
  }

  void visitFunctionRefInst(FunctionRefInst *FRI) {
    OS << "function_ref ";
    FRI->getReferencedFunction()->printName(OS);
    OS << " : " << FRI->getType();
  }
  
  void visitBuiltinFunctionRefInst(BuiltinFunctionRefInst *BFI) {
    OS << "builtin_function_ref "
       << QuotedString(BFI->getName().str()) << " : " << BFI->getType();
  }
  
  void visitGlobalAddrInst(GlobalAddrInst *GAI) {
    OS << "global_addr #" << GAI->getGlobal()->getName()
       << " : " << GAI->getType();
  }

  void visitSILGlobalAddrInst(SILGlobalAddrInst *GAI) {
    OS << "sil_global_addr ";
    GAI->getReferencedGlobal()->printName(OS);
    OS << " : " << GAI->getType();
  }

  void visitIntegerLiteralInst(IntegerLiteralInst *ILI) {
    const auto &lit = ILI->getValue();
    OS << "integer_literal " << ILI->getType() << ", " << lit;
  }
  void visitFloatLiteralInst(FloatLiteralInst *FLI) {
    OS << "float_literal " << FLI->getType() << ", 0x";
    APInt bits = FLI->getBits();
    OS << bits.toString(16, /*Signed*/ false);
    llvm::SmallString<12> decimal;
    FLI->getValue().toString(decimal);
    OS << " // " << decimal;
  }
  static StringRef getStringEncodingName(StringLiteralInst::Encoding kind) {
    switch (kind) {
    case StringLiteralInst::Encoding::UTF8: return "utf8 ";
    case StringLiteralInst::Encoding::UTF16: return "utf16 ";
    }
    llvm_unreachable("bad string literal encoding");
  }
  void visitStringLiteralInst(StringLiteralInst *SLI) {
    OS << "string_literal " << getStringEncodingName(SLI->getEncoding())
       << QuotedString(SLI->getValue());
  }
  void visitLoadInst(LoadInst *LI) {
    OS << "load " << getIDAndType(LI->getOperand());
  }
  void visitStoreInst(StoreInst *SI) {
    OS << "store " << getID(SI->getSrc()) << " to "
       << getIDAndType(SI->getDest());
  }
  void visitAssignInst(AssignInst *AI) {
    OS << "assign " << getID(AI->getSrc()) << " to "
       << getIDAndType(AI->getDest());
  }
  void visitMarkUninitializedInst(MarkUninitializedInst *MU) {
    OS << "mark_uninitialized ";
    switch (MU->getKind()) {
    case MarkUninitializedInst::Var: OS << "[var] "; break;
    case MarkUninitializedInst::RootSelf:  OS << "[rootself] "; break;
    case MarkUninitializedInst::DerivedSelf:  OS << "[derivedself] "; break;
    case MarkUninitializedInst::DerivedSelfOnly:
      OS << "[derivedselfonly] ";
      break;
    case MarkUninitializedInst::DelegatingSelf: OS << "[delegatingself] ";break;
    }
    
    OS << getIDAndType(MU->getOperand());
  }
  void visitMarkFunctionEscapeInst(MarkFunctionEscapeInst *MFE) {
    OS << "mark_function_escape ";
    interleave(MFE->getElements(),
               [&](SILValue Var) {
                 OS << getIDAndType(Var);
               },
               [&] { OS << ", "; });
  }

  void visitDebugValueInst(DebugValueInst *DVI) {
    OS << "debug_value " << getIDAndType(DVI->getOperand());

    if (VarDecl *vd = DVI->getDecl())
      OS << "  // " << (vd->isVal() ? "val " : "var ") << vd->getName();
  }

  void visitDebugValueAddrInst(DebugValueAddrInst *DVAI) {
    OS << "debug_value_addr " << getIDAndType(DVAI->getOperand());

    if (VarDecl *vd = DVAI->getDecl())
      OS << "  // " << (vd->isVal() ? "val " : "var ") << vd->getName();
}

  void visitLoadWeakInst(LoadWeakInst *LI) {
    OS << "load_weak ";
    if (LI->isTake())
      OS << "[take] ";
    OS << getIDAndType(LI->getOperand());
  }
  void visitStoreWeakInst(StoreWeakInst *SI) {
    OS << "store_weak " << getID(SI->getSrc()) << " to ";
    if (SI->isInitializationOfDest())
      OS << "[initialization] ";
    OS << getIDAndType(SI->getDest());
  }
  void visitCopyAddrInst(CopyAddrInst *CI) {
    OS << "copy_addr ";
    if (CI->isTakeOfSrc())
      OS << "[take] ";
    OS << getID(CI->getSrc()) << " to ";
    if (CI->isInitializationOfDest())
      OS << "[initialization] ";
    OS << getIDAndType(CI->getDest());
  }
  
  void printUncheckedConversionInst(ConversionInst *CI, SILValue operand,
                                    StringRef name) {
    OS << name << " " << getIDAndType(operand) << " to " << CI->getType();
  }

  StringRef getCastKindName(CheckedCastKind kind) const {
    switch (kind) {
    case CheckedCastKind::Unresolved:
    case CheckedCastKind::Coercion:
      llvm_unreachable("invalid cast kind for SIL");
    case CheckedCastKind::Downcast:
      return "downcast";
    case CheckedCastKind::SuperToArchetype:
      return "super_to_archetype";
    case CheckedCastKind::ArchetypeToArchetype:
      return "archetype_to_archetype";
    case CheckedCastKind::ArchetypeToConcrete:
      return "archetype_to_concrete";
    case CheckedCastKind::ExistentialToArchetype:
      return "existential_to_archetype";
    case CheckedCastKind::ExistentialToConcrete:
      return "existential_to_concrete";
    case CheckedCastKind::ConcreteToArchetype:
      return "concrete_to_archetype";
    case CheckedCastKind::ConcreteToUnrelatedExistential:
      return "concrete_to_unrelated_existential";
    }
  }
  
  void visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *CI) {
    OS << "unconditional_checked_cast " << getCastKindName(CI->getCastKind())
       << ' ' << getIDAndType(CI->getOperand())
       << " to " << CI->getType();
  }
  
  void visitCheckedCastBranchInst(CheckedCastBranchInst *CI) {
    OS << "checked_cast_br " << getCastKindName(CI->getCastKind()) << ' '
       << getIDAndType(CI->getOperand())
       << " to " << CI->getCastType() << ", "
       << getID(CI->getSuccessBB()) << ", " << getID(CI->getFailureBB());
  }
  
  void visitConvertFunctionInst(ConvertFunctionInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "convert_function");
  }
  void visitUpcastInst(UpcastInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "upcast");
  }
  void visitAddressToPointerInst(AddressToPointerInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "address_to_pointer");
  }
  void visitPointerToAddressInst(PointerToAddressInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "pointer_to_address");
  }
  void visitRefToObjectPointerInst(RefToObjectPointerInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "ref_to_object_pointer");
  }
  void visitObjectPointerToRefInst(ObjectPointerToRefInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "object_pointer_to_ref");
  }
  void visitRefToRawPointerInst(RefToRawPointerInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "ref_to_raw_pointer");
  }
  void visitRawPointerToRefInst(RawPointerToRefInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "raw_pointer_to_ref");
  }
  void visitRefToUnownedInst(RefToUnownedInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "ref_to_unowned");
  }
  void visitUnownedToRefInst(UnownedToRefInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "unowned_to_ref");
  }
  void visitThinToThickFunctionInst(ThinToThickFunctionInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(),"thin_to_thick_function");
  }
  void visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(),"thick_to_objc_metatype");
  }
  void visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(),"objc_to_thick_metatype");
  }
  void visitBridgeToBlockInst(BridgeToBlockInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(), "bridge_to_block");
  }
  void visitArchetypeRefToSuperInst(ArchetypeRefToSuperInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(),"archetype_ref_to_super");
  }
  void visitUpcastExistentialRefInst(UpcastExistentialRefInst *CI) {
    printUncheckedConversionInst(CI, CI->getOperand(),"upcast_existential_ref");
  }

  void visitIsNonnullInst(IsNonnullInst *I) {
    OS << "is_nonnull " << getIDAndType(I->getOperand());
  }
  
  void visitCopyValueInst(CopyValueInst *I) {
    OS << "copy_value " << getIDAndType(I->getOperand());
  }

  void visitDestroyValueInst(DestroyValueInst *I) {
    OS << "destroy_value " << getIDAndType(I->getOperand());
  }

  void visitStructInst(StructInst *SI) {
    OS << "struct " << SI->getType() << " (";
    interleave(SI->getElements(),
               [&](const SILValue &V) { OS << getIDAndType(V); },
               [&] { OS << ", "; });
    OS << ')';
  }

  void visitTupleInst(TupleInst *TI) {
    OS << "tuple ";
    
    // Check to see if the type of the tuple can be inferred accurately from the
    // elements.
    bool SimpleType = true;
    for (auto &Elt : TI->getType().castTo<TupleType>()->getFields()) {
      if (Elt.hasName() || Elt.isVararg() || Elt.hasInit()) {
        SimpleType = false;
        break;
      }
    }
    
    // If the type is simple, just print the tuple elements.
    if (SimpleType) {
      OS << '(';
      interleave(TI->getElements(),
                 [&](const SILValue &V){ OS << getIDAndType(V); },
                 [&] { OS << ", "; });
      OS << ')';
    } else {
      // Otherwise, print the type, then each value.
      OS << TI->getType() << " (";
      interleave(TI->getElements(),
                 [&](const SILValue &V){ OS << getID(V); },
                 [&] { OS << ", "; });
      OS << ')';
    }
  }
  
  void visitEnumInst(EnumInst *UI) {
    OS << "enum " << UI->getType() << ", "
       << SILDeclRef(UI->getElement(), SILDeclRef::Kind::EnumElement);
    if (UI->hasOperand()) {
      OS << ", " << getIDAndType(UI->getOperand());
    }
  }
  
  void visitInitEnumDataAddrInst(InitEnumDataAddrInst *UDAI) {
    OS << "init_enum_data_addr "
       << getIDAndType(UDAI->getOperand()) << ", "
       << SILDeclRef(UDAI->getElement(), SILDeclRef::Kind::EnumElement);
  }
  
  void visitTakeEnumDataAddrInst(TakeEnumDataAddrInst *UDAI) {
    OS << "take_enum_data_addr "
       << getIDAndType(UDAI->getOperand()) << ", "
       << SILDeclRef(UDAI->getElement(), SILDeclRef::Kind::EnumElement);
  }
  
  void visitInjectEnumAddrInst(InjectEnumAddrInst *IUAI) {
    OS << "inject_enum_addr "
       << getIDAndType(IUAI->getOperand()) << ", "
       << SILDeclRef(IUAI->getElement(), SILDeclRef::Kind::EnumElement);
  }
  
  void visitTupleExtractInst(TupleExtractInst *EI) {
    OS << "tuple_extract " << getIDAndType(EI->getOperand()) << ", "
       << EI->getFieldNo();
  }
  void visitTupleElementAddrInst(TupleElementAddrInst *EI) {
    OS << "tuple_element_addr " << getIDAndType(EI->getOperand()) << ", "
       << EI->getFieldNo();
  }
  void visitStructExtractInst(StructExtractInst *EI) {
    OS << "struct_extract " << getIDAndType(EI->getOperand()) << ", #"
       << EI->getField()->getName().get();
  }
  void visitStructElementAddrInst(StructElementAddrInst *EI) {
    OS << "struct_element_addr " << getIDAndType(EI->getOperand()) << ", #"
    << EI->getField()->getName().get();
  }
  void visitRefElementAddrInst(RefElementAddrInst *EI) {
    OS << "ref_element_addr " << getIDAndType(EI->getOperand()) << ", #"
       << EI->getField()->getName().get();
  }

  void printMethodInst(MethodInst *I, SILValue Operand, StringRef Name) {
    OS << Name << " ";
    if (I->isVolatile())
      OS << "[volatile] ";
    
    OS << getIDAndType(Operand) << ", ";
    I->getMember().print(OS);
  }
  
  void visitClassMethodInst(ClassMethodInst *AMI) {
    printMethodInst(AMI, AMI->getOperand(), "class_method");
    OS << " : " << AMI->getType();
  }
  void visitSuperMethodInst(SuperMethodInst *AMI) {
    printMethodInst(AMI, AMI->getOperand(), "super_method");
    OS << " : " << AMI->getType();
  }
  void visitPeerMethodInst(PeerMethodInst *AMI) {
    printMethodInst(AMI, AMI->getOperand(), "peer_method");
    OS << " : " << AMI->getType();
  }
  void visitArchetypeMethodInst(ArchetypeMethodInst *AMI) {
    OS << "archetype_method ";
    if (AMI->isVolatile())
      OS << "[volatile] ";
    AMI->getLookupType().print(OS);
    OS << ", ";
    AMI->getMember().print(OS);
    OS << " : " << AMI->getType(0);
  }
  void visitProtocolMethodInst(ProtocolMethodInst *AMI) {
    printMethodInst(AMI, AMI->getOperand(), "protocol_method");
    OS << " : " << AMI->getType();
  }
  void visitDynamicMethodInst(DynamicMethodInst *DMI) {
    printMethodInst(DMI, DMI->getOperand(), "dynamic_method");
    OS << " : " << DMI->getType();
  }
  void visitProjectExistentialInst(ProjectExistentialInst *PI) {
    OS << "project_existential " << getIDAndType(PI->getOperand())
       << " to " << PI->getType();
  }
  void visitProjectExistentialRefInst(ProjectExistentialRefInst *PI) {
    OS << "project_existential_ref " << getIDAndType(PI->getOperand())
       << " to " << PI->getType();
  }
  void visitOpenExistentialInst(OpenExistentialInst *OI) {
    OS << "open_existential " << getIDAndType(OI->getOperand())
       << " to " << OI->getType();
  }
  void visitOpenExistentialRefInst(OpenExistentialRefInst *OI) {
    OS << "open_existential_ref " << getIDAndType(OI->getOperand())
       << " to " << OI->getType();
  }
  void visitInitExistentialInst(InitExistentialInst *AEI) {
    OS << "init_existential " << getIDAndType(AEI->getOperand()) << ", ";
    AEI->getConcreteType().print(OS);
  }
  void visitInitExistentialRefInst(InitExistentialRefInst *AEI) {
    OS << "init_existential_ref " << getIDAndType(AEI->getOperand()) << ", ";
    AEI->getType().print(OS);
  }
  void visitUpcastExistentialInst(UpcastExistentialInst *UEI) {
    OS << "upcast_existential ";
    if (UEI->isTakeOfSrc())
      OS << "[take] ";
    OS << getIDAndType(UEI->getSrcExistential())
       << " to " << getIDAndType(UEI->getDestExistential());
  }
  void visitDeinitExistentialInst(DeinitExistentialInst *DEI) {
    OS << "deinit_existential " << getIDAndType(DEI->getOperand());
  }
  void visitClassMetatypeInst(ClassMetatypeInst *MI) {
    OS << "class_metatype " << MI->getType() << ", "
       << getIDAndType(MI->getOperand());
  }
  void visitArchetypeMetatypeInst(ArchetypeMetatypeInst *MI) {
    OS << "archetype_metatype " << MI->getType() << ", "
       << getIDAndType(MI->getOperand());
  }
  void visitProtocolMetatypeInst(ProtocolMetatypeInst *MI) {
    OS << "protocol_metatype " << MI->getType() << ", "
       << getIDAndType(MI->getOperand());
  }
  void visitMetatypeInst(MetatypeInst *MI) {
    OS << "metatype " << MI->getType();
  }
  
  void visitStrongRetainInst(StrongRetainInst *RI) {
    OS << "strong_retain " << getIDAndType(RI->getOperand());
  }
  void visitStrongRetainAutoreleasedInst(StrongRetainAutoreleasedInst *RI) {
    OS << "strong_retain_autoreleased " << getIDAndType(RI->getOperand());
  }
  void visitStrongReleaseInst(StrongReleaseInst *RI) {
    OS << "strong_release " << getIDAndType(RI->getOperand());
  }
  void visitStrongRetainUnownedInst(StrongRetainUnownedInst *RI) {
    OS << "strong_retain_unowned " << getIDAndType(RI->getOperand());
  }
  void visitUnownedRetainInst(UnownedRetainInst *RI) {
    OS << "unowned_retain " << getIDAndType(RI->getOperand());
  }
  void visitUnownedReleaseInst(UnownedReleaseInst *RI) {
    OS << "unowned_release " << getIDAndType(RI->getOperand());
  }
  void visitDeallocStackInst(DeallocStackInst *DI) {
    OS << "dealloc_stack " << getIDAndType(DI->getOperand());
  }
  void visitDeallocRefInst(DeallocRefInst *DI) {
    OS << "dealloc_ref " << getIDAndType(DI->getOperand());
  }
  void visitDeallocBoxInst(DeallocBoxInst *DI) {
    OS << "dealloc_box " << DI->getElementType() << ", "
       << getIDAndType(DI->getOperand());
  }
  void visitDestroyAddrInst(DestroyAddrInst *DI) {
    OS << "destroy_addr " << getIDAndType(DI->getOperand());
  }
  
  void visitCondFailInst(CondFailInst *FI) {
    OS << "cond_fail " << getIDAndType(FI->getOperand());
  }
  
  void visitIndexAddrInst(IndexAddrInst *IAI) {
    OS << "index_addr " << getIDAndType(IAI->getBase()) << ", "
       << getIDAndType(IAI->getIndex());
  }

  void visitIndexRawPointerInst(IndexRawPointerInst *IAI) {
    OS << "index_raw_pointer " << getIDAndType(IAI->getBase()) << ", "
    << getIDAndType(IAI->getIndex());
  }
  
  void visitUnreachableInst(UnreachableInst *UI) {
    OS << "unreachable";
  }

  void visitReturnInst(ReturnInst *RI) {
    OS << "return " << getIDAndType(RI->getOperand());
  }
  
  void visitAutoreleaseReturnInst(AutoreleaseReturnInst *RI) {
    OS << "autorelease_return " << getIDAndType(RI->getOperand());
  }
  
  void visitSwitchIntInst(SwitchIntInst *SII) {
    OS << "switch_int " << getIDAndType(SII->getOperand());
    for (unsigned i = 0, e = SII->getNumCases(); i < e; ++i) {
      APInt value;
      SILBasicBlock *dest;
      std::tie(value, dest) = SII->getCase(i);
      OS << ", case " << value << ": " << getID(dest);
    }
    if (SII->hasDefault())
      OS << ", default " << getID(SII->getDefaultBB());
  }
  
  void printSwitchEnumInst(SwitchEnumInstBase *SOI) {
    OS << getIDAndType(SOI->getOperand());
    for (unsigned i = 0, e = SOI->getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILBasicBlock *dest;
      std::tie(elt, dest) = SOI->getCase(i);
      OS << ", case " << SILDeclRef(elt, SILDeclRef::Kind::EnumElement)
      << ": " << getID(dest);
    }
    if (SOI->hasDefault())
      OS << ", default " << getID(SOI->getDefaultBB());
  }
  
  void visitSwitchEnumInst(SwitchEnumInst *SOI) {
    OS << "switch_enum ";
    printSwitchEnumInst(SOI);
  }
  void visitSwitchEnumAddrInst(SwitchEnumAddrInst *SOI){
    OS << "switch_enum_addr ";
    printSwitchEnumInst(SOI);
  }
  void visitDynamicMethodBranchInst(DynamicMethodBranchInst *DMBI) {
    OS << "dynamic_method_br " << getIDAndType(DMBI->getOperand()) << ", ";
    DMBI->getMember().print(OS);
    OS << ", " << getID(DMBI->getHasMethodBB()) << ", "
       << getID(DMBI->getNoMethodBB());
  }

  void printBranchArgs(OperandValueArrayRef args) {
    if (args.empty()) return;

    OS << '(';
    interleave(args,
               [&](SILValue v) { OS << getIDAndType(v); },
               [&] { OS << ", "; });
    OS << ')';
  }
  
  void visitBranchInst(BranchInst *UBI) {
    OS << "br " << getID(UBI->getDestBB());
    printBranchArgs(UBI->getArgs());
  }

  void visitCondBranchInst(CondBranchInst *CBI) {
    OS << "cond_br " << getID(CBI->getCondition()) << ", "
       << getID(CBI->getTrueBB());
    printBranchArgs(CBI->getTrueArgs());
    OS << ", " << getID(CBI->getFalseBB());
    printBranchArgs(CBI->getFalseArgs());
  }
};
} // end anonymous namespace

ID SILPrinter::getID(const SILBasicBlock *Block) {
  // Lazily initialize the Blocks-to-IDs mapping.
  if (BlocksToIDMap.empty()) {
    unsigned idx = 0;
    for (const SILBasicBlock &B : *Block->getParent())
      BlocksToIDMap[&B] = idx++;
  }

  ID R = { ID::SILBasicBlock, BlocksToIDMap[Block], -1 };
  return R;
}

ID SILPrinter::getID(SILValue V) {
  if (isa<SILUndef>(V))
    return { ID::SILUndef, 0, 0 };

  // Lazily initialize the instruction -> ID mapping.
  if (ValueToIDMap.empty()) {
    const SILBasicBlock *ParentBB;
    if (const SILInstruction *I = dyn_cast<SILInstruction>(V))
      ParentBB = I->getParent();
    else
      ParentBB = cast<SILArgument>(V)->getParent();

    // Keep the values in ValueToIDMap with a +1 bias so that lookups will get
    // 0 for invalid numbers.
    unsigned idx = 0;
    for (auto &BB : *ParentBB->getParent()) {
      for (auto I = BB.bbarg_begin(), E = BB.bbarg_end(); I != E; ++I)
        ValueToIDMap[*I] = ++idx;

      for (auto &I : BB) {
        ValueToIDMap[&I] = ++idx;
      }
    }
  }

  int ResultNumber = -1;
  if (V.getDef()->getTypes().size() > 1)
    ResultNumber = V.getResultNumber();

  ID R = { ID::SSAValue, ValueToIDMap[V.getDef()]-1, ResultNumber };
  return R;
}

void SILBasicBlock::printAsOperand(raw_ostream &OS, bool PrintType) {
  OS << SILPrinter(OS).getID(this);
}

//===----------------------------------------------------------------------===//
// Printing for SILInstruction, SILBasicBlock, SILFunction, and SILModule
//===----------------------------------------------------------------------===//

void SILValue::dump() const {
  print(llvm::errs());
}

void SILValue::print(raw_ostream &OS) const {
  SILPrinter(OS).print(*this);
}

void ValueBase::dump() const {
  print(llvm::errs());
}

void ValueBase::print(raw_ostream &OS) const {
  SILPrinter(OS).print(this);
}

/// Pretty-print the SILBasicBlock to errs.
void SILBasicBlock::dump() const {
  print(llvm::errs());
}

/// Pretty-print the SILBasicBlock to the designated stream.
void SILBasicBlock::print(raw_ostream &OS) const {
  SILPrinter(OS).print(this);
}

/// Pretty-print the SILFunction to errs.
void SILFunction::dump(bool Verbose) const {
  print(llvm::errs(), Verbose);
}

// This is out of line so the debugger can find it.
void SILFunction::dump() const {
  dump(false);
}

static StringRef getLinkageString(SILLinkage linkage) {
  switch (linkage) {
  case SILLinkage::Public: return "public ";
  case SILLinkage::Hidden: return "hidden ";
  case SILLinkage::Shared: return "shared ";
  case SILLinkage::Private: return "private ";
  case SILLinkage::PublicExternal: return "public_external ";
  case SILLinkage::HiddenExternal: return "hidden_external ";
  }
  llvm_unreachable("bad linkage");
}

static void printLinkage(llvm::raw_ostream &OS, SILLinkage linkage,
                         bool isDefinition) {
  if ((isDefinition && linkage == SILLinkage::DefaultForDefinition) ||
      (!isDefinition && linkage == SILLinkage::DefaultForDeclaration))
    return;

  OS << getLinkageString(linkage);
}

/// Pretty-print the SILFunction to the designated stream.
void SILFunction::print(llvm::raw_ostream &OS, bool Verbose) const {
  OS << "// " << demangleSymbolAsString(getName()) << '\n';
  OS << "sil ";
  printLinkage(OS, getLinkage(), isDefinition());

  if (isTransparent())
    OS << "[transparent] ";
  
  printName(OS);
  OS << " : $";
  
  // Print the type by substituting our context parameters for the dependent
  // parameters.
  {
    PrintOptions withContextGenericParams;
    withContextGenericParams.ContextGenericParams = ContextGenericParams;
    LoweredType->print(OS, withContextGenericParams);
  }
  
  if (!isExternalDeclaration()) {
    OS << " {\n";
    SILPrinter(OS, Verbose).print(this);
    OS << "}";
  }
  
  OS << "\n\n";
}
      
/// Pretty-print the SILFunction's name using SIL syntax,
/// '@function_mangled_name'.
void SILFunction::printName(raw_ostream &OS) const {
  OS << "@" << Name;  
}

/// Pretty-print a global variable to the designated stream.
void SILGlobalVariable::print(llvm::raw_ostream &OS, bool Verbose) const {
  OS << "// " << demangleSymbolAsString(getName()) << '\n';
  
  OS << "sil_global ";
  printLinkage(OS, getLinkage(), isDefinition());

  printName(OS);
  OS << " : " << LoweredType;
  
  OS << "\n\n";
}

void SILGlobalVariable::dump(bool Verbose) const {
  print(llvm::errs(), Verbose);
}

void SILGlobalVariable::printName(raw_ostream &OS) const {
  OS << "@" << Name;
}
      
/// Pretty-print the SILModule to errs.
void SILModule::dump() const {
  print(llvm::errs());
}

/// Pretty-print the SILModule to the designated stream.
void SILModule::print(llvm::raw_ostream &OS, bool Verbose,
                      Module *M) const {
  OS << "sil_stage ";
  switch (Stage) {
  case SILStage::Raw:
    OS << "raw";
    break;
  case SILStage::Canonical:
    OS << "canonical";
    break;
  }
  
  OS << "\n\nimport Builtin\nimport " << STDLIB_NAME << "\n\n";

  // Print the declarations and types from the origin module.
  // FIXME: What about multi-file modules?
  if (M && M->getFiles().size() == 1) {
    // Compute the list of emitted functions, whose AST Decls we do not need to
    // print.
    llvm::DenseSet<const Decl*> emittedFunctions;
    for (const SILFunction &f : *this)
      if (f.hasLocation())
        emittedFunctions.insert(f.getLocation().getAsASTNode<Decl>());

    PrintOptions Options;
    Options.FunctionDefinitions = false;
    Options.TypeDefinitions = true;
    Options.VarInitializers = true;
    Options.SkipImplicit = true;

    // FIXME: Use some kind of visitor interface here.
    SmallVector<Decl *, 32> topLevelDecls;
    M->getFiles().front()->getTopLevelDecls(topLevelDecls);
    for (const Decl *D : topLevelDecls) {
      if ((isa<ValueDecl>(D) || isa<OperatorDecl>(D)) &&
          !emittedFunctions.count(D) &&
          !D->isImplicit()) {
        D->print(OS, Options);
        OS << "\n\n";
      }
    }
  }

  for (const SILGlobalVariable &g : getSILGlobals())
    g.print(OS, Verbose);
    
  for (const SILFunction &f : *this)
    f.print(OS, Verbose);

  for (const SILVTable &vt : getVTables())
    vt.print(OS, Verbose);
  
  for (const SILWitnessTable &wt : getWitnessTables())
    wt.print(OS, Verbose);
  
  OS << "\n\n";
}

void ValueBase::dumpInContext() const {
  printInContext(llvm::errs());
}
void ValueBase::printInContext(llvm::raw_ostream &OS) const {
  SILPrinter(OS).printInContext(this);
}

void SILVTable::print(llvm::raw_ostream &OS, bool Verbose) const {
  OS << "sil_vtable " << getClass()->getName() << " {\n";
  for (auto &entry : getEntries()) {
    OS << "  ";
    entry.first.print(OS);
    OS << ": " << entry.second->getName()
       << "\t// " << demangleSymbolAsString(entry.second->getName()) << "\n";
  }
  OS << "}\n\n";
}

void SILVTable::dump() const {
  print(llvm::errs());
}

void SILWitnessTable::print(llvm::raw_ostream &OS, bool Verbose) const {
  OS << "sil_witness_table ";
  getConformance()->printName(OS);
  OS << " {\n";
  
  for (auto &witness : getEntries()) {
    OS << "  ";
    switch (witness.getKind()) {
    case Invalid:
      llvm_unreachable("invalid witness?!");
    case Method: {
      // method #declref: @function
      auto &methodWitness = witness.getMethodWitness();
      OS << "method ";
      methodWitness.Requirement.print(OS);
      OS << ": ";
      methodWitness.Witness->printName(OS);
      OS << "\t// "
         << demangleSymbolAsString(methodWitness.Witness->getName());
      break;
    }
    case AssociatedType: {
      // associated_type AssociatedTypeName: ConformingType
      auto &assocWitness = witness.getAssociatedTypeWitness();
      OS << "associated_type ";
      OS << assocWitness.Requirement->getName() << ": ";
      assocWitness.Witness->print(OS);
      break;
    }
    case AssociatedTypeProtocol: {
      // associated_type_protocol (AssociatedTypeName: Protocol): <conformance>
      auto &assocProtoWitness = witness.getAssociatedTypeProtocolWitness();
      OS << "associated_type_protocol ("
         << assocProtoWitness.Requirement->getName() << ": "
         << assocProtoWitness.Protocol->getName() << "): ";
      if (assocProtoWitness.Witness)
        assocProtoWitness.Witness->printName(OS);
      else
        OS << "dependent";
      break;
    }
    case BaseProtocol: {
      // base_protocol Protocol: <conformance>
      auto &baseProtoWitness = witness.getBaseProtocolWitness();
      OS << "base_protocol "
         << baseProtoWitness.Requirement->getName() << ": ";
      baseProtoWitness.Witness->printName(OS);
    }
    }
    OS << '\n';
  }
  
  OS << "}\n\n";
}

void SILWitnessTable::dump() const {
  print(llvm::errs());
}
