//===--- ClangImporter.cpp - Import Clang Modules -------------------------===//
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
// This file implements support for loading Clang modules into Swift.
//
//===----------------------------------------------------------------------===//
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "ImporterImpl.h"
#include "ClangDiagnosticConsumer.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/ClangImporterOptions.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Version.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/Parser.h"
#include "clang/Rewrite/Frontend/FrontendActions.h"
#include "clang/Rewrite/Frontend/Rewriters.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <memory>

using namespace swift;

//===--------------------------------------------------------------------===//
// Importer statistics
//===--------------------------------------------------------------------===//
#define DEBUG_TYPE "Clang module importer"
STATISTIC(NumNullaryMethodNames,
          "nullary selectors imported");
STATISTIC(NumUnaryMethodNames,
          "unary selectors imported");
STATISTIC(NumNullaryInitMethodsMadeUnary,
          "nullary Objective-C init methods turned into unary initializers");
STATISTIC(NumMultiMethodNames,
          "multi-part selector method names imported");
STATISTIC(NumPrepositionSplitMethodNames,
          "selectors where the first selector piece was split on a "
          "preposition");
STATISTIC(NumLinkingVerbNonSplits,
          "selectors where splitting was prevented by a linking verb ");
STATISTIC(NumLinkingVerbNonSplitsPreposition,
          "selectors where preposition splitting was prevented by a linking "
          "verb ");
STATISTIC(NumPrepositionTrailingFirstPiece,
          "selectors where the first piece ends in a preposition");
STATISTIC(NumSelectorsNotSplit,
          "selectors that were not split (for any reason)");
STATISTIC(NumMethodsMissingFirstArgName,
          "selectors where the first argument name is missing");
STATISTIC(NumFactoryMethodsNullary,
          "# of factory methods not mapped due to nullary with long name");
STATISTIC(NumInitsDroppedWith,
          "# of initializer selectors from which \"with\" was dropped");
STATISTIC(NumInitsPrepositionSplit,
          "# of initializer selectors where the split was on a preposition");
STATISTIC(NumInitsNonPrepositionSplit,
          "# of initializer selectors where the split wasn't on a preposition");

// Commonly-used Clang classes.
using clang::CompilerInstance;
using clang::CompilerInvocation;

#pragma mark Internal data structures

namespace {
  class HeaderImportCallbacks : public clang::PPCallbacks {
    ClangImporter &Importer;
    ClangImporter::Implementation &Impl;
  public:
    HeaderImportCallbacks(ClangImporter &importer,
                          ClangImporter::Implementation &impl)
      : Importer(importer), Impl(impl) {}

    void handleImport(const clang::Module *imported) {
      if (!imported)
        return;
      Module *nativeImported = Impl.finishLoadingClangModule(Importer, imported,
                                                             /*adapter=*/true);
      Impl.ImportedHeaderExports.push_back({ /*filter=*/{}, nativeImported });
    }

    virtual void InclusionDirective(clang::SourceLocation HashLoc,
                                    const clang::Token &IncludeTok,
                                    StringRef FileName,
                                    bool IsAngled,
                                    clang::CharSourceRange FilenameRange,
                                    const clang::FileEntry *File,
                                    StringRef SearchPath,
                                    StringRef RelativePath,
                                    const clang::Module *Imported) override {
      handleImport(Imported);
      if (!Imported && File)
        Importer.addDependency(File->getName());
    }

    virtual void moduleImport(clang::SourceLocation ImportLoc,
                              clang::ModuleIdPath Path,
                              const clang::Module *Imported) override {
      handleImport(Imported);
    }
  };

  class ASTReaderCallbacks : public clang::ASTReaderListener {
    ClangImporter &Importer;
  public:
    explicit ASTReaderCallbacks(ClangImporter &importer) : Importer(importer) {}

    bool needsInputFileVisitation() override { return true; }

    bool visitInputFile(StringRef file, bool isSystem,
                        bool isOverridden) override {
      if (!isOverridden)
        Importer.addDependency(file);
      return true;
    }
  };

  class StdStringMemBuffer : public llvm::MemoryBuffer {
    const std::string storage;
    const std::string name;
  public:
    StdStringMemBuffer(std::string &&source, StringRef name)
        : storage(std::move(source)), name(name.str()) {
      init(storage.data(), storage.data() + storage.size(),
           /*null-terminated=*/true);
    }

    const char *getBufferIdentifier() const override {
      return name.c_str();
    }

    BufferKind getBufferKind() const override {
      return MemoryBuffer_Malloc;
    }
  };
}

ClangImporter::ClangImporter(ASTContext &ctx,
                             const ClangImporterOptions &clangImporterOpts,
                             const llvm::Triple &triple,
                             DependencyTracker *tracker)
  : ClangModuleLoader(tracker),
    Impl(*new Implementation(ctx, clangImporterOpts, triple))
{
}

ClangImporter::~ClangImporter() {
  delete &Impl;
}

void ClangImporter::setTypeResolver(LazyResolver &resolver) {
  Impl.setTypeResolver(&resolver);
}

void ClangImporter::clearTypeResolver() {
  Impl.setTypeResolver(nullptr);
}

#pragma mark Module loading

#ifdef NDEBUG
#define SHIMS_INCLUDE_FLAG "-isystem"
#else
#define SHIMS_INCLUDE_FLAG "-I"
#endif

std::unique_ptr<ClangImporter>
ClangImporter::create(ASTContext &ctx,
                      const ClangImporterOptions &importerOpts,
                      const IRGenOptions &irGenOpts,
                      DependencyTracker *tracker) {

  llvm::Triple triple(irGenOpts.Triple);

  std::unique_ptr<ClangImporter> importer{
    new ClangImporter(ctx, importerOpts, triple, tracker)
  };

  SearchPathOptions &searchPathOpts = ctx.SearchPathOpts;

  // Construct the invocation arguments for Objective-C ARC with the current
  // target.
  std::vector<std::string> invocationArgStrs = {
    "-x", "objective-c", "-std=gnu11", "-fobjc-arc", "-fmodules", "-fblocks",
    "-fsyntax-only", "-w",
    "-triple", irGenOpts.Triple, "-target-cpu", irGenOpts.TargetCPU,
    "-target-abi", irGenOpts.TargetABI,
    SHIMS_INCLUDE_FLAG, searchPathOpts.RuntimeResourcePath,
    "-DSWIFT_CLASS_EXTRA=__attribute__((annotate(\""
      SWIFT_NATIVE_ANNOTATION_STRING "\")))",
    "-DSWIFT_PROTOCOL_EXTRA=__attribute__((annotate(\""
      SWIFT_NATIVE_ANNOTATION_STRING "\")))",
    "-DSWIFT_EXTENSION_EXTRA=__attribute__((annotate(\""
      SWIFT_NATIVE_ANNOTATION_STRING "\")))",
    "-DCF_ENABLE_BRIDGED_TYPES_SO_YOU_CAN_FIX_BUILD_FAILURES",
    "-DCF_ENABLE_BRIDGED_TYPES",
    "-D_ISO646_H_", "-D__ISO646_H",
    "-fretain-comments-from-system-headers",
    "-fmodules-validate-system-headers",
    "-Werror=non-modular-include-in-framework-module",
    "<swift-imported-modules>"
  };

  std::string runtimeArgBuf{"-fobjc-runtime="};
  llvm::raw_string_ostream runtimeArg{runtimeArgBuf};
  unsigned major, minor, micro;
  if (triple.isiOS()) {
    runtimeArg << "ios";
    triple.getiOSVersion(major, minor, micro);
  } else {
    assert(triple.isMacOSX());
    runtimeArg << "macosx";
    triple.getMacOSXVersion(major, minor, micro);
  }
  runtimeArg << '-' << clang::VersionTuple(major, minor, micro);
  invocationArgStrs.push_back(std::move(runtimeArg.str()));

  if (ctx.LangOpts.EnableAppExtensionRestrictions) {
    invocationArgStrs.push_back("-fapplication-extension");
  }

  if (searchPathOpts.SDKPath.empty()) {
    invocationArgStrs.push_back("-nostdsysteminc");
  } else {
    invocationArgStrs.push_back("-isysroot");
    invocationArgStrs.push_back(searchPathOpts.SDKPath);
  }

  for (auto path : searchPathOpts.ImportSearchPaths) {
    invocationArgStrs.push_back("-I");
    invocationArgStrs.push_back(path);
  }

  for (auto path : searchPathOpts.FrameworkSearchPaths) {
    invocationArgStrs.push_back("-F");
    invocationArgStrs.push_back(path);
  }

  const std::string &moduleCachePath = importerOpts.ModuleCachePath;

  // Set the module cache path.
  if (moduleCachePath.empty()) {
    llvm::SmallString<128> DefaultModuleCache;
    llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/false,
                                           DefaultModuleCache);
    llvm::sys::path::append(DefaultModuleCache, "org.llvm.clang");
    llvm::sys::path::append(DefaultModuleCache, "ModuleCache");
    invocationArgStrs.push_back("-fmodules-cache-path=");
    invocationArgStrs.back().append(DefaultModuleCache.str());
  } else {
    invocationArgStrs.push_back("-fmodules-cache-path=");
    invocationArgStrs.back().append(moduleCachePath);
  }

  const std::string &overrideResourceDir = importerOpts.OverrideResourceDir;

  if (overrideResourceDir.empty()) {
    llvm::SmallString<128> resourceDir(searchPathOpts.RuntimeResourcePath);
  
    // Adjust the path torefer to our copy of the Clang headers under
    // lib/swift/clang.
  
    llvm::sys::path::append(resourceDir, "clang", CLANG_VERSION_STRING);
  
    // Set the Clang resource directory to the path we computed.
    invocationArgStrs.push_back("-resource-dir");
    invocationArgStrs.push_back(resourceDir.str());
  } else {
    invocationArgStrs.push_back("-resource-dir");
    invocationArgStrs.push_back(overrideResourceDir);
  }

  for (auto &feature : irGenOpts.TargetFeatures) {
    invocationArgStrs.push_back("-target-feature");
    invocationArgStrs.push_back(feature);
  }

  for (auto extraArg : importerOpts.ExtraArgs) {
    invocationArgStrs.push_back(extraArg);
  }

  std::vector<const char *> invocationArgs;
  for (auto &argStr : invocationArgStrs)
    invocationArgs.push_back(argStr.c_str());

  // Create a new Clang compiler invocation.
  llvm::IntrusiveRefCntPtr<CompilerInvocation> invocation{
    new CompilerInvocation
  };
  std::unique_ptr<ClangDiagnosticConsumer> diagClient{
    new ClangDiagnosticConsumer(importer->Impl, invocation->getDiagnosticOpts(),
                                importerOpts.DumpClangDiagnostics)
  };
  auto clangDiags =
    CompilerInstance::createDiagnostics(&invocation->getDiagnosticOpts(),
                                        diagClient.release());

  // Don't stop emitting messages if we ever can't load a module.
  // FIXME: This is actually a general problem: any "fatal" error could mess up
  // the CompilerInvocation.
  clangDiags->setSeverity(clang::diag::err_module_not_found,
                          clang::diag::Severity::Error,
                          clang::SourceLocation());
  clangDiags->setSeverity(clang::diag::err_module_not_built,
                          clang::diag::Severity::Error,
                          clang::SourceLocation());

  if (!CompilerInvocation::CreateFromArgs(*invocation,
                                          invocationArgs.data(),
                                          (invocationArgs.data() +
                                           invocationArgs.size()),
                                          *clangDiags))
    return nullptr;
  importer->Impl.Invocation = invocation;

  // Create an almost-empty memory buffer.
  auto sourceBuffer = llvm::MemoryBuffer::getMemBuffer(
    "extern int __swift __attribute__((unavailable));");
  invocation->getPreprocessorOpts().addRemappedFile("<swift-imported-modules>",
                                                    sourceBuffer);

  // Create a compiler instance.
  importer->Impl.Instance.reset(new CompilerInstance);
  auto &instance = *importer->Impl.Instance;
  instance.setDiagnostics(&*clangDiags);
  instance.setInvocation(&*invocation);

  // Create the associated action.
  importer->Impl.Action.reset(new clang::SyntaxOnlyAction);
  auto *action = importer->Impl.Action.get();

  // Execute the action. We effectively inline most of
  // CompilerInstance::ExecuteAction here, because we need to leave the AST
  // open for future module loading.
  // FIXME: This has to be cleaned up on the Clang side before we can improve
  // things here.

  // Create the target instance.
  instance.setTarget(
    clang::TargetInfo::CreateTargetInfo(*clangDiags,
                                        instance.getInvocation().TargetOpts));
  if (!instance.hasTarget())
    return nullptr;

  // Inform the target of the language options.
  //
  // FIXME: We shouldn't need to do this, the target should be immutable once
  // created. This complexity should be lifted elsewhere.
  instance.getTarget().adjust(instance.getLangOpts());

  bool canBegin = action->BeginSourceFile(instance,
                                          instance.getFrontendOpts().Inputs[0]);
  assert(canBegin);

  clang::Preprocessor &clangPP = instance.getPreprocessor();
  clangPP.enableIncrementalProcessing();
  clangPP.addPPCallbacks(new HeaderImportCallbacks(*importer, importer->Impl));

  instance.createModuleManager();
  instance.getModuleManager()->addListener(new ASTReaderCallbacks(*importer));

  // Manually run the action, so that the TU stays open for additional parsing.
  instance.createSema(action->getTranslationUnitKind(), nullptr);
  importer->Impl.Parser.reset(new clang::Parser(clangPP, instance.getSema(),
                                                /*skipFunctionBodies=*/false));

  clangPP.EnterMainSourceFile();
  importer->Impl.Parser->Initialize();

  clang::Parser::DeclGroupPtrTy parsed;
  while (!importer->Impl.Parser->ParseTopLevelDecl(parsed)) {}

  // Create the selectors we'll be looking for.
  auto &clangContext = importer->Impl.Instance->getASTContext();
  importer->Impl.objectAtIndexedSubscript
    = clangContext.Selectors.getUnarySelector(
        &clangContext.Idents.get("objectAtIndexedSubscript"));
  clang::IdentifierInfo *setObjectAtIndexedSubscriptIdents[2] = {
    &clangContext.Idents.get("setObject"),
    &clangContext.Idents.get("atIndexedSubscript")
  };
  importer->Impl.setObjectAtIndexedSubscript
    = clangContext.Selectors.getSelector(2, setObjectAtIndexedSubscriptIdents);
  importer->Impl.objectForKeyedSubscript
    = clangContext.Selectors.getUnarySelector(
        &clangContext.Idents.get("objectForKeyedSubscript"));
  clang::IdentifierInfo *setObjectForKeyedSubscriptIdents[2] = {
    &clangContext.Idents.get("setObject"),
    &clangContext.Idents.get("forKeyedSubscript")
  };
  importer->Impl.setObjectForKeyedSubscript
    = clangContext.Selectors.getSelector(2, setObjectForKeyedSubscriptIdents);

  // Set up the imported header module.
  auto *importedHeaderModule = Module::create(ctx.getIdentifier("__ObjC"), ctx);
  importer->Impl.ImportedHeaderUnit =
    new (ctx) ClangModuleUnit(*importedHeaderModule, *importer, nullptr);
  importedHeaderModule->addFile(*importer->Impl.ImportedHeaderUnit);

  return importer;
}

