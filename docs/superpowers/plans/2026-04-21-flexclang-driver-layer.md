# flexclang Driver Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Add driver-level support so `CMAKE_CXX_COMPILER=flexclang` works for HIP builds, handling `--offload-arch`, host+device compilation, and offload bundling.

**Architecture:** flexclang becomes a dual-mode binary. If `argv[1] == "-cc1"`, it runs the existing cc1 path. Otherwise, it runs the clang `Driver` to orchestrate compilation, injecting `--flex-*` flags into AMDGCN cc1 jobs. The driver calls back into flexclang's own cc1 function in-process.

**Tech Stack:** C++17, LLVM/Clang Driver API, existing flexclang Phase 1 code

**Design Spec:** `docs/superpowers/specs/2026-04-21-flexclang-driver-layer.md`

**LLVM Source:** `/home/poyechen/workspace/repo/llvm-project` (ROCm/llvm-project, amd-staging branch)

---

## File Structure

```
friendly-clang/
  src/
    main.cpp                  # Restructure: new main() dispatch, flexclang_cc1_main(), flexclang_driver_main()
    FlexConfig.h              # Add originalFlexArgs field
    FlexConfig.cpp            # Populate originalFlexArgs during parsing
  examples/
    validate.sh               # Add -cc1 to cc1 tests, add driver-mode tests
    test_kernel.hip            # Restore full HIP syntax for driver-mode testing
```

---

### Task 1: Add `originalFlexArgs` to FlexConfig

**Files:**
- Modify: `src/FlexConfig.h`
- Modify: `src/FlexConfig.cpp`

Save the raw `--flex-*` argv strings during parsing so they can be re-injected into cc1 commands in driver mode.

- [x] **Step 1: Add `originalFlexArgs` field to FlexConfig**

In `src/FlexConfig.h`, add a new field to `FlexConfig` struct after `dryRun`:

```cpp
  bool dryRun = false;
  std::vector<std::string> originalFlexArgs; // Raw --flex-* strings for driver mode injection
```

- [x] **Step 2: Save original flex strings in `parseFlexArgs`**

In `src/FlexConfig.cpp`, inside each `--flex-*` branch (lines 27-46), add `config.originalFlexArgs.push_back(argv[i]);` before the existing logic. The cleanest way: save the original string at the top of the if/else chain, before `consume_front` modifies the StringRef.

Replace the loop body (lines 24-49 of `FlexConfig.cpp`) with:

```cpp
  for (int i = 1; i < argc; ++i) {
    StringRef arg(argv[i]);

    if (arg.starts_with("--flex-")) {
      config.originalFlexArgs.push_back(argv[i]);
    }

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
```

Note: the `originalFlexArgs.push_back` must happen BEFORE `consume_front` modifies the StringRef. The `starts_with("--flex-")` check is separate from the if/else chain so it captures the original string before any consume_front.

- [x] **Step 3: Build**

```bash
cd /home/poyechen/workspace/repo/friendly-clang/build && make -j$(nproc) 2>&1 | tail -3
```

Expected: Build succeeds.

- [x] **Step 4: Test that existing behavior is unchanged**

```bash
echo 'int foo(int x) { return x + 1; }' > /tmp/test.c
./flexclang --flex-verbose --flex-disable-ir-pass=instcombine \
  -O2 -emit-llvm -o /tmp/test.ll /tmp/test.c 2>&1 | head -3
echo "Exit: $?"
```

Expected: Still shows "skipping IR pass" messages, exits 0.

- [x] **Step 5: Commit**

```bash
git add src/FlexConfig.h src/FlexConfig.cpp
git commit -m "feat: save original --flex-* args for driver mode injection"
```

---

### Task 2: Extract `flexclang_cc1_main` from `main()`

**Files:**
- Modify: `src/main.cpp`

Refactor the current `main()` into a static `flexclang_cc1_main()` function matching the `Driver::CC1ToolFunc` signature. The new `main()` dispatches based on `-cc1`.

- [x] **Step 1: Add new includes and forward declarations**

Add at the top of `src/main.cpp` after existing includes:

```cpp
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
```

