//===--- GenDecl.cpp - IR Generation for Declarations ---------------------===//
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
//  This file implements IR generation for local and global
//  declarations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/TypeMemberVisitor.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/IRGen/Options.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/TypeBuilder.h"

#include "CallingConvention.h"
#include "Explosion.h"
#include "FormalType.h"
#include "GenClass.h"
#include "GenObjC.h"
#include "GenMeta.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "Linking.h"
#include "TypeInfo.h"

using namespace swift;
using namespace irgen;

static bool isTrivialGlobalInit(llvm::Function *fn) {
  // Must be exactly one basic block.
  if (std::next(fn->begin()) != fn->end()) return false;

  // Basic block must have exactly one instruction.
  llvm::BasicBlock *entry = &fn->getEntryBlock();
  if (std::next(entry->begin()) != entry->end()) return false;

  // That instruction is necessarily a 'ret' instruction.
  assert(isa<llvm::ReturnInst>(entry->front()));
  return true;
}

/// Generates a function to call +load on all the given classes. 
static llvm::Function *emitObjCClassInitializer(IRGenModule &IGM,
                                                ArrayRef<llvm::WeakVH> classes){
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(llvm::Type::getVoidTy(IGM.LLVMContext), false);
  llvm::Function *initFn =
    llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                           "_swift_initObjCClasses", &IGM.Module);

  IRGenFunction initIGF(IGM, ExplosionKind::Minimal, initFn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->createArtificialFunction(initIGF, initFn);

  llvm::Constant *loadSelRef = IGM.getAddrOfObjCSelectorRef("load");
  llvm::Value *loadSel =
    initIGF.Builder.CreateLoad(Address(loadSelRef,
                                       initIGF.IGM.getPointerAlignment()));
  loadSel = initIGF.Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), loadSel);

  llvm::Type *msgSendParams[] = {
    IGM.ObjCPtrTy,
    IGM.ObjCSELTy
  };
  llvm::FunctionType *msgSendType =
    llvm::FunctionType::get(llvm::Type::getVoidTy(IGM.LLVMContext),
                            msgSendParams, false);
  llvm::Constant *msgSend =
    llvm::ConstantExpr::getBitCast(IGM.getObjCMsgSendFn(),
                                   msgSendType->getPointerTo());

  for (auto nextClass : classes) {
    llvm::Constant *receiver =
      llvm::ConstantExpr::getBitCast(cast<llvm::Constant>(nextClass),
                                     IGM.ObjCPtrTy);
    initIGF.Builder.CreateCall2(msgSend, receiver, loadSel);
  }

  initIGF.Builder.CreateRetVoid();

  return initFn;
}

namespace {
  
class CategoryInitializerVisitor
  : public ClassMemberVisitor<CategoryInitializerVisitor>
{
  IRGenFunction &IGF;
  
  llvm::Function *class_replaceMethod;
  
  llvm::Constant *classMetadata;
  llvm::Constant *metaclassMetadata;
  
public:
  CategoryInitializerVisitor(IRGenFunction &IGF, ExtensionDecl *ext)
    : IGF(IGF)
  {
    // FIXME: Should also register new ObjC protocol conformances using
    // class_addProtocol.

    // IMP class_replaceMethod(Class cls, SEL name, IMP imp, const char *types);
    llvm::Type *class_replaceMethod_params[] = {
      IGF.IGM.TypeMetadataPtrTy,
      IGF.IGM.Int8PtrTy,
      IGF.IGM.Int8PtrTy,
      IGF.IGM.Int8PtrTy
    };
    llvm::FunctionType *class_replaceMethod_ty =
      llvm::FunctionType::get(IGF.IGM.Int8PtrTy,
                              class_replaceMethod_params,
                              false);
    class_replaceMethod = IGF.IGM.Module.getFunction("class_replaceMethod");
    if (!class_replaceMethod) {
      class_replaceMethod = llvm::Function::Create(class_replaceMethod_ty,
                                             llvm::GlobalValue::ExternalLinkage,
                                             "class_replaceMethod",
                                             &IGF.IGM.Module);
      if (IGF.IGM.DebugInfo)
        IGF.IGM.DebugInfo->createArtificialFunction(IGF, class_replaceMethod);
    }
    
    CanType origTy = ext->getDeclaredTypeOfContext()->getCanonicalType();
    classMetadata = tryEmitConstantHeapMetadataRef(IGF.IGM, origTy);
    assert(classMetadata &&
           "extended objc class doesn't have constant metadata?!");
    classMetadata = llvm::ConstantExpr::getBitCast(classMetadata,
                                                   IGF.IGM.TypeMetadataPtrTy);
    metaclassMetadata = IGF.IGM.getAddrOfObjCMetaclass(
                                       origTy->getClassOrBoundGenericClass());
    metaclassMetadata = llvm::ConstantExpr::getBitCast(metaclassMetadata,
                                                   IGF.IGM.TypeMetadataPtrTy);
  }
  
  void visitMembers(ExtensionDecl *ext) {
    for (Decl *member : ext->getMembers())
      visit(member);
  }
  
  void visitFuncDecl(FuncDecl *method) {
    if (!requiresObjCMethodDescriptor(method)) return;
    llvm::Constant *name, *imp, *types;
    emitObjCMethodDescriptorParts(IGF.IGM, method, name, types, imp);
    
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = IGF.Builder.CreateCall(IGF.IGM.getObjCSelRegisterNameFn(),
                                              name);
    
    llvm::Value *args[] = {
      method->isStatic() ? metaclassMetadata : classMetadata,
      sel,
      imp,
      types
    };
    
    IGF.Builder.CreateCall(class_replaceMethod, args);
  }
  
  void visitVarDecl(VarDecl *prop) {
    if (!requiresObjCPropertyDescriptor(prop)) return;
    
    llvm::Constant *name, *imp, *types;
    emitObjCGetterDescriptorParts(IGF.IGM, prop,
                                  name, types, imp);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = IGF.Builder.CreateCall(IGF.IGM.getObjCSelRegisterNameFn(),
                                              name);
    llvm::Value *getterArgs[] = {classMetadata, sel, imp, types};
    IGF.Builder.CreateCall(class_replaceMethod, getterArgs);

    if (prop->isSettable()) {
      emitObjCSetterDescriptorParts(IGF.IGM, prop,
                                    name, types, imp);
      sel = IGF.Builder.CreateCall(IGF.IGM.getObjCSelRegisterNameFn(),
                                   name);
      llvm::Value *setterArgs[] = {classMetadata, sel, imp, types};
      
      IGF.Builder.CreateCall(class_replaceMethod, setterArgs);
    }

    // FIXME: register property metadata in addition to the methods.
  }
};

} // end anonymous namespace

static llvm::Function *emitObjCCategoryInitializer(IRGenModule &IGM,
                                         ArrayRef<ExtensionDecl*> categories) {
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(llvm::Type::getVoidTy(IGM.LLVMContext), false);
  llvm::Function *initFn =
    llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                           "_swift_initObjCCategories", &IGM.Module);
  
  IRGenFunction initIGF(IGM, ExplosionKind::Minimal, initFn);
  if (IGM.DebugInfo)
    IGM.DebugInfo->createArtificialFunction(initIGF, initFn);
  
  for (ExtensionDecl *ext : categories) {
    CategoryInitializerVisitor(initIGF, ext).visitMembers(ext);
  }
  
  initIGF.Builder.CreateRetVoid();
  return initFn;
}