void ClangImporter::Implementation::importHeader(
    Module *adapter, StringRef headerName, SourceLoc diagLoc,
    std::unique_ptr<llvm::MemoryBuffer> sourceBuffer) {
  assert(adapter);
  ImportedHeaderOwners.push_back(adapter);

  bool hadError = getClangASTContext().getDiagnostics().hasErrorOccurred();

  clang::Preprocessor &pp = getClangPreprocessor();
  clang::SourceManager &sourceMgr = getClangASTContext().getSourceManager();

  clang::SourceLocation includeLoc =
    sourceMgr.getLocForStartOfFile(sourceMgr.getMainFileID());
  clang::FileID bufferID = sourceMgr.createFileID(sourceBuffer.release(),
                                                  clang::SrcMgr::C_User,
                                                  /*LoadedID=*/0,
                                                  /*LoadedOffset=*/0,
                                                  includeLoc);

  pp.EnterSourceFile(bufferID, /*directoryLookup=*/nullptr, /*loc=*/{});
  // Force the import to occur.
  pp.LookAhead(0);

  clang::Parser::DeclGroupPtrTy parsed;
  while (!Parser->ParseTopLevelDecl(parsed)) {}
  pp.EndSourceFile();

  if (!hadError && getClangASTContext().getDiagnostics().hasErrorOccurred()) {
    SwiftContext.Diags.diagnose(diagLoc, diag::bridging_header_error,
                                headerName);
  }

  bumpGeneration();
}

void ClangImporter::importHeader(StringRef header, Module *adapter,
                                 off_t expectedSize, time_t expectedModTime,
                                 StringRef cachedContents, SourceLoc diagLoc) {
  clang::FileManager &fileManager = Impl.Instance->getFileManager();
  const clang::FileEntry *headerFile = fileManager.getFile(header,
                                                           /*open=*/true);
  if (headerFile && headerFile->getSize() == expectedSize &&
      headerFile->getModificationTime() == expectedModTime) {
    return importBridgingHeader(header, adapter, diagLoc);
  }

  std::unique_ptr<llvm::MemoryBuffer> sourceBuffer{
    llvm::MemoryBuffer::getMemBuffer(cachedContents, header)
  };
  Impl.importHeader(adapter, header, diagLoc, std::move(sourceBuffer));
}

void ClangImporter::importBridgingHeader(StringRef header, Module *adapter,
                                         SourceLoc diagLoc) {
  clang::FileManager &fileManager = Impl.Instance->getFileManager();
  const clang::FileEntry *headerFile = fileManager.getFile(header,
                                                           /*open=*/true);
  if (!headerFile) {
    Impl.SwiftContext.Diags.diagnose(diagLoc, diag::bridging_header_missing,
                                     header);
    return;
  }

  llvm::SmallString<128> importLine{"#import \""};
  importLine += header;
  importLine += "\"\n";

  std::unique_ptr<llvm::MemoryBuffer> sourceBuffer{
    llvm::MemoryBuffer::getMemBufferCopy(
      importLine, Implementation::bridgingHeaderBufferName)
  };

  Impl.importHeader(adapter, header, diagLoc, std::move(sourceBuffer));
}

std::string ClangImporter::getBridgingHeaderContents(StringRef headerPath,
                                                     off_t &fileSize,
                                                     time_t &fileModTime) {
  llvm::IntrusiveRefCntPtr<clang::CompilerInvocation> invocation{
    new clang::CompilerInvocation(*Impl.Invocation)
  };
  invocation->getFrontendOpts().DisableFree = false;
  invocation->getFrontendOpts().Inputs.clear();
  invocation->getFrontendOpts().Inputs.push_back(
      clang::FrontendInputFile(headerPath, clang::IK_ObjC));

  invocation->getPreprocessorOpts().resetNonModularOptions();

  clang::CompilerInstance rewriteInstance;
  rewriteInstance.setInvocation(&*invocation);
  rewriteInstance.createDiagnostics(new clang::IgnoringDiagConsumer);

  clang::FileManager &fileManager = Impl.Instance->getFileManager();
  rewriteInstance.setFileManager(&fileManager);
  rewriteInstance.createSourceManager(fileManager);
  rewriteInstance.setTarget(&Impl.Instance->getTarget());

  std::string result;
  bool success = llvm::CrashRecoveryContext().RunSafelyOnThread([&] {
    clang::RewriteIncludesAction action;
    action.BeginSourceFile(rewriteInstance,
                           invocation->getFrontendOpts().Inputs.front());
    llvm::raw_string_ostream os(result);
    clang::RewriteIncludesInInput(rewriteInstance.getPreprocessor(), &os,
                                  rewriteInstance.getPreprocessorOutputOpts());
    action.EndSourceFile();
  });

  success |= !rewriteInstance.getDiagnostics().hasErrorOccurred();
  if (!success) {
    Impl.SwiftContext.Diags.diagnose({},
                                     diag::could_not_rewrite_bridging_header);
    return "";
  }

  const clang::FileEntry *fileInfo = fileManager.getFile(headerPath);
  fileSize = fileInfo->getSize();
  fileModTime = fileInfo->getModificationTime();
  return result;
}

