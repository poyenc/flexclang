# flexclang Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build flexclang, a drop-in clang replacement that lets AMDGPU HIP kernel developers disable, replace, and insert MIR/IR passes via CLI flags and YAML config.

**Architecture:** flexclang links against installed LLVM/Clang libraries (no fork). It mirrors `cc1_main()` but registers a `RegisterTargetPassConfigCallback` to intercept the MIR codegen pipeline, and uses `PassBuilder` callbacks for IR pass control. Pass plugins are loaded as `.so` via `DynamicLibrary`.

**Tech Stack:** C++17, LLVM/Clang libraries (amd-staging), CMake 3.20+, LLVM YAML I/O

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
    test_kernel.hip                       # Test HIP kernel
    validate.sh                           # End-to-end validation script
    ir-pass-counter/
      IRInstCounter.cpp                   # Example IR pass plugin
      CMakeLists.txt
    mir-pass-nop-inserter/
      MIRNopInserter.cpp                  # Example MIR pass plugin
      CMakeLists.txt
    configs/
      disable-memory-clauses.yaml
      insert-nop-after-sched.yaml
      combined.yaml
```

---

### Task 1: CMake Build System

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/main.cpp` (minimal skeleton)

This task sets up the build system and creates a minimal flexclang binary that just forwards to clang's `cc1_main()` -- proving we can link against LLVM/Clang and compile HIP code.

- [ ] **Step 1: Create CMakeLists.txt**

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(flexclang VERSION 0.1.0)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(flexclang
  src/main.cpp
)

# Use llvm_map_components_to_libnames for portability
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

- [ ] **Step 2: Create minimal main.cpp**

This is a near-copy of `cc1_main()` from `clang/tools/driver/cc1_main.cpp` (lines 219-296 on amd-staging). We bypass the driver layer and operate directly as `cc1` for now.

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

  // Initialize all LLVM targets.
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  // PCH container operations.
  auto PCHOps = std::make_shared<PCHContainerOperations>();
  PCHOps->registerWriter(std::make_unique<ObjectFilePCHContainerWriter>());
  PCHOps->registerReader(std::make_unique<ObjectFilePCHContainerReader>());

  // Set up diagnostics.
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID = DiagnosticIDs::create();
  DiagnosticOptions DiagOpts;
  TextDiagnosticBuffer *DiagsBuffer = new TextDiagnosticBuffer;
  DiagnosticsEngine Diags(DiagID, DiagOpts, DiagsBuffer);

  // Parse args into a CompilerInvocation.
  // Skip argv[0] (program name). All args are cc1-style args.
  ArrayRef<const char *> Args(argv + 1, argv + argc);
  auto Invocation = std::make_shared<CompilerInvocation>();
  bool Success =
      CompilerInvocation::CreateFromArgs(*Invocation, Args, Diags, argv[0]);

  // Create the CompilerInstance.
  auto Clang =
      std::make_unique<CompilerInstance>(std::move(Invocation), std::move(PCHOps));

  // Set up virtual filesystem.
  auto VFS = vfs::getRealFileSystem();
  Clang->createVirtualFileSystem(std::move(VFS), DiagsBuffer);
  Clang->createDiagnostics();

  install_fatal_error_handler(
      [](void *UserData, const char *Message, bool GenCrashDiag) {
        auto &Diags = *static_cast<DiagnosticsEngine *>(UserData);
        Diags.Report(diag::err_fe_error_backend) << Message;
        exit(1);
      },
      static_cast<void *>(&Clang->getDiagnostics()));

  DiagsBuffer->FlushDiagnostics(Clang->getDiagnostics());
  if (!Success)
    return 1;

  // Execute the frontend actions (compile, emit obj/asm, etc.).
  Success = ExecuteCompilerInvocation(Clang.get());

  remove_fatal_error_handler();

  return Success ? 0 : 1;
}
```

- [ ] **Step 3: Build and test baseline compilation**

```bash
cd /home/poyechen/workspace/repo/friendly-clang
mkdir -p build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build
make -j$(nproc)
```

Test with a simple C file (cc1-style invocation):
```bash
echo 'int main() { return 0; }' > /tmp/test.c
./flexclang -cc1 -emit-obj -o /tmp/test.o /tmp/test.c
echo "Exit code: $?"
```

Expected: Exit code 0, produces `/tmp/test.o`.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt src/main.cpp
git commit -m "feat: minimal flexclang binary that mirrors cc1_main"
```

---

### Task 2: FlexConfig -- CLI Flag Parsing

**Files:**
- Create: `src/FlexConfig.h`
- Create: `src/FlexConfig.cpp`
- Modify: `src/main.cpp` (add flex flag extraction before CompilerInvocation)
- Modify: `CMakeLists.txt` (add FlexConfig.cpp)

Parse `--flex-*` CLI flags, strip them from args before passing to clang, and store them in a `FlexConfig` struct.

- [ ] **Step 1: Create FlexConfig.h**