/// Emit all the top-level code in the translation unit.
void IRGenModule::emitTranslationUnit(TranslationUnit *tunit,
                                      unsigned StartElem) {
  /// Emit all the code from the SIL module and declarations.
  emitGlobalTopLevel(tunit, StartElem);
  
  llvm::Function *topLevelCodeFn = Module.getFunction("top_level_code");
  assert(topLevelCodeFn && "no top_level_code in SIL module?!");
  
  Type emptyTuple = TupleType::getEmpty(Context);
  auto unitToUnit = CanType(FunctionType::get(emptyTuple, emptyTuple, Context));
  Pattern *params[] = {
    TuplePattern::create(Context, SourceLoc(),
                         ArrayRef<TuplePatternElt>(), SourceLoc())
  };
  params[0]->setType(TupleType::getEmpty(Context));

  llvm::AttributeSet attrs;
  llvm::FunctionType *fnType =
      getFunctionType(AbstractCC::Freestanding,
                      unitToUnit, ExplosionKind::Minimal, 0, ExtraData::None,
                      attrs);
  llvm::Function *initFn = nullptr;
  if (tunit->Kind != TranslationUnit::Main &&
      tunit->Kind != TranslationUnit::REPL) {
    // Create a global initializer for library modules.
    // FIXME: This is completely, utterly, wrong.
    initFn = llvm::Function::Create(fnType, llvm::GlobalValue::ExternalLinkage,
                                    tunit->Name.str() + ".init", &Module);
    initFn->setAttributes(attrs);
    
    // Insert a call to the top_level_code symbol from the SIL module.
    IRGenFunction initIGF(*this, ExplosionKind::Minimal, initFn);
    if (DebugInfo)
      DebugInfo->createArtificialFunction(initIGF, initFn);

    initIGF.Builder.CreateCall(topLevelCodeFn);
    initIGF.Builder.CreateRetVoid();
  }
  
  SmallVector<llvm::Constant *, 2> allInits;
  if (tunit->Kind == TranslationUnit::Main ||
      tunit->Kind == TranslationUnit::REPL) {
    // We don't need global init to call main().
  } else if (isTrivialGlobalInit(topLevelCodeFn)) {
    // Not all translation units need a global initialization function.
    initFn->eraseFromParent();
    topLevelCodeFn->eraseFromParent();
  } else {
    // Build the initializer for the module.
    llvm::Constant *initAndPriority[] = {
      llvm::ConstantInt::get(Int32Ty, 1),
      initFn
    };
    allInits.push_back(llvm::ConstantStruct::getAnon(LLVMContext,
                                                     initAndPriority));
  }

  if (!allInits.empty()) {
    llvm::ArrayType *initListType =
      llvm::ArrayType::get(allInits[0]->getType(), allInits.size());
    llvm::Constant *globalInits =
      llvm::ConstantArray::get(initListType, allInits);

    // Add this as a global initializer.
    (void) new llvm::GlobalVariable(Module,
                                    globalInits->getType(),
                                    /*is constant*/ true,
                                    llvm::GlobalValue::AppendingLinkage,
                                    globalInits,
                                    "llvm.global_ctors");
  }
  
  emitGlobalLists();
  
  if (tunit->Kind == TranslationUnit::Main ||
      tunit->Kind == TranslationUnit::REPL) {
    // Emit main().
    // FIXME: We should only emit this in non-JIT modes.

    llvm::Type* argcArgvTypes[2] = {
      llvm::TypeBuilder<llvm::types::i<32>, true>::get(LLVMContext),
      llvm::TypeBuilder<llvm::types::i<8>**, true>::get(LLVMContext)
    };

    llvm::Function *mainFn =
      llvm::Function::Create(
        llvm::FunctionType::get(Int32Ty, argcArgvTypes, false),
          llvm::GlobalValue::ExternalLinkage, "main", &Module);

    IRGenFunction mainIGF(*this, ExplosionKind::Minimal, mainFn);
    if (DebugInfo)
      DebugInfo->createArtificialFunction(mainIGF, mainFn);

    // Poke argc and argv into variables declared in the Swift stdlib
    auto args = mainFn->arg_begin();
    for(auto varNames: { 
        std::make_pair("argc", "C_ARGC"), 
          std::make_pair("argv", "C_ARGV") }) {
      const char *fnParameterName;
      const char *swiftVarName;
      std::tie(fnParameterName, swiftVarName) = varNames;

      llvm::Value* fnParameter = args++;
      fnParameter->setName(fnParameterName);
      
      UnqualifiedLookup lookup(
        Context.getIdentifier(swiftVarName), tunit);

      // If you're running without a standard library, there's nowhere
      // to poke the variable.
      unsigned const resultCount = lookup.Results.size();
      if (resultCount != 0) {
        assert(lookup.Results.size() == 1);
        auto swiftVarDecl = cast<VarDecl>(
          lookup.Results.front().getValueDecl());
        Address swiftVarAddress = getAddrOfGlobalVariable(swiftVarDecl);

        // The swift vars are structs whose first member is a raw LLVM value
        Address firstMemberAddress = mainIGF.Builder.CreateStructGEP(
          swiftVarAddress, 0, Size(0));
        
        if (fnParameterName[3] == 'v') { // extra step for argv
          // The first member of UnsafePointer<T> is just an opaque LLVM
          // void*; interpret it as char** so we can store into it.
          firstMemberAddress = mainIGF.Builder.CreateBitCast(
              firstMemberAddress, 
              llvm::TypeBuilder<
                llvm::types::i<8>***, true
              >::get(LLVMContext));
        }
      
        mainIGF.Builder.CreateStore(fnParameter, firstMemberAddress);
      }
    }

    // Emit Objective-C runtime interop setup for immediate-mode code.
    if (ObjCInterop && Opts.UseJIT) {
      if (!ObjCClasses.empty()) {
        // Emit an initializer for the Objective-C classes.
        mainIGF.Builder.CreateCall(emitObjCClassInitializer(*this,ObjCClasses));
      }
      if (!ObjCCategoryDecls.empty()) {
        // Emit an initializer to add declarations from category decls.
        mainIGF.Builder.CreateCall(emitObjCCategoryInitializer(*this,
                                                            ObjCCategoryDecls));
      }
    }
    
    // Call the top-level code.
    mainIGF.Builder.CreateCall(topLevelCodeFn);
    mainIGF.Builder.CreateRet(mainIGF.Builder.getInt32(0));
  }

  // Objective-C image information.
  // Generate module-level named metadata to convey this information to the
  // linker and code-gen.
  unsigned version = 0; // Version is unused?
  const char *section = "__DATA, __objc_imageinfo, regular, no_dead_strip";

  // Add the ObjC ABI version to the module flags.
  Module.addModuleFlag(llvm::Module::Error, "Objective-C Version", 2);
  Module.addModuleFlag(llvm::Module::Error, "Objective-C Image Info Version",
                       version);
  Module.addModuleFlag(llvm::Module::Error, "Objective-C Image Info Section",
                       llvm::MDString::get(LLVMContext, section));

  Module.addModuleFlag(llvm::Module::Override,
                       "Objective-C Garbage Collection", (uint32_t)0);
  // FIXME: Simulator flag.

  // Fix up the DICompileUnit.
  if (DebugInfo)
    DebugInfo->finalize();
}

