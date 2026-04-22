# flexclang Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Build flexclang, a drop-in clang replacement that lets AMDGPU HIP kernel developers disable, replace, and insert MIR/IR passes via CLI flags and YAML config.

**Architecture:** flexclang links against installed LLVM/Clang libraries (no fork). It mirrors `cc1_main()` but registers a `RegisterTargetPassConfigCallback` to intercept the MIR codegen pipeline, and uses `PassBuilder::getPassInstrumentationCallbacks()` for IR pass control. MIR pass plugins are loaded as `.so` via `DynamicLibrary`.

**Tech Stack:** C++17, LLVM/Clang libraries (amd-staging, LLVM 21+), CMake 3.20+, LLVM YAML I/O

**Design Spec:** `docs/superpowers/specs/2026-04-21-flexclang-design.md`

**LLVM Source:** `/home/poyechen/workspace/repo/llvm-project` (ROCm/llvm-project, amd-staging branch)

---

## File Structure

```
friendly-clang/
  CMakeLists.txt                          # Top-level build config
  src/
    main.cpp                              # Entry point (mirrors cc1_main, adds flex hooks)
    FlexConfig.h                          # FlexConfig struct + parser declarations
    FlexConfig.cpp                        # CLI parsing + YAML parsing
    FlexPassLoader.h                      # Dynamic .so loader declarations
    FlexPassLoader.cpp                    # DynamicLibrary-based pass loading
    FlexPassConfigCallback.h              # RegisterTargetPassConfigCallback declarations
    FlexPassConfigCallback.cpp            # MIR pass interception via TargetPassConfig API
    FlexPassNameRegistry.h                # Pass name string -> AnalysisID resolver
    FlexPassNameRegistry.cpp              # PassRegistry-based resolution
  examples/
    test_kernel.hip                       # Test HIP kernel with MFMA builtins
    validate.sh                           # End-to-end validation script
    ir-pass-counter/
      IRInstCounter.cpp                   # Example IR pass plugin
      CMakeLists.txt
    mir-pass-nop-inserter/
      MIRNopInserter.cpp                  # Example MIR pass plugin
      CMakeLists.txt
    configs/
      disable-scheduler.yaml
      insert-nop-after-sched.yaml
      combined.yaml
```

---

### Task 1: CMake Build System + Minimal Binary

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/main.cpp`

Build a minimal flexclang that mirrors `cc1_main()` and compiles a simple C file. Proves linking against LLVM/Clang works.

- [x] **Step 1: Create CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(flexclang VERSION 0.1.0)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(flexclang src/main.cpp)

llvm_map_components_to_libnames(LLVM_LIBS
  AMDGPUCodeGen AMDGPUAsmParser AMDGPUDesc AMDGPUInfo
  AllTargetsAsmParsers AllTargetsCodeGens AllTargetsDescs AllTargetsInfos
  CodeGen Core Support Target Passes Analysis
  TransformUtils ScalarOpts InstCombine MC MCParser Option ipo
)

target_link_libraries(flexclang PRIVATE
  clangFrontend clangFrontendTool clangCodeGen
  clangDriver clangBasic clangSerialization clangOptions
  ${LLVM_LIBS}
)

target_compile_features(flexclang PRIVATE cxx_std_17)
install(TARGETS flexclang DESTINATION bin)
```

- [x] **Step 2: Create minimal src/main.cpp**

```cpp
// src/main.cpp
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Serialization/PCHContainerOperations.h"
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

  ArrayRef<const char *> Args(argv + 1, argv + argc);
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
```