```cpp
// src/FlexConfig.h
#ifndef FLEXCLANG_FLEXCONFIG_H
#define FLEXCLANG_FLEXCONFIG_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace flexclang {

struct MIRPassRule {
  enum Action { Disable, Replace, InsertAfter, InsertAt };
  Action action;
  std::string target; // pass name or hook name
  std::string plugin; // path to .so (empty for disable)
};

struct IRPassRule {
  enum Action { Disable, LoadPlugin };
  Action action;
  std::string target; // pass name (for disable)
  std::string plugin; // path to .so (for load-plugin)
};

struct FlexConfig {
  std::vector<MIRPassRule> mirRules;
  std::vector<IRPassRule> irRules;
  std::string configFile;
  std::string latencyModelFile;
  bool listPasses = false;

  bool hasModifications() const {
    return !mirRules.empty() || !irRules.empty();
  }
};

/// Extract --flex-* flags from argv, populate FlexConfig, return remaining args.
/// The remaining args are suitable for passing to CompilerInvocation::CreateFromArgs.
FlexConfig parseFlexArgs(llvm::SmallVectorImpl<const char *> &remainingArgs,
                         int argc, const char **argv);

/// Parse YAML config file into FlexConfig.
/// Returns true on success.
bool parseFlexYAML(FlexConfig &config, llvm::StringRef path);

} // namespace flexclang

#endif // FLEXCLANG_FLEXCONFIG_H
```

- [ ] **Step 2: Create FlexConfig.cpp**

```cpp
// src/FlexConfig.cpp
#include "FlexConfig.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

/// Split "name:path" into {name, path}. If no colon, path is empty.
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

    if (arg.starts_with("--flex-disable-pass=")) {
      MIRPassRule rule;
      rule.action = MIRPassRule::Disable;
      rule.target = arg.substr(strlen("--flex-disable-pass=")).str();
      config.mirRules.push_back(rule);
    } else if (arg.starts_with("--flex-replace-pass=")) {
      auto [name, plugin] = splitNamePlugin(
          arg.substr(strlen("--flex-replace-pass=")));
      MIRPassRule rule;
      rule.action = MIRPassRule::Replace;
      rule.target = name;
      rule.plugin = plugin;
      config.mirRules.push_back(rule);
    } else if (arg.starts_with("--flex-insert-after=")) {
      auto [name, plugin] = splitNamePlugin(
          arg.substr(strlen("--flex-insert-after=")));
      MIRPassRule rule;
      rule.action = MIRPassRule::InsertAfter;
      rule.target = name;
      rule.plugin = plugin;
      config.mirRules.push_back(rule);
    } else if (arg.starts_with("--flex-insert-at=")) {
      auto [hook, plugin] = splitNamePlugin(
          arg.substr(strlen("--flex-insert-at=")));
      MIRPassRule rule;
      rule.action = MIRPassRule::InsertAt;
      rule.target = hook;
      rule.plugin = plugin;
      config.mirRules.push_back(rule);
    } else if (arg.starts_with("--flex-disable-ir-pass=")) {
      IRPassRule rule;
      rule.action = IRPassRule::Disable;
      rule.target = arg.substr(strlen("--flex-disable-ir-pass=")).str();
      config.irRules.push_back(rule);
    } else if (arg.starts_with("--flex-config=")) {
      config.configFile = arg.substr(strlen("--flex-config=")).str();
    } else if (arg.starts_with("--flex-latency-model=")) {
      config.latencyModelFile =
          arg.substr(strlen("--flex-latency-model=")).str();
    } else if (arg == "--flex-list-passes") {
      config.listPasses = true;
    } else {
      // Not a flex flag -- pass through to clang.
      remainingArgs.push_back(argv[i]);
    }
  }

  // Check FLEXCLANG_CONFIG env var if no --flex-config was given.
  if (config.configFile.empty()) {
    if (const char *envConfig = std::getenv("FLEXCLANG_CONFIG"))
      config.configFile = envConfig;
  }

  return config;
}

bool parseFlexYAML(FlexConfig &config, StringRef path) {
  auto BufOrErr = MemoryBuffer::getFile(path);
  if (!BufOrErr) {
    errs() << "flexclang: error: cannot open config file '" << path
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
      SmallString<32> KeyStr;
      StringRef KeyRef = Key->getValue(KeyStr);

      if (KeyRef == "mir-passes") {
        auto *Seq = dyn_cast<yaml::SequenceNode>(KV.getValue());
        if (!Seq) continue;
        for (auto &Item : *Seq) {
          auto *Map = dyn_cast<yaml::MappingNode>(&Item);
          if (!Map) continue;

          MIRPassRule rule;
          std::string action;
          for (auto &Field : *Map) {
            auto *FK = dyn_cast<yaml::ScalarNode>(Field.getKey());
            auto *FV = dyn_cast<yaml::ScalarNode>(Field.getValue());
            if (!FK || !FV) continue;
            SmallString<32> FKStr, FVStr;
            StringRef FKRef = FK->getValue(FKStr);
            StringRef FVRef = FV->getValue(FVStr);

            if (FKRef == "action") action = FVRef.str();
            else if (FKRef == "target") rule.target = FVRef.str();
            else if (FKRef == "hook") rule.target = FVRef.str();
            else if (FKRef == "plugin") rule.plugin = FVRef.str();
          }

          if (action == "disable") rule.action = MIRPassRule::Disable;
          else if (action == "replace") rule.action = MIRPassRule::Replace;
          else if (action == "insert-after") rule.action = MIRPassRule::InsertAfter;
          else if (action == "insert-at") rule.action = MIRPassRule::InsertAt;
          else {
            errs() << "flexclang: warning: unknown mir-passes action: "
                   << action << "\n";
            continue;
          }
          config.mirRules.push_back(rule);
        }
      } else if (KeyRef == "ir-passes") {
        auto *Seq = dyn_cast<yaml::SequenceNode>(KV.getValue());
        if (!Seq) continue;
        for (auto &Item : *Seq) {
          auto *Map = dyn_cast<yaml::MappingNode>(&Item);
          if (!Map) continue;

          IRPassRule rule;
          std::string action;
          for (auto &Field : *Map) {
            auto *FK = dyn_cast<yaml::ScalarNode>(Field.getKey());
            auto *FV = dyn_cast<yaml::ScalarNode>(Field.getValue());
            if (!FK || !FV) continue;
            SmallString<32> FKStr, FVStr;
            StringRef FKRef = FK->getValue(FKStr);
            StringRef FVRef = FV->getValue(FVStr);

            if (FKRef == "action") action = FVRef.str();
            else if (FKRef == "target") rule.target = FVRef.str();
            else if (FKRef == "plugin") rule.plugin = FVRef.str();
          }

          if (action == "disable") rule.action = IRPassRule::Disable;
          else if (action == "load-plugin") rule.action = IRPassRule::LoadPlugin;
          else {
            errs() << "flexclang: warning: unknown ir-passes action: "
                   << action << "\n";
            continue;
          }
          config.irRules.push_back(rule);
        }
      } else if (KeyRef == "latency-model") {
        auto *Map = dyn_cast<yaml::MappingNode>(KV.getValue());
        if (!Map) continue;
        for (auto &Field : *Map) {
          auto *FK = dyn_cast<yaml::ScalarNode>(Field.getKey());
          auto *FV = dyn_cast<yaml::ScalarNode>(Field.getValue());
          if (!FK || !FV) continue;
          SmallString<32> FKStr, FVStr;
          if (FK->getValue(FKStr) == "file")
            config.latencyModelFile = FV->getValue(FVStr).str();
        }
      }
    }
  }

  return !Stream.failed();
}

} // namespace flexclang
```

