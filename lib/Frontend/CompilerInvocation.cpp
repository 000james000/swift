//===-- CompilerInvocation.cpp - CompilerInvocation methods ---------------===//
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

#include "swift/Frontend/Frontend.h"

#include "swift/Strings.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Platform.h"
#include "swift/Option/Options.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace swift;
using namespace llvm::opt;

swift::CompilerInvocation::CompilerInvocation() {
  IRGenOpts.Triple = llvm::sys::getDefaultTargetTriple();
}

void CompilerInvocation::setMainExecutablePath(StringRef Path) {
  llvm::SmallString<128> LibPath(Path);
  llvm::sys::path::remove_filename(LibPath); // Remove /swift
  llvm::sys::path::remove_filename(LibPath); // Remove /bin
  llvm::sys::path::append(LibPath, "lib", "swift");
  setRuntimeResourcePath(LibPath.str());
}

void CompilerInvocation::setRuntimeResourcePath(StringRef Path) {
  SearchPathOpts.RuntimeResourcePath = Path;
  updateRuntimeLibraryPath();
}

void CompilerInvocation::updateRuntimeLibraryPath() {
  llvm::SmallString<128> LibPath(SearchPathOpts.RuntimeResourcePath);

  llvm::Triple Triple(IRGenOpts.Triple);
  llvm::sys::path::append(LibPath, getPlatformNameForTriple(Triple));
  SearchPathOpts.RuntimeLibraryPath = LibPath.str();

  if (Triple.isArch32Bit())
    llvm::sys::path::append(LibPath, "32");
  SearchPathOpts.RuntimeLibraryImportPath = LibPath.str();
}

void CompilerInvocation::setTargetTriple(StringRef Triple) {
  IRGenOpts.Triple = Triple.str();
  updateRuntimeLibraryPath();
}