/// Add the given global value to @llvm.used.
void IRGenModule::addUsedGlobal(llvm::GlobalValue *global) {
  assert(!global->isDeclaration() &&
         "Only globals with definition can force usage.");
  LLVMUsed.push_back(global);
}

/// Add the given global value to the Objective-C class list.
void IRGenModule::addObjCClass(llvm::Constant *classPtr) {
  ObjCClasses.push_back(classPtr);
}

/// Emit a global list, i.e. a global constant array holding all of a
/// list of values.  Generally these lists are for various LLVM
/// metadata or runtime purposes.
static void emitGlobalList(IRGenModule &IGM, ArrayRef<llvm::WeakVH> handles,
                           StringRef name, StringRef section,
                           llvm::GlobalValue::LinkageTypes linkage) {
  // Do nothing if the list is empty.
  if (handles.empty()) return;

  // For global lists that actually get linked (as opposed to notional
  // ones like @llvm.used), it's important to set an explicit alignment
  // so that the linker doesn't accidentally put padding in the list.
  Alignment alignment = IGM.getPointerAlignment();
  auto eltTy = IGM.Int8PtrTy;

  // We have an array of value handles, but we need an array of constants.
  SmallVector<llvm::Constant*, 8> elts;
  elts.reserve(handles.size());
  for (auto &handle : handles) {
    auto elt = cast<llvm::Constant>(&*handle);
    elt = llvm::ConstantExpr::getBitCast(elt, eltTy);
    elts.push_back(elt);
  }

  auto varTy = llvm::ArrayType::get(eltTy, elts.size());
  auto init = llvm::ConstantArray::get(varTy, elts);
  auto var = new llvm::GlobalVariable(IGM.Module, varTy, false, linkage,
                                      init, name);
  var->setSection(section);
  var->setAlignment(alignment.getValue());

  // Mark the variable as used if doesn't have external linkage.
  // (Note that we'd specifically like to not put @llvm.used in itself.)
  if (llvm::GlobalValue::isLocalLinkage(linkage))
    IGM.addUsedGlobal(var);
}

void IRGenModule::emitGlobalLists() {
  // Objective-C class references go in a variable with a meaningless
  // name but a magic section.
  emitGlobalList(*this, ObjCClasses, "objc_classes",
                 "__DATA, __objc_classlist, regular, no_dead_strip",
                 llvm::GlobalValue::InternalLinkage);
  // So do categories.
  emitGlobalList(*this, ObjCCategories, "objc_categories",
                 "__DATA, __objc_catlist, regular, no_dead_strip",
                 llvm::GlobalValue::InternalLinkage);

  // FIXME: We also emit the class references in a second magic section to make
  // sure they are "realized" by the Objective-C runtime before any instances
  // are allocated.
  emitGlobalList(*this, ObjCClasses, "objc_non_lazy_classes",
                 "__DATA, __objc_nlclslist, regular, no_dead_strip",
                 llvm::GlobalValue::InternalLinkage);

  // @llvm.used
  emitGlobalList(*this, LLVMUsed, "llvm.used", "llvm.metadata",
                 llvm::GlobalValue::AppendingLinkage);
}

void IRGenModule::emitGlobalTopLevel(TranslationUnit *TU, unsigned StartElem) {
  // Emit global variables.
  for (VarDecl *global : SILMod->getGlobals()) {
    TypeInfo const &ti = getFragileTypeInfo(global->getType());
    emitGlobalVariable(global, ti);
  }
  
  // Emit SIL functions.
  for (SILFunction &f : *SILMod) {
    emitSILFunction(&f);
  }

  // Emit types and other global decls.
  for (unsigned i = StartElem, e = TU->Decls.size(); i != e; ++i) {
    emitGlobalDecl(TU->Decls[i]);
  }

  // Emit external definitions used by this translation unit.
  for (auto def : Context.ExternalDefinitions) {
    emitExternalDefinition(def);
  }
}

static bool isLocalLinkageDecl(Decl *D) {
  DeclContext *DC = D->getDeclContext();
  while (!DC->isModuleContext()) {
    if (DC->isLocalContext())
      return true;
    DC = DC->getParent();
  }

  return false;
}

static bool isLocalLinkageType(CanType type);
static bool isLocalLinkageGenericClause(const GenericParamList &params) {
  // Type parameters are local-linkage if any of their constraining
  // types are.
  for (auto &param : params) {
    for (auto proto : param.getAsTypeParam()->getProtocols())
      if (isLocalLinkageType(CanType(proto->getDeclaredType())))
        return true;
    if (auto superclass = param.getAsTypeParam()->getSuperclass())
      if (isLocalLinkageType(superclass->getCanonicalType()))
        return true;
  }
  return false;
}

static bool isLocalLinkageType(CanType type) {
  switch (type->getKind()) {
  case TypeKind::Error:
    llvm_unreachable("error type in IRGen");
  case TypeKind::TypeVariable:
    llvm_unreachable("type variable in IRgen");
      
  case TypeKind::MetaType:
    return isLocalLinkageType(cast<MetaTypeType>(type).getInstanceType());
  case TypeKind::Module:
    return false;

  case TypeKind::Archetype:
    return false;

  // We don't care about these types being a bit verbose because we
  // don't expect them to come up that often in API names.
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinOpaquePointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinVector:
    return false;

#define SUGARED_TYPE(id, parent)                \
  case TypeKind::id:                            \
    llvm_unreachable("type is not canonical!");
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"

  case TypeKind::LValue:
    return isLocalLinkageType(cast<LValueType>(type).getObjectType());

  case TypeKind::Tuple: {
    CanTupleType tuple = cast<TupleType>(type);
    for (auto fieldType : tuple.getElementTypes()) {
      if (isLocalLinkageType(fieldType))
        return true;
    }
    return false;
  }

  case TypeKind::UnboundGeneric:
    return isLocalLinkageDecl(cast<UnboundGenericType>(type)->getDecl());

  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericUnion:
  case TypeKind::BoundGenericStruct: {
    CanBoundGenericType BGT = cast<BoundGenericType>(type);
    if (isLocalLinkageDecl(BGT->getDecl()))
      return true;
    for (Type arg : BGT->getGenericArgs()) {
      if (isLocalLinkageType(CanType(arg)))
        return true;
    }
    return false;
  }

  case TypeKind::Union:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::Protocol:
    return isLocalLinkageDecl(cast<NominalType>(type)->getDecl());

  case TypeKind::PolymorphicFunction: {
    auto fn = cast<PolymorphicFunctionType>(type);
    if (isLocalLinkageGenericClause(fn->getGenericParams()))
      return true;
    SWIFT_FALLTHROUGH;
  }
  case TypeKind::Function: {
    CanAnyFunctionType fn = cast<AnyFunctionType>(type);
    return isLocalLinkageType(fn.getInput()) ||
           isLocalLinkageType(fn.getResult());
  }

  case TypeKind::ReferenceStorage: {
    auto ref = cast<ReferenceStorageType>(type);
    return isLocalLinkageType(ref.getReferentType());
  }

  case TypeKind::Array:
    return isLocalLinkageType(cast<ArrayType>(type).getBaseType());

  case TypeKind::ProtocolComposition:
    for (Type t : cast<ProtocolCompositionType>(type)->getProtocols())
      if (isLocalLinkageType(CanType(t)))
        return true;
    return false;
  }
  llvm_unreachable("bad type kind");
}