- [ ] **Step 3: Update main.cpp to use FlexConfig**

Add flex arg parsing before `CompilerInvocation::CreateFromArgs`:

```cpp
// At the top of main.cpp, add:
#include "FlexConfig.h"

// Replace the existing Args and CreateFromArgs section with:

  // Extract --flex-* flags; pass the rest to clang.
  SmallVector<const char *, 256> clangArgs;
  flexclang::FlexConfig config =
      flexclang::parseFlexArgs(clangArgs, argc, argv);

  // Load YAML config if specified.
  if (!config.configFile.empty()) {
    if (!flexclang::parseFlexYAML(config, config.configFile))
      return 1;
  }

  // Parse remaining args into a CompilerInvocation.
  auto Invocation = std::make_shared<CompilerInvocation>();
  bool Success = CompilerInvocation::CreateFromArgs(
      *Invocation, clangArgs, Diags, argv[0]);
```

- [ ] **Step 4: Update CMakeLists.txt**

Add `src/FlexConfig.cpp` to the `add_executable`:

```cmake
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
)
```

- [ ] **Step 5: Build and test CLI parsing**

```bash
cd build && make -j$(nproc) 2>&1 | tail -5
echo 'int main() { return 0; }' > /tmp/test.c
# Verify flex flags are stripped and compilation still works
./flexclang --flex-disable-pass=si-form-memory-clauses -cc1 -emit-obj -o /tmp/test.o /tmp/test.c
echo "Exit code: $?"
```

Expected: Exit code 0 (flex flags stripped, compilation succeeds).

- [ ] **Step 6: Commit**

```bash
git add src/FlexConfig.h src/FlexConfig.cpp src/main.cpp CMakeLists.txt
git commit -m "feat: FlexConfig CLI and YAML parsing for --flex-* flags"
```

---

### Task 3: FlexPassNameRegistry -- Resolving Pass Names to AnalysisIDs

**Files:**
- Create: `src/FlexPassNameRegistry.h`
- Create: `src/FlexPassNameRegistry.cpp`
- Modify: `CMakeLists.txt`

The `TargetPassConfig` API (`disablePass`, `substitutePass`, `insertPass`) takes `AnalysisID` (a `void*` pointer), but users specify pass names as strings. This component resolves strings like `"si-form-memory-clauses"` to the corresponding `AnalysisID` using LLVM's `PassRegistry`.

- [ ] **Step 1: Create FlexPassNameRegistry.h**