static bool ParseFrontendArgs(FrontendOptions &Opts, ArgList &Args,
                              DiagnosticEngine &Diags) {
  using namespace options;

  if (const Arg *A = Args.getLastArg(OPT_debug_crash_Group)) {
    Option Opt = A->getOption();
    if (Opt.matches(OPT_debug_assert_immediately)) {
      // This assertion should always fail, per the user's request, and should
      // not be converted to llvm_unreachable.
      assert(0 && "This is an assertion!");
    } else if (Opt.matches(OPT_debug_crash_immediately)) {
      LLVM_BUILTIN_TRAP;
    } else if (Opt.matches(OPT_debug_assert_after_parse)) {
      // Set in FrontendOptions
      Opts.CrashMode = FrontendOptions::DebugCrashMode::AssertAfterParse;
    } else if (Opt.matches(OPT_debug_crash_after_parse)) {
      // Set in FrontendOptions
      Opts.CrashMode = FrontendOptions::DebugCrashMode::CrashAfterParse;
    } else {
      llvm_unreachable("Unknown debug_crash_Group option!");
    }
  }

  Opts.EmitVerboseSIL |= Args.hasArg(OPT_emit_verbose_sil);

  Opts.EmitSortedSIL |= Args.hasArg(OPT_emit_sorted_sil);

  Opts.DelayedFunctionBodyParsing |= Args.hasArg(OPT_delayed_function_body_parsing);

  Opts.PrintStats |= Args.hasArg(OPT_print_stats);

  Opts.PrintClangStats |= Args.hasArg(OPT_print_clang_stats);

  Opts.Playground |= Args.hasArg(OPT_playground);

  if (const Arg *A = Args.getLastArg(OPT_help, OPT_help_hidden)) {
    if (A->getOption().matches(OPT_help)) {
      Opts.PrintHelp = true;
    } else if (A->getOption().matches(OPT_help_hidden)) {
      Opts.PrintHelpHidden = true;
    } else {
      llvm_unreachable("Unknown help option parsed");
    }
  }

  for (const Arg *A : make_range(Args.filtered_begin(OPT_INPUT,
                                                     OPT_primary_file),
                                 Args.filtered_end())) {
    if (A->getOption().matches(OPT_INPUT)) {
      Opts.InputFilenames.push_back(A->getValue());
    } else if (A->getOption().matches(OPT_primary_file)) {
      Opts.PrimaryInput = SelectedInput(Opts.InputFilenames.size());
      Opts.InputFilenames.push_back(A->getValue());
    } else {
      llvm_unreachable("Unknown input-related argument!");
    }
  }

  Opts.ParseStdlib |= Args.hasArg(OPT_parse_stdlib);

  // Determine what the user has asked the frontend to do.
  FrontendOptions::ActionType Action;
  if (const Arg *A = Args.getLastArg(OPT_modes_Group)) {
    Option Opt = A->getOption();
    if (Opt.matches(OPT_emit_object)) {
      Action = FrontendOptions::EmitObject;
    } else if (Opt.matches(OPT_emit_assembly)) {
      Action = FrontendOptions::EmitAssembly;
    } else if (Opt.matches(OPT_emit_ir)) {
      Action = FrontendOptions::EmitIR;
    } else if (Opt.matches(OPT_emit_bc)) {
      Action = FrontendOptions::EmitBC;
    } else if (Opt.matches(OPT_emit_sil)) {
      Action = FrontendOptions::EmitSIL;
    } else if (Opt.matches(OPT_emit_silgen)) {
      Action = FrontendOptions::EmitSILGen;
    } else if (Opt.matches(OPT_parse)) {
      Action = FrontendOptions::Parse;
    } else if (Opt.matches(OPT_dump_parse)) {
      Action = FrontendOptions::DumpParse;
    } else if (Opt.matches(OPT_dump_ast)) {
      Action = FrontendOptions::DumpAST;
    } else if (Opt.matches(OPT_print_ast)) {
      Action = FrontendOptions::PrintAST;
    } else if (Opt.matches(OPT_repl) || Opt.matches(OPT_integrated_repl)) {
      Action = FrontendOptions::REPL;
    } else if (Opt.matches(OPT_interpret)) {
      Action = FrontendOptions::Immediate;
    } else {
      llvm_unreachable("Unhandled mode option");
    }
  } else {
    // We don't have a mode, so determine a default.
    if (Opts.InputFilenames.empty()) {
      // We don't have any input files, so default to the REPL.
      Action = FrontendOptions::REPL;
    } else if (Args.hasArg(OPT_emit_module, OPT_emit_module_path)) {
      // We've been told to emit a module, but have no other mode indicators.
      // As a result, put the frontend into EmitModuleOnly mode.
      // (Setting up module output will be handled below.)
      Action = FrontendOptions::EmitModuleOnly;
    } else {
      // In the absence of any other mode indicators, emit an object file.
      Action = FrontendOptions::EmitObject;
    }
  }
  Opts.RequestedAction = Action;

  if (Opts.RequestedAction == FrontendOptions::Immediate &&
      Opts.PrimaryInput.hasValue()) {
    Diags.diagnose(SourceLoc(), diag::error_immediate_mode_primary_file);
    return true;
  }

  bool TreatAsSIL = Args.hasArg(OPT_parse_sil);
  if (!TreatAsSIL && Opts.InputFilenames.size() == 1) {
    // If we have exactly one input filename, and its extension is "sil",
    // treat the input as SIL.
    StringRef Input(Opts.InputFilenames[0]);
    TreatAsSIL = llvm::sys::path::extension(Input).endswith(SIL_EXTENSION);
  }

  if (Opts.RequestedAction == FrontendOptions::REPL) {
    if (!Opts.InputFilenames.empty()) {
      Diags.diagnose(SourceLoc(), diag::error_repl_requires_no_input_files);
      return true;
    }
  } else if (TreatAsSIL) {
    if (Opts.InputFilenames.size() != 1) {
      Diags.diagnose(SourceLoc(), diag::error_mode_requires_one_input_file);
      return true;
    }
  } else {
    if (Opts.InputFilenames.empty()) {
      Diags.diagnose(SourceLoc(), diag::error_mode_requires_an_input_file);
      return true;
    }
  }

  if (Opts.RequestedAction == FrontendOptions::Immediate) {
    assert(!Opts.InputFilenames.empty());
    Opts.ImmediateArgv.push_back(Opts.InputFilenames[0]); // argv[0]
    if (const Arg *A = Args.getLastArg(OPT__DASH_DASH)) {
      for (unsigned i = 0, e = A->getNumValues(); i != e; ++i) {
        Opts.ImmediateArgv.push_back(A->getValue(i));
      }
    }
  }

  if (TreatAsSIL)
    Opts.InputKind = SourceFileKind::SIL;
  else if (Args.hasArg(OPT_parse_as_library))
    Opts.InputKind = SourceFileKind::Library;
  else if (Action == FrontendOptions::REPL)
    Opts.InputKind = SourceFileKind::REPL;
  else
    Opts.InputKind = SourceFileKind::Main;

  if (const Arg *A = Args.getLastArg(OPT_o)) {
    Opts.OutputFilename = A->getValue();
  }

  bool UserSpecifiedModuleName = false;
  {
    const Arg *A = Args.getLastArg(OPT_module_name);
    StringRef ModuleName = Opts.ModuleName;
    if (A) {
      ModuleName = A->getValue();
      UserSpecifiedModuleName = true;
    } else if (ModuleName.empty()) {
      // The user did not specify a module name, so determine a default fallback
      // based on other options.

      // Note: this code path will only be taken when running the frontend
      // directly; the driver should always pass -module-name when invoking the
      // frontend.
      if (Opts.RequestedAction == FrontendOptions::REPL) {
        // Default to a module named "REPL" if we're in REPL mode.
        ModuleName = "REPL";
      } else {
        StringRef OutputFilename(Opts.OutputFilename);
        if (OutputFilename.empty() || OutputFilename == "-" ||
            llvm::sys::fs::is_directory(OutputFilename)) {
          ModuleName = Opts.InputFilenames[0];
        } else {
          ModuleName = OutputFilename;
        }

        ModuleName = llvm::sys::path::stem(ModuleName);
      }
    }

    if (!Lexer::isIdentifier(ModuleName) ||
        (ModuleName == STDLIB_NAME && !Opts.ParseStdlib)) {
      if (!Opts.actionHasOutput() ||
          (Opts.InputKind == SourceFileKind::Main &&
           Opts.InputFilenames.size() == 1)) {
        ModuleName = "main";
      } else {
        auto DID = (ModuleName == STDLIB_NAME) ? diag::error_stdlib_module_name
                                               : diag::error_bad_module_name;
        Diags.diagnose(SourceLoc(), DID, ModuleName, A == nullptr);
        ModuleName = "__bad__";
      }
    }

    Opts.ModuleName = ModuleName;
  }

  if (Opts.OutputFilename.empty() ||
      llvm::sys::fs::is_directory(Opts.OutputFilename)) {
    // No output filename was specified, or an output directory was specified.
    // Determine the correct output filename.

    // Note: this should typically only be used when invoking the frontend
    // directly, as the driver will always pass -o with an appropriate filename
    // if output is required for the requested action.

    StringRef Suffix;
    switch (Opts.RequestedAction) {
    case FrontendOptions::Parse:
    case FrontendOptions::DumpParse:
    case FrontendOptions::DumpAST:
    case FrontendOptions::PrintAST:
      // Textual modes.
      Opts.OutputFilename = "-";
      break;

    case FrontendOptions::EmitSILGen:
    case FrontendOptions::EmitSIL: {
      if (Opts.OutputFilename.empty())
        Opts.OutputFilename = "-";
      else
        Suffix = SIL_EXTENSION;
      break;
    }

    case FrontendOptions::EmitModuleOnly:
      Suffix = SERIALIZED_MODULE_EXTENSION;
      break;

    case FrontendOptions::Immediate:
    case FrontendOptions::REPL:
      // These modes have no frontend-generated output.
      Opts.OutputFilename = "";
      break;

    case FrontendOptions::EmitAssembly: {
      if (Opts.OutputFilename.empty())
        Opts.OutputFilename = "-";
      else
        Suffix = "s";
      break;
    }

    case FrontendOptions::EmitIR: {
      if (Opts.OutputFilename.empty())
        Opts.OutputFilename = "-";
      else
        Suffix = "ll";
      break;
    }

    case FrontendOptions::EmitBC: {
      Suffix = "bc";
      break;
    }

    case FrontendOptions::EmitObject:
      Suffix = "o";
      break;
    }

    if (!Suffix.empty()) {
      // We need to deduce a file name.

      // First, if we're reading from stdin and we don't have a directory,
      // output to stdout.
      if (Opts.InputFilenames.size() == 1 && Opts.InputFilenames[0] == "-" &&
          Opts.OutputFilename.empty())
        Opts.OutputFilename = "-";
      else {
        // We have a suffix, so determine an appropriate name.
        llvm::SmallString<128> Path(Opts.OutputFilename);

        StringRef BaseName;
        if (Opts.PrimaryInput.hasValue() && Opts.PrimaryInput->isFilename()) {
          unsigned Index = Opts.PrimaryInput->Index;
          BaseName = llvm::sys::path::stem(Opts.InputFilenames[Index]);
        } else if (!UserSpecifiedModuleName &&
                   Opts.InputFilenames.size() == 1) {
          BaseName = llvm::sys::path::stem(Opts.InputFilenames[0]);
        } else {
          BaseName = Opts.ModuleName;
        }

        llvm::sys::path::append(Path, BaseName);
        llvm::sys::path::replace_extension(Path, Suffix);

        Opts.OutputFilename = Path.str();
      }
    }

    if (Opts.OutputFilename.empty()) {
      if (Opts.RequestedAction != FrontendOptions::REPL &&
          Opts.RequestedAction != FrontendOptions::Immediate) {
        Diags.diagnose(SourceLoc(), diag::error_no_output_filename_specified);
        return true;
      }
    } else if (Opts.OutputFilename != "-" &&
        llvm::sys::fs::is_directory(Opts.OutputFilename)) {
      Diags.diagnose(SourceLoc(), diag::error_implicit_output_file_is_directory,
                     Opts.OutputFilename);
      return true;
    }
  }

  auto determineOutputFilename = [&](std::string &output,
                                     OptSpecifier optWithoutPath,
                                     OptSpecifier optWithPath,
                                     const char *extension,
                                     bool useMainOutput) {
    if (const Arg *A = Args.getLastArg(optWithPath)) {
      Args.ClaimAllArgs(optWithoutPath);
      output = A->getValue();
      return;
    }

    if (!Args.hasArg(optWithoutPath))
      return;

    if (useMainOutput && !Opts.OutputFilename.empty()) {
      output = Opts.OutputFilename;
      return;
    }

    if (!output.empty())
      return;

    StringRef OriginalPath;
    if (!Opts.OutputFilename.empty() && Opts.OutputFilename != "-")
      // Put the serialized diagnostics file next to the output file.
      OriginalPath = Opts.OutputFilename;
    else if (Opts.PrimaryInput.hasValue() && Opts.PrimaryInput->isFilename())
      // We have a primary input, so use that as the basis for the name of the
      // serialized diagnostics file.
      OriginalPath = llvm::sys::path::filename(
        Opts.InputFilenames[Opts.PrimaryInput->Index]);
    else
      // We don't have any better indication of name, so fall back on the
      // module name.
      OriginalPath = Opts.ModuleName;

    llvm::SmallString<128> Path(OriginalPath);
    llvm::sys::path::replace_extension(Path, extension);
    output = Path.str();
  };

  determineOutputFilename(Opts.DependenciesFilePath,
                          OPT_emit_dependencies,
                          OPT_emit_dependencies_path,
                          "d", false);
  determineOutputFilename(Opts.SerializedDiagnosticsPath,
                          OPT_serialize_diagnostics,
                          OPT_serialize_diagnostics_path,
                          "dia", false);
  determineOutputFilename(Opts.ObjCHeaderOutputPath,
                          OPT_emit_objc_header,
                          OPT_emit_objc_header_path,
                          "h", false);

  bool canUseMainOutputForModule =
    Opts.RequestedAction == FrontendOptions::EmitModuleOnly;
  determineOutputFilename(Opts.ModuleOutputPath,
                          OPT_emit_module,
                          OPT_emit_module_path,
                          SERIALIZED_MODULE_EXTENSION,
                          canUseMainOutputForModule);

  determineOutputFilename(Opts.ModuleDocOutputPath,
                          OPT_emit_module_doc,
                          OPT_emit_module_doc_path,
                          SERIALIZED_MODULE_DOC_EXTENSION,
                          false);

  if (!Opts.DependenciesFilePath.empty()) {
    switch (Opts.RequestedAction) {
    case FrontendOptions::DumpParse:
    case FrontendOptions::DumpAST:
    case FrontendOptions::PrintAST:
    case FrontendOptions::Immediate:
    case FrontendOptions::REPL:
      Diags.diagnose(SourceLoc(), diag::error_mode_cannot_emit_dependencies);
      return true;
    case FrontendOptions::Parse:
    case FrontendOptions::EmitModuleOnly:
    case FrontendOptions::EmitSILGen:
    case FrontendOptions::EmitSIL:
    case FrontendOptions::EmitIR:
    case FrontendOptions::EmitBC:
    case FrontendOptions::EmitAssembly:
    case FrontendOptions::EmitObject:
      break;
    }
  }

  if (!Opts.ObjCHeaderOutputPath.empty()) {
    switch (Opts.RequestedAction) {
    case FrontendOptions::DumpParse:
    case FrontendOptions::DumpAST:
    case FrontendOptions::PrintAST:
    case FrontendOptions::Immediate:
    case FrontendOptions::REPL:
      Diags.diagnose(SourceLoc(), diag::error_mode_cannot_emit_header);
      return true;
    case FrontendOptions::Parse:
    case FrontendOptions::EmitModuleOnly:
    case FrontendOptions::EmitSILGen:
    case FrontendOptions::EmitSIL:
    case FrontendOptions::EmitIR:
    case FrontendOptions::EmitBC:
    case FrontendOptions::EmitAssembly:
    case FrontendOptions::EmitObject:
      break;
    }
  }

  if (!Opts.ModuleOutputPath.empty() ||
      !Opts.ModuleDocOutputPath.empty()) {
    switch (Opts.RequestedAction) {
    case FrontendOptions::Parse:
    case FrontendOptions::DumpParse:
    case FrontendOptions::DumpAST:
    case FrontendOptions::PrintAST:
    case FrontendOptions::EmitSILGen:
    case FrontendOptions::Immediate:
    case FrontendOptions::REPL:
      if (!Opts.ModuleOutputPath.empty())
        Diags.diagnose(SourceLoc(), diag::error_mode_cannot_emit_module);
      else
        Diags.diagnose(SourceLoc(), diag::error_mode_cannot_emit_module_doc);
      return true;
    case FrontendOptions::EmitModuleOnly:
    case FrontendOptions::EmitSIL:
    case FrontendOptions::EmitIR:
    case FrontendOptions::EmitBC:
    case FrontendOptions::EmitAssembly:
    case FrontendOptions::EmitObject:
      break;
    }
  }

  if (const Arg *A = Args.getLastArg(OPT_module_link_name)) {
    Opts.ModuleLinkName = A->getValue();
  }

  Opts.EnableSourceImport |= Args.hasArg(OPT_enable_source_import);
  Opts.SILSerializeAll |= Args.hasArg(OPT_sil_serialize_all);
  Opts.ImportUnderlyingModule |= Args.hasArg(OPT_import_underlying_module);

  if (const Arg *A = Args.getLastArg(OPT_import_objc_header)) {
    Opts.ImplicitObjCHeaderPath = A->getValue();
    Opts.SerializeBridgingHeader |=
      !Opts.PrimaryInput && !Opts.ModuleOutputPath.empty();
  }

  for (const Arg *A : make_range(Args.filtered_begin(OPT_Xllvm),
                                 Args.filtered_end())) {
    Opts.LLVMArgs.push_back(A->getValue());
  }

  return false;
}

