// src/main.cpp
#include "FlexConfig.h"
#include "FlexPassConfigCallback.h"
#include "clang/Basic/DiagnosticFrontend.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/CodeGen/ObjectFilePCHContainerWriter.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Serialization/ObjectFilePCHContainerReader.h"
#include <set>
#include <vector>
#include "llvm/ADT/Any.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/IOSandbox.h"
#include "llvm/Support/Process.h"
#include "llvm/TargetParser/Host.h"

using namespace clang;
using namespace llvm;

static int flexclang_cc1_main(SmallVectorImpl<const char *> &ArgV) {
  cl::ResetAllOptionOccurrences();

  int cc1Argc = ArgV.size() - 1;
  const char **cc1Argv = ArgV.data() + 1;

  auto PCHOps = std::make_shared<PCHContainerOperations>();
  PCHOps->registerWriter(std::make_unique<ObjectFilePCHContainerWriter>());
  PCHOps->registerReader(std::make_unique<ObjectFilePCHContainerReader>());

  IntrusiveRefCntPtr<DiagnosticIDs> DiagID = DiagnosticIDs::create();
  DiagnosticOptions DiagOpts;
  TextDiagnosticBuffer *DiagsBuffer = new TextDiagnosticBuffer;
  DiagnosticsEngine Diags(DiagID, DiagOpts, DiagsBuffer);

  SmallVector<const char *, 256> clangArgs;
  flexclang::FlexConfig config =
      flexclang::parseFlexArgs(clangArgs, cc1Argc, cc1Argv);

  if (!config.configFile.empty()) {
    if (!flexclang::parseFlexYAML(config, config.configFile))
      return 1;
  }

  if (config.dryRun) {
    config.printDryRun();
    return 0;
  }

  // Shared buffer for MIR pass names (populated by MIRPassListPrinter during
  // compilation, printed after compilation together with IR pass names).
  auto mirPassNames = std::make_shared<std::vector<std::string>>();

  if (config.hasModifications() || config.listPasses) {
    flexclang::registerFlexPassConfigCallback(config, mirPassNames);
  }

  ArrayRef<const char *> Args(clangArgs);
  auto Invocation = std::make_shared<CompilerInvocation>();
  bool Success =
      CompilerInvocation::CreateFromArgs(*Invocation, Args, Diags, ArgV[0]);

  auto Clang =
      std::make_unique<CompilerInstance>(std::move(Invocation), std::move(PCHOps));

  auto VFS = [] {
    auto BypassSandbox = llvm::sys::sandbox::scopedDisable();
    return vfs::getRealFileSystem();
  }();
  Clang->createVirtualFileSystem(std::move(VFS), DiagsBuffer);
  Clang->createDiagnostics();

  install_fatal_error_handler(
      [](void *UserData, const char *Message, bool) {
        auto &D = *static_cast<DiagnosticsEngine *>(UserData);
        D.Report(diag::err_fe_error_backend) << Message;
        exit(1);
      },
      static_cast<void *>(&Clang->getDiagnostics()));

  DiagsBuffer->FlushDiagnostics(Clang->getDiagnostics());
  if (!Success) return 1;

  // IR/MIR pass listing is handled via callbacks:
  //  - MIR: MIRPassListPrinter in FlexPassConfigCallback (introspects FPPassManager)
  //  - IR:  registerBeforeNonSkippedPassCallback below

  // Shared state for IR pass collection (used by both list-passes and disable)
  auto irPassNames = std::make_shared<std::vector<std::string>>();
  auto irPassNamesSeen = std::make_shared<std::set<std::string>>();

  // IR pass modifications via PassBuilderCallbacks
  std::vector<std::string> irDisable;
  auto irMatched = std::make_shared<std::set<std::string>>();
  {
    std::vector<std::string> irPlugins;
    for (const auto &r : config.irRules) {
      if (r.action == flexclang::IRPassRule::Disable)
        irDisable.push_back(r.target);
      else if (r.action == flexclang::IRPassRule::LoadPlugin)
        irPlugins.push_back(r.plugin);
    }

    bool needCallbacks = !irDisable.empty() || !irPlugins.empty() ||
                         config.listPasses;
    if (needCallbacks) {
      bool verbose = config.verbose;
      bool listPasses = config.listPasses;
      auto irDisableShared = std::make_shared<std::vector<std::string>>(irDisable);
      Clang->getCodeGenOpts().PassBuilderCallbacks.push_back(
          [irDisableShared, irPlugins, verbose, irMatched, listPasses,
           irPassNames, irPassNamesSeen](PassBuilder &PB) {
            auto *PIC = PB.getPassInstrumentationCallbacks();
            if (PIC) {
              if (!irDisableShared->empty()) {
                PIC->registerShouldRunOptionalPassCallback(
                    [irDisableShared, verbose, irMatched,
                     PIC](StringRef Name, Any) {
                      StringRef PipelineName =
                          PIC->getPassNameForClassName(Name);
                      for (const auto &d : *irDisableShared) {
                        StringRef D(d);
                        if (D.equals_insensitive(PipelineName) ||
                            D.equals_insensitive(Name)) {
                          irMatched->insert(d);
                          if (verbose)
                            errs() << "flexclang: skipping IR pass '"
                                   << Name << "' (" << PipelineName << ")\n";
                          return false;
                        }
                      }
                      return true;
                    });
              }
              // Collect IR pass names for --flex-list-passes
              if (listPasses) {
                PIC->registerBeforeNonSkippedPassCallback(
                    [irPassNames, irPassNamesSeen,
                     PIC](StringRef Name, Any) {
                      StringRef PipelineName =
                          PIC->getPassNameForClassName(Name);
                      std::string key = PipelineName.str();
                      if (irPassNamesSeen->insert(key).second)
                        irPassNames->push_back(key);
                    });
              }
            }
            for (const auto &path : irPlugins) {
              auto Plugin = PassPlugin::Load(path);
              if (Plugin) {
                Plugin->registerPassBuilderCallbacks(PB);
              } else {
                errs() << "flexclang: error: "
                       << toString(Plugin.takeError()) << "\n";
              }
            }
          });
    }
  }

  Success = ExecuteCompilerInvocation(Clang.get());
  remove_fatal_error_handler();

  // Print pass listing: IR first (runs before MIR), then MIR.
  if (config.listPasses) {
    errs() << "\n=== IR Optimization Pipeline ===\n";
    int irIdx = 1;
    for (const auto &name : *irPassNames)
      errs() << "  [ir." << irIdx++ << "]  " << name << "\n";
    errs() << "\nUse --flex-disable-ir-pass=<pass-name> to disable any IR pass "
              "listed above.\n";

    errs() << "\n=== MIR Codegen Pipeline ===\n";
    int mirIdx = 1;
    for (const auto &name : *mirPassNames)
      errs() << "  [mir." << mirIdx++ << "]  " << name << "\n";
    errs() << "\nUse --flex-disable-pass=<name> or "
              "--flex-insert-after=<name>:<plugin.so>\n";
  }

  // Warn about IR passes that were requested for disabling but never matched.
  // This typically means the pass is required (isRequired() == true) and cannot
  // be skipped by shouldRunOptionalPassCallback, or the name was misspelled.
  for (const auto &d : irDisable) {
    if (!irMatched->count(d))
      errs() << "flexclang: warning: IR pass '" << d
             << "' was not disabled (pass may be required or name misspelled)\n";
  }

  return Success ? 0 : 1;
}