```cpp
// src/FlexPassNameRegistry.h
#ifndef FLEXCLANG_FLEXPASSNAMEREGISTRY_H
#define FLEXCLANG_FLEXPASSNAMEREGISTRY_H

#include "llvm/ADT/StringRef.h"

namespace flexclang {

/// Resolve a pass argument string (e.g., "si-form-memory-clauses",
/// "machine-scheduler") to its AnalysisID (void* pointer to the pass's
/// static ID member). Returns nullptr if not found.
///
/// Uses LLVM's PassRegistry::getPassInfo(StringRef) which looks up
/// passes by the argument string registered via INITIALIZE_PASS.
const void *resolvePassID(llvm::StringRef passArg);

/// List of critical passes that produce a warning if disabled.
bool isCriticalPass(llvm::StringRef passArg);

} // namespace flexclang

#endif // FLEXCLANG_FLEXPASSNAMEREGISTRY_H
```

- [ ] **Step 2: Create FlexPassNameRegistry.cpp**

```cpp
// src/FlexPassNameRegistry.cpp
#include "FlexPassNameRegistry.h"
#include "llvm/PassInfo.h"
#include "llvm/PassRegistry.h"
#include "llvm/ADT/StringSet.h"

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
  static const StringSet<> CriticalPasses = {
      "si-lower-control-flow",
      "si-insert-waitcnts",
      "prologepilog",
      "phi-node-elimination",
      "virtregrewriter",
      "si-fix-sgpr-copies",
  };
  return CriticalPasses.contains(passArg);
}

} // namespace flexclang
```

- [ ] **Step 3: Update CMakeLists.txt**

Add `src/FlexPassNameRegistry.cpp`:

```cmake
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
  src/FlexPassNameRegistry.cpp
)
```

- [ ] **Step 4: Build and verify**

```bash
cd build && make -j$(nproc) 2>&1 | tail -5
echo "Build succeeded: $?"
```

- [ ] **Step 5: Commit**

```bash
git add src/FlexPassNameRegistry.h src/FlexPassNameRegistry.cpp CMakeLists.txt
git commit -m "feat: FlexPassNameRegistry resolves pass name strings to AnalysisID"
```

---

### Task 4: FlexPassLoader -- Dynamic .so Loading

**Files:**
- Create: `src/FlexPassLoader.h`
- Create: `src/FlexPassLoader.cpp`
- Modify: `CMakeLists.txt`

Load MIR pass plugin `.so` files at runtime and call their `flexclangCreatePass()` factory function.

- [ ] **Step 1: Create FlexPassLoader.h**

```cpp
// src/FlexPassLoader.h
#ifndef FLEXCLANG_FLEXPASSLOADER_H
#define FLEXCLANG_FLEXPASSLOADER_H

#include "llvm/ADT/StringRef.h"

namespace llvm {
class Pass;
} // namespace llvm

namespace flexclang {

/// Load a MIR pass plugin from the given .so path.
/// The .so must export: extern "C" MachineFunctionPass* flexclangCreatePass();
/// Returns a new Pass instance, or nullptr on failure.
llvm::Pass *loadMIRPassPlugin(llvm::StringRef soPath);

/// Load an IR pass plugin from the given .so path and register it.
/// The .so must export: extern "C" PassPluginLibraryInfo llvmGetPassPluginInfo();
/// Returns true on success.
bool loadIRPassPlugin(llvm::StringRef soPath);

} // namespace flexclang

#endif // FLEXCLANG_FLEXPASSLOADER_H
```

- [ ] **Step 2: Create FlexPassLoader.cpp**

```cpp
// src/FlexPassLoader.cpp
#include "FlexPassLoader.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

Pass *loadMIRPassPlugin(StringRef soPath) {
  std::string errMsg;
  auto Lib =
      sys::DynamicLibrary::getPermanentLibrary(soPath.str().c_str(), &errMsg);
  if (!Lib.isValid()) {
    errs() << "flexclang: error: cannot load plugin '" << soPath
           << "': " << errMsg << "\n";
    return nullptr;
  }

  using CreatePassFn = MachineFunctionPass *(*)();
  auto *CreatePass = reinterpret_cast<CreatePassFn>(
      Lib.getAddressOfSymbol("flexclangCreatePass"));
  if (!CreatePass) {
    errs() << "flexclang: error: plugin '" << soPath
           << "' does not export flexclangCreatePass()\n";
    return nullptr;
  }

  Pass *P = CreatePass();
  if (!P) {
    errs() << "flexclang: error: flexclangCreatePass() returned null in '"
           << soPath << "'\n";
    return nullptr;
  }

  // Print plugin name if available.
  using PassNameFn = const char *(*)();
  auto *GetName = reinterpret_cast<PassNameFn>(
      Lib.getAddressOfSymbol("flexclangPassName"));
  if (GetName) {
    errs() << "flexclang: loaded MIR pass plugin '" << GetName() << "' from "
           << soPath << "\n";
  }

  return P;
}

bool loadIRPassPlugin(StringRef soPath) {
  auto Plugin = PassPlugin::Load(soPath.str());
  if (!Plugin) {
    errs() << "flexclang: error: cannot load IR plugin '" << soPath
           << "': " << toString(Plugin.takeError()) << "\n";
    return false;
  }
  errs() << "flexclang: loaded IR pass plugin '" << Plugin->getPluginName()
         << "' from " << soPath << "\n";
  return true;
}

} // namespace flexclang
```