- [x] **Step 2: Extract cc1 function**

Rename the current `main()` to `flexclang_cc1_main` with this signature:

```cpp
static int flexclang_cc1_main(SmallVectorImpl<const char *> &ArgV) {
  cl::ResetAllOptionOccurrences();

  // ArgV[0] = executable path, ArgV[1] = "-cc1" (when called from driver)
  // or ArgV[0] = program name (when called from main's -cc1 check)
  // parseFlexArgs starts at i=1, skipping ArgV[0].
  // We pass ArgV.data()+1, ArgV.size()-1 so parseFlexArgs's i=1 start
  // skips "-cc1" (or whatever ArgV[1] is).
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

  if (config.hasModifications()) {
    flexclang::registerFlexPassConfigCallback(config);
  }

  ArrayRef<const char *> Args(clangArgs);
  auto Invocation = std::make_shared<CompilerInvocation>();
  bool Success =
      CompilerInvocation::CreateFromArgs(*Invocation, Args, Diags, ArgV[0]);

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

  if (config.listPasses) {
    const char *args[] = {"flexclang", "-debug-pass=Structure"};
    cl::ParseCommandLineOptions(2, args, "flexclang pass listing\n");
  }

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

    if (!irDisable.empty() || !irPlugins.empty()) {
      bool verbose = config.verbose;
      auto irDisableShared = std::make_shared<std::vector<std::string>>(irDisable);
      Clang->getCodeGenOpts().PassBuilderCallbacks.push_back(
          [irDisableShared, irPlugins, verbose, irMatched](PassBuilder &PB) {
            if (!irDisableShared->empty()) {
              auto *PIC = PB.getPassInstrumentationCallbacks();
              if (PIC) {
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

  for (const auto &d : irDisable) {
    if (!irMatched->count(d))
      errs() << "flexclang: warning: IR pass '" << d
             << "' was not disabled (pass may be required or name misspelled)\n";
  }

  return Success ? 0 : 1;
}
```

Key changes from the original `main()`:
- Signature: `static int flexclang_cc1_main(SmallVectorImpl<const char *> &ArgV)`
- Adds `cl::ResetAllOptionOccurrences()` at the top (multi-job safety)
- Removes `InitLLVM`, target initialization (moved to `main()`)
- Uses `ArgV[0]` instead of `argv[0]` for `CreateFromArgs`
- Computes `cc1Argc`/`cc1Argv` from `ArgV` to skip the first element

- [x] **Step 3: Write new `main()` dispatch**

Add below `flexclang_cc1_main`:

```cpp
int main(int argc, const char **argv) {
  InitLLVM X(argc, argv);

  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  // Check for cc1 mode
  if (argc >= 2 && StringRef(argv[1]) == "-cc1") {
    SmallVector<const char *, 256> ArgV(argv, argv + argc);
    return flexclang_cc1_main(ArgV);
  }

  // Driver mode (placeholder -- implemented in Task 3)
  errs() << "flexclang: driver mode not yet implemented. Use -cc1 for cc1 mode.\n";
  return 1;
}
```

- [x] **Step 4: Build and test cc1 mode still works**

```bash
cd /home/poyechen/workspace/repo/friendly-clang/build && cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build && make -j$(nproc) 2>&1 | tail -5
```

Test cc1 mode with `-cc1` prefix:

```bash
echo 'int main() { return 0; }' > /tmp/test.c
./flexclang -cc1 -emit-obj -o /tmp/test.o /tmp/test.c
echo "cc1 mode exit: $?"
```

Expected: Exit 0.

Test that driver mode shows placeholder message:

```bash
./flexclang /tmp/test.c -o /tmp/test.o 2>&1
echo "Driver mode exit: $?"
```

Expected: "flexclang: driver mode not yet implemented", exit 1.