Module *ClangImporter::loadModule(
    SourceLoc importLoc,
    ArrayRef<std::pair<Identifier, SourceLoc>> path) {

  // Convert the Swift import path over to a Clang import path.
  // FIXME: Map source locations over. Fun, fun!
  SmallVector<std::pair<clang::IdentifierInfo *, clang::SourceLocation>, 4>
    clangPath;

  auto &clangContext = Impl.Instance->getASTContext();
  for (auto component : path) {
    clangPath.push_back({&clangContext.Idents.get(component.first.str()),
                         clang::SourceLocation()} );
  }

  clang::ModuleLoadResult clangModule;
  {
    auto &rawDiagClient = Impl.Instance->getDiagnosticClient();
    auto &diagClient = static_cast<ClangDiagnosticConsumer &>(rawDiagClient);
    auto importRAII = diagClient.handleImport(clangPath.front().first,
                                              importLoc);

    // Load the Clang module.
    // FIXME: The source location here is completely bogus. It can't be
    // invalid, and it can't be the same thing twice in a row, so we just use
    // a counter. Having real source locations would be far, far better.
    auto &srcMgr = clangContext.getSourceManager();
    clang::SourceLocation clangImportLoc
      = srcMgr.getLocForStartOfFile(srcMgr.getMainFileID())
              .getLocWithOffset(Impl.ImportCounter++);
    clangModule = Impl.Instance->loadModule(clangImportLoc,
                                            clangPath,
                                            clang::Module::AllVisible,
                                            /*IsInclusionDirective=*/false);

    if (!clangModule) {
      if (diagClient.isCurrentImportMissing())
        return nullptr;

      // Otherwise, the module is present, but we've failed to load it for some
      // reason. Create an empty module to serve as a placeholder.
      Identifier name = path.back().first;
      return Module::create(name, Impl.SwiftContext);
    }
  }

  return Impl.finishLoadingClangModule(*this, clangModule, /*adapter=*/false);
}

Module *ClangImporter::Implementation::finishLoadingClangModule(
    ClangImporter &owner,
    const clang::Module *clangModule,
    bool findAdapter) {
  assert(clangModule);

  // Bump the generation count.
  bumpGeneration();

  auto &cacheEntry = ModuleWrappers[clangModule];
  Module *result;
  ClangModuleUnit *wrapperUnit;
  if ((wrapperUnit = cacheEntry.getPointer())) {
    result = wrapperUnit->getParentModule();
    if (!cacheEntry.getInt()) {
      // Force load adapter modules for all imported modules.
      // FIXME: This forces the creation of wrapper modules for all imports as
      // well, and may do unnecessary work.
      cacheEntry.setInt(true);
      result->forAllVisibleModules({}, [&](Module::ImportedModule import) {});
    }
  } else {
    // Build the representation of the Clang module in Swift.
    // FIXME: The name of this module could end up as a key in the ASTContext,
    // but that's not correct for submodules.
    Identifier name = SwiftContext.getIdentifier((*clangModule).Name);
    result = Module::create(name, SwiftContext);

    wrapperUnit =
      new (SwiftContext) ClangModuleUnit(*result, owner, clangModule);
    result->addFile(*wrapperUnit);
    cacheEntry.setPointerAndInt(wrapperUnit, true);

    // Force load adapter modules for all imported modules.
    // FIXME: This forces the creation of wrapper modules for all imports as
    // well, and may do unnecessary work.
    result->forAllVisibleModules({}, [](Module::ImportedModule import) {});
  }

  // Try to load the API notes for this module.
  (void)getAPINotesForModule(clangModule->getTopLevelModule());

  if (clangModule->isSubModule()) {
    finishLoadingClangModule(owner, clangModule->getTopLevelModule(), true);
  } else {
    Module *&loaded = SwiftContext.LoadedModules[result->Name.str()];
    if (!loaded)
      loaded = result;
  }

  if (findAdapter)
    if (Module *adapter = wrapperUnit->getAdapterModule())
      result = adapter;

  return result;
}

Module *ClangImporter::getImportedHeaderModule() {
  return Impl.ImportedHeaderUnit->getParentModule();
}

ClangImporter::Implementation::Implementation(ASTContext &ctx,
                                              const ClangImporterOptions &opts,
                                              const llvm::Triple &triple)
  : SwiftContext(ctx),
    SplitPrepositions(ctx.LangOpts.SplitPrepositions),
    InferImplicitProperties(opts.InferImplicitProperties)
{
  // Add filters to determine if a Clang availability attribute
  // applies in Swift, and if so, what is the cutoff for deprecated
  // declarations that are now considered unavailable in Swift.
  if (triple.isiOS()) {
    if (!ctx.LangOpts.EnableAppExtensionRestrictions) {
      PlatformAvailabilityFilter =
        [](StringRef Platform) { return Platform == "ios"; };
    }
    else {
      PlatformAvailabilityFilter =
        [](StringRef Platform) {
          return Platform == "ios" ||
                 Platform == "ios_app_extension"; };
    }
    // Anything deprecated in iOS 7.x and earlier is unavailable in Swift.
    DeprecatedAsUnavailableFilter =
      [](unsigned major, llvm::Optional<unsigned> minor) { return major <= 7; };
    DeprecatedAsUnavailableMessage =
      "APIs deprecated as of iOS 7 and earlier are unavailable in Swift";
  }
  else if (triple.isMacOSX()) {
    if (!ctx.LangOpts.EnableAppExtensionRestrictions) {
      PlatformAvailabilityFilter =
      [](StringRef Platform) { return Platform == "macosx"; };
    }
    else {
      PlatformAvailabilityFilter =
      [](StringRef Platform) {
        return Platform == "macosx" ||
               Platform == "macosx_app_extension"; };
    }
    // Anything deprecated in OSX 10.9.x and earlier is unavailable in Swift.
    DeprecatedAsUnavailableFilter =
      [](unsigned major, llvm::Optional<unsigned> minor) {
        return minor.hasValue() && minor.getValue() <= 9;
    };
    DeprecatedAsUnavailableMessage =
      "APIs deprecated as of OSX 10.9 and earlier are unavailable in Swift";
  }
}


ClangImporter::Implementation::~Implementation() {
  assert(NumCurrentImportingEntities == 0);
}

ClangModuleUnit *ClangImporter::Implementation::getWrapperForModule(
    ClangImporter &importer,
    const clang::Module *underlying) {
  auto &cacheEntry = ModuleWrappers[underlying];
  if (ClangModuleUnit *cached = cacheEntry.getPointer())
    return cached;

  // FIXME: Handle hierarchical names better.
  Identifier name = SwiftContext.getIdentifier(underlying->Name);
  auto wrapper = Module::create(name, SwiftContext);

  auto file = new (SwiftContext) ClangModuleUnit(*wrapper, importer,
                                                 underlying);
  wrapper->addFile(*file);
  cacheEntry.setPointer(file);

  return file;
}

api_notes::APINotesReader *
ClangImporter::Implementation::getAPINotesForModule(
    const clang::Module *underlying) {
  assert(underlying == underlying->getTopLevelModule() &&
         "Only allowed on a top-level module");
  // Check whether we already have an API notes reader.
  auto known = APINotesReaders.find(underlying);
  if (known != APINotesReaders.end())
    return known->second.get();

  /// Determine the name of the API notes we're looking for.
  llvm::SmallString<64> notesFilename(underlying->Name);
  notesFilename += '.';
  notesFilename += api_notes::BINARY_APINOTES_EXTENSION;

  // Look for a compiled API notes file in at the given search path. Sets
  // the reader and returns true if one was found.
  llvm::SmallString<64> scratch;
  std::unique_ptr<api_notes::APINotesReader> reader;
  auto findAPINotes = [&](StringRef searchPath) -> bool {
    // Compute the file name we're looking for.
    scratch.clear();
    llvm::sys::path::append(scratch, searchPath, notesFilename.str());

    // Try to open the file.
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> bufferOrErr
      = llvm::MemoryBuffer::getFile(scratch.str());
    if (!bufferOrErr)
      return false;

    // We found the API notes file; try to load it.
    reader = api_notes::APINotesReader::get(std::move(bufferOrErr.get()));
    return true;
  };

  // Look for a ModuleName.apinotes file in the import search paths.
  // FIXME: Good thing we have no notion of layering for these paths.
  bool foundAny = false;
  for (const auto& searchPath : SwiftContext.SearchPathOpts.ImportSearchPaths) {
    if (findAPINotes(searchPath)) {
      foundAny = true;
      break;
    }
  }

  // If we didn't find anything in the user-provided import search paths, look
  // in the runtime library import path.
  if (!foundAny)
    findAPINotes(SwiftContext.SearchPathOpts.RuntimeLibraryImportPath);

  // Add the reader we formed (if any) to the table.
  return APINotesReaders.insert({underlying, std::move(reader)})
           .first->second.get();
}

Optional<clang::Module *>
ClangImporter::Implementation::getClangSubmoduleForDecl(
    const clang::Decl *D,
    bool allowForwardDeclaration) {
  const clang::Decl *actual = nullptr;
  if (auto OID = dyn_cast<clang::ObjCInterfaceDecl>(D)) {
    // Put the Objective-C class into the module that contains the @interface
    // definition, not just some @class forward declaration.
    actual = OID->getDefinition();
    if (!actual && !allowForwardDeclaration)
      return Nothing;

  } else if (auto TD = dyn_cast<clang::TagDecl>(D)) {
    actual = TD->getDefinition();
    if (!actual && !allowForwardDeclaration)
      return Nothing;
  } else if (auto OPD = dyn_cast<clang::ObjCProtocolDecl>(D)) {
    actual = OPD->getDefinition();
    if (!actual && !allowForwardDeclaration)
      return Nothing;
  }

  if (!actual)
    actual = D->getCanonicalDecl();

  return actual->getOwningModule();
}

ClangModuleUnit *ClangImporter::Implementation::getClangModuleForDecl(
    const clang::Decl *D,
    bool allowForwardDeclaration) {
  auto maybeModule = getClangSubmoduleForDecl(D, allowForwardDeclaration);
  if (!maybeModule)
    return nullptr;
  if (!maybeModule.getValue())
    return ImportedHeaderUnit;

  // Get the parent module because currently we don't represent submodules with
  // ClangModuleUnit.
  auto *M = maybeModule.getValue()->getTopLevelModule();

  auto &importer =
    static_cast<ClangImporter &>(*SwiftContext.getClangModuleLoader());
  return getWrapperForModule(importer, M);
}

#pragma mark Source locations
clang::SourceLocation
ClangImporter::Implementation::importSourceLoc(SourceLoc loc) {
  // FIXME: Implement!
  return clang::SourceLocation();
}

SourceLoc
ClangImporter::Implementation::importSourceLoc(clang::SourceLocation loc) {
  // FIXME: Implement!
  return SourceLoc();
}

SourceRange
ClangImporter::Implementation::importSourceRange(clang::SourceRange loc) {
  // FIXME: Implement!
  return SourceRange();
}

#pragma mark Importing names

/// \brief Determine whether the given name is reserved for Swift.
static bool isSwiftReservedName(StringRef name) {
  /// FIXME: Check Swift keywords.
  return llvm::StringSwitch<bool>(name)
           .Cases("true", "false", true)
           .Default(false);
}

clang::DeclarationName
ClangImporter::Implementation::importName(Identifier name) {
  // FIXME: When we start dealing with C++, we can map over some operator
  // names.
  if (name.isOperator())
    return clang::DeclarationName();

  if (isSwiftReservedName(name.str()))
    return clang::DeclarationName();

  // Map the identifier. If it's some kind of keyword, it can't be mapped.
  auto ident = &Instance->getASTContext().Idents.get(name.str());
  if (ident->getTokenID() != clang::tok::identifier)
    return clang::DeclarationName();

  return ident;
}