- [ ] **Step 3: Update CMakeLists.txt**

Add `src/FlexPassLoader.cpp`:

```cmake
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
  src/FlexPassNameRegistry.cpp
  src/FlexPassLoader.cpp
)
```

- [ ] **Step 4: Build and verify**

```bash
cd build && make -j$(nproc) 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add src/FlexPassLoader.h src/FlexPassLoader.cpp CMakeLists.txt
git commit -m "feat: FlexPassLoader loads MIR/IR pass plugins from .so files"
```

---

### Task 5: FlexPassConfigCallback -- MIR Pipeline Interception

**Files:**
- Create: `src/FlexPassConfigCallback.h`
- Create: `src/FlexPassConfigCallback.cpp`
- Modify: `src/main.cpp` (register the callback)
- Modify: `CMakeLists.txt`

This is the core of flexclang. Register a `RegisterTargetPassConfigCallback` that receives the `TargetPassConfig*` and applies MIR pass rules (disable, replace, insert-after).

- [ ] **Step 1: Create FlexPassConfigCallback.h**

```cpp
// src/FlexPassConfigCallback.h
#ifndef FLEXCLANG_FLEXPASSCONFIGCALLBACK_H
#define FLEXCLANG_FLEXPASSCONFIGCALLBACK_H

#include "FlexConfig.h"

namespace flexclang {

/// Register a global TargetPassConfig callback that will apply the
/// MIR pass rules from the given FlexConfig.
/// Must be called before ExecuteCompilerInvocation().
/// The config reference must outlive the compilation.
void registerFlexPassConfigCallback(const FlexConfig &config);

} // namespace flexclang

#endif // FLEXCLANG_FLEXPASSCONFIGCALLBACK_H
```

- [ ] **Step 2: Create FlexPassConfigCallback.cpp**

```cpp
// src/FlexPassConfigCallback.cpp
#include "FlexPassConfigCallback.h"
#include "FlexPassLoader.h"
#include "FlexPassNameRegistry.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/Target/RegisterTargetPassConfigCallback.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace flexclang {

// Static storage for the callback registration.
// Must live for the duration of the process.
static std::unique_ptr<RegisterTargetPassConfigCallback> FlexCallbackReg;

void registerFlexPassConfigCallback(const FlexConfig &config) {
  FlexCallbackReg = std::make_unique<RegisterTargetPassConfigCallback>(
      [&config](TargetMachine &TM, PassManagerBase &PM,
                TargetPassConfig *PassConfig) {
        // Only apply to AMDGPU targets.
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
            errs() << "flexclang: disabled MIR pass '" << rule.target << "'\n";
            break;
          }
          case MIRPassRule::Replace: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            Pass *Replacement = loadMIRPassPlugin(rule.plugin);
            if (!Replacement) break;
            PassConfig->substitutePass(ID, Replacement);
            errs() << "flexclang: replaced MIR pass '" << rule.target
                   << "' with plugin " << rule.plugin << "\n";
            break;
          }
          case MIRPassRule::InsertAfter: {
            const void *ID = resolvePassID(rule.target);
            if (!ID) break;
            Pass *NewPass = loadMIRPassPlugin(rule.plugin);
            if (!NewPass) break;
            PassConfig->insertPass(ID, NewPass);
            errs() << "flexclang: inserted MIR pass after '" << rule.target
                   << "' from plugin " << rule.plugin << "\n";
            break;
          }
          case MIRPassRule::InsertAt: {
            // insert-at uses hook names, not pass names.
            // Hook names are resolved by the virtual method overrides in
            // GCNPassConfig. We map hook names to the last pass added by
            // each hook, then use insertPass.
            // For now, treat insert-at the same as insert-after with the
            // hook name as the pass name. This will be refined.
            const void *ID = resolvePassID(rule.target);
            if (!ID) {
              errs() << "flexclang: warning: hook '" << rule.target
                     << "' could not be resolved to a pass ID\n";
              break;
            }
            Pass *NewPass = loadMIRPassPlugin(rule.plugin);
            if (!NewPass) break;
            PassConfig->insertPass(ID, NewPass);
            errs() << "flexclang: inserted MIR pass at hook '" << rule.target
                   << "' from plugin " << rule.plugin << "\n";
            break;
          }
          }
        }
      });
}

} // namespace flexclang
```

- [ ] **Step 3: Update main.cpp to register the callback**

Add after YAML parsing, before `CompilerInvocation::CreateFromArgs`:

```cpp
// At the top, add:
#include "FlexPassConfigCallback.h"

// After YAML parsing (config is populated), add:
  // Register MIR pass interception callback.
  if (config.hasModifications() || config.listPasses) {
    flexclang::registerFlexPassConfigCallback(config);
  }
```

- [ ] **Step 4: Update CMakeLists.txt**

```cmake
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
  src/FlexPassNameRegistry.cpp
  src/FlexPassLoader.cpp
  src/FlexPassConfigCallback.cpp
)
```

- [ ] **Step 5: Build and test MIR pass disable**