- [x] **Step 3: Build**

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build
make -j$(nproc) 2>&1 | tail -5
```

Expected: Build succeeds, produces `./flexclang`.

- [x] **Step 4: Test baseline compilation**

```bash
echo 'int main() { return 0; }' > /tmp/test.c
./flexclang -emit-obj -o /tmp/test.o /tmp/test.c
echo "Exit code: $?"
```

Expected: Exit code 0.

- [x] **Step 5: Commit**

```bash
git add CMakeLists.txt src/main.cpp
git commit -m "feat: minimal flexclang binary mirroring cc1_main"
```

---

### Task 2: FlexConfig -- CLI and YAML Parsing

**Files:**
- Create: `src/FlexConfig.h`
- Create: `src/FlexConfig.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

Parse `--flex-*` flags, strip them before passing to clang, store in `FlexConfig`. Also parse YAML config files.

- [x] **Step 1: Create src/FlexConfig.h**

```cpp
#ifndef FLEXCLANG_FLEXCONFIG_H
#define FLEXCLANG_FLEXCONFIG_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace flexclang {

struct MIRPassRule {
  enum Action { Disable, Replace, InsertAfter };
  Action action;
  std::string target;
  std::string plugin;
  std::string config; // optional: plugin-specific config file path
};

struct IRPassRule {
  enum Action { Disable, LoadPlugin };
  Action action;
  std::string target;
  std::string plugin;
};

struct FlexConfig {
  std::vector<MIRPassRule> mirRules;
  std::vector<IRPassRule> irRules;
  std::string configFile;
  std::string latencyModelFile;
  bool listPasses = false;
  bool verbose = false;
  bool verifyPlugins = false;
  bool dryRun = false;

  bool hasModifications() const {
    return !mirRules.empty() || !irRules.empty();
  }
};

FlexConfig parseFlexArgs(llvm::SmallVectorImpl<const char *> &remainingArgs,
                         int argc, const char **argv);

bool parseFlexYAML(FlexConfig &config, llvm::StringRef path);

} // namespace flexclang

#endif
```

- [x] **Step 2: Create src/FlexConfig.cpp**