Identifier
ClangImporter::Implementation::importName(clang::DeclarationName name,
                                          StringRef suffix,
                                          StringRef removePrefix) {
  // FIXME: At some point, we'll be able to import operators as well.
  if (!name || name.getNameKind() != clang::DeclarationName::Identifier)
    return Identifier();

  StringRef nameStr = name.getAsIdentifierInfo()->getName();
  // Remove the prefix, if any.
  if (!removePrefix.empty()) {
    assert(nameStr.startswith(removePrefix)
           && "name doesn't start with given removal prefix");
    nameStr = nameStr.slice(removePrefix.size(), nameStr.size());
  }

  // Get the Swift identifier.
  if (suffix.empty()) {
    if (isSwiftReservedName(nameStr))
      return Identifier();

    return SwiftContext.getIdentifier(nameStr);
  }

  // Append the suffix, and try again.
  llvm::SmallString<64> nameBuf;
  nameBuf += nameStr;
  nameBuf += suffix;

  if (isSwiftReservedName(nameBuf))
    return Identifier();

  return SwiftContext.getIdentifier(nameBuf);
}

/// Split the given selector piece at the given index, updating
/// capitalization as required.
///
/// \param selector The selector piece.
/// \param index The index at which to split.
/// \param buffer Buffer used for scratch space.
///
/// \returns a pair of strings \c (before,after), where \c after has
/// has its capitalization adjusted if necessary.
static std::pair<StringRef, StringRef>
splitSelectorPieceAt(StringRef selector, unsigned index,
                     SmallVectorImpl<char> &buffer) {
  // If the split point is at the end of the selector, the solution is
  // trivial.
  if (selector.str().size() == index) {
    return { selector, "" };
  }

  // Split at the index and lowercase the parameter name.
  return { selector.substr(0, index),
           camel_case::toLowercaseWord(selector.substr(index), buffer) };
}

/// Split the first selector piece into a function name and first
/// parameter name, if possible.
///
/// \param selector The selector piece to split.
/// \param scratch Scratch space to use when forming strings.
///
/// \returns a (function name, first parameter name) pair describing
/// the split. If no split is possible, the function name will be \c
/// selector and the parameter name will be empty.
static std::pair<StringRef, StringRef> 
splitFirstSelectorPiece(StringRef selector,
                        SmallVectorImpl<char> &scratch) {
  // Get the camelCase words in this selector.
  auto words = camel_case::getWords(selector);
  if (words.empty())
    return { selector, "" };

  // If "init" is the first word, we have an initializer.
  if (*words.begin() == "init") {
    return splitSelectorPieceAt(selector, 4, scratch);
  }

  // Scan words from the back of the selector looking for a
  // preposition.
  auto lastPrep = words.rbegin();
  auto end = words.rend();
  for (; lastPrep != end; ++lastPrep) {
    // If we found a linking verb, don't split.
    if (isLinkingVerb(*lastPrep)) {
      ++NumLinkingVerbNonSplits;
      return { selector, "" };
    }

    if (getPrepositionKind(*lastPrep) != PK_None) {
      // Found a preposition.
      break;
    }
  }

  // If we found a preposition, split here.
  if (lastPrep != end) {
    // .. unless there is a linking verb.
    for (auto i = std::next(lastPrep); i != end; ++i) {
      // If we found a linking verb, don't split.
      if (isLinkingVerb(*i)) {
        ++NumLinkingVerbNonSplitsPreposition;
        return { selector, "" };
      }
    }

    ++NumPrepositionSplitMethodNames;
    if (lastPrep == words.rbegin())
      ++NumPrepositionTrailingFirstPiece;
    return splitSelectorPieceAt(selector,
                                std::prev(lastPrep.base()).getPosition(),
                                scratch);
  }

  // We did not find a preposition. Nothing to split.
  ++NumSelectorsNotSplit;
  return { selector, "" };
}

/// Import an argument name.
static Identifier importArgName(ASTContext &ctx, StringRef name, bool dropWith){
  // Simple case: empty name.
  if (name.empty())
    return Identifier();

  llvm::SmallString<16> scratch;
  auto words = camel_case::getWords(name);
  auto firstWord = *words.begin();

  // If we're dropping "with", handle that now.
  if (dropWith) {
    // If the first word is "with"...
    StringRef argName;
    if (name.size() > 4 &&
        camel_case::sameWordIgnoreFirstCase(firstWord, "with")) {
      // Drop it.
      ++NumInitsDroppedWith;

      auto iter = words.begin();
      ++iter;

      argName = name.substr(iter.getPosition());
    } else {
      // If we're tracking statistics, check whether the name starts with
      // a preposition.
      if (llvm::AreStatisticsEnabled()) {
        if (getPrepositionKind(firstWord))
          ++NumInitsPrepositionSplit;
        else
          ++NumInitsNonPrepositionSplit;
      }

      argName = name;
    }

    return ctx.getIdentifier(camel_case::toLowercaseWord(argName, scratch));
  }

  // If the first word isn't a non-directional preposition, lowercase
  // it to form the argument name.
  if (!ctx.LangOpts.SplitPrepositions ||
      getPrepositionKind(firstWord) != PK_Nondirectional)
    return ctx.getIdentifier(camel_case::toLowercaseWord(name, scratch));

  // The first word is a non-directional preposition.

  // If there's only one word, we're left with no argument name.
  if (firstWord.size() == name.size())
    return Identifier();
  
  return ctx.getIdentifier(
           camel_case::toLowercaseWord(name.substr(firstWord.size()), scratch));
}

/// Map an Objective-C selector name to a Swift method name.
static DeclName mapSelectorName(ASTContext &ctx,
                                ObjCSelector selector,
                                bool isInitializer) {
  // Zero-argument selectors.
  if (selector.getNumArgs() == 0) {
    ++NumNullaryMethodNames;

    auto name = selector.getSelectorPieces()[0];
    StringRef nameText = name.empty()? "" : name.str();

    // Simple case.
    if (!isInitializer || nameText.size() == 4)
      return DeclName(ctx, name, { });

    // This is an initializer with no parameters but a name that
    // contains more than 'init', so synthesize an argument to capture
    // what follows 'init'.
    ++NumNullaryInitMethodsMadeUnary;
    assert(camel_case::getFirstWord(nameText).equals("init"));
    auto baseName = ctx.Id_init;
    auto argName = importArgName(ctx, nameText.substr(4), /*dropWith=*/true);
    return DeclName(ctx, baseName, argName);
  }

  // Determine the base name and first argument name.
  Identifier baseName;
  SmallVector<Identifier, 2> argumentNames;
  Identifier firstPiece = selector.getSelectorPieces()[0];
  StringRef firstPieceText = firstPiece.empty()? "" : firstPiece.str();
  if (isInitializer) {
    assert(camel_case::getFirstWord(firstPieceText).equals("init"));
    baseName = ctx.Id_init;
    argumentNames.push_back(
      importArgName(ctx, firstPieceText.substr(4), /*dropWith=*/true));
  } else if (ctx.LangOpts.SplitPrepositions) {
    llvm::SmallString<16> scratch;
    StringRef funcName, firstArgName;
    std::tie(funcName, firstArgName) = splitFirstSelectorPiece(firstPieceText,
                                                               scratch);
    baseName = ctx.getIdentifier(funcName);
    argumentNames.push_back(importArgName(ctx, firstArgName,
                                          /*dropWith=*/false));
  } else {
    baseName = firstPiece;
    argumentNames.push_back(Identifier());
  }

  if (argumentNames[0].empty())
    ++NumMethodsMissingFirstArgName;

  // Determine the remaining argument names.
  unsigned n = selector.getNumArgs();
  if (n == 1)
    ++NumUnaryMethodNames;
  else
    ++NumMultiMethodNames;

  for (auto piece : selector.getSelectorPieces().slice(1)) {
    if (piece.empty())
      argumentNames.push_back(piece);
    else
      argumentNames.push_back(importArgName(ctx, piece.str(),
                                            /*dropWith=*/false));
  }
  return DeclName(ctx, baseName, argumentNames);
}

namespace {
  /// Function object used to create Clang selectors from strings.
  class CreateSelector {
    ASTContext &Ctx;
      
  public:
    CreateSelector(ASTContext &ctx) : Ctx(ctx){ }

    template<typename ...Strings>
    ObjCSelector operator()(unsigned numParams, Strings ...strings) const {
      Identifier pieces[sizeof...(Strings)] = {
        (strings[0]? Ctx.getIdentifier(strings) : Identifier())...
      };
      
      assert((numParams == 0 && sizeof...(Strings) == 1) ||
             (numParams > 0 && sizeof...(Strings) == numParams));
      return ObjCSelector(Ctx, numParams, pieces);
    }
  };

  /// Function object used to create Swift method names from strings.
  class CreateMethodName {
    ASTContext &Ctx;
    Identifier BaseName;

  public:
    CreateMethodName(ASTContext &ctx, StringRef baseName) 
      : Ctx(ctx)
    { 
      BaseName = Ctx.getIdentifier(baseName);
    }

    template<typename ...Strings>
    DeclName operator()(Strings ...strings) const {
      Identifier pieces[sizeof...(Strings)] = {
        (strings[0]? Ctx.getIdentifier(strings) : Identifier())...
      };

      return DeclName(Ctx, BaseName, pieces);
    }
  };
}

ObjCSelector ClangImporter::Implementation::importSelector(
               clang::Selector selector) {
  auto &ctx = SwiftContext;

  // Handle zero-argument selectors directly.
  if (selector.isUnarySelector()) {
    Identifier name;
    if (auto id = selector.getIdentifierInfoForSlot(0))
      name = ctx.getIdentifier(id->getName());
    return ObjCSelector(ctx, 0, name);
  }

  SmallVector<Identifier, 2> pieces;
  for (auto i = 0u, n = selector.getNumArgs(); i != n; ++i) {
    Identifier piece;
    if (auto id = selector.getIdentifierInfoForSlot(i))
      piece = ctx.getIdentifier(id->getName());
    pieces.push_back(piece);
  }

  return ObjCSelector(ctx, pieces.size(), pieces);
}