```bash
cd build && make -j$(nproc) 2>&1 | tail -5

# Test: disable si-form-memory-clauses on a simple kernel
echo '__global__ void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip
./flexclang --flex-disable-pass=si-form-memory-clauses \
  -cc1 -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -emit-obj -o /tmp/k.o /tmp/k.hip 2>&1
```

Expected: Output includes `flexclang: disabled MIR pass 'si-form-memory-clauses'`.

- [ ] **Step 6: Commit**

```bash
git add src/FlexPassConfigCallback.h src/FlexPassConfigCallback.cpp \
        src/main.cpp CMakeLists.txt
git commit -m "feat: RegisterTargetPassConfigCallback for MIR pass interception"
```

---

### Task 6: IR Pass Disable via PassInstrumentationCallbacks

**Files:**
- Modify: `src/main.cpp` (register IR pass disable callbacks via PassBuilderCallbacks)

Add support for `--flex-disable-ir-pass` by hooking into the `PassBuilder` via `CodeGenOpts.PassBuilderCallbacks` and registering a `shouldRunOptionalPassCallback`.

- [ ] **Step 1: Add IR pass disable to main.cpp**

After `CompilerInvocation::CreateFromArgs` succeeds, before `ExecuteCompilerInvocation`:

```cpp
// Add includes at top:
#include "llvm/Passes/PassBuilder.h"

// After CompilerInstance creation, before ExecuteCompilerInvocation:

  // Register IR pass modifications.
  {
    // Collect IR pass names to disable.
    std::vector<std::string> irPassesToDisable;
    for (const auto &rule : config.irRules) {
      if (rule.action == flexclang::IRPassRule::Disable)
        irPassesToDisable.push_back(rule.target);
    }

    // Collect IR plugins to load.
    std::vector<std::string> irPluginsToLoad;
    for (const auto &rule : config.irRules) {
      if (rule.action == flexclang::IRPassRule::LoadPlugin)
        irPluginsToLoad.push_back(rule.plugin);
    }

    if (!irPassesToDisable.empty() || !irPluginsToLoad.empty()) {
      // Push a PassBuilder callback into CodeGenOpts.
      Clang->getCodeGenOpts().PassBuilderCallbacks.push_back(
          [irPassesToDisable, irPluginsToLoad](PassBuilder &PB) {
            // Register shouldRunOptionalPassCallback to skip disabled passes.
            if (!irPassesToDisable.empty()) {
              auto *PIC = PB.getPassInstrumentationCallbacks();
              if (PIC) {
                PIC->registerShouldRunOptionalPassCallback(
                    [irPassesToDisable](StringRef PassName, Any) {
                      for (const auto &disabled : irPassesToDisable) {
                        if (PassName.contains(disabled)) {
                          errs() << "flexclang: skipping IR pass '"
                                 << PassName << "'\n";
                          return false;
                        }
                      }
                      return true;
                    });
              }
            }

            // Load IR pass plugins.
            for (const auto &pluginPath : irPluginsToLoad) {
              auto Plugin = PassPlugin::Load(pluginPath);
              if (Plugin) {
                Plugin->registerPassBuilderCallbacks(PB);
                errs() << "flexclang: loaded IR plugin '"
                       << Plugin->getPluginName() << "'\n";
              } else {
                errs() << "flexclang: error loading IR plugin '"
                       << pluginPath << "': "
                       << toString(Plugin.takeError()) << "\n";
              }
            }
          });
    }
  }
```

- [ ] **Step 2: Build and test IR pass disable**

```bash
cd build && make -j$(nproc) 2>&1 | tail -5

echo 'int foo(int x) { return x + 1; }' > /tmp/test_ir.c
./flexclang --flex-disable-ir-pass=instcombine \
  -cc1 -O2 -emit-llvm -o /tmp/test_ir.ll /tmp/test_ir.c 2>&1
```

Expected: Output includes `flexclang: skipping IR pass` lines containing `instcombine`.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat: --flex-disable-ir-pass via shouldRunOptionalPassCallback"
```

---

### Task 7: --flex-list-passes Implementation

**Files:**
- Modify: `src/FlexPassConfigCallback.cpp` (add pass listing to the callback)
- Modify: `src/main.cpp` (handle list-passes early exit)

Implement `--flex-list-passes` which dumps all MIR pass names in pipeline order.

- [ ] **Step 1: Add pass listing to FlexPassConfigCallback.cpp**

In the `RegisterTargetPassConfigCallback` lambda, at the beginning (before applying rules), add pass listing. The callback receives the `TargetPassConfig*` -- we can iterate the `PassRegistry` to list all registered passes:

```cpp
// In FlexPassConfigCallback.cpp, inside the callback lambda, add at the start:
        if (config.listPasses) {
          errs() << "=== MIR Codegen Pipeline (registered passes) ===\n";
          const PassRegistry &PR = *PassRegistry::getPassRegistry();
          // List passes that are relevant to codegen.
          // The PassRegistry contains all registered passes; we print those
          // that have a pass argument (used by -start-before/-stop-after).
          // For a complete ordered list, we would need to trace addMachinePasses(),
          // but for discovery, the registered names are sufficient.
          PR.enumerateWith([](const PassInfo &PI) {
            if (!PI.getPassArgument().empty())
              errs() << "  " << PI.getPassArgument() << " - "
                     << PI.getPassName() << "\n";
          });
          errs() << "\n=== Hook Points (for --flex-insert-at) ===\n";
          errs() << "  pre-isel, machine-ssa-opt, ilp-opts, pre-regalloc,\n";
          errs() << "  post-regalloc, pre-sched2, pre-emit, pre-emit2\n";
        }
