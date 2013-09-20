#include "SILGen.h"
#include "OwnershipConventions.h"
#include "swift/Basic/Fallthrough.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclObjC.h"

using namespace swift;
using namespace Lowering;

/// Apply a macro FAMILY(Name, Prefix) to all ObjC selector families.
#define FOREACH_FAMILY(FAMILY)       \
  FAMILY(Alloc, "alloc")             \
  FAMILY(Copy, "copy")               \
  FAMILY(Init, "init")               \
  FAMILY(MutableCopy, "mutableCopy") \
  FAMILY(New, "new")

enum class swift::Lowering::SelectorFamily : unsigned {
  None,
#define GET_LABEL(LABEL, PREFIX) LABEL,
  FOREACH_FAMILY(GET_LABEL)
#undef GET_LABEL
};

/// Derive the ObjC selector family from an identifier.
static SelectorFamily getSelectorFamily(Identifier name) {
  StringRef text = name.get();
  while (!text.empty() && text[0] == '_') text = text.substr(1);

  /// Does the given selector start with the given string as a
  /// prefix, in the sense of the selector naming conventions?
  auto hasPrefix = [](StringRef text, StringRef prefix) {
    if (!text.startswith(prefix)) return false;
    if (text.size() == prefix.size()) return true;
    assert(text.size() > prefix.size());
    return !islower(text[prefix.size()]);
  };

  #define CHECK_PREFIX(LABEL, PREFIX) \
    if (hasPrefix(text, PREFIX)) return SelectorFamily::LABEL;
  
  FOREACH_FAMILY(CHECK_PREFIX)

  #undef CHECK_PREFIX

  return SelectorFamily::None;
}
  
/// Get the ObjC selector family a SILDeclRef implicitly belongs to.
static SelectorFamily getSelectorFamily(SILDeclRef c) {
  switch (c.kind) {
  case SILDeclRef::Kind::Func:
    return getSelectorFamily(c.getDecl()->getName());

  case SILDeclRef::Kind::Initializer:
    return SelectorFamily::Init;

  case SILDeclRef::Kind::Getter:
    // Getter selectors can belong to families if their name begins with the
    // wrong thing.
    if (c.getDecl()->isObjC() || c.isObjC)
      return getSelectorFamily(c.getDecl()->getName());
    
    SWIFT_FALLTHROUGH;

  // Setter selectors shouldn't belong to any family we care about.
  case SILDeclRef::Kind::Setter:
  /// Currently IRGen wraps alloc/init methods into Swift constructors
  /// with Swift conventions.
  case SILDeclRef::Kind::Allocator:
  /// These constants don't correspond to method families we care about yet.
  case SILDeclRef::Kind::EnumElement:
  case SILDeclRef::Kind::Destroyer:
  case SILDeclRef::Kind::GlobalAccessor:
  case SILDeclRef::Kind::DefaultArgGenerator:
    return SelectorFamily::None;
  }
}

/// Try to find a clang method declaration for the given function.
static const clang::Decl *findClangMethod(ValueDecl *method) {
  if (FuncDecl *methodFn = dyn_cast<FuncDecl>(method)) {
    if (auto *decl = methodFn->getClangDecl())
      return decl;
    
    if (auto overridden = methodFn->getOverriddenDecl())
      return findClangMethod(overridden);
  }

  if (ConstructorDecl *constructor = dyn_cast<ConstructorDecl>(method)) {
    if (auto *decl = constructor->getClangDecl())
      return decl;
  }
  
  return nullptr;
}

OwnershipConventions OwnershipConventions::get(SILGenFunction &gen,
                                               SILDeclRef c,
                                               SILType ty) {
  // Native functions use the default Swift convention.
  if (!c.isObjC)
    return getDefault(gen, ty);
  
  SILFunctionTypeInfo *ft = ty.getFunctionTypeInfo(gen.SGM.M);
  
  // If we have a clang decl associated with the Swift decl, derive its
  // ownership conventions.
  // FIXME: When we support calling ObjC blocks, we'll need to handle anonymous
  // SILDeclRefs here too.
  if (auto *decl = c.loc.dyn_cast<ValueDecl*>())
    if (auto *clangDecl = findClangMethod(decl))
      return getForClangDecl(clangDecl, ft);
  
  // If the decl belongs to an ObjC method family, use that family's ownership
  // conventions.
  return getForObjCSelectorFamily(getSelectorFamily(c), ft);
}