- [x] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "refactor: extract flexclang_cc1_main, add -cc1 dispatch"
```

---

### Task 3: Implement Driver Mode

**Files:**
- Modify: `src/main.cpp`

Implement `flexclang_driver_main()` using clang's `Driver` class.

- [x] **Step 1: Add helper function to detect AMDGCN cc1 jobs**

Add above `main()` in `src/main.cpp`:

```cpp
/// Check if a Command's arguments contain -triple amdgcn*.
static bool hasAMDGCNTriple(const llvm::opt::ArgStringList &Args) {
  for (size_t i = 0; i + 1 < Args.size(); ++i) {
    if (StringRef(Args[i]) == "-triple" &&
        StringRef(Args[i + 1]).starts_with("amdgcn"))
      return true;
  }
  return false;
}
```

- [x] **Step 2: Implement `flexclang_driver_main`**

Add above `main()`:

```cpp
static int flexclang_driver_main(int argc, const char **argv) {
  // Strip --flex-* flags from argv
  SmallVector<const char *, 256> driverArgs;
  driverArgs.push_back(argv[0]); // program name for Driver
  flexclang::FlexConfig config =
      flexclang::parseFlexArgs(driverArgs, argc, argv);

  if (!config.configFile.empty()) {
    if (!flexclang::parseFlexYAML(config, config.configFile))
      return 1;
  }

  // Handle --flex-dry-run in driver mode
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

  // Resolve executable path for the Driver
  void *MainAddr = (void *)(intptr_t)flexclang_driver_main;
  std::string ExePath =
      llvm::sys::fs::getMainExecutable(argv[0], MainAddr);

  // Create driver diagnostics (TextDiagnosticPrinter for driver mode)
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID = DiagnosticIDs::create();
  DiagnosticOptions DiagOpts;
  auto *DiagPrinter = new TextDiagnosticPrinter(errs(), DiagOpts);
  DiagnosticsEngine Diags(DiagID, DiagOpts, DiagPrinter);

  // Create the Driver
  clang::driver::Driver TheDriver(ExePath,
                                   llvm::sys::getDefaultTargetTriple(),
                                   Diags, "flexclang LLVM compiler");

  // Set CC1Main for in-process cc1 execution
  TheDriver.CC1Main = [](SmallVectorImpl<const char *> &ArgV) {
    return flexclang_cc1_main(ArgV);
  };
  CrashRecoveryContext::Enable();

  // Build the compilation
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
```

- [x] **Step 3: Replace the driver mode placeholder in `main()`**

Replace the placeholder in `main()`:

```cpp
int main(int argc, const char **argv) {
  InitLLVM X(argc, argv);

  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  // Check for cc1 mode
  if (argc >= 2 && StringRef(argv[1]) == "-cc1") {
    SmallVector<const char *, 256> ArgV(argv, argv + argc);
    return flexclang_cc1_main(ArgV);
  }

  // Driver mode
  return flexclang_driver_main(argc, argv);
}
```

- [x] **Step 4: Build**

```bash
cd /home/poyechen/workspace/repo/friendly-clang/build && cmake .. -DCMAKE_PREFIX_PATH=/home/poyechen/workspace/repo/llvm-project/build && make -j$(nproc) 2>&1 | tail -5
```

Expected: Build succeeds. Fix any missing includes or link errors. If link errors occur for `clang::driver::Driver`, try adding `clangLex` to `target_link_libraries` in `CMakeLists.txt`.

- [x] **Step 5: Test driver mode with simple C file**

```bash
echo 'int main() { return 0; }' > /tmp/test.c
./flexclang /tmp/test.c -c -o /tmp/test.o
echo "Driver mode C: $?"
```

Expected: Exit 0 (driver compiles C file via cc1).

- [x] **Step 6: Test driver mode with HIP kernel (requires co-installation)**

First, check if flexclang can find HIP headers by checking where the linked clang is:

```bash
CLANG_BIN=$(dirname $(readlink -f /home/poyechen/workspace/repo/llvm-project/build/bin/clang))
echo "Clang bin: $CLANG_BIN"

# Copy flexclang alongside clang for testing
cp ./flexclang $CLANG_BIN/flexclang

# Test driver mode HIP
echo '__attribute__((amdgpu_kernel)) void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip
$CLANG_BIN/flexclang -x hip /tmp/k.hip --offload-arch=gfx942 -c -o /tmp/k.o 2>&1
echo "Driver mode HIP: $?"
```

Expected: Exit 0. If HIP headers are missing, the error will be about `hip_runtime.h` -- that's a setup issue, not a flexclang bug.

- [x] **Step 7: Test flex flags in driver mode**

```bash
$CLANG_BIN/flexclang --flex-verbose --flex-disable-pass=machine-scheduler \
  -x hip /tmp/k.hip --offload-arch=gfx942 -c -o /tmp/k.o 2>&1 | grep "flexclang:"
```

Expected: Shows `flexclang: requesting disable of MIR pass 'machine-scheduler'` (from the AMDGCN cc1 job only, not from the host cc1 job).

- [x] **Step 8: Commit**

```bash
git add src/main.cpp CMakeLists.txt
git commit -m "feat: driver mode for CMAKE_CXX_COMPILER=flexclang support"
```

---

### Task 4: Update validate.sh and test_kernel.hip

**Files:**
- Modify: `examples/validate.sh`
- Modify: `examples/test_kernel.hip`

Update the validation script to add `-cc1` to existing cc1-mode tests and add new driver-mode tests.

- [x] **Step 1: Update validate.sh**

Rewrite `examples/validate.sh` to include both cc1-mode and driver-mode test sections:

```bash
#!/bin/bash
# examples/validate.sh
# Validates flexclang pass plugin system in both cc1 and driver modes

set -e
FLEXCLANG=${FLEXCLANG:-./build/flexclang}
ARCH=${ARCH:-gfx942}
KERNEL=examples/test_kernel.hip

# cc1-style flags
CC1_FLAGS="-x hip -triple amdgcn-amd-amdhsa -target-cpu $ARCH -O2"

# Plugin paths (relative to repo root)
IR_PLUGIN=examples/ir-pass-counter/build/ir-inst-counter.so
MIR_PLUGIN=examples/mir-pass-nop-inserter/build/mir-nop-inserter.so

echo "========================================="
echo "  cc1 Mode Tests"
echo "========================================="

echo "=== cc1 Test 1: Baseline compilation ==="
$FLEXCLANG -cc1 $CC1_FLAGS -S -o /tmp/baseline.s $KERNEL
echo "PASS: Baseline compiles"

echo "=== cc1 Test 2: --flex-list-passes ==="
$FLEXCLANG -cc1 --flex-list-passes $CC1_FLAGS -S -o /dev/null /dev/null \
  > /tmp/pass-list.txt 2>&1
grep -q "machine-scheduler" /tmp/pass-list.txt
echo "PASS: machine-scheduler found in pass list"

echo "=== cc1 Test 3: Disable pass ==="
$FLEXCLANG -cc1 --flex-disable-pass=machine-scheduler \
  $CC1_FLAGS -S -o /tmp/disabled.s $KERNEL
if diff -q /tmp/baseline.s /tmp/disabled.s > /dev/null 2>&1; then
  echo "WARNING: disabling machine-scheduler produced identical output"
else
  echo "PASS: Disabling pass changed assembly output"
fi

echo "=== cc1 Test 4: IR pass plugin ==="
$FLEXCLANG -cc1 -fpass-plugin=$IR_PLUGIN \
  $CC1_FLAGS -S -o /tmp/ir-plugin.s $KERNEL \
  2>/tmp/ir-plugin-stderr.txt
grep -q "\[IRInstCounter\]" /tmp/ir-plugin-stderr.txt
echo "PASS: IR pass plugin ran and produced output"

echo "=== cc1 Test 5: MIR pass plugin ==="
$FLEXCLANG -cc1 --flex-insert-after=machine-scheduler:$MIR_PLUGIN \
  $CC1_FLAGS -S -o /tmp/mir-plugin.s $KERNEL \
  2>/tmp/mir-plugin-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/mir-plugin-stderr.txt
BASELINE_NOPS=$(grep -c "s_nop" /tmp/baseline.s || true)
PLUGIN_NOPS=$(grep -c "s_nop" /tmp/mir-plugin.s || true)
if [ "$PLUGIN_NOPS" -gt "$BASELINE_NOPS" ]; then
  echo "PASS: MIR pass plugin inserted NOPs (baseline=$BASELINE_NOPS, plugin=$PLUGIN_NOPS)"
else
  echo "FAIL: Expected more s_nop instructions"
  exit 1
fi

echo "=== cc1 Test 6: YAML config ==="
$FLEXCLANG -cc1 --flex-config=examples/configs/combined.yaml \
  $CC1_FLAGS -S -o /tmp/yaml-config.s $KERNEL \
  2>/tmp/yaml-stderr.txt
grep -q "\[IRInstCounter\]" /tmp/yaml-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/yaml-stderr.txt
echo "PASS: YAML config loaded both IR and MIR plugins"

echo ""
echo "========================================="
echo "  Driver Mode Tests"
echo "========================================="
echo "(Require flexclang co-installed with clang)"

# Driver mode needs flexclang in the same bin/ as clang
FLEXCLANG_DIR=$(dirname $(readlink -f $FLEXCLANG))
if [ ! -f "$FLEXCLANG_DIR/clang" ] && [ ! -L "$FLEXCLANG_DIR/clang" ]; then
  echo "SKIP: Driver mode tests require flexclang in same dir as clang"
  echo ""
  echo "cc1 tests passed!"
  exit 0
fi

echo "=== Driver Test 1: Baseline compilation ==="
$FLEXCLANG -x hip $KERNEL --offload-arch=$ARCH -O2 -S -o /tmp/driver-baseline.s
echo "PASS: Driver mode compiles"

echo "=== Driver Test 2: Flex flags in driver mode ==="
$FLEXCLANG --flex-verbose --flex-disable-pass=machine-scheduler \
  -x hip $KERNEL --offload-arch=$ARCH -O2 -S -o /tmp/driver-disabled.s \
  2>/tmp/driver-stderr.txt
grep -q "flexclang: disabled MIR pass" /tmp/driver-stderr.txt
echo "PASS: Flex flags work in driver mode"

echo ""
echo "All tests passed!"
```

Make it executable: `chmod +x examples/validate.sh`

- [x] **Step 2: Run cc1 tests**

```bash
cd /home/poyechen/workspace/repo/friendly-clang
FLEXCLANG=./build/flexclang ./examples/validate.sh
```

Expected: cc1 tests pass (driver tests may skip if not co-installed).

- [x] **Step 3: Commit**

```bash
git add examples/validate.sh
git commit -m "feat: update validate.sh for dual-mode (cc1 + driver) testing"
```

---

### Task 5: Bit-Identity Test

**Files:** None new.

Verify flexclang produces identical output to clang in both modes.

- [x] **Step 1: cc1 mode bit-identity**

```bash
echo '__attribute__((amdgpu_kernel)) void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip

./build/flexclang -cc1 -x hip -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -O2 -S -o /tmp/flex-cc1.s /tmp/k.hip

/home/poyechen/workspace/repo/llvm-project/build/bin/clang \
  -cc1 -x hip -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  -O2 -S -o /tmp/upstream-cc1.s /tmp/k.hip

diff /tmp/flex-cc1.s /tmp/upstream-cc1.s
echo "cc1 bit-identical: $? (0=yes)"
```

Expected: Exit 0.

- [x] **Step 2: Driver mode bit-identity (if co-installed)**

```bash
CLANG_BIN=$(dirname $(readlink -f /home/poyechen/workspace/repo/llvm-project/build/bin/clang))

$CLANG_BIN/flexclang -x hip /tmp/k.hip --offload-arch=gfx942 \
  -O2 -S -o /tmp/flex-driver.s 2>/dev/null

$CLANG_BIN/clang -x hip /tmp/k.hip --offload-arch=gfx942 \
  -O2 -S -o /tmp/upstream-driver.s 2>/dev/null

diff /tmp/flex-driver.s /tmp/upstream-driver.s
echo "Driver bit-identical: $? (0=yes)"
```

Expected: Exit 0.

- [x] **Step 3: Commit**

```bash
git commit --allow-empty -m "chore: driver layer integration verified"
```
