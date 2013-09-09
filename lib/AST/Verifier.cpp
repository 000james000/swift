//===--- Verifier.cpp - AST Invariant Verification ------------------------===//
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
//  This file implements a verifier of AST invariants.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "swift/AST/AST.h"
#include "swift/AST/ASTWalker.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
using namespace swift;

namespace {
  enum ShouldHalt { Continue, Halt };

  class Verifier : public ASTWalker {
    TranslationUnit *TU;
    ASTContext &Ctx;
    llvm::raw_ostream &Out;
    bool HadError;

    /// \brief The stack of functions we're visiting.
    SmallVector<FuncExprLike, 4> Functions;

  public:
    Verifier(TranslationUnit *TU) : TU(TU), Ctx(TU->Ctx), Out(llvm::errs()),
                                    HadError(TU->Ctx.hadError()) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      switch (E->getKind()) {
#define DISPATCH(ID) return dispatchVisitPreExpr(static_cast<ID##Expr*>(E))
#define EXPR(ID, PARENT) \
      case ExprKind::ID: \
        DISPATCH(ID);
#define UNCHECKED_EXPR(ID, PARENT) \
      case ExprKind::ID: \
        assert((TU->ASTStage < TranslationUnit::TypeChecked || HadError) && \
               #ID "in wrong phase");\
        DISPATCH(ID);
#include "swift/AST/ExprNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    Expr *walkToExprPost(Expr *E) {
      switch (E->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Expr*>(E))
#define EXPR(ID, PARENT) \
      case ExprKind::ID: \
        DISPATCH(ID);
#define UNCHECKED_EXPR(ID, PARENT) \
      case ExprKind::ID: \
        assert((TU->ASTStage < TranslationUnit::TypeChecked || HadError) && \
               #ID "in wrong phase");\
        DISPATCH(ID);
#include "swift/AST/ExprNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
      switch (S->getKind()) {
#define DISPATCH(ID) return dispatchVisitPreStmt(static_cast<ID##Stmt*>(S))
#define STMT(ID, PARENT) \
      case StmtKind::ID: \
        DISPATCH(ID);
#include "swift/AST/StmtNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    Stmt *walkToStmtPost(Stmt *S) {
      switch (S->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Stmt*>(S))
#define STMT(ID, PARENT) \
      case StmtKind::ID: \
        DISPATCH(ID);
#include "swift/AST/StmtNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    bool walkToDeclPre(Decl *D) {
      switch (D->getKind()) {
#define DISPATCH(ID) return dispatchVisitPre(static_cast<ID##Decl*>(D))
#define DECL(ID, PARENT) \
      case DeclKind::ID: \
        DISPATCH(ID);
#include "swift/AST/DeclNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    bool walkToDeclPost(Decl *D) {
      switch (D->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Decl*>(D))
#define DECL(ID, PARENT) \
      case DeclKind::ID: \
        DISPATCH(ID);
#include "swift/AST/DeclNodes.def"
#undef DISPATCH
      }

      llvm_unreachable("Unhandled declaratiom kind");
    }

  private:
    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> bool dispatchVisitPre(T node) {
      return shouldVerify(node);
    }

    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> std::pair<bool, Expr *> dispatchVisitPreExpr(T node) {
      return { shouldVerify(node), node };
    }

    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> std::pair<bool, Stmt *> dispatchVisitPreStmt(T node) {
      return { shouldVerify(node), node };
    }

    /// Helper template for dispatching post-visitation.
    template <class T> T dispatchVisitPost(T node) {
      // We always verify source ranges.
      checkSourceRanges(node);

      // Check that nodes marked invalid have the correct type.
      checkErrors(node);

      // Always verify the node as a parsed node.
      verifyParsed(node);

      // If we've bound names already, verify as a bound node.
      if (TU->ASTStage >= TranslationUnit::NameBound)
        verifyBound(node);

      // If we've checked types already, do some extra verification.
      if (TU->ASTStage >= TranslationUnit::TypeChecked) {
        verifyChecked(node);
        if (!HadError)
          checkBoundGenericTypes(node);
      }

      // Clean up anything that we've placed into a stack to check.
      cleanup(node);

      // Always continue.
      return node;
    }

    // Default cases for whether we should verify within the given subtree.
    bool shouldVerify(Expr *E) { return true; }
    bool shouldVerify(Stmt *S) { return true; }
    bool shouldVerify(Decl *S) { return true; }

    // Default cases for cleaning up as we exit a node.
    void cleanup(Expr *E) { }
    void cleanup(Stmt *S) { }
    void cleanup(Decl *D) { }
    
    // Base cases for the various stages of verification.
    void verifyParsed(Expr *E) {}
    void verifyParsed(Stmt *S) {}
    void verifyParsed(Decl *D) {}
    void verifyBound(Expr *E) {}
    void verifyBound(Stmt *S) {}
    void verifyBound(Decl *D) {}
    void verifyChecked(Expr *E) {}
    void verifyChecked(Stmt *S) {}
    void verifyChecked(Decl *D) {}

    // Specialized verifiers.

    bool shouldVerify(FuncExpr *func) {
      Functions.push_back(func);
      return shouldVerify(cast<Expr>(func));
    }

    bool shouldVerify(PipeClosureExpr *closure) {
      Functions.push_back(closure);
      return shouldVerify(cast<Expr>(closure));
    }

    bool shouldVerify(ConstructorDecl *cd) {
      Functions.push_back(cd);
      return shouldVerify(cast<Decl>(cd));
    }
    
    bool shouldVerify(DestructorDecl *dd) {
      Functions.push_back(dd);
      return shouldVerify(cast<Decl>(dd));
    }
    
    void cleanup(FuncExpr *func) {
      assert(Functions.back().get<FuncExpr*>() == func);
      Functions.pop_back();
    }
    void cleanup(PipeClosureExpr *closure) {
      assert(Functions.back().get<PipeClosureExpr*>() == closure);
      Functions.pop_back();
    }
    void cleanup(ConstructorDecl *cd) {
      assert(Functions.back().get<ConstructorDecl*>() == cd);
      Functions.pop_back();
    }
    void cleanup(DestructorDecl *cd) {
      assert(Functions.back().get<DestructorDecl*>() == cd);
      Functions.pop_back();
    }

    void verifyChecked(ReturnStmt *S) {
      if (HadError)
        return;
      auto func = Functions.back();
      Type resultType;
      if (FuncExpr *fe = func.dyn_cast<FuncExpr*>()) {
        resultType = fe->getResultType(Ctx);
      } else if (auto closure = func.dyn_cast<PipeClosureExpr *>()) {
        resultType = closure->getResultType();
      } else {
        resultType = TupleType::getEmpty(Ctx);
      }
      
      if (S->hasResult()) {
        auto result = S->getResult();
        auto returnType = result->getType();
        // Make sure that the return has the same type as the function.
        checkSameType(resultType, returnType, "return type");
      } else {
        // Make sure that the function has a Void result type.
        checkSameType(resultType, TupleType::getEmpty(Ctx), "return type");
      }
    }

    void verifyChecked(IfStmt *S) {
      if (HadError)
        return;
      checkSameType(S->getCond()->getType(), BuiltinIntegerType::get(1, Ctx),
                    "if condition type");
    }

    void verifyChecked(WhileStmt *S) {
      if (HadError)
        return;
      checkSameType(S->getCond()->getType(), BuiltinIntegerType::get(1, Ctx),
                    "while condition type");
    }

    Type checkAssignDest(Expr *Dest) {
      if (TupleExpr *TE = dyn_cast<TupleExpr>(Dest)) {
        SmallVector<TupleTypeElt, 4> lhsTupleTypes;
        for (unsigned i = 0; i != TE->getNumElements(); ++i) {
          Type SubType = checkAssignDest(TE->getElement(i));
          lhsTupleTypes.push_back(TupleTypeElt(SubType, TE->getElementName(i)));
        }
        return TupleType::get(lhsTupleTypes, Ctx);
      }
      return checkLValue(Dest->getType(), "LHS of assignment");
    }

    void verifyChecked(AssignExpr *S) {
      if (HadError)
        return;
      Type lhsTy = checkAssignDest(S->getDest());
      checkSameType(lhsTy, S->getSrc()->getType(), "assignment operands");
    }

    void verifyChecked(AddressOfExpr *E) {
      if (HadError)
        return;
      LValueType::Qual resultQuals;
      Type resultObj = checkLValue(E->getType(), resultQuals,
                                   "result of AddressOfExpr");

      LValueType::Qual srcQuals;
      Type srcObj = checkLValue(E->getSubExpr()->getType(), srcQuals,
                                "source of AddressOfExpr");

      checkSameType(resultObj, srcObj, "object types for AddressOfExpr");

      if ((resultQuals | LValueType::Qual::Implicit) !=
          (srcQuals | LValueType::Qual::Implicit)) {
        Out << "mismatched qualifiers";
        E->print(Out);
        Out << "\n";
        abort();
      }
    }

    void verifyChecked(RequalifyExpr *E) {
      if (HadError)
        return;
      LValueType::Qual dstQuals, srcQuals;
      Type dstObj = checkLValue(E->getType(), dstQuals,
                                "result of RequalifyExpr");
      Type srcObj = checkLValue(E->getSubExpr()->getType(), srcQuals,
                                "input to RequalifyExpr");
      checkSameType(dstObj, srcObj,
                    "objects of result and operand of RequalifyExpr");

      // As a hack, requalifications in the object operand are
      // permitted to remove the 'non-settable' qualifier (so that you
      // can call methods on immutable values) and 'implicit'
      // qualifier (so that you don't have to explicitly qualify take
      // the address of the object).
      if (E->isForObjectOperand()) {
        dstQuals |= LValueType::Qual::NonSettable;
        dstQuals |= LValueType::Qual::Implicit;
      }
      
      // FIXME: Should either properly check implicit here, or model the dropping
      // of 'implicit' differently.
      if (!(srcQuals < dstQuals) && !(srcQuals == dstQuals)) {
        Out << "bad qualifier sets for RequalifyExpr:\n";
        E->print(Out);
        Out << "\n";
        abort();
      }
    }

    void verifyChecked(MetatypeConversionExpr *E) {
      if (HadError)
        return;
      auto destTy = checkMetatypeType(E->getType(),
                                      "result of MetatypeConversionExpr");
      auto srcTy = checkMetatypeType(E->getSubExpr()->getType(),
                                     "source of MetatypeConversionExpr");

      if (destTy->isEqual(srcTy)) {
        Out << "trivial MetatypeConversionExpr:\n";
        E->print(Out);
        Out << "\n";
        abort();
      }

      checkTrivialSubtype(srcTy, destTy, "MetatypeConversionExpr");
    }

    void verifyChecked(MaterializeExpr *E) {
      if (HadError)
        return;
      Type obj = checkLValue(E->getType(), "result of MaterializeExpr");
      checkSameType(obj, E->getSubExpr()->getType(),
                    "result and operand of MaterializeExpr");
    }

    void verifyChecked(TupleElementExpr *E) {
      if (HadError)
        return;
      Type resultType = E->getType();
      Type baseType = E->getBase()->getType();
      checkSameLValueness(baseType, resultType,
                          "base and result of TupleElementExpr");

      TupleType *tupleType = baseType->getAs<TupleType>();
      if (!tupleType) {
        Out << "base of TupleElementExpr does not have tuple type: ";
        E->getBase()->getType().print(Out);
        Out << "\n";
        abort();
      }

      if (E->getFieldNumber() >= tupleType->getFields().size()) {
        Out << "field index " << E->getFieldNumber()
            << " for TupleElementExpr is out of range [0,"
            << tupleType->getFields().size() << ")\n";
        abort();
      }

      checkSameType(resultType, tupleType->getElementType(E->getFieldNumber()),
                    "TupleElementExpr and the corresponding tuple element");
    }

    void verifyChecked(ApplyExpr *E) {
      if (HadError)
        return;
      FunctionType *FT = E->getFn()->getType()->getAs<FunctionType>();
      if (!FT) {
        Out << "callee of apply expression does not have function type:";
        E->getFn()->getType().print(Out);
        Out << "\n";
        abort();
      }
      CanType InputExprTy = E->getArg()->getType()->getCanonicalType();
      CanType ResultExprTy = E->getType()->getCanonicalType();
      if (ResultExprTy != FT->getResult()->getCanonicalType()) {
        Out << "result of ApplyExpr does not match result type of callee:";
        E->getType().print(Out);
        Out << " vs. ";
        FT->getResult()->print(Out);
        Out << "\n";
        abort();
      }
      if (InputExprTy != FT->getInput()->getCanonicalType()) {
        TupleType *TT = FT->getInput()->getAs<TupleType>();
        if (isa<SelfApplyExpr>(E)) {
          LValueType::Qual InputExprQuals;
          Type InputExprObjectTy;
          if (InputExprTy->hasReferenceSemantics() ||
              InputExprTy->is<MetaTypeType>())
            InputExprObjectTy = InputExprTy;
          else
            InputExprObjectTy = checkLValue(InputExprTy, InputExprQuals,
                                            "object argument");
          LValueType::Qual FunctionInputQuals;
          Type FunctionInputObjectTy = checkLValue(FT->getInput(),
                                                   FunctionInputQuals,
                                                   "'self' parameter");
          
          checkSameOrSubType(InputExprObjectTy, FunctionInputObjectTy,
                             "object argument and 'self' parameter");
        } else if (!TT || TT->getFields().size() != 1 ||
                   TT->getFields()[0].getType()->getCanonicalType()
                     != InputExprTy) {
          Out << "Argument type does not match parameter type in ApplyExpr:"
                 "\nArgument type: ";
          E->getArg()->getType().print(Out);
          Out << "\nParameter type: ";
          FT->getInput()->print(Out);
          Out << "\n";
          E->dump();
          abort();
        }
      }
    }

    void verifyChecked(MemberRefExpr *E) {
      if (HadError)
        return;
      if (!E->getBase()->getType()->is<LValueType>() &&
          !E->getBase()->getType()->hasReferenceSemantics()) {
        Out << "Member reference base type is not an lvalue:\n";
        E->dump();
        abort();
      }
      
      if (!E->getType()->is<LValueType>()) {
        Out << "Member reference type is not an lvalue\n";
        E->dump();
        abort();
      }
      
      if (!E->getMember()) {
        Out << "Member reference is missing declaration\n";
        E->dump();
        abort();
      }

      LValueType *ResultLV = E->getType()->getAs<LValueType>();
      if (!ResultLV) {
        Out << "Member reference has non-lvalue type\n";
        E->dump();
        abort();
      }

      // FIXME: Check container/member types through substitutions.
    }

    void verifyChecked(DynamicMemberRefExpr *E) {
      // The base type must be DynamicLookup.
      auto baseTy = E->getBase()->getType();
      auto baseProtoTy = baseTy->getAs<ProtocolType>();
      if (!baseProtoTy ||
          !baseProtoTy->getDecl()->isSpecificProtocol(
             KnownProtocolKind::DynamicLookup)) {
        Out << "Dynamic member reference has non-DynamicLookup base\n";
        E->dump();
        abort();
      }

      // The member must be [objc].
      if (!E->getMember().getDecl()->isObjC()) {
        Out << "Dynamic member reference to non-[objc] member\n";
        E->dump();
        abort();
      }
    }

    void verifyChecked(SubscriptExpr *E) {
      if (HadError)
        return;
      if (!E->getBase()->getType()->is<LValueType>() &&
          !E->getBase()->getType()->hasReferenceSemantics()) {
        Out << "Subscript base type is not an lvalue";
        abort();
      }

      if (!E->getType()->is<LValueType>()) {
        Out << "Subscript type is not an lvalue";
        abort();
      }
      
      if (!E->getDecl()) {
        Out << "Subscript expression is missing subscript declaration";
        abort();
      }

      // FIXME: Check base/member types through substitutions.
    }

    void verifyChecked(UnconditionalCheckedCastExpr *E) {
      if (HadError)
        return;
      Type Ty = E->getCastTypeLoc().getType();
      if (!Ty->isEqual(E->getType())) {
        Out << "UnconditionalCheckedCast types don't match\n";
        abort();
      }
      if (!E->isResolved()) {
        Out << "UnconditionalCheckedCast kind not resolved\n";
        abort();
      }
    }
    
    void verifyChecked(CheckedCastExpr *E) {
      if (HadError)
        return;
      if (!E->isResolved()) {
        Out << "CheckedCast kind not resolved\n";
        abort();
      }
    }

    void verifyChecked(SpecializeExpr *E) {
      if (HadError)
        return;
      if (!E->getType()->is<FunctionType>()) {
        Out << "SpecializeExpr must have FunctionType result\n";
        abort();
      }

      Type SubType = E->getSubExpr()->getType()->getRValueType();
      if (!SubType->is<PolymorphicFunctionType>()) {
        Out << "Non-polymorphic expression specialized\n";
        abort();
      }

      // Verify that the protocol conformances line up with the archetypes.
      // FIXME: It's not clear how many levels we're substituting here.
      for (auto &Subst : E->getSubstitutions()) {
        auto Archetype = Subst.Archetype;
        if (Subst.Conformance.size() != Archetype->getConformsTo().size()) {
          Out << "Wrong number of protocol conformances for archetype\n";
          abort();
        }

        for (unsigned I = 0, N = Subst.Conformance.size(); I != N; ++I) {
          const auto &Conformance = Subst.Conformance[I];
          if (!Conformance || Conformance->getWitnesses().empty())
            continue;

          if (Conformance->getWitnesses().begin()->first->getDeclContext()
                != Archetype->getConformsTo()[I]) {
            Out << "Protocol conformance doesn't match up with archetype "
                   "requirement\n";
            abort();
          }
        }
      }
    }

    void verifyChecked(TupleShuffleExpr *E) {
      if (HadError)
        return;
      TupleType *TT = E->getType()->getAs<TupleType>();
      TupleType *SubTT = E->getSubExpr()->getType()->getAs<TupleType>();
      if (!TT || !SubTT) {
        Out << "Unexpected types in TupleShuffleExpr\n";
        abort();
      }
      unsigned varargsStartIndex = 0;
      Type varargsType;
      unsigned callerDefaultArgIndex = 0;
      for (unsigned i = 0, e = E->getElementMapping().size(); i != e; ++i) {
        int subElem = E->getElementMapping()[i];
        if (subElem == TupleShuffleExpr::DefaultInitialize)
          continue;
        if (subElem == TupleShuffleExpr::FirstVariadic) {
          varargsStartIndex = i + 1;
          varargsType = TT->getFields()[i].getVarargBaseTy();
          break;
        }
        if (subElem == TupleShuffleExpr::CallerDefaultInitialize) {
          auto init = E->getCallerDefaultArgs()[callerDefaultArgIndex++];
          if (!TT->getElementType(i)->isEqual(init->getType())) {
            Out << "Type mismatch in TupleShuffleExpr\n";
            abort();
          }
          continue;
        }
        if (!TT->getElementType(i)->isEqual(SubTT->getElementType(subElem))) {
          Out << "Type mismatch in TupleShuffleExpr\n";
          abort();
        }
      }
      if (varargsStartIndex) {
        unsigned i = varargsStartIndex, e = E->getElementMapping().size();
        for (; i != e; ++i) {
          unsigned subElem = E->getElementMapping()[i];
          if (!SubTT->getElementType(subElem)->isEqual(varargsType)) {
            Out << "Vararg type mismatch in TupleShuffleExpr\n";
            abort();
          }
        }
      }
    }

    void verifyChecked(MetatypeExpr *E) {
      if (HadError)
        return;
      auto metatype = E->getType()->getAs<MetaTypeType>();
      if (!metatype) {
        Out << "MetatypeExpr must have metatype type\n";
        abort();
      }

      if (E->getBase()) {
        checkSameType(E->getBase()->getType(), metatype->getInstanceType(),
                      "base type of .metatype expression");
      }
    }

    void verifyParsed(NewArrayExpr *E) {
      if (E->getBounds().empty()) {
        Out << "NewArrayExpr has an empty bounds list\n";
        abort();
      }
      if (E->getBounds()[0].Value == nullptr) {
        Out << "First bound of NewArrayExpr is missing\n";
        abort();
      }
    }

    void verifyChecked(NewArrayExpr *E) {
      if (HadError)
        return;
      if (!E->hasElementType()) {
        Out << "NewArrayExpr is missing its element type";
        abort();
      }

      if (!E->hasInjectionFunction()) {
        Out << "NewArrayExpr is missing an injection function";
        abort();
      }
    }

    void verifyChecked(IfExpr *expr) {
      if (HadError)
        return;
      auto condTy
        = expr->getCondExpr()->getType()->getAs<BuiltinIntegerType>();
      if (!condTy || condTy->getBitWidth() != 1) {
        Out << "IfExpr condition is not an i1\n";
        abort();
      }

      checkSameType(expr->getThenExpr()->getType(),
                    expr->getElseExpr()->getType(),
                    "then and else branches of an if-expr");
    }
    
    void verifyChecked(SuperRefExpr *expr) {
      if (HadError)
        return;
      if (!expr->getType()->is<LValueType>()) {
        Out << "Type of SuperRefExpr should be an LValueType";
        abort();
      }
    }

    void verifyChecked(VarDecl *var) {
      if (HadError)
        return;
      // The fact that this is *directly* be a reference storage type
      // cuts the code down quite a bit in getTypeOfReference.
      if (var->getAttrs().hasOwnership() !=
          isa<ReferenceStorageType>(var->getType().getPointer())) {
        if (var->getAttrs().hasOwnership()) {
          Out << "VarDecl has an ownership attribute, but its type"
                 " is not a ReferenceStorageType: ";
        } else {
          Out << "VarDecl has no ownership attribute, but its type"
                 " is a ReferenceStorageType: ";
        }
        var->getType().print(Out);
        abort();
      }
    }

    void verifyParsed(UnionElementDecl *UED) {
      if (!isa<UnionDecl>(UED->getDeclContext())) {
        Out << "UnionElementDecl has wrong DeclContext";
        abort();
      }
    }

    /// Look through a possible l-value type, returning true if it was
    /// an l-value.
    bool lookThroughLValue(Type &type, LValueType::Qual &qs) {
      if (LValueType *lv = type->getAs<LValueType>()) {
        Type objectType = lv->getObjectType();
        if (objectType->is<LValueType>()) {
          Out << "type is an lvalue of lvalue type: ";
          type.print(Out);
          Out << "\n";
        }
        type = objectType;
        return true;
      }
      return false;
    }
    bool lookThroughLValue(Type &type) {
      LValueType::Qual qs;
      return lookThroughLValue(type, qs);
    }

    /// The two types are required to either both be l-values or
    /// both not be l-values.  They are adjusted to not be l-values.
    /// Returns true if they are both l-values.
    bool checkSameLValueness(Type &T0, Type &T1,
                             const char *what) {
      LValueType::Qual Q0, Q1;
      bool isLValue0 = lookThroughLValue(T0, Q0);
      bool isLValue1 = lookThroughLValue(T1, Q1);
      
      if (isLValue0 != isLValue1) {
        Out << "lvalue-ness of " << what << " do not match: "
            << isLValue0 << ", " << isLValue1 << "\n";
        abort();
      }

      if (isLValue0 && Q0 != Q1) {
        Out << "qualification of " << what << " do not match\n";
        abort();
      }

      return isLValue0;
    }

    /// The two types are required to either both be l-values or
    /// both not be l-values, and one or the other is expected.
    /// They are adjusted to not be l-values.
    void checkSameLValueness(Type &T0, Type &T1, bool expected,
                             const char *what) {
      if (checkSameLValueness(T0, T1, what) == expected)
        return;

      Out << "lvalue-ness of " << what << " does not match expectation of "
          << expected << "\n";
      abort();
    }

    Type checkLValue(Type T, LValueType::Qual &Q, const char *what) {
      LValueType *LV = T->getAs<LValueType>();
      if (LV) {
        Q = LV->getQualifiers() - LValueType::Qual(LValueType::Qual::Implicit);
        return LV->getObjectType();
      }

      Out << "type is not an l-value in " << what << ": ";
      T.print(Out);
      Out << "\n";
      abort();
    }
    Type checkLValue(Type T, const char *what) {
      LValueType::Qual qs;
      return checkLValue(T, qs, what);
    }

    // Verification utilities.
    Type checkMetatypeType(Type type, const char *what) {
      auto metatype = type->getAs<MetaTypeType>();
      if (metatype) return metatype->getInstanceType();

      Out << what << " is not a metatype: ";
      type.print(Out);
      Out << "\n";
      abort();
    }

    void checkIsTypeOfRValue(ValueDecl *D, Type rvalueType, const char *what) {
      auto declType = D->getType();
      if (auto refType = declType->getAs<ReferenceStorageType>())
        declType = refType->getReferentType();
      checkSameType(declType, rvalueType, what);
    }

    void checkSameType(Type T0, Type T1, const char *what) {
      if (T0->getCanonicalType() == T1->getCanonicalType())
        return;

      Out << "different types for " << what << ": ";
      T0.print(Out);
      Out << " vs. ";
      T1.print(Out);
      Out << "\n";
      abort();
    }

    void checkTrivialSubtype(Type srcTy, Type destTy, const char *what) {
      if (srcTy->isEqual(destTy)) return;

      if (auto srcMetaType = srcTy->getAs<MetaTypeType>()) {
        if (auto destMetaType = destTy->getAs<MetaTypeType>()) {
          return checkTrivialSubtype(srcMetaType->getInstanceType(),
                                     destMetaType->getInstanceType(),
                                     what);
        }
        goto fail;
      }

      // FIXME: don't just check the hierarchy.
      {
        ClassDecl *srcClass = srcTy->getClassOrBoundGenericClass();
        ClassDecl *destClass = destTy->getClassOrBoundGenericClass();

        if (!srcClass || !destClass) {
          Out << "subtype conversion in " << what
              << " doesn't involve class types: ";
          srcTy.print(Out);
          Out << " to ";
          destTy.print(Out);
          Out << "\n";
          abort();
        }

        assert(srcClass != destClass);
        while (srcClass->hasSuperclass()) {
          srcClass = srcClass->getSuperclass()->getClassOrBoundGenericClass();
          assert(srcClass);
          if (srcClass == destClass) return;
        }

        Out << "subtype conversion in " << what << " is not to super class: ";
        srcTy.print(Out);
        Out << " to ";
        destTy.print(Out);
        Out << "\n";
        abort();
      }

    fail:
      Out << "subtype conversion in " << what << " is invalid: ";
      srcTy.print(Out);
      Out << " to ";
      destTy.print(Out);
      Out << "\n";
      abort();
    }

    void checkSameOrSubType(Type T0, Type T1, const char *what) {
      if (T0->getCanonicalType() == T1->getCanonicalType())
        return;

      // Protocol subtyping.
      if (auto Proto0 = T0->getAs<ProtocolType>())
        if (auto Proto1 = T1->getAs<ProtocolType>())
          if (Proto0->getDecl()->inheritsFrom(Proto1->getDecl()))
            return;
      
      // FIXME: Actually check this?
      if (T0->isExistentialType() || T1->isExistentialType())
        return;
      
      Out << "incompatible types for " << what << ": ";
      T0.print(Out);
      Out << " vs. ";
      T1.print(Out);
      Out << "\n";
      abort();
    }

    bool isGoodSourceRange(SourceRange SR) {
      if (SR.isInvalid())
        return false;
      (void) Ctx.SourceMgr.findBufferContainingLoc(SR.Start);
      (void) Ctx.SourceMgr.findBufferContainingLoc(SR.End);
      return true;
    }

    void checkSourceRanges(FuncExpr *FE) {
      for (auto P : FE->getArgParamPatterns()) {
        if (!P->isImplicit() && !isGoodSourceRange(P->getSourceRange())) {
          Out << "bad source range for arg param pattern: ";
          P->print(Out);
          Out << "\n";
          abort();
        }
      }
      checkSourceRanges(cast<Expr>(FE));
    }

    void checkSourceRanges(Expr *E) {
      if (!E->getSourceRange().isValid()) {
        // We don't care about source ranges on implicitly-generated
        // expressions.
        if (E->isImplicit())
          return;
        
        Out << "invalid source range for expression: ";
        E->print(Out);
        Out << "\n";
        abort();
      }
      if (!isGoodSourceRange(E->getSourceRange())) {
        Out << "bad source range for expression: ";
        E->print(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(E->getSourceRange(), Parent,
                        [&]{ E->print(Out); } );
    }
    
    void checkSourceRanges(Stmt *S) {
      if (!S->getSourceRange().isValid()) {
        // We don't care about source ranges on implicitly-generated
        // expressions.
        if (S->isImplicit())
          return;

        Out << "invalid source range for statement: ";
        S->print(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(S->getSourceRange(), Parent,
                        [&]{ S->print(Out); });
    }

    void checkSourceRanges(Decl *D) {
      if (!D->getSourceRange().isValid()) {
        Out << "invalid source range for decl: ";
        D->print(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(D->getSourceRange(), Parent,
                        [&]{ D->print(Out); });
    }
    
    /// \brief Verify that the given source ranges is contained within the
    /// parent's source range.
    void checkSourceRanges(SourceRange Current,
                           ASTWalker::ParentTy Parent,
                           std::function<void()> printEntity) {
      SourceRange Enclosing;
      if (Parent.isNull())
          return;
          
      if (Stmt *S = Parent.dyn_cast<Stmt*>()) {
        Enclosing = S->getSourceRange();
        if (S->isImplicit())
          return;
      } else if (Pattern *P = Parent.dyn_cast<Pattern*>()) {
        Enclosing = P->getSourceRange();
      } else if (Expr *E = Parent.dyn_cast<Expr*>()) {
        // FIXME: This hack is required because the inclusion check below
        // doesn't compares the *start* of the ranges, not the end of the
        // ranges.  In the case of an interpolated string literal expr, the
        // subexpressions are contained within the string token.  This means
        // that comparing the start of the string token to the end of an
        // embedded expression will fail.
        if (isa<InterpolatedStringLiteralExpr>(E))
          return;

        if (E->isImplicit())
          return;
        
        Enclosing = E->getSourceRange();
      } else {
        llvm_unreachable("impossible parent node");
      }
      
      if (!Ctx.SourceMgr.rangeContains(Enclosing, Current)) {
        Out << "child source range not contained within its parent: ";
        printEntity();
        Out << "\n  parent range: ";
        Enclosing.print(Out, Ctx.SourceMgr);
        Out << "\n  child range: ";
        Current.print(Out, Ctx.SourceMgr);
        Out << "\n";
        abort();
      }
    }

    void checkBoundGenericTypes(Type type) {
      if (!type)
        return;

      TypeBase *typePtr = type.getPointer();

      switch (typePtr->getKind()) {
#define ALWAYS_CANONICAL_TYPE(Id, Parent) \
      case TypeKind::Id:
#define UNCHECKED_TYPE(Id, Parent) ALWAYS_CANONICAL_TYPE(Id, Parent)
#define TYPE(Id, Parent)
#include "swift/AST/TypeNodes.def"
      case TypeKind::NameAlias:
      case TypeKind::ProtocolComposition:
      case TypeKind::AssociatedType:
      case TypeKind::GenericTypeParam:
      case TypeKind::DependentMember:
        return;
        
      case TypeKind::Union:
      case TypeKind::Struct:
      case TypeKind::Class:
        return checkBoundGenericTypes(cast<NominalType>(typePtr)->getParent());

      case TypeKind::BoundGenericClass:
      case TypeKind::BoundGenericUnion:
      case TypeKind::BoundGenericStruct: {
        auto BGT = cast<BoundGenericType>(typePtr);
        if (!BGT->hasSubstitutions()) {
          Out << "BoundGenericType without substitutions!\n";
          abort();
        }

        if (BGT->getDecl()->getGenericParams()->size() !=
            BGT->getGenericArgs().size()) {
          Out << "BoundGenericType has the wrong number of arguments!\n";
          abort();
        }

        checkBoundGenericTypes(BGT->getParent());
        for (Type Arg : BGT->getGenericArgs())
          checkBoundGenericTypes(Arg);
        return;
      }

      case TypeKind::MetaType:
        return checkBoundGenericTypes(
                               cast<MetaTypeType>(typePtr)->getInstanceType());

      case TypeKind::UnownedStorage:
      case TypeKind::WeakStorage:
        return checkBoundGenericTypes(
                       cast<ReferenceStorageType>(typePtr)->getReferentType());

      case TypeKind::Paren:
        return checkBoundGenericTypes(
                                cast<ParenType>(typePtr)->getUnderlyingType());

      case TypeKind::Tuple:
        for (auto elt : cast<TupleType>(typePtr)->getFields())
          checkBoundGenericTypes(elt.getType());
        return;
        
      case TypeKind::Substituted:
        return checkBoundGenericTypes(
                         cast<SubstitutedType>(typePtr)->getReplacementType());

      case TypeKind::Function:
      case TypeKind::PolymorphicFunction: {
        auto function = cast<AnyFunctionType>(typePtr);
        checkBoundGenericTypes(function->getInput());
        return checkBoundGenericTypes(function->getResult());
      }

      case TypeKind::Array:
        return checkBoundGenericTypes(cast<ArrayType>(typePtr)->getBaseType());
        
      case TypeKind::ArraySlice:
      case TypeKind::Optional:
        return checkBoundGenericTypes(
                                 cast<SyntaxSugarType>(typePtr)->getBaseType());

      case TypeKind::LValue:
        return checkBoundGenericTypes(
                                   cast<LValueType>(typePtr)->getObjectType());
      }
    }

    void checkBoundGenericTypes(Expr *E) {
      checkBoundGenericTypes(E->getType());
    }

    void checkBoundGenericTypes(Stmt *S) {
    }

    void checkBoundGenericTypes(Decl *D) {
    }

    void checkBoundGenericTypes(ValueDecl *D) {
      checkBoundGenericTypes(D->getType());
    }

    void checkErrors(Expr *E) {}
    void checkErrors(Stmt *S) {}
    void checkErrors(Decl *D) {}
    void checkErrors(ValueDecl *D) {
      if (!D->hasType())
        return;
      if (D->isInvalid() && !D->getType()->is<ErrorType>()) {
        Out << "Invalid decl has non-error type!\n";
        D->dump();
        abort();
      }
      if (D->getType()->is<ErrorType>() && !D->isInvalid()) {
        Out << "Valid decl has error type!\n";
        D->dump();
        abort();
      }
    }
  };
}

void swift::verify(TranslationUnit *TUnit) {
  Verifier verifier(TUnit);
  for (Decl *D : TUnit->Decls)
    D->walk(verifier);
}