```cpp
#include "FlexConfig.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>

using namespace llvm;

namespace flexclang {

static std::pair<std::string, std::string> splitNamePlugin(StringRef arg) {
  auto pos = arg.find(':');
  if (pos == StringRef::npos)
    return {arg.str(), ""};
  return {arg.substr(0, pos).str(), arg.substr(pos + 1).str()};
}

FlexConfig parseFlexArgs(SmallVectorImpl<const char *> &remainingArgs,
                         int argc, const char **argv) {
  FlexConfig config;

  for (int i = 0; i < argc; ++i) {
    StringRef arg(argv[i]);

    if (arg.consume_front("--flex-disable-pass=")) {
      config.mirRules.push_back({MIRPassRule::Disable, arg.str(), ""});
    } else if (arg.consume_front("--flex-replace-pass=")) {
      auto [name, plugin] = splitNamePlugin(arg);
      config.mirRules.push_back({MIRPassRule::Replace, name, plugin});
    } else if (arg.consume_front("--flex-insert-after=")) {
      auto [name, plugin] = splitNamePlugin(arg);
      config.mirRules.push_back({MIRPassRule::InsertAfter, name, plugin});
    } else if (arg.consume_front("--flex-disable-ir-pass=")) {
      config.irRules.push_back({IRPassRule::Disable, arg.str(), ""});
    } else if (arg.consume_front("--flex-config=")) {
      config.configFile = arg.str();
    } else if (arg == "--flex-list-passes") {
      config.listPasses = true;
    } else if (arg == "--flex-verbose") {
      config.verbose = true;
    } else if (arg == "--flex-verify-plugins") {
      config.verifyPlugins = true;
    } else if (arg == "--flex-dry-run") {
      config.dryRun = true;
    } else {
      remainingArgs.push_back(argv[i]);
    }
  }

  if (config.configFile.empty()) {
    if (const char *env = std::getenv("FLEXCLANG_CONFIG"))
      config.configFile = env;
  }

  return config;
}

bool parseFlexYAML(FlexConfig &config, StringRef path) {
  auto BufOrErr = MemoryBuffer::getFile(path);
  if (!BufOrErr) {
    errs() << "flexclang: error: cannot open config '" << path
           << "': " << BufOrErr.getError().message() << "\n";
    return false;
  }

  SourceMgr SM;
  yaml::Stream Stream(BufOrErr.get()->getBuffer(), SM);

  for (auto &Doc : Stream) {
    auto *Root = dyn_cast<yaml::MappingNode>(Doc.getRoot());
    if (!Root) continue;

    for (auto &KV : *Root) {
      auto *Key = dyn_cast<yaml::ScalarNode>(KV.getKey());
      if (!Key) continue;
      SmallString<32> KeyBuf;
      StringRef KeyRef = Key->getValue(KeyBuf);

      if (KeyRef == "mir-passes") {
        auto *Seq = dyn_cast<yaml::SequenceNode>(KV.getValue());
        if (!Seq) continue;
        for (auto &Item : *Seq) {
          auto *Map = dyn_cast<yaml::MappingNode>(&Item);
          if (!Map) continue;
          MIRPassRule rule{};
          std::string action;
          for (auto &F : *Map) {
            auto *FK = dyn_cast<yaml::ScalarNode>(F.getKey());
            auto *FV = dyn_cast<yaml::ScalarNode>(F.getValue());
            if (!FK || !FV) continue;
            SmallString<32> FKB, FVB;
            StringRef fk = FK->getValue(FKB), fv = FV->getValue(FVB);
            if (fk == "action") action = fv.str();
            else if (fk == "target") rule.target = fv.str();
            else if (fk == "plugin") rule.plugin = fv.str();
            else if (fk == "config") rule.config = fv.str();
          }
          if (action == "disable") rule.action = MIRPassRule::Disable;
          else if (action == "replace") rule.action = MIRPassRule::Replace;
          else if (action == "insert-after") rule.action = MIRPassRule::InsertAfter;
          else { errs() << "flexclang: warning: unknown mir action: " << action << "\n"; continue; }
          config.mirRules.push_back(rule);
        }
      } else if (KeyRef == "ir-passes") {
        auto *Seq = dyn_cast<yaml::SequenceNode>(KV.getValue());
        if (!Seq) continue;
        for (auto &Item : *Seq) {
          auto *Map = dyn_cast<yaml::MappingNode>(&Item);
          if (!Map) continue;
          IRPassRule rule{};
          std::string action;
          for (auto &F : *Map) {
            auto *FK = dyn_cast<yaml::ScalarNode>(F.getKey());
            auto *FV = dyn_cast<yaml::ScalarNode>(F.getValue());
            if (!FK || !FV) continue;
            SmallString<32> FKB, FVB;
            StringRef fk = FK->getValue(FKB), fv = FV->getValue(FVB);
            if (fk == "action") action = fv.str();
            else if (fk == "target") rule.target = fv.str();
            else if (fk == "plugin") rule.plugin = fv.str();
          }
          if (action == "disable") rule.action = IRPassRule::Disable;
          else if (action == "load-plugin") rule.action = IRPassRule::LoadPlugin;
          else { errs() << "flexclang: warning: unknown ir action: " << action << "\n"; continue; }
          config.irRules.push_back(rule);
        }
      }
    }
  }
  return !Stream.failed();
}

} // namespace flexclang
```

- [x] **Step 3: Update main.cpp to use FlexConfig**

Replace the `Args` and `CreateFromArgs` section in `main.cpp`:

```cpp
// Add at top:
#include "FlexConfig.h"

// Replace Args handling with:
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
```

- [x] **Step 4: Update CMakeLists.txt**

```cmake
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
)
```

- [x] **Step 5: Build and test**

```bash
cd build && make -j$(nproc) 2>&1 | tail -3

# Test flex flags stripped, compilation works
echo 'int main() { return 0; }' > /tmp/test.c
./flexclang --flex-disable-pass=machine-scheduler -emit-obj -o /tmp/test.o /tmp/test.c
echo "Flex flag stripping: $?"

# Test dry-run
./flexclang --flex-dry-run --flex-disable-pass=machine-scheduler --flex-insert-after=greedy:./fake.so
```