static bool ParseLangArgs(LangOptions &Opts, ArgList &Args,
                          DiagnosticEngine &Diags) {
  using namespace options;

  Opts.UseMalloc |= Args.hasArg(OPT_use_malloc);

  Opts.EnableExperimentalPatterns |= Args.hasArg(OPT_enable_experimental_patterns);

  Opts.EnableCharacterLiterals |= Args.hasArg(OPT_enable_character_literals);

  if (auto A = Args.getLastArg(OPT_enable_access_control,
                               OPT_disable_access_control)) {
    Opts.EnableAccessControl
      = A->getOption().matches(OPT_enable_access_control);
  }
  
  Opts.ImportUnions |= Args.hasArg(OPT_enable_union_import);
  
  Opts.EnableDynamic |= Args.hasArg(OPT_enable_dynamic);

  Opts.EnableOptionalLValues |= Args.hasArg(OPT_enable_optional_lvalues);

  Opts.DebugConstraintSolver |= Args.hasArg(OPT_debug_constraints);

  Opts.DebuggerSupport |= Args.hasArg(OPT_debugger_support);
  
  if (auto A = Args.getLastArg(OPT_enable_objc_attr_requires_objc_module,
                               OPT_disable_objc_attr_requires_objc_module)) {
    Opts.EnableObjCAttrRequiresObjCModule
      = A->getOption().matches(OPT_enable_objc_attr_requires_objc_module);
  }

  if (const Arg *A = Args.getLastArg(OPT_debug_constraints_attempt)) {
    unsigned attempt;
    if (StringRef(A->getValue()).getAsInteger(10, attempt)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }

    Opts.DebugConstraintSolverAttempt = attempt;
  }
  
  for (const Arg *A : make_range(Args.filtered_begin(OPT_D),
                                 Args.filtered_end())) {
    Opts.addBuildConfigOption(A->getValue());
  }

  Opts.EnableAppExtensionRestrictions |= Args.hasArg(OPT_enable_app_extension);
  Opts.SplitPrepositions |= Args.hasArg(OPT_split_objc_selectors);

  if (auto A = Args.getLastArg(OPT_implicit_objc_with,
                               OPT_no_implicit_objc_with)) {
    Opts.ImplicitObjCWith
      = A->getOption().matches(OPT_implicit_objc_with);
  }

  if (Opts.SplitPrepositions)
    Opts.addBuildConfigOption("OBJC_SELECTOR_SPLITTING");

  return false;
}