bool LinkEntity::isLocalLinkage() const {
  switch (getKind()) {
  // Value witnesses depend on the linkage of their type.
  case Kind::ValueWitness:
  case Kind::ValueWitnessTable:
  case Kind::TypeMetadata:
  case Kind::TypeMangling:
  case Kind::DebuggerTypeMangling:
    return isLocalLinkageType(getType());

  case Kind::WitnessTableOffset:
  case Kind::Constructor:
  case Kind::Destructor:
  case Kind::Function:
  case Kind::Getter:
  case Kind::Setter:
  case Kind::Other:
  case Kind::ObjCClass:
  case Kind::ObjCMetaclass:
  case Kind::SwiftMetaclassStub:
  case Kind::FieldOffset:
    return isLocalLinkageDecl(getDecl());
  
  case Kind::DirectProtocolWitnessTable:
  case Kind::LazyProtocolWitnessTableAccessor:
  case Kind::DependentProtocolWitnessTableGenerator:
    return false;
      
  case Kind::LazyProtocolWitnessTableTemplate:
  case Kind::DependentProtocolWitnessTableTemplate:
    return true;
  
  case Kind::AnonymousFunction:
    return true;

  case Kind::BridgeToBlockConverter:
    // Bridge-to-block shims are currently always provided from a stub.
    return false;

  case Kind::SILFunction:
    return getSILFunction()->getLinkage() == SILLinkage::Internal;
  }
  llvm_unreachable("bad link entity kind");
}

bool LinkEntity::isClangThunk() const {
  // Constructors, subscripts, properties, and type metadata synthesized in the
  // mapping to Clang modules are local.
  if (getKind() == Kind::SILFunction)
    return getSILFunction()->getLinkage() == SILLinkage::ClangThunk;

  if (isDeclKind(getKind())) {
    ValueDecl *D = static_cast<ValueDecl *>(Pointer);
    return isa<ClangModule>(D->getDeclContext()->getParentModule()) &&
      (isa<ConstructorDecl>(D) || isa<SubscriptDecl>(D) ||
       (isa<VarDecl>(D) && cast<VarDecl>(D)->isProperty()));
  } else { // isTypeKind(getKind())
    CanType ty = CanType(static_cast<TypeBase*>(Pointer));
    NominalTypeDecl *decl = ty->getNominalOrBoundGenericNominal();

    if (!decl)
      return false;

    return isa<ClangModule>(decl->getDeclContext()->getParentModule());
  }
}

LinkInfo LinkInfo::get(IRGenModule &IGM, const LinkEntity &entity) {
  LinkInfo result;

  entity.mangle(result.Name);

  if (entity.isLocalLinkage()) {
    // If an entity isn't visible outside this translation unit,
    // it has internal linkage.
    result.Linkage = llvm::GlobalValue::InternalLinkage;
    result.Visibility = llvm::GlobalValue::DefaultVisibility;
    return result;
  } else if (entity.isValueWitness()) {
    // The linkage for a value witness is linkonce_odr.
    result.Linkage = llvm::GlobalValue::LinkOnceODRLinkage;
    result.Visibility = llvm::GlobalValue::HiddenVisibility;
  } else if (entity.isClangThunk()) {
    // Clang thunks are linkonce_odr and hidden.
    result.Linkage = llvm::GlobalValue::LinkOnceODRLinkage;
    result.Visibility = llvm::GlobalValue::HiddenVisibility;
  } else {
    // Give everything else external linkage.
    result.Linkage = llvm::GlobalValue::ExternalLinkage;
    result.Visibility = llvm::GlobalValue::DefaultVisibility;
  }

  return result;
}

static bool isPointerTo(llvm::Type *ptrTy, llvm::Type *objTy) {
  return cast<llvm::PointerType>(ptrTy)->getElementType() == objTy;
}

/// Get or create an LLVM function with these linkage rules.
llvm::Function *LinkInfo::createFunction(IRGenModule &IGM,
                                         llvm::FunctionType *fnType,
                                         llvm::CallingConv::ID cc,
                                         const llvm::AttributeSet &attrs) {
  llvm::Function *existing = IGM.Module.getFunction(getName());
  if (existing) {
    if (isPointerTo(existing->getType(), fnType))
      return cast<llvm::Function>(existing);

    IGM.error(SourceLoc(),
              "program too clever: function collides with existing symbol "
                + getName());

    // Note that this will implicitly unique if the .unique name is also taken.
    existing->setName(getName() + ".unique");
  }

  llvm::Function *fn
    = llvm::Function::Create(fnType, getLinkage(), getName(), &IGM.Module);
  fn->setVisibility(getVisibility());
  fn->setCallingConv(cc);
  if (!attrs.isEmpty())
    fn->setAttributes(attrs);
  return fn;
}

/// Get or create an LLVM global variable with these linkage rules.
llvm::GlobalVariable *LinkInfo::createVariable(IRGenModule &IGM,
                                               llvm::Type *storageType,
                                               DebugTypeInfo DebugType,
                                               SILLocation DebugLoc) {
  llvm::GlobalValue *existing = IGM.Module.getNamedGlobal(getName());
  if (existing) {
    if (isa<llvm::GlobalVariable>(existing) &&
        isPointerTo(existing->getType(), storageType))
      return cast<llvm::GlobalVariable>(existing);

    IGM.error(SourceLoc(),
              "program too clever: variable collides with existing symbol "
                + getName());

    // Note that this will implicitly unique if the .unique name is also taken.
    existing->setName(getName() + ".unique");
  }

  llvm::GlobalVariable *var
    = new llvm::GlobalVariable(IGM.Module, storageType, /*constant*/ false,
                               getLinkage(), /*initializer*/ nullptr,
                               getName());
  var->setVisibility(getVisibility());

  if (IGM.DebugInfo)
    IGM.DebugInfo->emitGlobalVariableDeclaration(var,
                                                 getName(),
                                                 getName(),
                                                 DebugType,
                                                 DebugLoc);

  return var;
}