Expected: First exits 0. Second prints dry-run output and exits 0.

- [x] **Step 6: Commit**

```bash
git add src/FlexConfig.h src/FlexConfig.cpp src/main.cpp CMakeLists.txt
git commit -m "feat: FlexConfig CLI + YAML parsing with --flex-dry-run"
```

---

### Task 3: FlexPassNameRegistry

**Files:**
- Create: `src/FlexPassNameRegistry.h`
- Create: `src/FlexPassNameRegistry.cpp`
- Modify: `CMakeLists.txt`

Resolve pass argument strings (e.g., `"si-form-memory-clauses"`) to `AnalysisID` (`void*`) using `PassRegistry::getPassInfo(StringRef)`.

- [x] **Step 1: Create src/FlexPassNameRegistry.h**

```cpp
#ifndef FLEXCLANG_FLEXPASSNAMEREGISTRY_H
#define FLEXCLANG_FLEXPASSNAMEREGISTRY_H

#include "llvm/ADT/StringRef.h"

namespace flexclang {

/// Resolve pass argument string to AnalysisID. Returns nullptr if not found.
const void *resolvePassID(llvm::StringRef passArg);

/// Returns true if disabling this pass is likely to cause miscompilation.
bool isCriticalPass(llvm::StringRef passArg);

} // namespace flexclang

#endif
```

- [x] **Step 2: Create src/FlexPassNameRegistry.cpp**

```cpp
#include "FlexPassNameRegistry.h"
#include "llvm/PassInfo.h"
#include "llvm/PassRegistry.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

const void *resolvePassID(StringRef passArg) {
  const PassInfo *PI = PassRegistry::getPassRegistry()->getPassInfo(passArg);
  if (!PI) {
    errs() << "flexclang: error: unknown pass '" << passArg << "'\n";
    return nullptr;
  }
  return PI->getTypeInfo();
}

bool isCriticalPass(StringRef passArg) {
  static const StringSet<> Critical = {
      "si-lower-control-flow", "si-insert-waitcnts",
      "prologepilog",          "phi-node-elimination",
      "virtregrewriter",       "si-fix-sgpr-copies",
  };
  return Critical.contains(passArg);
}

} // namespace flexclang
```

- [x] **Step 3: Update CMakeLists.txt**

```cmake
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
  src/FlexPassNameRegistry.cpp
)
```

- [x] **Step 4: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -3
```

- [x] **Step 5: Commit**

```bash
git add src/FlexPassNameRegistry.h src/FlexPassNameRegistry.cpp CMakeLists.txt
git commit -m "feat: FlexPassNameRegistry resolves pass names to AnalysisID"
```

---

### Task 4: FlexPassLoader

**Files:**
- Create: `src/FlexPassLoader.h`
- Create: `src/FlexPassLoader.cpp`
- Modify: `CMakeLists.txt`

Load MIR pass plugin `.so` files via `DynamicLibrary`, find `flexclangCreatePass` or `flexclangCreatePassWithConfig` factory.

- [x] **Step 1: Create src/FlexPassLoader.h**

```cpp
#ifndef FLEXCLANG_FLEXPASSLOADER_H
#define FLEXCLANG_FLEXPASSLOADER_H

#include "llvm/ADT/StringRef.h"

namespace llvm {
class Pass;
} // namespace llvm

namespace flexclang {

/// Load MIR pass plugin. If configPath is non-empty, reads the file and
/// tries flexclangCreatePassWithConfig(contents) first.
/// Falls back to flexclangCreatePass(). Returns nullptr on failure.
llvm::Pass *loadMIRPassPlugin(llvm::StringRef soPath,
                               llvm::StringRef configPath = "");

} // namespace flexclang