clang::Selector
ClangImporter::Implementation::importSelector(DeclName name,
                                              bool allowSimpleName) {
  if (!allowSimpleName && name.isSimpleName())
    return {};

  clang::ASTContext &ctx = getClangASTContext();

  SmallVector<clang::IdentifierInfo *, 8> pieces;
  pieces.push_back(importName(name.getBaseName()).getAsIdentifierInfo());

  auto argNames = name.getArgumentNames();
  if (argNames.empty())
    return ctx.Selectors.getNullarySelector(pieces.front());

  if (!argNames.front().empty())
    return {};
  argNames = argNames.slice(1);

  for (Identifier argName : argNames)
    pieces.push_back(importName(argName).getAsIdentifierInfo());

  return ctx.Selectors.getSelector(pieces.size(), pieces.data());
}

DeclName ClangImporter::Implementation::mapSelectorToDeclName(
           ObjCSelector selector,
           bool isInitializer)
{
  // If we haven't computed any selector mappings before, load the
  // default mappings.
  if (SelectorMappings.empty() && SplitPrepositions) {
    // Function object that creates a selector.
    CreateSelector createSelector(SwiftContext);

#define MAP_SELECTOR(Selector, ForMethod, ForInitializer, BaseName, ArgNames) \
    {                                                                   \
      auto selector = createSelector Selector;                          \
      CreateMethodName createMethodName(SwiftContext, BaseName);        \
      auto methodName = createMethodName ArgNames;                      \
      assert((ForMethod || ForInitializer) &&                           \
             "Must be for a method or initializer");                    \
      assert(selector.getNumArgs() == methodName.getArgumentNames().size()); \
      if (ForMethod)                                                    \
        SelectorMappings[{selector, false}] = methodName;               \
      if (ForInitializer) {                                             \
        assert(methodName.getBaseName() == SwiftContext.Id_init);       \
        SelectorMappings[{selector, true}] = methodName;                \
      }                                                                 \
    }
#include "MappedNames.def"
  }

  // Check whether we've already mapped this selector.
  auto known = SelectorMappings.find({selector, isInitializer});
  if (known != SelectorMappings.end())
    return known->second;

  // Map the selector.
  auto result = mapSelectorName(SwiftContext, selector, isInitializer);

  // Cache the result and return.
  SelectorMappings[{selector, isInitializer}] = result;
  return result;
}

DeclName ClangImporter::Implementation::mapFactorySelectorToInitializerName(
           ObjCSelector selector,
           StringRef className) {
  auto firstPiece = selector.getSelectorPieces()[0];
  if (firstPiece.empty())
    return DeclName();

  // Match the camelCase beginning of the first selector piece to the
  // ending of the class name.
  auto firstPieceStr = firstPiece.str();
  auto methodWords = camel_case::getWords(firstPieceStr);
  auto classWords = camel_case::getWords(className);
  auto methodWordIter = methodWords.begin(),
    methodWordIterEnd = methodWords.end();
  auto classWordRevIter = classWords.rbegin(),
    classWordRevIterEnd = classWords.rend();

  // Find the last instance of the first word in the method's name within
  // the words in the class name.
  while (classWordRevIter != classWordRevIterEnd &&
         !camel_case::sameWordIgnoreFirstCase(*methodWordIter,
                                              *classWordRevIter)) {
    ++classWordRevIter;
  }

  // If we didn't find the first word in the method's name at all, we're
  // done.
  if (classWordRevIter == classWordRevIterEnd)
    return DeclName();

  // Now, match from the first word up until the end of the class.
  auto classWordIter = classWordRevIter.base(),
  classWordIterEnd = classWords.end();
  ++methodWordIter;
  while (classWordIter != classWordIterEnd &&
         methodWordIter != methodWordIterEnd &&
         camel_case::sameWordIgnoreFirstCase(*classWordIter,
                                             *methodWordIter)) {
    ++classWordIter;
    ++methodWordIter;
  }

  // If we didn't reach the end of the class name, don't match.
  if (classWordIter != classWordIterEnd)
    return DeclName();

  // We found the chopping point. Form the first argument name.
  llvm::SmallString<32> scratch;
  SmallVector<Identifier, 4> argumentNames;
  argumentNames.push_back(
    importArgName(SwiftContext,
                  splitSelectorPieceAt(firstPieceStr,
                                       methodWordIter.getPosition(),
                                       scratch).second,
                  /*dropWith=*/true));

  // Handle nullary factory methods.
  if (selector.getNumArgs() == 0) {
    if (argumentNames[0].empty())
      return DeclName(SwiftContext, SwiftContext.Id_init, { });

    // We don't have a convenience place to put the remaining argument name,
    // so leave it as a factory method.
    ++NumFactoryMethodsNullary;
    return DeclName();
  }

  // Map the remaining selector pieces.
  for (auto piece : selector.getSelectorPieces().slice(1)) {
    if (piece.empty())
      argumentNames.push_back(piece);
    else
      argumentNames.push_back(importArgName(SwiftContext, piece.str(),
                                            /*dropWith=*/false));
  }

  return DeclName(SwiftContext, SwiftContext.Id_init, argumentNames);
}

/// Translate the "nullability" notion from API notes into an optional type
/// kind.
OptionalTypeKind ClangImporter::Implementation::translateNullability(
                   api_notes::NullableKind kind) {
  switch (kind) {
  case api_notes::NullableKind::NonNullable:
    return OptionalTypeKind::OTK_None;

  case api_notes::NullableKind::Nullable:
    return OptionalTypeKind::OTK_Optional;

  case api_notes::NullableKind::Unknown:
    return OptionalTypeKind::OTK_ImplicitlyUnwrappedOptional;

  case api_notes::NullableKind::Absent:
    return OptionalTypeKind::OTK_ImplicitlyUnwrappedOptional;
  }
}

std::tuple<StringRef, api_notes::APINotesReader *, api_notes::APINotesReader *>
ClangImporter::Implementation::getAPINotesForContext(
    const clang::ObjCContainerDecl *container) {
  // Find the primary set of API notes, in the module where this container
  // was declared.
  api_notes::APINotesReader *primary = nullptr;
  if (auto primaryModule = getClangSubmoduleForDecl(container)) {
    if (*primaryModule) {
      primary = getAPINotesForModule((*primaryModule)->getTopLevelModule());
    }
  }

  StringRef name;

  // Find the secondary set of API notes, in the module where the original
  // class was declared.
  api_notes::APINotesReader *secondary = nullptr;
  if (auto category = dyn_cast<clang::ObjCCategoryDecl>(container)) {
    auto *objcClass = category->getClassInterface();
    if (auto secondaryModule = getClangSubmoduleForDecl(objcClass)) {
      if (*secondaryModule) {
        secondary = getAPINotesForModule(
                                     (*secondaryModule)->getTopLevelModule());
      }
    }

    // For categories, use the name of the class itself.
    name = objcClass->getName();
  } else {
    // For protocols and classes, we just need the class name.
    name = container->getName();
  }

  // If the primary and secondary are the same.
  if (primary == secondary) {
    // Drop the secondary; we only care about the primary.
    secondary = nullptr;
  }

  return std::make_tuple(name, primary, secondary);
}

void ClangImporter::Implementation::mergePropInfoIntoAccessor(
    const clang::ObjCMethodDecl *method, api_notes::ObjCMethodInfo &methodInfo){

  if (!method->isPropertyAccessor())
    return;

  const clang::ObjCPropertyDecl *pDecl = method->findPropertyDecl();
  if (!pDecl)
    return;

  if (auto pInfo = getKnownObjCProperty(pDecl)) {
    if (method->param_size() == 0) {
      methodInfo.mergePropInfoIntoGetter(*pInfo);
    } else {
      assert(method->param_size() == 1);
      methodInfo.mergePropInfoIntoSetter(*pInfo);
    }
  }
}

Optional<api_notes::ObjCMethodInfo>
ClangImporter::Implementation::getKnownObjCMethod(
    const clang::ObjCMethodDecl *method) {
  auto *container = cast<clang::ObjCContainerDecl>(method->getDeclContext());

  // Figure out where to look for context information.
  StringRef contextName;
  api_notes::APINotesReader *primary;
  api_notes::APINotesReader *secondary;
  std::tie(contextName, primary, secondary) = getAPINotesForContext(container);

  // Map the selector.
  SmallVector<StringRef, 2> selectorPieces;
  api_notes::ObjCSelectorRef selectorRef;
  selectorRef.NumPieces = method->getSelector().getNumArgs();
  for (unsigned i = 0, n = std::max(1u, selectorRef.NumPieces); i != n; ++i) {
    selectorPieces.push_back(method->getSelector().getNameForSlot(i));
  }
  selectorRef.Identifiers = selectorPieces;

  // Look for method and context information in the primary source.
  Optional<api_notes::ObjCMethodInfo> methodInfo;
  Optional<std::pair<api_notes::ContextID, api_notes::ObjCContextInfo>>
    contextInfo;

  if (primary) {
    // Look for context information in the primary source.
    if (isa<clang::ObjCProtocolDecl>(container))
      contextInfo = primary->lookupObjCProtocol(contextName);
    else
      contextInfo = primary->lookupObjCClass(contextName);

    // Look for method information in the primary source.
    if (contextInfo) {
      methodInfo = primary->lookupObjCMethod(contextInfo->first, selectorRef,
                                             method->isInstanceMethod());
    }
  }

  // If we found method or class information in the primary source, return what
  // we found.
  if (methodInfo || contextInfo) {
    if (!methodInfo)
      methodInfo = api_notes::ObjCMethodInfo();

    // If accessor, merge the property info in.
    mergePropInfoIntoAccessor(method, *methodInfo);

    // Merge class information into the method.
    *methodInfo |= contextInfo->second;

    return methodInfo;
  }

  // Look for method information in the secondary source.
  if (secondary) {
    // Look for the context information in the secondary source.
    if (isa<clang::ObjCProtocolDecl>(container))
      contextInfo = secondary->lookupObjCProtocol(contextName);
    else
      contextInfo = secondary->lookupObjCClass(contextName);

    // Look for the method in the secondary source. We don't merge context
    // information from the secondary source.
    if (contextInfo) {
      methodInfo = secondary->lookupObjCMethod(contextInfo->first, selectorRef,
                                               method->isInstanceMethod());

      if (!methodInfo)
        methodInfo = api_notes::ObjCMethodInfo();

      // If accessor, merge the property info in.
      mergePropInfoIntoAccessor(method, *methodInfo);

      return methodInfo;
    }
  }

  return Nothing;
}