```

Wait -- `PassRegistry` might not have an `enumerateWith` method. Let me check the actual API and use what's available. The simplest approach is to print all passes found in PassRegistry. If `enumerateWith` doesn't exist, we'll use a different approach -- intercepting `addPass` calls by wrapping the callback.

Actually, a simpler approach: use `-debug-pass=Structure` via LLVM's `cl::opt` system to dump the pass structure, then parse and format it. But that's fragile.

The most robust approach for Phase 1: print a known static list of AMDGPU MIR passes (from our research of `GCNPassConfig`), plus any passes registered in `PassRegistry`. Let me simplify:

```cpp
// In FlexPassConfigCallback.cpp, inside the callback lambda, add at the start:
        if (config.listPasses) {
          errs() << "=== MIR Pass Names (use with --flex-disable-pass, etc.) ===\n";
          // Iterate all registered passes
          unsigned count = 0;
          for (auto I = PassRegistry::getPassRegistry()->begin(),
                    E = PassRegistry::getPassRegistry()->end();
               I != E; ++I) {
            const PassInfo &PI = **I;
            if (!PI.getPassArgument().empty()) {
              errs() << "  [" << ++count << "] " << PI.getPassArgument()
                     << "\n";
            }
          }
          errs() << "\n=== Hook Points (for --flex-insert-at) ===\n";
          errs() << "  pre-isel, machine-ssa-opt, ilp-opts, pre-regalloc,\n";
          errs() << "  post-regalloc, pre-sched2, pre-emit, pre-emit2\n";
          // Don't actually compile -- just exit.
        }
```

Hmm, `PassRegistry` may not expose iterators either. Let me check.

- [ ] **Step 1 (revised): Check PassRegistry API and implement listing**

First, verify what iteration API exists:

```bash
grep -n 'begin\|end\|enumerate\|forEach\|iterator' \
  /home/poyechen/workspace/repo/llvm-project/llvm/include/llvm/PassRegistry.h
```

If no iteration API exists, we use a different approach: set `-debug-pass=Arguments` via `cl::ParseCommandLineOptions` before the compilation runs, capture stderr, and format it. Or simpler: just document that users can use `-mllvm -debug-pass=Arguments` as a workaround, and implement a proper `--flex-list-passes` by hooking into the pipeline construction.

For Phase 1, implement a minimal version that simply runs `PassConfig->addMachinePasses()` and captures which passes are added by instrumenting the `addPass` call. Since we can't override `addPass` (it's not virtual), we instead use `-mllvm -debug-pass=Structure` and parse the output.

Simplest viable implementation:

```cpp
// In main.cpp, handle --flex-list-passes by setting -debug-pass=Structure
// and compiling /dev/null:
  if (config.listPasses) {
    // Forward -debug-pass=Structure to LLVM to print the pass pipeline.
    const char *debugPassArgs[] = {"flexclang", "-debug-pass=Structure"};
    cl::ParseCommandLineOptions(2, debugPassArgs, "flexclang");
    // Continue with normal compilation -- the debug output will show all passes.
  }
```

- [ ] **Step 2: Build and test**

```bash
cd build && make -j$(nproc) 2>&1 | tail -5

echo '__global__ void k() {}' > /tmp/k.hip
./flexclang --flex-list-passes \
  -cc1 -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -emit-obj -o /dev/null /tmp/k.hip 2>&1 | head -50
```

Expected: Pass structure output including machine-scheduler, si-form-memory-clauses, etc.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp src/FlexPassConfigCallback.cpp
git commit -m "feat: --flex-list-passes shows MIR pipeline structure"
```

---

### Task 8: Example IR Pass Plugin

**Files:**
- Create: `examples/ir-pass-counter/IRInstCounter.cpp`
- Create: `examples/ir-pass-counter/CMakeLists.txt`

- [ ] **Step 1: Create IRInstCounter.cpp**

Copy from spec Section 8.1 (the full source is in the design spec).

- [ ] **Step 2: Create CMakeLists.txt for the plugin**

```cmake
# examples/ir-pass-counter/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(IRInstCounter)

find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})

add_library(ir-inst-counter MODULE IRInstCounter.cpp)
target_compile_features(ir-inst-counter PRIVATE cxx_std_17)
set_target_properties(ir-inst-counter PROPERTIES PREFIX "")
```

- [ ] **Step 3: Build the plugin**

```bash
cd examples/ir-pass-counter
mkdir -p build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build
make -j$(nproc)
ls -la ir-inst-counter.so
```

- [ ] **Step 4: Test with flexclang**