static bool ParseClangImporterArgs(ClangImporterOptions &Opts, ArgList &Args,
                                   DiagnosticEngine &Diags) {
  using namespace options;

  if (const Arg *A = Args.getLastArg(OPT_module_cache_path)) {
    Opts.ModuleCachePath = A->getValue();
  }

  for (const Arg *A : make_range(Args.filtered_begin(OPT_Xcc),
                                 Args.filtered_end())) {
    Opts.ExtraArgs.push_back(A->getValue());
  }

  Opts.InferImplicitProperties |=
    Args.hasArg(OPT_enable_objc_implicit_properties);

  Opts.DumpClangDiagnostics |= Args.hasArg(OPT_dump_clang_diagnostics);

  return false;
}

static bool ParseSearchPathArgs(SearchPathOptions &Opts, ArgList &Args,
                                DiagnosticEngine &Diags) {
  using namespace options;

  for (const Arg *A : make_range(Args.filtered_begin(OPT_I),
                                 Args.filtered_end())) {
    Opts.ImportSearchPaths.push_back(A->getValue());
  }

  for (const Arg *A : make_range(Args.filtered_begin(OPT_F),
                                 Args.filtered_end())) {
    Opts.FrameworkSearchPaths.push_back(A->getValue());
  }

  if (const Arg *A = Args.getLastArg(OPT_sdk))
    Opts.SDKPath = A->getValue();

  if (const Arg *A = Args.getLastArg(OPT_resource_dir))
    Opts.RuntimeResourcePath = A->getValue();

  Opts.SkipRuntimeLibraryImportPath |= Args.hasArg(OPT_nostdimport);

  // Opts.RuntimeIncludePath is set by calls to
  // setRuntimeIncludePath() or setMainExecutablePath().
  // Opts.RuntimeImportPath is set by calls to
  // setRuntimeIncludePath() or setMainExecutablePath() and 
  // updated by calls to setTargetTriple() or parseArgs().
  // Assumes exactly one of setMainExecutablePath() or setRuntimeIncludePath() 
  // is called before setTargetTriple() and parseArgs().
  // TODO: improve the handling of RuntimeIncludePath.

  return false;
}

