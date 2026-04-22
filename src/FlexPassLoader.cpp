#include "FlexPassLoader.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

Pass *loadMIRPassPlugin(StringRef soPath, StringRef configPath, bool verbose,
                        StringRef programName) {
  std::string errMsg;
  auto Lib =
      sys::DynamicLibrary::getPermanentLibrary(soPath.str().c_str(), &errMsg);
  if (!Lib.isValid()) {
    errs() << programName << ": error: cannot load '" << soPath
           << "': " << errMsg << "\n";
    return nullptr;
  }

  // Try parameterized factory if plugin-specific config is provided.
  if (!configPath.empty()) {
    using CreateWithConfigFn = MachineFunctionPass *(*)(const char *);
    auto *CreateWithConfig = reinterpret_cast<CreateWithConfigFn>(
        Lib.getAddressOfSymbol("flexclangCreatePassWithConfig"));
    if (CreateWithConfig) {
      auto Buf = MemoryBuffer::getFile(configPath);
      if (!Buf) {
        errs() << programName << ": error: cannot read plugin config '"
               << configPath << "': " << Buf.getError().message() << "\n";
        return nullptr;
      }
      Pass *P = CreateWithConfig((*Buf)->getBuffer().str().c_str());
      if (!P) {
        errs() << programName << ": error: flexclangCreatePassWithConfig returned null\n";
        return nullptr;
      }
      return P;
    }
    errs() << programName << ": warning: config specified but plugin '"
           << soPath << "' does not export flexclangCreatePassWithConfig\n";
  }

  // Fall back to simple factory.
  using CreatePassFn = MachineFunctionPass *(*)();
  auto *CreatePass = reinterpret_cast<CreatePassFn>(
      Lib.getAddressOfSymbol("flexclangCreatePass"));
  if (!CreatePass) {
    errs() << programName << ": error: '" << soPath
           << "' exports neither flexclangCreatePass nor flexclangCreatePassWithConfig\n";
    return nullptr;
  }

  Pass *P = CreatePass();
  if (!P) {
    errs() << programName << ": error: flexclangCreatePass returned null in '"
           << soPath << "'\n";
    return nullptr;
  }

  // Print name if available and verbose is enabled.
  if (verbose) {
    using PassNameFn = const char *(*)();
    auto *GetName = reinterpret_cast<PassNameFn>(
        Lib.getAddressOfSymbol("flexclangPassName"));
    if (GetName)
      errs() << programName << ": loaded MIR plugin '" << GetName() << "'\n";
  }

  return P;
}

} // namespace flexclang