Optional<api_notes::ObjCContextInfo>
ClangImporter::Implementation::getKnownObjCContext(
    const clang::ObjCContainerDecl *container) {
  // Figure out where to look for context information.
  StringRef name;
  api_notes::APINotesReader *primary;
  api_notes::APINotesReader *secondary;
  std::tie(name, primary, secondary) = getAPINotesForContext(container);

  Optional<std::pair<api_notes::ContextID, api_notes::ObjCContextInfo>>
    primaryInfo;
  if (primary)
    primaryInfo = primary->lookupObjCClass(name);

  Optional<std::pair<api_notes::ContextID, api_notes::ObjCContextInfo>>
    secondaryInfo;
  if (secondary)
    secondaryInfo = secondary->lookupObjCClass(name);

  // If neither place had information about this class, we're done.
  if (!primaryInfo && !secondaryInfo)
    return Nothing;

  api_notes::ObjCContextInfo info;

  // Merge in primary information, if available.
  if (primaryInfo) {
    info |= primaryInfo->second;
  }

  // Merge in secondary information after stripping out anything that is not
  // propagated from the context's defining module.
  if (secondaryInfo) {
    secondaryInfo->second.stripModuleLocalInfo();
    info |= secondaryInfo->second;
  }

  return info;
}

Optional<api_notes::ObjCPropertyInfo>
ClangImporter::Implementation::getKnownObjCProperty(
    const clang::ObjCPropertyDecl *property) {
  auto *container = cast<clang::ObjCContainerDecl>(property->getDeclContext());

  // Figure out where to look for context information.
  StringRef contextName;
  api_notes::APINotesReader *primary;
  api_notes::APINotesReader *secondary;
  std::tie(contextName, primary, secondary) = getAPINotesForContext(container);

  // Look for property and context information in the primary source.
  Optional<api_notes::ObjCPropertyInfo> propertyInfo;
  Optional<std::pair<api_notes::ContextID, api_notes::ObjCContextInfo>>
    contextInfo;

  if (primary) {
    // Look for context information in the primary source.
    if (isa<clang::ObjCProtocolDecl>(container))
      contextInfo = primary->lookupObjCProtocol(contextName);
    else
      contextInfo = primary->lookupObjCClass(contextName);

    // Look for property information in the primary source.
    if (contextInfo) {
      propertyInfo = primary->lookupObjCProperty(contextInfo->first,
                                                 property->getName());
    }
  }

  // If we found property or class information in the primary source, return what
  // we found.
  if (propertyInfo || contextInfo) {
    if (!propertyInfo)
      propertyInfo = api_notes::ObjCPropertyInfo();

    // Merge class information into the property.
    *propertyInfo |= contextInfo->second;

    return propertyInfo;
  }

  // Look for property information in the secondary source.
  if (secondary) {
    // Look for the context information in the secondary source.
    if (isa<clang::ObjCProtocolDecl>(container))
      contextInfo = secondary->lookupObjCProtocol(contextName);
    else
      contextInfo = secondary->lookupObjCClass(contextName);

    // Look for the property in the secondary source. We don't merge context
    // information from the secondary source.
    if (contextInfo) {
      return secondary->lookupObjCProperty(contextInfo->first,
                                           property->getName());
    }
  }

  return Nothing;
}

bool ClangImporter::Implementation::hasDesignatedInitializers(
       const clang::ObjCInterfaceDecl *classDecl) {
  if (classDecl->hasDesignatedInitializers())
    return true;

  if (auto info = getKnownObjCContext(classDecl))
    return info->hasDesignatedInits();

  return false;
}

bool ClangImporter::Implementation::isDesignatedInitializer(
       const clang::ObjCInterfaceDecl *classDecl,
       const clang::ObjCMethodDecl *method) {
  // If the information is on the AST, use it.
  if (classDecl->hasDesignatedInitializers())
    return method->hasAttr<clang::ObjCDesignatedInitializerAttr>();

  if (auto info = getKnownObjCMethod(method))
    return info->DesignatedInit;

  return false;
}

bool ClangImporter::Implementation::isRequiredInitializer(
       const clang::ObjCMethodDecl *method) {
  // FIXME: No way to express this in Objective-C.

  if (auto info = getKnownObjCMethod(method))
    return info->Required;

  return false;
}

FactoryAsInitKind ClangImporter::Implementation::getFactoryAsInit(
                    const clang::ObjCInterfaceDecl *classDecl,
                    const clang::ObjCMethodDecl *method) {
  if (auto info = getKnownObjCMethod(method))
    return info->getFactoryAsInitKind();

  return FactoryAsInitKind::Infer;
}

#pragma mark Name lookup
void ClangImporter::lookupValue(Identifier name, VisibleDeclConsumer &consumer){
  auto &pp = Impl.Instance->getPreprocessor();
  auto &sema = Impl.Instance->getSema();

  // Map the name. If we can't represent the Swift name in Clang, bail out now.
  auto clangName = Impl.importName(name);
  if (!clangName)
    return;
  
  // See if there's a preprocessor macro we can import by this name.
  clang::IdentifierInfo *clangID = clangName.getAsIdentifierInfo();
  if (clangID && clangID->hasMacroDefinition()) {
    if (auto clangMacro = pp.getMacroInfo(clangID)) {
      if (auto valueDecl = Impl.importMacro(name, clangMacro)) {
        consumer.foundDecl(valueDecl, DeclVisibilityKind::VisibleAtTopLevel);
      }
    }
  }

  bool FoundType = false;
  bool FoundAny = false;
  auto processResults = [&](clang::LookupResult &result) {
    // FIXME: Filter based on access path? C++ access control?
    for (auto decl : result) {
      if (auto swiftDecl = Impl.importDeclReal(decl->getUnderlyingDecl())) {
        if (auto valueDecl = dyn_cast<ValueDecl>(swiftDecl)) {
          // If the importer gave us a declaration from the stdlib, make sure
          // it does not show up in the lookup results for the imported module.
          if (valueDecl->getDeclContext()->isModuleScopeContext() &&
              valueDecl->getModuleContext() == Impl.getStdlibModule())
            continue;
          // Check that we didn't pick up something with a remapped name.
          if (valueDecl->getName() != name)
            continue;

          consumer.foundDecl(valueDecl, DeclVisibilityKind::VisibleAtTopLevel);
          FoundType = FoundType || isa<TypeDecl>(valueDecl);
          FoundAny = true;
        }
      }
    }
  };

  // Perform name lookup into the global scope.
  // FIXME: Map source locations over.
  clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                   clang::Sema::LookupOrdinaryName);
  if (sema.LookupName(lookupResult, /*Scope=*/nullptr))
    processResults(lookupResult);

  if (!FoundType) {
    // Look up a tag name if we did not find a type with this name already.
    // We don't want to introduce multiple types with same name.
    lookupResult.clear(clang::Sema::LookupTagName);
    if (sema.LookupName(lookupResult, /*Scope=*/nullptr))
      processResults(lookupResult);
  }

  // Look up protocol names as well.
  lookupResult.clear(clang::Sema::LookupObjCProtocolName);
  if (sema.LookupName(lookupResult, /*Scope=*/nullptr)) {
    processResults(lookupResult);

  } else if (!FoundAny && name.str().endswith(SWIFT_PROTOCOL_SUFFIX)) {
    auto noProtoNameStr = name.str().drop_back(strlen(SWIFT_PROTOCOL_SUFFIX));
    auto protoIdent = &Impl.getClangASTContext().Idents.get(noProtoNameStr);
    lookupResult.clear(clang::Sema::LookupObjCProtocolName);
    lookupResult.setLookupName(protoIdent);

    if (sema.LookupName(lookupResult, /*Scope=*/nullptr))
      processResults(lookupResult);

    lookupResult.setLookupName(clangName);
  }

  // If we *still* haven't found anything, try looking for '<name>Ref'.
  // Eventually, this should be optimized by recognizing this case when
  // generating the clang module.
  if (!FoundAny && clangID && Impl.SwiftContext.LangOpts.ImportCFTypes) {
    lookupResult.clear(clang::Sema::LookupOrdinaryName);

    llvm::SmallString<128> buffer;
    buffer += clangID->getName();
    buffer += SWIFT_CFTYPE_SUFFIX;
    auto refIdent = &Impl.Instance->getASTContext().Idents.get(buffer.str());
    lookupResult.setLookupName(refIdent);

    if (sema.LookupName(lookupResult, /*Scope=*/0)) {
      // FIXME: Filter based on access path? C++ access control?
      for (auto decl : lookupResult) {
        if (auto swiftDecl = Impl.importDeclReal(decl->getUnderlyingDecl())) {
          auto alias = dyn_cast<TypeAliasDecl>(swiftDecl);
          if (!alias) continue;

          Type underlyingTy = alias->getUnderlyingType();
          TypeDecl *underlying = nullptr;
          if (auto anotherAlias =
                dyn_cast<NameAliasType>(underlyingTy.getPointer())) {
            underlying = anotherAlias->getDecl();
          } else if (auto aliasedClass = underlyingTy->getAs<ClassType>()) {
            underlying = aliasedClass->getDecl();
          }
          
          if (!underlying)
            continue;
          if (underlying->getName() == name) {
            consumer.foundDecl(underlying,
                               DeclVisibilityKind::VisibleAtTopLevel);
          }
        }
      }
    }
  }
}

const clang::TypedefNameDecl *
ClangImporter::Implementation::lookupTypedef(clang::DeclarationName name) {
  clang::Sema &sema = Instance->getSema();
  clang::LookupResult lookupResult(sema, name,
                                   clang::SourceLocation(),
                                   clang::Sema::LookupOrdinaryName);

  if (sema.LookupName(lookupResult, /*scope=*/nullptr)) {
    for (auto decl : lookupResult) {
      if (auto typedefDecl =
          dyn_cast<clang::TypedefNameDecl>(decl->getUnderlyingDecl()))
        return typedefDecl;
    }
  }

  return nullptr;
}

static bool isDeclaredInModule(const ClangModuleUnit *ModuleFilter,
                               const Decl *VD) {
  auto ContainingUnit = VD->getDeclContext()->getModuleScopeContext();
  return ModuleFilter == ContainingUnit;
}

static const clang::Module *getClangOwningModule(ClangNode Node,
                                            const clang::ASTContext &ClangCtx) {
  auto ExtSource = ClangCtx.getExternalSource();
  assert(ExtSource);
  if (const clang::Decl *D = Node.getAsDecl())
    return ExtSource->getModule(D->getOwningModuleID());
  if (const clang::MacroInfo *MI = Node.getAsMacro())
    return ExtSource->getModule(MI->getOwningModuleID());

  return nullptr;
}