OwnershipConventions OwnershipConventions::getDefault(SILGenFunction &gen,
                                                      SILType ty) {
  SILFunctionTypeInfo *ft = ty.getFunctionTypeInfo(gen.SGM.M);
  size_t inputTypeCount = ft->getInputTypes().size();
  return {
    /*calleeConsumed*/ true,
    /*consumedArguments*/ llvm::SmallBitVector(inputTypeCount, true),
    /*returnKind*/ Return::Retained
  };
}

static OwnershipConventions::Return getReturnKind(const clang::Decl *clangDecl,
                                                  clang::QualType resultType) {
  // If the result type is an ObjC pointer, consult the decl attributes (if any)
  if (resultType->isObjCRetainableType()) {
    if (clangDecl->hasAttr<clang::NSReturnsRetainedAttr>())
      return OwnershipConventions::Return::Retained;
    else if (clangDecl->hasAttr<clang::NSReturnsNotRetainedAttr>())
      return OwnershipConventions::Return::Unretained;
    else
      return OwnershipConventions::Return::Autoreleased;
  }
  
  // Non-ObjC value types are always returned unretained in ObjC.
  return OwnershipConventions::Return::Unretained;
}

template<typename PARAM_RANGE>
static void getConsumedArgs(PARAM_RANGE params,
                            llvm::SmallBitVector &consumedArgs) {
  // FIXME: This assumes a 1:1 correspondence of SIL arguments to ObjC
  // arguments, which won't be true if we ever map tuples to foreign types.
  unsigned paramIndex = 0;
  for (const clang::ParmVarDecl *param : params) {
    if (param->hasAttr<clang::NSConsumedAttr>())
      consumedArgs.set(paramIndex);
    ++paramIndex;
  }
}

OwnershipConventions
OwnershipConventions::getForClangDecl(const clang::Decl *clangDecl,
                                      SILFunctionTypeInfo *ft) {
  size_t inputTypeCount = ft->getInputTypes().size();
  llvm::SmallBitVector consumedArgs(inputTypeCount, false);
  // FIXME: We don't support calling ObjC blocks yet. For now assume the callee
  // is always consumed.
  bool calleeConsumed = true;
  Return returnKind = Return::Unretained;
  
  if (auto *method = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
    // Determine the return kind.
    returnKind = getReturnKind(clangDecl, method->getResultType());
    
    // Check if the method consumes self.
    if (method->hasAttr<clang::NSConsumesSelfAttr>())
      consumedArgs.set(consumedArgs.size() - 1);
    
    // Check if the method consumes other arguments.
    getConsumedArgs(make_range(method->param_begin(), method->param_end()),
                    consumedArgs);
  } else if (auto *func = dyn_cast<clang::FunctionDecl>(clangDecl)) {
    // Determine the return kind.
    returnKind = getReturnKind(clangDecl, func->getResultType());
    
    // Check if the method consumes any arguments.
    getConsumedArgs(make_range(func->param_begin(), func->param_end()),
                    consumedArgs);
  }
  
  return {
    calleeConsumed,
    std::move(consumedArgs),
    returnKind
  };
}

OwnershipConventions
OwnershipConventions::getForObjCSelectorFamily(SelectorFamily family,
                                               SILFunctionTypeInfo *ft) {
  size_t inputTypeCount = ft->getInputTypes().size();
  llvm::SmallBitVector consumedArgs(inputTypeCount, false);
  // FIXME: We don't support calling ObjC blocks yet. For now assume the callee
  // is always consumed.
  bool calleeConsumed = true;
  Return returnKind = ft->getResultType().hasReferenceSemantics()
    ? Return::Autoreleased
    : Return::Unretained;
  
  switch (family) {
      // Init consumes self and returns a retained value.
    case SelectorFamily::Init:
      consumedArgs.set(consumedArgs.size() - 1);
      SWIFT_FALLTHROUGH;
      
      // These families all return a retained value.
    case SelectorFamily::Alloc:
    case SelectorFamily::Copy:
    case SelectorFamily::MutableCopy:
    case SelectorFamily::New:
      returnKind = Return::Retained;
      break;
      
      // Normal ObjC methods consume nothing and return autoreleased.
    case SelectorFamily::None:
      break;
  }
  
  return {
    calleeConsumed,
    std::move(consumedArgs),
    returnKind
  };
}