/// Emit a global declaration.
void IRGenModule::emitGlobalDecl(Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Extension:
    return emitExtension(cast<ExtensionDecl>(D));

  case DeclKind::Protocol:
    return emitProtocolDecl(cast<ProtocolDecl>(D));
      
  case DeclKind::PatternBinding:
    // The global initializations are in SIL.
    return;

  case DeclKind::Subscript:
    llvm_unreachable("there are no global subscript operations");
      
  case DeclKind::UnionElement:
    llvm_unreachable("there are no global union elements");

  case DeclKind::Constructor:
    llvm_unreachable("there are no global constructor");

  case DeclKind::Destructor:
    llvm_unreachable("there are no global destructor");

  case DeclKind::TypeAlias:
    return;

  case DeclKind::Union:
    return emitUnionDecl(cast<UnionDecl>(D));

  case DeclKind::Struct:
    return emitStructDecl(cast<StructDecl>(D));

  case DeclKind::Class:
    return emitClassDecl(cast<ClassDecl>(D));

  // These declarations are only included in the debug info.
  case DeclKind::Import:
    if (DebugInfo)
      DebugInfo->emitImport(cast<ImportDecl>(D));
    return;

  // We emit these as part of the PatternBindingDecl.
  case DeclKind::Var:
    return;

  case DeclKind::Func:
    // Emit local definitions from the function body.
    return emitLocalDecls(cast<FuncDecl>(D));

  case DeclKind::TopLevelCode:
    // All the top-level code will be lowered separately.
    return;
      
  // Operator decls aren't needed for IRGen.
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
    return;
  }

  llvm_unreachable("bad decl kind!");
}

void IRGenModule::emitExternalDefinition(Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Extension:
  case DeclKind::PatternBinding:
  case DeclKind::UnionElement:
  case DeclKind::Union:
  case DeclKind::Class:
  case DeclKind::TopLevelCode:
  case DeclKind::TypeAlias:
  case DeclKind::Var:
  case DeclKind::Import:
  case DeclKind::Subscript:
  case DeclKind::Destructor:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
    llvm_unreachable("Not a valid external definition for IRgen");

  case DeclKind::Func:
    return emitLocalDecls(cast<FuncDecl>(D));
  case DeclKind::Constructor:
    return emitLocalDecls(cast<ConstructorDecl>(D));
    
  case DeclKind::Struct:
    // Emit Swift metadata for the external struct.
    emitStructMetadata(*this, cast<StructDecl>(D));
    break;
    
  case DeclKind::Protocol:
    // Emit Swift metadata for the protocol type.
    emitProtocolDecl(cast<ProtocolDecl>(D));
    break;
  }
}

/// Find the address of a (fragile, constant-size) global variable
/// declaration.  The address value is always an llvm::GlobalVariable*.
Address IRGenModule::getAddrOfGlobalVariable(VarDecl *var) {
  // Check whether we've cached this.
  LinkEntity entity = LinkEntity::forNonFunction(var);
  llvm::GlobalVariable *&entry = GlobalVars[entity];
  if (entry) {
    llvm::GlobalVariable *gv = cast<llvm::GlobalVariable>(entry);
    return Address(gv, Alignment(gv->getAlignment()));
  }

  const TypeInfo &type = getFragileTypeInfo(var->getType());

  // Okay, we need to rebuild it.
  LinkInfo link = LinkInfo::get(*this, entity);
  DebugTypeInfo DbgTy(var->getType()->getCanonicalType(), type);
  auto addr = link.createVariable(*this, type.StorageType, DbgTy, var);
  // Ask the type to give us an Address.
  Address result = type.getAddressForPointer(addr);

  // Set that alignment back on the global variable.
  addr->setAlignment(result.getAlignment().getValue());

  // Write this to the cache and return.
  entry = addr;
  return result;
}

/// Fetch the declaration of the given known function.
llvm::Function *IRGenModule::getAddrOfFunction(FunctionRef fn,
                                               ExtraData extraData) {
  LinkEntity entity = LinkEntity::forFunction(fn);

  // Check whether we've cached this.
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return cast<llvm::Function>(entry);

  llvm::FunctionType *fnType;
  AbstractCC convention = fn.getAbstractCC();
  // A bit of a hack here. SIL represents closure functions with their context
  // expanded out and uses a partial application function to construct the
  // context. IRGen previously set up local functions to expect their extraData
  // prepackaged.
  llvm::AttributeSet attrs;
  fnType = getFunctionType(convention,
                           fn.getDecl()->getType()->getCanonicalType(),
                           fn.getExplosionLevel(), fn.getUncurryLevel(),
                           extraData,
                           attrs);

  auto cc = expandAbstractCC(*this, convention);

  LinkInfo link = LinkInfo::get(*this, entity);
  entry = link.createFunction(*this, fnType, cc, attrs);
  return entry;
}

/// getAddrOfGlobalInjectionFunction - Get the address of the function to
/// perform a particular injection into a union type.
llvm::Function *IRGenModule::getAddrOfInjectionFunction(UnionElementDecl *D) {
  // TODO: emit at more optimal explosion kinds when reasonable!
  ExplosionKind explosionLevel = ExplosionKind::Minimal;
  unsigned uncurryLevel = 0;

  LinkEntity entity =
    LinkEntity::forFunction(CodeRef::forUnionElement(D, ExplosionKind::Minimal,
                                                     uncurryLevel));

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return cast<llvm::Function>(entry);

  CanType formalType = D->getType()->getCanonicalType();

  llvm::AttributeSet attrs;
  auto cc = expandAbstractCC(*this, AbstractCC::Freestanding);

  llvm::FunctionType *fnType =
    getFunctionType(AbstractCC::Freestanding,
                    formalType, explosionLevel, uncurryLevel, ExtraData::None,
                    attrs);
  LinkInfo link = LinkInfo::get(*this, entity);
  entry = link.createFunction(*this, fnType, cc, attrs);
  return entry;
}

/// Fetch the declaration of the given known function.
llvm::Function *IRGenModule::getAddrOfConstructor(ConstructorDecl *cons,
                                                  ConstructorKind ctorKind,
                                                  ExplosionKind explodeLevel) {
  unsigned uncurryLevel = 1;
  auto codeRef = CodeRef::forConstructor(cons, explodeLevel, uncurryLevel);
  LinkEntity entity = LinkEntity::forConstructor(codeRef, ctorKind);

  // Check whether we've cached this.
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return cast<llvm::Function>(entry);

  CanType formalType;
  if (ctorKind == ConstructorKind::Initializing)
    formalType = cons->getInitializerType()->getCanonicalType();
  else
    formalType = cons->getType()->getCanonicalType();
  
  llvm::AttributeSet attrs;
  llvm::FunctionType *fnType =
    getFunctionType(AbstractCC::Method,
                    formalType, explodeLevel, uncurryLevel, ExtraData::None,
                    attrs);

  auto cc = expandAbstractCC(*this, AbstractCC::Method);

  LinkInfo link = LinkInfo::get(*this, entity);
  entry = link.createFunction(*this, fnType, cc, attrs);
  return entry;
}

/// Get or create a llvm::GlobalVariable.
///
/// If a definition type is given, the result will always be an
/// llvm::GlobalVariable of that type.  Otherwise, the result will
/// have type pointerToDefaultType and may involve bitcasts.
static llvm::Constant *getAddrOfLLVMVariable(IRGenModule &IGM,
                     llvm::DenseMap<LinkEntity, llvm::GlobalVariable*> &globals,
                                             LinkEntity entity,
                                             llvm::Type *definitionType,
                                             llvm::Type *defaultType,
                                             llvm::Type *pointerToDefaultType,
                                             DebugTypeInfo DebugType) {
  auto &entry = globals[entity];
  if (entry) {
    // If we're looking to define something, we may need to replace a
    // forward declaration.
    if (definitionType) {
      assert(entry->getType() == pointerToDefaultType);

      // If the type is right, we're done.
      if (definitionType == defaultType)
        return entry;

      // Fall out to the case below, clearing the name so that
      // createVariable doesn't detect a collision.
      entry->setName("");

    // Otherwise, we have a previous declaration or definition which
    // we need to ensure has the right type.
    } else {
      return llvm::ConstantExpr::getBitCast(entry, pointerToDefaultType);
    }
  }

  // If we're not defining the object now
  if (!definitionType) definitionType = defaultType;

  // Create the variable.
  LinkInfo link = LinkInfo::get(IGM, entity);
  auto var = link.createVariable(IGM, definitionType, DebugType);

  // If we have an existing entry, destroy it, replacing it with the
  // new variable.
  if (entry) {
    auto castVar = llvm::ConstantExpr::getBitCast(var, pointerToDefaultType);
    entry->replaceAllUsesWith(castVar);
    entry->eraseFromParent();
  }

  // Cache and return.
  entry = var;
  return var;
}