#endif
```

- [x] **Step 2: Create src/FlexPassLoader.cpp**

```cpp
#include "FlexPassLoader.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

Pass *loadMIRPassPlugin(StringRef soPath, StringRef configPath) {
  std::string errMsg;
  auto Lib =
      sys::DynamicLibrary::getPermanentLibrary(soPath.str().c_str(), &errMsg);
  if (!Lib.isValid()) {
    errs() << "flexclang: error: cannot load '" << soPath
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
        errs() << "flexclang: error: cannot read plugin config '"
               << configPath << "': " << Buf.getError().message() << "\n";
        return nullptr;
      }
      Pass *P = CreateWithConfig((*Buf)->getBuffer().str().c_str());
      if (!P) {
        errs() << "flexclang: error: flexclangCreatePassWithConfig returned null\n";
        return nullptr;
      }
      return P;
    }
    errs() << "flexclang: warning: config specified but plugin '"
           << soPath << "' does not export flexclangCreatePassWithConfig\n";
  }

  // Fall back to simple factory.
  using CreatePassFn = MachineFunctionPass *(*)();
  auto *CreatePass = reinterpret_cast<CreatePassFn>(
      Lib.getAddressOfSymbol("flexclangCreatePass"));
  if (!CreatePass) {
    errs() << "flexclang: error: '" << soPath
           << "' exports neither flexclangCreatePass nor flexclangCreatePassWithConfig\n";
    return nullptr;
  }

  Pass *P = CreatePass();
  if (!P) {
    errs() << "flexclang: error: flexclangCreatePass returned null in '"
           << soPath << "'\n";
    return nullptr;
  }

  // Print name if available.
  using PassNameFn = const char *(*)();
  auto *GetName = reinterpret_cast<PassNameFn>(
      Lib.getAddressOfSymbol("flexclangPassName"));
  if (GetName)
    errs() << "flexclang: loaded MIR plugin '" << GetName() << "'\n";

  return P;
}

} // namespace flexclang
```

- [x] **Step 3: Update CMakeLists.txt**

```cmake
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
  src/FlexPassNameRegistry.cpp
  src/FlexPassLoader.cpp
)
```

- [x] **Step 4: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -3
```

- [x] **Step 5: Commit**

```bash
git add src/FlexPassLoader.h src/FlexPassLoader.cpp CMakeLists.txt
git commit -m "feat: FlexPassLoader for dynamic .so MIR pass loading"
```

---

### Task 5: FlexPassConfigCallback -- MIR Pipeline Interception

**Files:**
- Create: `src/FlexPassConfigCallback.h`
- Create: `src/FlexPassConfigCallback.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

The core: register `RegisterTargetPassConfigCallback` to intercept the MIR pipeline.

- [x] **Step 1: Create src/FlexPassConfigCallback.h**

```cpp
#ifndef FLEXCLANG_FLEXPASSCONFIGCALLBACK_H
#define FLEXCLANG_FLEXPASSCONFIGCALLBACK_H

#include "FlexConfig.h"

namespace flexclang {

/// Register the TargetPassConfig callback. Must be called before
/// ExecuteCompilerInvocation(). The config reference must outlive compilation.
void registerFlexPassConfigCallback(const FlexConfig &config);

} // namespace flexclang

#endif
```

- [x] **Step 2: Create src/FlexPassConfigCallback.cpp**

```cpp
#include "FlexPassConfigCallback.h"
#include "FlexPassLoader.h"
#include "FlexPassNameRegistry.h"
#include "llvm/CodeGen/MachineVerifier.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/Target/RegisterTargetPassConfigCallback.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

static std::unique_ptr<RegisterTargetPassConfigCallback> CallbackReg;

