//===--- SILModule.cpp - SILModule implementation -------------------------===//
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

#include "swift/AST/CanTypeVisitor.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/FoldingSet.h"
using namespace swift;

namespace swift {
  /// SILTypeList - The uniqued backing store for the SILValue type list.  This
  /// is only exposed out of SILValue as an ArrayRef of types, so it should
  /// never be used outside of libSIL.
  class SILTypeList : public llvm::FoldingSetNode {
  public:
    unsigned NumTypes;
    SILType Types[1];  // Actually variable sized.
    
    void Profile(llvm::FoldingSetNodeID &ID) const {
      for (unsigned i = 0, e = NumTypes; i != e; ++i) {
        ID.AddPointer(Types[i].getOpaqueValue());
      }
    }
  };
} // end namespace swift.

/// SILTypeListUniquingType - This is the type of the folding set maintained by
/// SILModule that these things are uniqued into.
typedef llvm::FoldingSet<SILTypeList> SILTypeListUniquingType;

SILModule::SILModule(ASTContext &Context)
: TheASTContext(Context), Types(*this) {
  TypeListUniquing = new SILTypeListUniquingType();
}

SILModule::~SILModule() {
  delete (SILTypeListUniquingType*)TypeListUniquing;
}

ArrayRef<SILType> ValueBase::getTypes() const {
  // No results.
  if (TypeOrTypeList.isNull())
    return ArrayRef<SILType>();
  // Arbitrary list of results.
  if (auto *TypeList = TypeOrTypeList.dyn_cast<SILTypeList*>())
    return ArrayRef<SILType>(TypeList->Types, TypeList->NumTypes);
  // Single result.
  return TypeOrTypeList.get<SILType>();
}



/// getSILTypeList - Get a uniqued pointer to a SIL type list.  This can only
/// be used by SILValue.
SILTypeList *SILModule::getSILTypeList(ArrayRef<SILType> Types) const {
  assert(Types.size() > 1 && "Shouldn't use type list for 0 or 1 types");
  auto UniqueMap = (SILTypeListUniquingType*)TypeListUniquing;
  
  llvm::FoldingSetNodeID ID;
  for (auto T : Types) {
    ID.AddPointer(T.getOpaqueValue());
  }
  
  // If we already have this type list, just return it.
  void *InsertPoint = 0;
  if (SILTypeList *TypeList = UniqueMap->FindNodeOrInsertPos(ID, InsertPoint))
    return TypeList;
  
  // Otherwise, allocate a new one.
  void *NewListP = BPA.Allocate(sizeof(SILTypeList)+
                                sizeof(SILType)*(Types.size()-1),
                                alignof(SILTypeList));
  SILTypeList *NewList = new (NewListP) SILTypeList();
  NewList->NumTypes = Types.size();
  std::copy(Types.begin(), Types.end(), NewList->Types);
  
  UniqueMap->InsertNode(NewList, InsertPoint);
  return NewList;
}

namespace {
  /// Recursively destructure tuple-type arguments into SIL argument types.
  class LoweredFunctionInputTypeVisitor
    : public CanTypeVisitor<LoweredFunctionInputTypeVisitor>
  {
    SILModule &M;
    SmallVectorImpl<SILType> &inputTypes;
  public:
    LoweredFunctionInputTypeVisitor(SILModule &M,
                                    SmallVectorImpl<SILType> &inputTypes)
      : M(M), inputTypes(inputTypes) {}
    
    void visitType(CanType t) {
      inputTypes.push_back(M.Types.getLoweredType(t));
    }
    
    void visitTupleType(CanTupleType tt) {
      for (auto eltType : tt.getElementTypes()) {
        visit(eltType);
      }
    }
  };
} // end anonymous namespace

SILFunctionTypeInfo *SILModule::makeFunctionTypeInfo(AnyFunctionType *ft)
{
  SmallVector<SILType, 8> inputTypes;
  
  // If the result type lowers to an address-only type, add it as an indirect
  // return argument.
  SILType resultType = Types.getLoweredType(ft->getResult());
  bool hasIndirectReturn = resultType.isAddressOnly(*this);
  if (hasIndirectReturn) {
    inputTypes.push_back(resultType);
    resultType = Types.getEmptyTupleType();
  }
  
  // Destructure the input tuple type.
  LoweredFunctionInputTypeVisitor(*this, inputTypes)
    .visit(ft->getInput()->getCanonicalType());
  
  return SILFunctionTypeInfo::create(CanType(ft), inputTypes, resultType,
                                     hasIndirectReturn, *this);
}