/// Fetch a global reference to the given Objective-C class.  The
/// result is always a TypeMetadataPtrTy, but it may not be compatible
/// with IR-generation.
llvm::Constant *IRGenModule::getAddrOfObjCClass(ClassDecl *theClass) {
  assert(ObjCInterop && "getting address of ObjC class in no-interop mode");
  LinkEntity entity = LinkEntity::forObjCClass(theClass);
  DebugTypeInfo DbgTy(*theClass, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(*this, GlobalVars, entity,
                                    TypeMetadataStructTy, TypeMetadataStructTy,
                                    TypeMetadataPtrTy, DbgTy);
  return addr;
}

/// Fetch a global reference to the given Objective-C metaclass.
/// The result is always a GlobalVariable of ObjCClassPtrTy.
llvm::Constant *IRGenModule::getAddrOfObjCMetaclass(ClassDecl *theClass) {
  assert(ObjCInterop && "getting address of ObjC metaclass in no-interop mode");
  LinkEntity entity = LinkEntity::forObjCMetaclass(theClass);
  DebugTypeInfo DbgTy(*theClass, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(*this, GlobalVars, entity,
                                    ObjCClassStructTy, ObjCClassStructTy,
                                    ObjCClassPtrTy, DbgTy);
  return addr;
}

/// Fetch the declaration of the metaclass stub for the given class type.
/// The result is always a GlobalVariable of ObjCClassPtrTy.
llvm::Constant *IRGenModule::getAddrOfSwiftMetaclassStub(ClassDecl *theClass) {
  assert(ObjCInterop && "getting address of metaclass stub in no-interop mode");
  LinkEntity entity = LinkEntity::forSwiftMetaclassStub(theClass);
  DebugTypeInfo DbgTy(*theClass, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(*this, GlobalVars, entity,
                                    ObjCClassStructTy, ObjCClassStructTy,
                                    ObjCClassPtrTy, DbgTy);
  return addr;
}

/// Fetch the declaration of a metaclass object.  This performs either
/// getAddrOfSwiftMetaclassStub or getAddrOfObjCMetaclass, depending
/// on whether the class is published as an ObjC class.
llvm::Constant *IRGenModule::getAddrOfMetaclassObject(ClassDecl *decl) {
  if (decl->isObjC() || decl->hasClangNode()) {
    return getAddrOfObjCMetaclass(decl);
  } else {
    return getAddrOfSwiftMetaclassStub(decl);
  }
}

/// Fetch the declaration of the metadata (or metadata template) for a
/// class.
///
/// If the definition type is specified, the result will always be a
/// GlobalVariable of the given type, which may not be at the
/// canonical address point for a type metadata.
///
/// If the definition type is not specified, then:
///   - if the metadata is indirect, then the result will not be adjusted
///     and it will have the type pointer-to-T, where T is the type
///     of a direct metadata;
///   - if the metadata is a pattern, then the result will not be
///     adjusted and it will have TypeMetadataPatternPtrTy;
///   - otherwise it will be adjusted to the canonical address point
///     for a type metadata and it will have type TypeMetadataPtrTy.
llvm::Constant *IRGenModule::getAddrOfTypeMetadata(CanType concreteType,
                                                   bool isIndirect,
                                                   bool isPattern,
                                                   llvm::Type *storageType) {
  assert(isPattern || !isa<UnboundGenericType>(concreteType));

  llvm::Type *defaultVarTy;
  llvm::Type *defaultVarPtrTy;
  unsigned adjustmentIndex;
  ClassDecl *ObjCClass = nullptr;
  
  // Patterns use the pattern type and no adjustment.
  if (isPattern) {
    defaultVarTy = TypeMetadataPatternStructTy;
    defaultVarPtrTy = TypeMetadataPatternPtrTy;
    adjustmentIndex = 0;

  // Objective-C classes use the generic metadata type and need no adjustment.
  } else if (isa<ClassType>(concreteType) &&
             !hasKnownSwiftMetadata(*this,
                                    cast<ClassType>(concreteType)->getDecl())) {
    defaultVarTy = TypeMetadataStructTy;
    defaultVarPtrTy = TypeMetadataPtrTy;
    adjustmentIndex = 0;
    ObjCClass = cast<ClassType>(concreteType)->getDecl();
  // Class direct metadata use the heap type and require a two-word
  // adjustment (due to the heap-metadata header).
  } else if (isa<ClassType>(concreteType) ||
             isa<BoundGenericClassType>(concreteType)) {
    defaultVarTy = FullHeapMetadataStructTy;
    defaultVarPtrTy = FullHeapMetadataPtrTy;
    adjustmentIndex = 2;

  // All other non-pattern direct metadata use the full type and
  // require an adjustment.
  } else {
    defaultVarTy = FullTypeMetadataStructTy;
    defaultVarPtrTy = FullTypeMetadataPtrTy;
    adjustmentIndex = 1;
  }

  // When indirect, this is always a pointer variable and has no
  // adjustment.
  if (isIndirect) {
    defaultVarTy = defaultVarPtrTy;
    defaultVarPtrTy = defaultVarTy->getPointerTo();
    adjustmentIndex = 0;
  }

  LinkEntity entity
    = ObjCClass? LinkEntity::forObjCClass(ObjCClass)
               : LinkEntity::forTypeMetadata(concreteType, isIndirect,
                                             isPattern);

  auto DbgTy = ObjCClass 
    ? DebugTypeInfo(*ObjCClass, getPointerSize(), getPointerAlignment())
    : DebugTypeInfo();

  auto addr = getAddrOfLLVMVariable(*this, GlobalVars, entity,
                                    storageType, defaultVarTy,
                                    defaultVarPtrTy, DbgTy);

  // Do an adjustment if necessary.
  if (adjustmentIndex && !storageType) {
    llvm::Constant *indices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, adjustmentIndex)
    };
    addr = llvm::ConstantExpr::getInBoundsGetElementPtr(addr, indices);
  }

  return addr;
}

/// Fetch the declaration of the given known function.
llvm::Function *IRGenModule::getAddrOfDestructor(ClassDecl *cd,
                                                 DestructorKind kind) {
  LinkEntity entity = LinkEntity::forDestructor(cd, kind);

  // Check whether we've cached this.
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return cast<llvm::Function>(entry);

  llvm::AttributeSet attrs;
  auto cc = expandAbstractCC(*this, AbstractCC::Method);

  LinkInfo link = LinkInfo::get(*this, entity);
  llvm::FunctionType *dtorTy;
  if (kind == DestructorKind::Deallocating) {
    dtorTy = DeallocatingDtorTy;
  } else {
    const TypeInfo &info =
      getFragileTypeInfo(cd->getDeclaredTypeInContext());
    dtorTy = llvm::FunctionType::get(RefCountedPtrTy,
                                     info.getStorageType(),
                                     /*isVarArg*/ false);
  }
  
  entry = link.createFunction(*this, dtorTy, cc, attrs);
  return entry;
}


/// Returns the address of a value-witness function.
llvm::Function *IRGenModule::getAddrOfValueWitness(CanType concreteType,
                                                   ValueWitness index) {
  LinkEntity entity = LinkEntity::forValueWitness(concreteType, index);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return entry;

  // Find the appropriate function type.
  llvm::FunctionType *fnType =
    cast<llvm::FunctionType>(
      cast<llvm::PointerType>(getValueWitnessTy(index))
        ->getElementType());
  LinkInfo link = LinkInfo::get(*this, entity);
  entry = link.createFunction(*this, fnType, RuntimeCC, llvm::AttributeSet());
  return entry;
}

/// Returns the address of a value-witness table.  If a definition
/// type is provided, the table is created with that type; the return
/// value will be an llvm::GlobalVariable.  Otherwise, the result will
/// have type WitnessTablePtrTy.
llvm::Constant *IRGenModule::getAddrOfValueWitnessTable(CanType concreteType,
                                                  llvm::Type *definitionType) {
  LinkEntity entity = LinkEntity::forValueWitnessTable(concreteType);
  DebugTypeInfo DbgTy(concreteType, getPointerSize(), getPointerAlignment());
  return getAddrOfLLVMVariable(*this, GlobalVars, entity, definitionType,
                               WitnessTableTy, WitnessTablePtrTy, DbgTy);
}

static CanType addOwnerArgument(ASTContext &ctx, DeclContext *DC,
                                CanType resultType) {
  Type argType = DC->getDeclaredTypeInContext();
  if (!argType->hasReferenceSemantics()) {
    argType = LValueType::get(argType, LValueType::Qual::DefaultForMemberAccess,
                              ctx);
  }
  if (auto params = DC->getGenericParamsOfContext())
    return PolymorphicFunctionType::get(argType, resultType, params, ctx)
             ->getCanonicalType();
  return CanType(FunctionType::get(CanType(argType), resultType, ctx));
}

static AbstractCC addOwnerArgument(ASTContext &ctx, ValueDecl *value,
                                   CanType &resultType, unsigned &uncurryLevel) {
  DeclContext *DC = value->getDeclContext();
  switch (DC->getContextKind()) {
  case DeclContextKind::TranslationUnit:
  case DeclContextKind::BuiltinModule:
  case DeclContextKind::SerializedModule:
  case DeclContextKind::ClangModule:
  case DeclContextKind::CapturingExpr:
  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::ConstructorDecl:
  case DeclContextKind::DestructorDecl:
    return AbstractCC::Freestanding;

  case DeclContextKind::ExtensionDecl:
  case DeclContextKind::NominalTypeDecl:
    resultType = addOwnerArgument(ctx, DC, resultType);
    uncurryLevel++;
    return AbstractCC::Method;
  }
  llvm_unreachable("bad decl context");
}

/// Add the 'index' argument to a getter or setter.
static void addIndexArgument(ASTContext &Context, ValueDecl *value,
                             CanType &formalType, unsigned &uncurryLevel) {
  if (SubscriptDecl *sub = dyn_cast<SubscriptDecl>(value)) {
    formalType = FunctionType::get(sub->getIndices()->getType(),
                                   formalType, Context)->getCanonicalType();
    uncurryLevel++;
  }
}

static CanType getObjectType(ValueDecl *decl) {
  if (SubscriptDecl *sub = dyn_cast<SubscriptDecl>(decl))
    return sub->getElementType()->getCanonicalType();
  return decl->getType()->getCanonicalType();
}

/// getTypeOfGetter - Return the formal type of a getter for a
/// variable or subscripted object.
FormalType IRGenModule::getTypeOfGetter(ValueDecl *value) {
  // The formal type of a getter function is one of:
  //   S -> () -> T (for a nontype member)
  //   A -> S -> () -> T (for a type member)
  // where T is the value type of the object and S is the index type
  // (this clause is skipped for a non-subscript getter).
  unsigned uncurryLevel = 0;
  CanType formalType = CanType(FunctionType::get(TupleType::getEmpty(Context),
                                              getObjectType(value), Context));
  addIndexArgument(Context, value, formalType, uncurryLevel);
  AbstractCC cc = addOwnerArgument(Context, value, formalType, uncurryLevel);

  return FormalType(formalType, cc, uncurryLevel);
}

llvm::Function *IRGenModule::getAddrOfGetter(ValueDecl *value,
                                             ExplosionKind explosionLevel) {
  return getAddrOfGetter(value, getTypeOfGetter(value), explosionLevel);
}

/// getAddrOfGetter - Get the address of the function which performs a
/// get of a variable or subscripted object.
llvm::Function *IRGenModule::getAddrOfGetter(ValueDecl *value,
                                             FormalType formal,
                                             ExplosionKind explosionLevel) {
  LinkEntity entity =
    LinkEntity::forFunction(CodeRef::forGetter(value, explosionLevel, 0));

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return entry;

  llvm::AttributeSet attrs;
  auto convention = expandAbstractCC(*this, formal.getAbstractCC());
  llvm::FunctionType *fnType =
    getFunctionType(formal.getAbstractCC(),
                    formal.getType(), explosionLevel,
                    formal.getNaturalUncurryLevel(), ExtraData::None,
                    attrs);


  LinkInfo link = LinkInfo::get(*this, entity);
  entry = link.createFunction(*this, fnType, convention, attrs);
  return entry;
}

/// getTypeOfSetter - Return the formal type of a setter for a
/// variable or subscripted object.
FormalType IRGenModule::getTypeOfSetter(ValueDecl *value) {
  // The formal type of a setter function is one of:
  //   S -> T -> () (for a nontype member)
  //   A -> S -> T -> () (for a type member)
  // where T is the value type of the object and S is the index type
  // (this clause is skipped for a non-subscript setter).
  unsigned uncurryLevel = 0;
  CanType argType = getObjectType(value);
  CanType formalType = CanType(FunctionType::get(argType,
                                                 TupleType::getEmpty(Context),
                                                 Context));
  addIndexArgument(Context, value, formalType, uncurryLevel);
  auto cc = addOwnerArgument(Context, value, formalType, uncurryLevel);

  return FormalType(formalType, cc, uncurryLevel);
}

llvm::Function *IRGenModule::getAddrOfSetter(ValueDecl *value,
                                             ExplosionKind explosionLevel) {
  return getAddrOfSetter(value, getTypeOfSetter(value), explosionLevel);
}

/// getAddrOfSetter - Get the address of the function which performs a
/// set of a variable or subscripted object.
llvm::Function *IRGenModule::getAddrOfSetter(ValueDecl *value,
                                             FormalType formal,
                                             ExplosionKind explosionLevel) {
  LinkEntity entity =
    LinkEntity::forFunction(CodeRef::forSetter(value, explosionLevel, 0));

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return entry;

  llvm::AttributeSet attrs;
  llvm::FunctionType *fnType =
    getFunctionType(formal.getAbstractCC(),
                    formal.getType(), explosionLevel,
                    formal.getNaturalUncurryLevel(), ExtraData::None, attrs);

  auto convention = expandAbstractCC(*this, formal.getAbstractCC());

  LinkInfo link = LinkInfo::get(*this, entity);
  entry = link.createFunction(*this, fnType, convention, attrs);
  return entry;
}

static Address getAddrOfSimpleVariable(IRGenModule &IGM,
                    llvm::DenseMap<LinkEntity, llvm::GlobalVariable*> &cache,
                                       LinkEntity entity,
                                       llvm::Type *type,
                                       Alignment alignment) {
  // Check whether it's already cached.
  llvm::GlobalVariable *&entry = cache[entity];
  if (entry) {
    assert(alignment == Alignment(entry->getAlignment()));
    return Address(entry, alignment);
  }

  // Otherwise, we need to create it.
  LinkInfo link = LinkInfo::get(IGM, entity);
  auto addr = link.createVariable(IGM, type);
  addr->setConstant(true);

  addr->setAlignment(alignment.getValue());

  entry = addr;
  return Address(addr, alignment);
}

/// getAddrOfWitnessTableOffset - Get the address of the global
/// variable which contains an offset within a witness table for the
/// value associated with the given function.
Address IRGenModule::getAddrOfWitnessTableOffset(CodeRef code) {
  LinkEntity entity =
    LinkEntity::forWitnessTableOffset(code.getDecl(), code.getExplosionLevel(),
                                      code.getUncurryLevel());
  return getAddrOfSimpleVariable(*this, GlobalVars, entity,
                                 SizeTy, getPointerAlignment());
}

/// getAddrOfWitnessTableOffset - Get the address of the global
/// variable which contains an offset within a witness table for the
/// value associated with the given member variable..
Address IRGenModule::getAddrOfWitnessTableOffset(VarDecl *field) {
  LinkEntity entity =
    LinkEntity::forWitnessTableOffset(field, ExplosionKind::Minimal, 0);
  return ::getAddrOfSimpleVariable(*this, GlobalVars, entity,
                                   SizeTy, getPointerAlignment());
}

/// getAddrOfFieldOffset - Get the address of the global variable
/// which contains an offset to apply to either an object (if direct)
/// or a metadata object in order to find an offset to apply to an
/// object (if indirect).
///
/// The result is always a GlobalVariable.
Address IRGenModule::getAddrOfFieldOffset(VarDecl *var, bool isIndirect) {
  LinkEntity entity = LinkEntity::forFieldOffset(var, isIndirect);
  return getAddrOfSimpleVariable(*this, GlobalVars, entity,
                                 SizeTy, getPointerAlignment());
}

static bool protocolExtensionRequiresCategory(ProtocolDecl *protocol,
                                            ProtocolConformance *conformance) {
  if (protocol->isObjC())
    return true;
  for (auto &inherited : conformance->getInheritedConformances())
    if (protocolExtensionRequiresCategory(inherited.first, inherited.second))
      return true;
  return false;
}

/// Emit a type extension.
void IRGenModule::emitExtension(ExtensionDecl *ext) {
  for (Decl *member : ext->getMembers()) {
    switch (member->getKind()) {
    case DeclKind::Import:
    case DeclKind::UnionElement:
    case DeclKind::TopLevelCode:
    case DeclKind::Protocol:
    case DeclKind::Extension:
    case DeclKind::Destructor:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
      llvm_unreachable("decl not allowed in extension!");

    // PatternBindingDecls don't really make sense here, but we
    // produce one as a side-effect of parsing a var property.
    // Just ignore it.
    case DeclKind::PatternBinding:
      continue;

    case DeclKind::Subscript:
      // Getter/setter will be handled separately.
      continue;
    case DeclKind::TypeAlias:
      continue;
    case DeclKind::Union:
      emitUnionDecl(cast<UnionDecl>(member));
      continue;
    case DeclKind::Struct:
      emitStructDecl(cast<StructDecl>(member));
      continue;
    case DeclKind::Class:
      emitClassDecl(cast<ClassDecl>(member));
      continue;
    case DeclKind::Var:
      if (cast<VarDecl>(member)->isProperty())
        // Getter/setter will be handled separately.
        continue;
      llvm_unreachable("decl not allowed in extension!");
    case DeclKind::Func:
      emitLocalDecls(cast<FuncDecl>(member));
      continue;
    case DeclKind::Constructor:
      emitLocalDecls(cast<ConstructorDecl>(member));
      continue;
    }
    llvm_unreachable("bad extension member kind");
  }
  
  // If the original class is ObjC, or the extension introduces a conformance to
  // an ObjC protocol, generate a category.
  ClassDecl *origClass = ext->getDeclaredTypeInContext()
    ->getClassOrBoundGenericClass();
  if (!origClass)
    return;
  bool needsCategory = origClass->isObjC();
  if (!needsCategory) {
    for (unsigned i = 0, size = ext->getProtocols().size(); i < size; ++i)
      if (protocolExtensionRequiresCategory(ext->getProtocols()[i],
                                            ext->getConformances()[i])) {
        needsCategory = true;
        break;
      }
  }
  
  if (needsCategory) {
    llvm::Constant *category = emitCategoryData(*this, ext);
    category = llvm::ConstantExpr::getBitCast(category, Int8PtrTy);
    ObjCCategories.push_back(category);
    ObjCCategoryDecls.push_back(ext);
  }
}


/// Create an allocation on the stack.
Address IRGenFunction::createAlloca(llvm::Type *type,
                                    Alignment alignment,
                                    const llvm::Twine &name) {
  llvm::AllocaInst *alloca = new llvm::AllocaInst(type, name, AllocaIP);
  alloca->setAlignment(alignment.getValue());
  return Address(alloca, alignment);
}

/// Get or create a global string constant.
///
/// \returns an i8* with a null terminator; note that embedded nulls
///   are okay
llvm::Constant *IRGenModule::getAddrOfGlobalString(llvm::StringRef data) {
  // Check whether this string already exists.
  auto &entry = GlobalStrings[data];
  if (entry) return entry;

  // If not, create it.  This implicitly adds a trailing null.
  auto init = llvm::ConstantDataArray::getString(LLVMContext, data);
  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init);
  global->setUnnamedAddr(true);

  // Drill down to make an i8*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(global, indices);

  // Cache and return.
  entry = address;
  return address;
}

/// Mangle the name of a type.
StringRef IRGenModule::mangleType(CanType type, SmallVectorImpl<char> &buffer) {
  LinkEntity::forTypeMangling(type).mangle(buffer);
  return StringRef(buffer.data(), buffer.size());
}

/// Is the given declaration resilient?
bool IRGenModule::isResilient(Decl *theDecl, ResilienceScope scope) {
  // Classes defined by Clang are resilient.
  if (auto theClass = dyn_cast<ClassDecl>(theDecl)) {
    return theClass->hasClangNode();
  }

  return false;
}
