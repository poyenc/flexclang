// src/main.cpp
#include "FlexConfig.h"
#include "clang/Basic/DiagnosticFrontend.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/CodeGen/ObjectFilePCHContainerWriter.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Serialization/ObjectFilePCHContainerReader.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace llvm;

int main(int argc, const char **argv) {
  InitLLVM X(argc, argv);

  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  auto PCHOps = std::make_shared<PCHContainerOperations>();
  PCHOps->registerWriter(std::make_unique<ObjectFilePCHContainerWriter>());
  PCHOps->registerReader(std::make_unique<ObjectFilePCHContainerReader>());

  IntrusiveRefCntPtr<DiagnosticIDs> DiagID = DiagnosticIDs::create();
  DiagnosticOptions DiagOpts;
  TextDiagnosticBuffer *DiagsBuffer = new TextDiagnosticBuffer;
  DiagnosticsEngine Diags(DiagID, DiagOpts, DiagsBuffer);

  SmallVector<const char *, 256> clangArgs;
  flexclang::FlexConfig config =
      flexclang::parseFlexArgs(clangArgs, argc, argv);

  if (!config.configFile.empty()) {
    if (!flexclang::parseFlexYAML(config, config.configFile))
      return 1;
  }

  if (config.dryRun && config.hasModifications()) {
    for (const auto &r : config.mirRules) {
      const char *acts[] = {"disable", "replace", "insert-after"};
      errs() << "flexclang: [dry-run] MIR " << acts[r.action]
             << " target='" << r.target << "'";
      if (!r.plugin.empty()) errs() << " plugin=" << r.plugin;
      errs() << "\n";
    }
    for (const auto &r : config.irRules) {
      const char *acts[] = {"disable", "load-plugin"};
      errs() << "flexclang: [dry-run] IR " << acts[r.action]
             << " target='" << r.target << "'";
      if (!r.plugin.empty()) errs() << " plugin=" << r.plugin;
      errs() << "\n";
    }
    return 0;
  }

  ArrayRef<const char *> Args(clangArgs);
  auto Invocation = std::make_shared<CompilerInvocation>();
  bool Success =
      CompilerInvocation::CreateFromArgs(*Invocation, Args, Diags, argv[0]);

  auto Clang =
      std::make_unique<CompilerInstance>(std::move(Invocation), std::move(PCHOps));

  auto VFS = vfs::getRealFileSystem();
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

  Success = ExecuteCompilerInvocation(Clang.get());
  remove_fatal_error_handler();
  return Success ? 0 : 1;
}