void registerFlexPassConfigCallback(const FlexConfig &config) {
  CallbackReg = std::make_unique<RegisterTargetPassConfigCallback>(
      [&config](TargetMachine &TM, PassManagerBase &PM,
                TargetPassConfig *PassConfig) {
        if (TM.getTargetTriple().getArch() != Triple::amdgcn)
          return;

        for (const auto &rule : config.mirRules) {
          switch (rule.action) {
          case MIRPassRule::Disable: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            if (isCriticalPass(rule.target))
              errs() << "flexclang: warning: disabling '" << rule.target
                     << "' may cause incorrect code generation\n";
            PassConfig->disablePass(ID);
            if (config.verbose)
              errs() << "flexclang: disabled MIR pass '" << rule.target << "'\n";
            break;
          }
          case MIRPassRule::Replace: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            Pass *Replacement = loadMIRPassPlugin(rule.plugin, rule.config);
            if (!Replacement) break;
            PassConfig->substitutePass(ID, Replacement);
            if (config.verbose)
              errs() << "flexclang: replaced '" << rule.target
                     << "' with " << rule.plugin << "\n";
            break;
          }
          case MIRPassRule::InsertAfter: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            Pass *NewPass = loadMIRPassPlugin(rule.plugin, rule.config);
            if (!NewPass) break;
            PassConfig->insertPass(ID, NewPass);
            if (config.verbose)
              errs() << "flexclang: inserted after '" << rule.target
                     << "' from " << rule.plugin << "\n";
            // Optionally insert MachineVerifier after the plugin pass.
            if (config.verifyPlugins) {
              PassConfig->insertPass(NewPass->getPassID(),
                                     &MachineVerifierPass::ID);
            }
            break;
          }
          }
        }
      });
}

} // namespace flexclang
```

- [x] **Step 3: Update main.cpp**

Add after YAML parsing, before `CompilerInvocation::CreateFromArgs`:

```cpp
#include "FlexPassConfigCallback.h"

// After config parsing:
  if (config.hasModifications()) {
    flexclang::registerFlexPassConfigCallback(config);
  }
```

- [x] **Step 4: Update CMakeLists.txt**

```cmake
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
  src/FlexPassNameRegistry.cpp
  src/FlexPassLoader.cpp
  src/FlexPassConfigCallback.cpp
)
```

- [x] **Step 5: Build and test MIR disable**

```bash
cd build && make -j$(nproc) 2>&1 | tail -3

echo '__global__ void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip
./flexclang --flex-verbose --flex-disable-pass=machine-scheduler \
  -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -emit-obj -o /tmp/k.o /tmp/k.hip 2>&1
```

Expected: Output includes `flexclang: requesting disable of MIR pass 'machine-scheduler'`.

- [x] **Step 6: Commit**

```bash
git add src/FlexPassConfigCallback.h src/FlexPassConfigCallback.cpp src/main.cpp CMakeLists.txt
git commit -m "feat: MIR pass interception via RegisterTargetPassConfigCallback"
```

---

### Task 6: IR Pass Disable + --flex-list-passes

**Files:**
- Modify: `src/main.cpp`

Add `--flex-disable-ir-pass` via `shouldRunOptionalPassCallback` and `--flex-list-passes` via `-debug-pass=Structure`.

- [x] **Step 1: Add IR pass disable and list-passes to main.cpp**

After `CompilerInstance` creation, before `ExecuteCompilerInvocation`:

```cpp
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

  // --flex-list-passes: use LLVM's -debug-pass=Structure
  if (config.listPasses) {
    const char *args[] = {"flexclang", "-debug-pass=Structure"};
    cl::ParseCommandLineOptions(2, args, "flexclang pass listing\n");
  }

  // IR pass modifications via PassBuilderCallbacks
  {
    std::vector<std::string> irDisable;
    std::vector<std::string> irPlugins;
    for (const auto &r : config.irRules) {
      if (r.action == flexclang::IRPassRule::Disable)
        irDisable.push_back(r.target);
      else if (r.action == flexclang::IRPassRule::LoadPlugin)
        irPlugins.push_back(r.plugin);
    }

    if (!irDisable.empty() || !irPlugins.empty()) {
      bool verbose = config.verbose;
      Clang->getCodeGenOpts().PassBuilderCallbacks.push_back(
          [irDisable, irPlugins, verbose](PassBuilder &PB) {
            if (!irDisable.empty()) {
              auto *PIC = PB.getPassInstrumentationCallbacks();
              if (PIC) {
                PIC->registerShouldRunOptionalPassCallback(
                    [irDisable, verbose](StringRef Name, Any) {
                      for (const auto &d : irDisable) {
                        if (Name.contains(d)) {
                          if (verbose)
                            errs() << "flexclang: skipping IR pass '"
                                   << Name << "'\n";
                          return false;
                        }
                      }
                      return true;
                    });
              }
            }
            for (const auto &path : irPlugins) {
              auto Plugin = PassPlugin::Load(path);
              if (Plugin) {
                Plugin->registerPassBuilderCallbacks(PB);
              } else {
                errs() << "flexclang: error: " << toString(Plugin.takeError()) << "\n";
              }
            }
          });
    }
  }