```bash
echo 'int foo(int x) { return x * 2 + 1; }' > /tmp/test.c
../../build/flexclang -fpass-plugin=./ir-inst-counter.so \
  -cc1 -O2 -emit-obj -o /tmp/test.o /tmp/test.c 2>&1 | grep IRInstCounter
```

Expected: `[IRInstCounter] foo: N instructions`

- [ ] **Step 5: Commit**

```bash
git add examples/ir-pass-counter/
git commit -m "feat: example IR pass plugin (instruction counter)"
```

---

### Task 9: Example MIR Pass Plugin

**Files:**
- Create: `examples/mir-pass-nop-inserter/MIRNopInserter.cpp`
- Create: `examples/mir-pass-nop-inserter/CMakeLists.txt`

- [ ] **Step 1: Create MIRNopInserter.cpp**

Copy from spec Section 8.2. Note: the AMDGPU-specific `AMDGPU::S_NOP` requires including AMDGPU headers. Since the plugin links against LLVMAMDGPUCodeGen, this is available.

- [ ] **Step 2: Create CMakeLists.txt**

```cmake
# examples/mir-pass-nop-inserter/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(MIRNopInserter)

find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})

llvm_map_components_to_libnames(LLVM_LIBS
  AMDGPUCodeGen AMDGPUDesc AMDGPUInfo CodeGen Core Support
)

add_library(mir-nop-inserter MODULE MIRNopInserter.cpp)
target_link_libraries(mir-nop-inserter PRIVATE ${LLVM_LIBS})
target_compile_features(mir-nop-inserter PRIVATE cxx_std_17)
set_target_properties(mir-nop-inserter PROPERTIES PREFIX "")
```

- [ ] **Step 3: Build the plugin**

```bash
cd examples/mir-pass-nop-inserter
mkdir -p build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build
make -j$(nproc)
ls -la mir-nop-inserter.so
```

- [ ] **Step 4: Test with flexclang**

```bash
echo '__global__ void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip
../../build/flexclang \
  --flex-insert-after=machine-scheduler:./mir-nop-inserter.so \
  -cc1 -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -S -o /tmp/k.s /tmp/k.hip 2>&1
grep -c "s_nop" /tmp/k.s
```

Expected: Output includes `flexclang: inserted MIR pass after 'machine-scheduler'` and `s_nop` appears in the assembly.

- [ ] **Step 5: Commit**

```bash
git add examples/mir-pass-nop-inserter/
git commit -m "feat: example MIR pass plugin (NOP inserter)"
```

---

### Task 10: Test Kernel, Config Examples, and Validation Script

**Files:**
- Create: `examples/test_kernel.hip`
- Create: `examples/configs/disable-memory-clauses.yaml`
- Create: `examples/configs/insert-nop-after-sched.yaml`
- Create: `examples/configs/combined.yaml`
- Create: `examples/validate.sh`

- [ ] **Step 1: Create test_kernel.hip**

Copy from spec Section 8.3.

- [ ] **Step 2: Create YAML config examples**

Copy from spec Section 8.4 (three files).

- [ ] **Step 3: Create validate.sh**

Copy from spec Section 8.5. Make it executable.

- [ ] **Step 4: Run validation**

```bash
cd /home/poyechen/workspace/repo/friendly-clang
chmod +x examples/validate.sh
FLEXCLANG=./build/flexclang ./examples/validate.sh
```

Expected: All 6 tests pass.

- [ ] **Step 5: Commit**

```bash
git add examples/
git commit -m "feat: test kernel, YAML configs, and validation script"
```

---

### Task 11: End-to-End Integration Test

**Files:**
- None new (uses existing build artifacts)

Verify the complete workflow: build flexclang, build both example plugins, run the validation script.

- [ ] **Step 1: Clean build everything**

```bash
cd /home/poyechen/workspace/repo/friendly-clang
rm -rf build examples/ir-pass-counter/build examples/mir-pass-nop-inserter/build

# Build flexclang
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build
make -j$(nproc)

# Build IR plugin
cd ../examples/ir-pass-counter
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build
make -j$(nproc)

# Build MIR plugin
cd ../../mir-pass-nop-inserter
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build
make -j$(nproc)
```

- [ ] **Step 2: Run validation**

```bash
cd /home/poyechen/workspace/repo/friendly-clang
FLEXCLANG=./build/flexclang ./examples/validate.sh
```

Expected: `All tests passed!`

- [ ] **Step 3: Test bit-identical output (no flex flags)**

```bash
echo '__global__ void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip

# Compile with flexclang (no flex flags)
./build/flexclang -cc1 -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -S -o /tmp/flex.s /tmp/k.hip

# Compile with upstream clang (from LLVM build)
/home/poyechen/workspace/repo/llvm-project/build/bin/clang \
  -cc1 -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -S -o /tmp/upstream.s /tmp/k.hip

# Compare
diff /tmp/flex.s /tmp/upstream.s
echo "Diff exit code: $? (0 = identical)"
```

Expected: Exit code 0 (bit-identical output).

- [ ] **Step 4: Commit final state**

```bash
git add -A
git commit -m "chore: end-to-end integration verified"
```