static bool isVisibleFromModule(const ClangModuleUnit *ModuleFilter,
                                const ValueDecl *VD) {
  // Include a value from module X if:
  // * no particular module was requested, or
  // * module X was specifically requested.
  if (!ModuleFilter)
    return true;

  auto ContainingUnit = VD->getDeclContext()->getModuleScopeContext();
  if (ModuleFilter == ContainingUnit)
    return true;

  auto Wrapper = dyn_cast<ClangModuleUnit>(ContainingUnit);
  if (!Wrapper)
    return false;

  auto ClangNode = VD->getClangNode();
  assert(ClangNode);

  auto &ClangASTContext = ModuleFilter->getClangASTContext();
  auto OwningClangModule = getClangOwningModule(ClangNode, ClangASTContext);

  // We don't handle Clang submodules; pop everything up to the top-level
  // module.
  if (OwningClangModule)
    OwningClangModule = OwningClangModule->getTopLevelModule();

  if (OwningClangModule == ModuleFilter->getClangModule())
    return true;

  if (auto D = ClangNode.getAsDecl()) {
    // Handle redeclared decls.
    if (isa<clang::FunctionDecl>(D) || isa<clang::VarDecl>(D) ||
        isa<clang::TypedefNameDecl>(D)) {
      for (auto Redeclaration : D->redecls()) {
        if (Redeclaration == D)
          continue;
        auto OwningClangModule = getClangOwningModule(Redeclaration,
                                                      ClangASTContext);

        if (OwningClangModule == ModuleFilter->getClangModule())
          return true;
      }
    }
  }

  return false;
}


namespace {
class ClangVectorDeclConsumer : public clang::VisibleDeclConsumer {
  std::vector<clang::NamedDecl *> results;
public:
  ClangVectorDeclConsumer() = default;

  void FoundDecl(clang::NamedDecl *ND, clang::NamedDecl *Hiding,
                 clang::DeclContext *Ctx, bool InBaseClass) override {
    if (!ND->getIdentifier())
      return;

    if (ND->isModulePrivate())
      return;

    results.push_back(ND);
  }

  llvm::MutableArrayRef<clang::NamedDecl *> getResults() {
    return results;
  }
};

class FilteringVisibleDeclConsumer : public swift::VisibleDeclConsumer {
  swift::VisibleDeclConsumer &NextConsumer;
  const ClangModuleUnit *ModuleFilter = nullptr;

public:
  FilteringVisibleDeclConsumer(swift::VisibleDeclConsumer &consumer,
                               const ClangModuleUnit *CMU)
      : NextConsumer(consumer), ModuleFilter(CMU) {}

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason) override {
    if (isVisibleFromModule(ModuleFilter, VD))
      NextConsumer.foundDecl(VD, Reason);
  }
};

class FilteringDeclaredDeclConsumer : public swift::VisibleDeclConsumer {
  swift::VisibleDeclConsumer &NextConsumer;
  SmallVectorImpl<ExtensionDecl *> &ExtensionResults;
  const ClangModuleUnit *ModuleFilter = nullptr;

public:
  FilteringDeclaredDeclConsumer(swift::VisibleDeclConsumer &consumer,
                             SmallVectorImpl<ExtensionDecl *> &ExtensionResults,
                                const ClangModuleUnit *CMU)
      : NextConsumer(consumer),
        ExtensionResults(ExtensionResults),
        ModuleFilter(CMU) {}

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason) override {
    if (isDeclaredInModule(ModuleFilter, VD)) {
      NextConsumer.foundDecl(VD, Reason);
      return;
    }
    // Also report the extensions of the module that extend types from another
    // module.
    if (auto NTD = dyn_cast<NominalTypeDecl>(VD)) {
      for (auto Ext : NTD->getExtensions()) {
        if (isDeclaredInModule(ModuleFilter, Ext))
          ExtensionResults.push_back(Ext);
      }
    }
  }
};

} // unnamed namespace

void ClangImporter::lookupVisibleDecls(VisibleDeclConsumer &Consumer) const {
  if (Impl.CurrentCacheState != Implementation::CacheState::Valid) {
    do {
      Impl.CurrentCacheState = Implementation::CacheState::InProgress;
      Impl.CachedVisibleDecls.clear();

      ClangVectorDeclConsumer clangConsumer;
      auto &sema = Impl.getClangSema();
      sema.LookupVisibleDecls(sema.getASTContext().getTranslationUnitDecl(),
                              clang::Sema::LookupNameKind::LookupAnyName,
                              clangConsumer);

      // Sort all the Clang decls we find, so that we process them
      // deterministically. This *shouldn't* be necessary, but the importer
      // definitely still has ordering dependencies.
      auto results = clangConsumer.getResults();
      llvm::array_pod_sort(results.begin(), results.end(),
                           [](clang::NamedDecl * const *lhs,
                              clang::NamedDecl * const *rhs) -> int {
        return clang::DeclarationName::compare((*lhs)->getDeclName(),
                                               (*rhs)->getDeclName());
      });

      for (const clang::NamedDecl *clangDecl : results) {
        if (Impl.CurrentCacheState != Implementation::CacheState::InProgress)
          break;
        if (Decl *imported = Impl.importDeclReal(clangDecl))
          Impl.CachedVisibleDecls.push_back(cast<ValueDecl>(imported));
      }
      
      // If we changed things /while/ we were caching, we need to start over
      // and try again. Fortunately we record a map of decls we've already
      // imported, so most of the work is just the lookup and then going
      // through the list.
    } while (Impl.CurrentCacheState != Implementation::CacheState::InProgress);

    auto &ClangPP = Impl.getClangPreprocessor();
    for (auto I = ClangPP.macro_begin(), E = ClangPP.macro_end(); I != E; ++I) {
      if (!I->first->hasMacroDefinition())
        continue;
      auto Name = Impl.importName(I->first);
      if (Name.empty())
        continue;
      if (auto *Imported =
              Impl.importMacro(Name, I->second->getMacroInfo())) {
        Impl.CachedVisibleDecls.push_back(Imported);
      }
    }

    Impl.CurrentCacheState = Implementation::CacheState::Valid;
  }

  for (auto VD : Impl.CachedVisibleDecls)
    Consumer.foundDecl(VD, DeclVisibilityKind::VisibleAtTopLevel);
}

void ClangModuleUnit::lookupVisibleDecls(Module::AccessPathTy AccessPath,
                                         VisibleDeclConsumer &Consumer,
                                         NLKind LookupKind) const {
  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  // FIXME: Respect the access path.
  FilteringVisibleDeclConsumer filterConsumer(Consumer, this);
  owner.lookupVisibleDecls(filterConsumer);
}

namespace {
class VectorDeclPtrConsumer : public swift::VisibleDeclConsumer {
public:
  SmallVectorImpl<Decl *> &Results;
  explicit VectorDeclPtrConsumer(SmallVectorImpl<Decl *> &Decls)
    : Results(Decls) {}

  virtual void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason) override {
    Results.push_back(VD);
  }
};
} // unnamed namespace

void ClangModuleUnit::getTopLevelDecls(SmallVectorImpl<Decl*> &results) const {
  VectorDeclPtrConsumer Consumer(results);
  SmallVector<ExtensionDecl *, 16> Extensions;
  FilteringDeclaredDeclConsumer FilterConsumer(Consumer, Extensions, this);
  owner.lookupVisibleDecls(FilterConsumer);
  results.append(Extensions.begin(), Extensions.end());
}

static void getImportDecls(ClangModuleUnit *ClangUnit, const clang::Module *M,
                           SmallVectorImpl<Decl *> &Results) {
  assert(M);
  SmallVector<clang::Module *, 1> Exported;
  M->getExportedModules(Exported);

  ASTContext &Ctx = ClangUnit->getASTContext();

  for (auto *ImportedMod : M->Imports) {
    SmallVector<std::pair<swift::Identifier, swift::SourceLoc>, 4> AccessPath;
    auto *TmpMod = ImportedMod;
    while (TmpMod) {
      AccessPath.push_back({ Ctx.getIdentifier(TmpMod->Name), SourceLoc() });
      TmpMod = TmpMod->Parent;
    }
    std::reverse(AccessPath.begin(), AccessPath.end());

    bool IsExported = false;
    for (auto *ExportedMod : Exported) {
      if (ImportedMod == ExportedMod) {
        IsExported = true;
        break;
      }
    }

    auto *ID = ImportDecl::create(Ctx, ClangUnit, SourceLoc(),
                                  ImportKind::Module, SourceLoc(), AccessPath,
                                  ImportedMod);
    if (IsExported)
      ID->getAttrs().add(new (Ctx) ExportedAttr(/*IsImplicit=*/false));
    Results.push_back(ID);
  }
}

void ClangModuleUnit::getDisplayDecls(SmallVectorImpl<Decl*> &results) const {
  if (clangModule)
    getImportDecls(const_cast<ClangModuleUnit *>(this), clangModule, results);
  getTopLevelDecls(results);
}

void ClangModuleUnit::lookupValue(Module::AccessPathTy accessPath,
                                  DeclName name, NLKind lookupKind,
                                  SmallVectorImpl<ValueDecl*> &results) const {
  if (!Module::matchesAccessPath(accessPath, name))
    return;

  // There should be no multi-part top-level decls in a Clang module.
  if (!name.isSimpleName())
    return;

  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  VectorDeclConsumer vectorWriter(results);
  FilteringVisibleDeclConsumer filteringConsumer(vectorWriter, this);
  
  owner.lookupValue(name.getBaseName(), filteringConsumer);
}

void ClangImporter::loadExtensions(NominalTypeDecl *nominal,
                                   unsigned previousGeneration) {
  auto objcClass = dyn_cast_or_null<clang::ObjCInterfaceDecl>(
                     nominal->getClangDecl());
  if (!objcClass) {
    if (auto typeResolver = Impl.getTypeResolver())
      typeResolver->resolveDeclSignature(nominal);
    if (nominal->isObjC()) {
      // Map the name. If we can't represent the Swift name in Clang, bail out
      // now.
      auto clangName = Impl.importName(nominal->getName());
      if (!clangName)
        return;

      auto &sema = Impl.Instance->getSema();

      // Perform name lookup into the global scope.
      // FIXME: Map source locations over.
      clang::LookupResult lookupResult(sema, clangName,
                                       clang::SourceLocation(),
                                       clang::Sema::LookupOrdinaryName);
      if (sema.LookupName(lookupResult, /*Scope=*/0)) {
        // FIXME: Filter based on access path? C++ access control?
        for (auto clangDecl : lookupResult) {
          objcClass = dyn_cast<clang::ObjCInterfaceDecl>(clangDecl);
          if (objcClass)
            break;
        }
      }
    }
  }

  if (!objcClass)
    return;

  // Import all of the visible categories. Simply loading them adds them to
  // the list of extensions.
  for (auto I = objcClass->visible_categories_begin(),
            E = objcClass->visible_categories_end();
       I != E; ++I) {
    Impl.importDeclReal(*I);
  }
}

// FIXME: This should just be the implementation of
// llvm::array_pod_sort_comparator. The only difference is that it uses
// std::less instead of operator<.
// FIXME: Copied from IRGenModule.cpp.
template <typename T>
static int pointerPODSortComparator(T * const *lhs, T * const *rhs) {
  std::less<T *> lt;
  if (lt(*lhs, *rhs))
    return -1;
  if (lt(*rhs, *lhs))
    return -1;
  return 0;
}