```

- [x] **Step 2: Build and test**

```bash
cd build && make -j$(nproc) 2>&1 | tail -3

# Test IR disable
echo 'int foo(int x) { return x + 1; }' > /tmp/ir.c
./flexclang --flex-verbose --flex-disable-ir-pass=instcombine \
  -O2 -emit-llvm -o /tmp/ir.ll /tmp/ir.c 2>&1 | grep -i "skipping\|instcombine"

# Test list-passes
echo '__global__ void k() {}' > /tmp/k.hip
./flexclang --flex-list-passes \
  -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -emit-obj -o /dev/null /tmp/k.hip 2>&1 | head -30
```

Expected: IR disable shows skipping messages. List-passes shows pass structure.

- [x] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat: --flex-disable-ir-pass and --flex-list-passes"
```

---

### Task 7: Example IR Pass Plugin

**Files:**
- Create: `examples/ir-pass-counter/IRInstCounter.cpp`
- Create: `examples/ir-pass-counter/CMakeLists.txt`

- [x] **Step 1: Create IRInstCounter.cpp**

Copy verbatim from spec Section 10.1.

- [x] **Step 2: Create CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(IRInstCounter)

find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_library(ir-inst-counter MODULE IRInstCounter.cpp)
target_compile_options(ir-inst-counter PRIVATE -fno-rtti -fPIC)
set_target_properties(ir-inst-counter PROPERTIES PREFIX "" SUFFIX ".so")
```

- [x] **Step 3: Build and test**

```bash
cd examples/ir-pass-counter
cmake -B build -DLLVM_DIR=/home/poyechen/workspace/repo/llvm-project/build/lib/cmake/llvm
cmake --build build

echo 'int foo(int x) { return x * 2 + 1; }' > /tmp/test.c
../../build/flexclang -fpass-plugin=./build/ir-inst-counter.so \
  -O2 -emit-obj -o /tmp/test.o /tmp/test.c 2>&1 | grep IRInstCounter
```

Expected: `[IRInstCounter] foo: N instructions`

- [x] **Step 4: Commit**

```bash
git add examples/ir-pass-counter/
git commit -m "feat: example IR pass plugin (instruction counter)"
```

---

### Task 8: Example MIR Pass Plugin

**Files:**
- Create: `examples/mir-pass-nop-inserter/MIRNopInserter.cpp`
- Create: `examples/mir-pass-nop-inserter/CMakeLists.txt`

- [x] **Step 1: Create MIRNopInserter.cpp**

Copy verbatim from spec Section 10.2 (includes `AMDGPU.h` and `SIInstrInfo.h`).

- [x] **Step 2: Create CMakeLists.txt**

Copy verbatim from spec Section 10.6.

- [x] **Step 3: Build and test**

```bash
cd examples/mir-pass-nop-inserter
LLVM_BUILD=/home/poyechen/workspace/repo/llvm-project/build
cmake -B build \
  -DLLVM_DIR=${LLVM_BUILD}/lib/cmake/llvm \
  -DLLVM_BUILD_DIR=${LLVM_BUILD} \
  -DLLVM_MAIN_SRC_DIR=/home/poyechen/workspace/repo/llvm-project/llvm