/// Check if a Command's arguments contain -triple amdgcn*.
static bool hasAMDGCNTriple(const llvm::opt::ArgStringList &Args) {
  for (size_t i = 0; i + 1 < Args.size(); ++i) {
    if (StringRef(Args[i]) == "-triple" &&
        StringRef(Args[i + 1]).starts_with("amdgcn"))
      return true;
  }
  return false;
}

static int flexclang_driver_main(int argc, const char **argv) {
  // Strip --flex-* flags, keep driver args
  SmallVector<const char *, 256> driverArgs;
  driverArgs.push_back(argv[0]); // program name for Driver
  flexclang::FlexConfig config =
      flexclang::parseFlexArgs(driverArgs, argc, argv);

  if (!config.configFile.empty()) {
    if (!flexclang::parseFlexYAML(config, config.configFile))
      return 1;
  }

  // Handle --flex-dry-run
  if (config.dryRun) {
    config.printDryRun();
    return 0;
  }

  // Resolve executable path
  void *MainAddr = (void *)(intptr_t)flexclang_driver_main;
  std::string ExePath =
      llvm::sys::fs::getMainExecutable(argv[0], MainAddr);

  // Create driver diagnostics
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID = DiagnosticIDs::create();
  DiagnosticOptions DiagOpts;
  auto *DiagPrinter = new TextDiagnosticPrinter(errs(), DiagOpts);
  DiagnosticsEngine Diags(DiagID, DiagOpts, DiagPrinter);

  // Create the Driver
  clang::driver::Driver TheDriver(ExePath,
                                   llvm::sys::getDefaultTargetTriple(),
                                   Diags, "flexclang LLVM compiler");

  // In-process cc1 execution
  TheDriver.CC1Main = [](SmallVectorImpl<const char *> &ArgV) {
    return flexclang_cc1_main(ArgV);
  };
  CrashRecoveryContext::Enable();

  // Build compilation from driver args
  std::unique_ptr<clang::driver::Compilation> C(
      TheDriver.BuildCompilation(driverArgs));
  if (!C)
    return 1;

  // Inject --flex-* flags into AMDGCN cc1 commands
  if (!config.originalFlexArgs.empty()) {
    for (auto &Job : C->getJobs()) {
      if (!hasAMDGCNTriple(Job.getArguments()))
        continue;
      auto Args = Job.getArguments();
      for (const auto &FlexArg : config.originalFlexArgs)
        Args.push_back(C->getArgs().MakeArgString(FlexArg));
      Job.replaceArguments(std::move(Args));
    }
  }

  // Execute
  SmallVector<std::pair<int, const clang::driver::Command *>, 4> FailingCommands;
  int Res = TheDriver.ExecuteCompilation(*C, FailingCommands);
  return Res;
}

int main(int argc, const char **argv) {
  InitLLVM X(argc, argv);

  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  if (argc >= 2 && StringRef(argv[1]) == "-cc1") {
    SmallVector<const char *, 256> ArgV(argv, argv + argc);
    return flexclang_cc1_main(ArgV);
  }

  return flexclang_driver_main(argc, argv);
}