void ClangModuleUnit::getImportedModules(
    SmallVectorImpl<Module::ImportedModule> &imports,
    Module::ImportFilter filter) const {
  if (filter != Module::ImportFilter::Public)
    imports.push_back({Module::AccessPathTy(),
                       getASTContext().getStdlibModule()});

  if (!clangModule) {
    // This is the special "imported headers" module.
    if (filter != Module::ImportFilter::Private) {
      imports.append(owner.Impl.ImportedHeaderExports.begin(),
                     owner.Impl.ImportedHeaderExports.end());
    }
    return;
  }

  auto topLevelAdapter = getAdapterModule();

  SmallVector<clang::Module *, 8> imported;
  clangModule->getExportedModules(imported);
  if (filter != Module::ImportFilter::Public) {
    if (filter == Module::ImportFilter::All) {
      llvm::SmallPtrSet<clang::Module *, 8> knownModules;
      imported.append(clangModule->Imports.begin(), clangModule->Imports.end());
      imported.erase(std::remove_if(imported.begin(), imported.end(),
                                    [&](clang::Module *mod) -> bool {
                                      return !knownModules.insert(mod);
                                    }),
                     imported.end());
    } else {
      llvm::SmallPtrSet<clang::Module *, 8> knownModules(imported.begin(),
                                                         imported.end());
      SmallVector<clang::Module *, 8> privateImports;
      std::copy_if(clangModule->Imports.begin(), clangModule->Imports.end(),
                   std::back_inserter(privateImports), [&](clang::Module *mod) {
                     return knownModules.count(mod) == 0;
      });
      imported.swap(privateImports);
    }

    // FIXME: The parent module isn't exactly a private import, but it is
    // needed for link dependencies.
    if (clangModule->Parent)
      imported.push_back(clangModule->Parent);
  }

  for (auto importMod : imported) {
    auto wrapper = owner.Impl.getWrapperForModule(owner, importMod);

    auto actualMod = wrapper->getAdapterModule();
    if (!actualMod || actualMod == topLevelAdapter)
      actualMod = wrapper->getParentModule();

    assert(actualMod && "Missing imported adapter module");
    imports.push_back({Module::AccessPathTy(), actualMod});
  }
}

static void lookupClassMembersImpl(ClangImporter::Implementation &Impl,
                                   VisibleDeclConsumer &consumer,
                                   DeclName name) {
  // When looking for a subscript, we actually look for the getters
  // and setters.
  bool isSubscript = name.isSimpleName(Impl.SwiftContext.Id_subscript);

  // FIXME: Does not include methods from protocols.
  auto importMethodsImpl = [&](const clang::ObjCMethodList &start) {
    for (auto *list = &start; list != nullptr; list = list->getNext()) {
      if (list->Method->isUnavailable())
        continue;

      // If the method is a property accessor, we want the property.
      const clang::NamedDecl *searchForDecl = list->Method;
      if (list->Method->isPropertyAccessor()) {
        if (auto property = list->Method->findPropertyDecl()) {
          // ... unless we are enumerating all decls.  In this case, if we see
          // a getter, return a property.  If we see a setter, we know that
          // there is a getter, and we will visit it and return a property at
          // that time.
          if (!name && list->Method->param_size() != 0)
            continue;
          searchForDecl = property;
        }
      }

      auto VD = cast_or_null<ValueDecl>(Impl.importDeclReal(searchForDecl));
      if (!VD)
        continue;

      if (auto func = dyn_cast<FuncDecl>(VD)) {
        if (auto storage = func->getAccessorStorageDecl()) {
          consumer.foundDecl(storage, DeclVisibilityKind::DynamicLookup);
          continue;
        } else if (isSubscript || !name) {
          auto known = Impl.Subscripts.find({func, nullptr});
          if (known != Impl.Subscripts.end()) {
            consumer.foundDecl(known->second,
                               DeclVisibilityKind::DynamicLookup);
          }

          // If we were looking only for subscripts, don't report the getter.
          if (isSubscript)
            continue;
        }
      }

      consumer.foundDecl(VD, DeclVisibilityKind::DynamicLookup);
    }
  };

  auto importMethods = [=](const clang::Sema::GlobalMethods &methodListPair) {
    if (methodListPair.first.Method)
      importMethodsImpl(methodListPair.first);
    if (methodListPair.second.Method)
      importMethodsImpl(methodListPair.second);
  };

  clang::Sema &S = Impl.getClangSema();

  if (isSubscript) {
    clang::Selector sels[] = {
      Impl.objectAtIndexedSubscript,
      Impl.setObjectAtIndexedSubscript,
      Impl.objectForKeyedSubscript,
      Impl.setObjectForKeyedSubscript
    };
    for (auto sel : sels) {
      S.ReadMethodPool(sel);
      importMethods(S.MethodPool[sel]);
    }

  } else if (name) {
    auto sel = Impl.importSelector(name);
    if (!sel.isNull()) {
      S.ReadMethodPool(sel);
      importMethods(S.MethodPool[sel]);

      // If this is a simple name, we only checked nullary selectors. Check
      // unary ones as well.
      // Note: If this is ever used to look up init methods, we'd need to do
      // the reverse as well.
      if (name.isSimpleName()) {
        auto *II = Impl.importName(name.getBaseName()).getAsIdentifierInfo();
        sel = Impl.getClangASTContext().Selectors.getUnarySelector(II);
        assert(!sel.isNull());

        S.ReadMethodPool(sel);
        importMethods(S.MethodPool[sel]);
      }
    }

  } else {
    // Force load all external methods.
    // FIXME: Copied from Clang's SemaCodeComplete.
    clang::ExternalASTSource *source = S.getExternalSource();
    for (uint32_t i = 0, n = source->GetNumExternalSelectors(); i != n; ++i) {
      clang::Selector sel = source->GetExternalSelector(i);
      if (sel.isNull() || S.MethodPool.count(sel))
        continue;

      S.ReadMethodPool(sel);
    }

    for (auto entry : S.MethodPool)
      importMethods(entry.second);
  }
}

void
ClangModuleUnit::lookupClassMember(Module::AccessPathTy accessPath,
                                   DeclName name,
                                   SmallVectorImpl<ValueDecl*> &results) const {
  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  // FIXME: Not limited by module.
  VectorDeclConsumer consumer(results);
  lookupClassMembersImpl(owner.Impl, consumer, name);
}

void ClangModuleUnit::lookupClassMembers(Module::AccessPathTy accessPath,
                                         VisibleDeclConsumer &consumer) const {
  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  // FIXME: Not limited by module.
  lookupClassMembersImpl(owner.Impl, consumer, {});
}

void ClangModuleUnit::collectLinkLibraries(
    Module::LinkLibraryCallback callback) const {
  if (!clangModule)
    return;
  for (auto clangLinkLib : clangModule->LinkLibraries) {
    LibraryKind kind;
    if (clangLinkLib.IsFramework)
      kind = LibraryKind::Framework;
    else
      kind = LibraryKind::Library;
    
    callback(LinkLibrary(clangLinkLib.Library, kind));
  }
}

StringRef ClangModuleUnit::getFilename() const {
  return clangModule ? clangModule->getASTFile()->getName() : "<imports>";
}

clang::TargetInfo &ClangImporter::getTargetInfo() const {
  return Impl.Instance->getTarget();
}

clang::ASTContext &ClangImporter::getClangASTContext() const {
  return Impl.getClangASTContext();
}

clang::Preprocessor &ClangImporter::getClangPreprocessor() const {
  return Impl.getClangPreprocessor();
}
const clang::Module *ClangImporter::getClangOwningModule(ClangNode Node) const {
  return ::getClangOwningModule(Node, getClangASTContext());
}

bool ClangImporter::hasTypedef(const clang::Decl *typeDecl) const {
  return Impl.DeclsWithSuperfluousTypedefs.count(typeDecl);
}

std::string ClangImporter::getClangModuleHash() const {
  return Impl.Invocation->getModuleHash();
}

void ClangImporter::printStatistics() const {
  Impl.Instance->getModuleManager()->PrintStats();
}

void ClangImporter::verifyAllModules() {
#ifndef NDEBUG
  if (Impl.ImportCounter == Impl.VerifiedImportCounter)
    return;

  // Collect the Decls before verifying them; the act of verifying may cause
  // more decls to be imported and modify the map while we are iterating it.
  SmallVector<Decl *, 8> Decls;
  for (auto &I : Impl.ImportedDecls)
    if (Decl *D = I.second)
      Decls.push_back(D);

  for (auto D : Decls)
    verify(D);

  Impl.VerifiedImportCounter = Impl.ImportCounter;
#endif
}

//===----------------------------------------------------------------------===//
// ClangModule Implementation
//===----------------------------------------------------------------------===//

ClangModuleUnit::ClangModuleUnit(Module &M, ClangImporter &owner,
                                 const clang::Module *clangModule)
  : LoadedFile(FileUnitKind::ClangModule, M), owner(owner),
    clangModule(clangModule) {
}

bool ClangModuleUnit::hasClangModule(Module *M) {
  for (auto F : M->getFiles()) {
    if (isa<ClangModuleUnit>(F))
      return true;
  }
  return false;
}

bool ClangModuleUnit::isTopLevel() const {
  return !clangModule || !clangModule->isSubModule();
}

bool ClangModuleUnit::isSystemModule() const {
  return clangModule && clangModule->IsSystem;
}

clang::ASTContext &ClangModuleUnit::getClangASTContext() const {
  return owner.getClangASTContext();
}

Module *ClangModuleUnit::getAdapterModule() const {
  if (!clangModule)
    return nullptr;

  if (!isTopLevel()) {
    // FIXME: Is this correct for submodules?
    auto topLevel = clangModule->getTopLevelModule();
    auto wrapper = owner.Impl.getWrapperForModule(owner, topLevel);
    return wrapper->getAdapterModule();
  }

  if (!adapterModule.getInt()) {
    // FIXME: Include proper source location.
    Module *M = getParentModule();
    ASTContext &Ctx = M->Ctx;
    auto adapter = Ctx.getModule(Module::AccessPathTy({M->Name, SourceLoc()}));
    if (adapter == M) {
      adapter = nullptr;
    } else {
      auto &sharedModuleRef = Ctx.LoadedModules[M->Name.str()];
      assert(!sharedModuleRef || sharedModuleRef == adapter ||
             sharedModuleRef == M);
      sharedModuleRef = adapter;
    }

    auto mutableThis = const_cast<ClangModuleUnit *>(this);
    mutableThis->adapterModule.setPointerAndInt(adapter, true);
  }

  return adapterModule.getPointer();
}