cmake --build build

echo '__global__ void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip
../../build/flexclang --flex-verbose \
  --flex-insert-after=machine-scheduler:./build/mir-nop-inserter.so \
  -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -S -o /tmp/k.s /tmp/k.hip 2>&1
grep -c "s_nop" /tmp/k.s
```

Expected: `flexclang: inserted after 'machine-scheduler'` and `s_nop` count > 0.

- [x] **Step 4: Commit**

```bash
git add examples/mir-pass-nop-inserter/
git commit -m "feat: example MIR pass plugin (NOP inserter)"
```

---

### Task 9: Test Kernel, Configs, Validation Script

**Files:**
- Create: `examples/test_kernel.hip`
- Create: `examples/configs/disable-scheduler.yaml`
- Create: `examples/configs/insert-nop-after-sched.yaml`
- Create: `examples/configs/combined.yaml`
- Create: `examples/validate.sh`

- [x] **Step 1: Create test_kernel.hip**

Copy verbatim from spec Section 10.3 (uses `__builtin_amdgcn_mfma_f32_32x32x8f16`).

- [x] **Step 2: Create YAML configs**

Copy from spec Section 10.4 (three files).

- [x] **Step 3: Create validate.sh**

Copy from spec Section 10.5. Add `chmod +x`.

- [x] **Step 4: Run validation**

```bash
cd /home/poyechen/workspace/repo/friendly-clang
chmod +x examples/validate.sh
FLEXCLANG=./build/flexclang ./examples/validate.sh
```

Expected: `All tests passed!`

- [x] **Step 5: Commit**

```bash
git add examples/
git commit -m "feat: test kernel, YAML configs, and validation script"
```

---

### Task 10: End-to-End Integration + Bit-Identity Test

**Files:** None new.

- [x] **Step 1: Clean build everything**

```bash
cd /home/poyechen/workspace/repo/friendly-clang
rm -rf build examples/ir-pass-counter/build examples/mir-pass-nop-inserter/build

LLVM_BUILD=/home/poyechen/workspace/repo/llvm-project/build

mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=${LLVM_BUILD}
make -j$(nproc)
cd ..

cd examples/ir-pass-counter
cmake -B build -DLLVM_DIR=${LLVM_BUILD}/lib/cmake/llvm
cmake --build build
cd ../..

cd examples/mir-pass-nop-inserter
cmake -B build -DLLVM_DIR=${LLVM_BUILD}/lib/cmake/llvm \
  -DLLVM_BUILD_DIR=${LLVM_BUILD} \
  -DLLVM_MAIN_SRC_DIR=/home/poyechen/workspace/repo/llvm-project/llvm
cmake --build build
cd ../..
```

- [x] **Step 2: Run validation script**

```bash
FLEXCLANG=./build/flexclang ./examples/validate.sh
```

Expected: `All tests passed!`

- [x] **Step 3: Bit-identity test (no flex flags)**

```bash
echo '__global__ void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip

./build/flexclang -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -S -o /tmp/flex.s /tmp/k.hip

/home/poyechen/workspace/repo/llvm-project/build/bin/clang \
  -cc1 -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -S -o /tmp/upstream.s /tmp/k.hip

diff /tmp/flex.s /tmp/upstream.s
echo "Bit-identical: $? (0=yes)"
```

Expected: Exit code 0.

- [x] **Step 4: Commit**

```bash
git add -A && git commit -m "chore: end-to-end integration verified"
```