static bool ParseDiagnosticArgs(DiagnosticOptions &Opts, ArgList &Args,
                                DiagnosticEngine &Diags) {
  using namespace options;

  Opts.VerifyDiagnostics |= Args.hasArg(OPT_verify);
  Opts.SkipDiagnosticPasses |= Args.hasArg(OPT_disable_diagnostic_passes);
  Opts.ShowDiagnosticsAfterFatalError |=
    Args.hasArg(OPT_show_diagnostics_after_fatal);
  Opts.UseColor |= Args.hasArg(OPT_color_diagnostics);

  return false;
}

// Lifted from the clang driver.
static void PrintArg(raw_ostream &OS, const char *Arg, bool Quote) {
  const bool Escape = std::strpbrk(Arg, "\"\\$ ");

  if (!Quote && !Escape) {
    OS << Arg;
    return;
  }

  // Quote and escape. This isn't really complete, but good enough.
  OS << '"';
  while (const char c = *Arg++) {
    if (c == '"' || c == '\\' || c == '$')
      OS << '\\';
    OS << c;
  }
  OS << '"';
}

static bool ParseSILArgs(SILOptions &Opts, ArgList &Args,
                         IRGenOptions &IRGenOpts,
                         FrontendOptions &FEOpts,
                         DiagnosticEngine &Diags) {
  using namespace options;

  if (const Arg *A = Args.getLastArg(OPT_sil_inline_threshold)) {
    if (StringRef(A->getValue()).getAsInteger(10, Opts.InlineThreshold)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  }
  if (const Arg *A = Args.getLastArg(OPT_sil_devirt_threshold)) {
    if (StringRef(A->getValue()).getAsInteger(10, Opts.DevirtThreshold)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  }
  if (const Arg *A = Args.getLastArg(OPT_sil_opt_pass_count)) {
    if (StringRef(A->getValue()).getAsInteger(10, Opts.NumOptPassesToRun)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  }
  
  if (const Arg *A = Args.getLastArg(OPT_disable_sil_linking,
                                     OPT_sil_link_all)) {
    if (A->getOption().matches(OPT_disable_sil_linking))
      Opts.LinkMode = SILOptions::LinkNone;
    else if (A->getOption().matches(OPT_sil_link_all))
      Opts.LinkMode = SILOptions::LinkAll;
    else
      llvm_unreachable("Unknown SIL linking option!");
  }

  // Parse the optimization level.
  if (const Arg *A = Args.getLastArg(OPT_O_Group)) {
    // The maximum optimization level we currently support.
    unsigned MaxLevel = 3;

    if (A->getOption().matches(OPT_O0)) {
      IRGenOpts.OptLevel = 0;
    } else if (A->getOption().matches(OPT_Ounchecked)) {
      // Set the maximum optimization level and remove all runtime checks.
      IRGenOpts.OptLevel = MaxLevel;
      // Removal of cond_fail (overflow on binary operations).
      Opts.RemoveRuntimeAsserts = true;
      Opts.AssertConfig = SILOptions::Fast;
    } else if (!StringRef(A->getValue()).size()) {
      // -O is an alias to -O3.
      IRGenOpts.OptLevel = MaxLevel;
    } else {
      unsigned OptLevel;
      if (StringRef(A->getValue()).getAsInteger(10, OptLevel) ||
          OptLevel > MaxLevel) {
        Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                       A->getAsString(Args), A->getValue());
        return true;
      }

      IRGenOpts.OptLevel = OptLevel;
    }
  }

  // Parse the build configuration identifier.
  if (const Arg *A = Args.getLastArg(OPT_AssertConfig)) {
    // We currently understand build configuration up to 3 of which we only use
    // 0 and 1 in the standard library.
    StringRef Configuration = A->getValue();
    if (Configuration == "DisableReplacement") {
      Opts.AssertConfig = SILOptions::DisableReplacement;
    } else if (Configuration == "Debug") {
      Opts.AssertConfig = SILOptions::Debug;
    } else if (Configuration == "Release") {
      Opts.AssertConfig = SILOptions::Release;
    } else if (Configuration == "Fast") {
      Opts.AssertConfig = SILOptions::Fast;
    } else {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  } else if (FEOpts.ParseStdlib) {
    // Disable assertion configuration replacement when we build the standard
    // library.
    Opts.AssertConfig = SILOptions::DisableReplacement;
  } else if (Opts.AssertConfig == SILOptions::Debug) {
    // Set the assert configuration according to the optimization level if it
    // has not been set by the -Ounchecked flag.
    Opts.AssertConfig =
        IRGenOpts.OptLevel > 0 ? SILOptions::Release : SILOptions::Debug;
  }

  // -Ounchecked might also set removal of runtime asserts (cond_fail).
  Opts.RemoveRuntimeAsserts |= Args.hasArg(OPT_remove_runtime_asserts);

  Opts.EnableARCOptimizations |= !Args.hasArg(OPT_disable_arc_opts);
  Opts.VerifyAll |= Args.hasArg(OPT_sil_verify_all);
  Opts.PrintAll |= Args.hasArg(OPT_sil_print_all);
  Opts.TimeTransforms |= Args.hasArg(OPT_sil_time_transforms);
  Opts.DebugSerialization |= Args.hasArg(OPT_sil_debug_serialization);
  Opts.PrintInstCounts |= Args.hasArg(OPT_print_inst_counts);

  return false;
}

void CompilerInvocation::buildDWARFDebugFlags(std::string &Output,
                                              const ArrayRef<const char*> &Args,
                                              StringRef SDKPath,
                                              StringRef ResourceDir) {
  llvm::raw_string_ostream OS(Output);
  interleave(Args,
             [&](const char *Argument) { PrintArg(OS, Argument, false); },
             [&] { OS << " "; });

  // Inject the SDK path and resource dir if they are nonempty and missing.
  bool haveSDKPath = SDKPath.empty();
  bool haveResourceDir = ResourceDir.empty();
  for (auto A : Args) {
    StringRef Arg(A);
    // FIXME: this should distinguish between key and value.
    if (!haveSDKPath && Arg.equals("-sdk"))
      haveSDKPath = true;
    if (!haveResourceDir && Arg.equals("-resource-dir"))
      haveResourceDir = true;
  }
  if (!haveSDKPath) {
    OS << " -sdk ";
    PrintArg(OS, SDKPath.data(), false);
  }
  if (!haveResourceDir) {
    OS << " -resource-dir ";
    PrintArg(OS, ResourceDir.data(), false);
  }
}

static bool ParseIRGenArgs(IRGenOptions &Opts, ArgList &Args,
                           DiagnosticEngine &Diags,
                           const FrontendOptions &FrontendOpts,
                           StringRef SDKPath,
                           StringRef ResourceDir) {
  using namespace options;

  if (Args.hasArg(OPT_g)) {
    Opts.DebugInfo = true;
    ArgStringList RenderedArgs;
    for (auto A : Args)
      A->render(Args, RenderedArgs);
    CompilerInvocation::buildDWARFDebugFlags(Opts.DWARFDebugFlags,
                                             RenderedArgs, SDKPath,
                                             ResourceDir);
  }

  for (const Arg *A : make_range(Args.filtered_begin(OPT_l, OPT_framework),
                                 Args.filtered_end())) {
    LibraryKind Kind;
    if (A->getOption().matches(OPT_l)) {
      Kind = LibraryKind::Library;
    } else if (A->getOption().matches(OPT_framework)) {
      Kind = LibraryKind::Framework;
    } else {
      llvm_unreachable("Unknown LinkLibrary option kind");
    }

    Opts.LinkLibraries.push_back(LinkLibrary(A->getValue(), Kind));
  }

  if (const Arg *A = Args.getLastArg(OPT_target_cpu))
    Opts.TargetCPU = A->getValue();
  if (const Arg *A = Args.getLastArg(OPT_target_abi))
    Opts.TargetABI = A->getValue();

  for (const Arg *A : make_range(Args.filtered_begin(OPT_target_feature),
                                 Args.filtered_end())) {
    Opts.TargetFeatures.push_back(A->getValue());
  }

  Opts.DisableLLVMOptzns |= Args.hasArg(OPT_disable_llvm_optzns);
  Opts.DisableLLVMARCOpts |= Args.hasArg(OPT_disable_llvm_arc_opts);
  Opts.EnableDynamicValueTypeLayout |=
    Args.hasArg(OPT_enable_dynamic_value_type_layout);
  Opts.HasUnderlyingModule |= Args.hasArg(OPT_import_underlying_module);

  if (Args.hasArg(OPT_autolink_force_load))
    Opts.ForceLoadSymbolName = Args.getLastArgValue(OPT_module_link_name);

  if (const Arg *A = Args.getLastArg(OPT_target))
    Opts.Triple = llvm::Triple::normalize(A->getValue());

  // TODO: investigate whether these should be removed, in favor of definitions
  // in other classes.
  if (FrontendOpts.PrimaryInput && FrontendOpts.PrimaryInput->isFilename()) {
    unsigned Index = FrontendOpts.PrimaryInput->Index;
    Opts.MainInputFilename = FrontendOpts.InputFilenames[Index];
  } else if (FrontendOpts.InputFilenames.size() == 1) {
    Opts.MainInputFilename = FrontendOpts.InputFilenames.front();
  }
  Opts.OutputFilename = FrontendOpts.OutputFilename;
  Opts.ModuleName = FrontendOpts.ModuleName;

  if (Args.hasArg(OPT_playground))
    Opts.Playground = true;
  
  return false;
}

bool CompilerInvocation::parseArgs(ArrayRef<const char *> Args,
                                   DiagnosticEngine &Diags) {
  using namespace options;

  if (Args.empty())
    return false;

  // Parse frontend command line options using Swift's option table.
  std::unique_ptr<llvm::opt::InputArgList> ParsedArgs;
  std::unique_ptr<llvm::opt::OptTable> Table = createSwiftOptTable();
  unsigned MissingIndex;
  unsigned MissingCount;
  ParsedArgs.reset(
      Table->ParseArgs(Args.begin(), Args.end(), MissingIndex, MissingCount,
                       FrontendOption));
  if (MissingCount) {
    Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                   ParsedArgs->getArgString(MissingIndex), MissingCount);
    return true;
  }

  if (ParsedArgs->hasArg(OPT_UNKNOWN)) {
    for (const Arg *A : make_range(ParsedArgs->filtered_begin(OPT_UNKNOWN),
                                   ParsedArgs->filtered_end())) {
      Diags.diagnose(SourceLoc(), diag::error_unknown_arg,
                     A->getAsString(*ParsedArgs));
    }
    return true;
  }

  if (ParseFrontendArgs(FrontendOpts, *ParsedArgs, Diags)) {
    return true;
  }

  if (ParseLangArgs(LangOpts, *ParsedArgs, Diags)) {
    return true;
  }

  if (ParseClangImporterArgs(ClangImporterOpts, *ParsedArgs, Diags)) {
    return true;
  }

  if (ParseSearchPathArgs(SearchPathOpts, *ParsedArgs, Diags)) {
    return true;
  }

  if (ParseSILArgs(SILOpts, *ParsedArgs, IRGenOpts, FrontendOpts, Diags)) {
    return true;
  }

  if (ParseIRGenArgs(IRGenOpts, *ParsedArgs, Diags, FrontendOpts,
                     getSDKPath(), SearchPathOpts.RuntimeResourcePath)) {
    return true;
  }

  if (ParseDiagnosticArgs(DiagnosticOpts, *ParsedArgs, Diags)) {
    return true;
  }

  updateRuntimeLibraryPath();

  return false;
}
