# flexclang Design Specification

**Date:** 2026-04-21
**Status:** Draft
**Author:** Design collaboration between AMDGPU HIP kernel developer and Clang System Designer

## 1. Problem Statement

AMDGPU HIP kernel developers face several pain points with the upstream LLVM/Clang compiler:

1. **Scheduler defaults to maximize occupancy** instead of ILP. The default `GCNMaxOccupancySchedStrategy` (AMDGPUTargetMachine.cpp:1346) prioritizes register pressure reduction for higher occupancy, often at the cost of instruction-level parallelism.

2. **Inaccurate instruction latency model.** Global memory loads are hardcoded to 80 cycles (`WriteVMEM` in SISchedule.td), LDS to 5 cycles (`WriteLDS`). These values are static across all GFX9 targets and don't reflect real measured latencies that vary by workload and memory access pattern.

3. **IGLP intrinsics partially ignored.** The `UnclusteredHighRPReschedule` stage explicitly skips IGLP mutation handling (GCNSchedStrategy.cpp:1780), potentially overriding user-specified instruction interleaving.

4. **No user control of register allocation.** The VGPR allocator treats VGPRs and AGPRs as a single pool via `AV_*` register classes. Users cannot hint that a variable should use AGPR or request double-buffering via separate register allocations.

5. **No per-pass enable/disable for Machine IR passes.** The only controls are `-start-before`/`-stop-after` for the legacy PM codegen pipeline. There's no mechanism to disable, replace, or insert individual MIR passes.

6. **Long upstream lead time.** Patches require review and execution by the compiler team. Some enhancements (e.g., inline asm improvements) are rejected on policy grounds, even when kernel developers need them for performance.

## 2. Solution: flexclang

**flexclang** is a standalone executable that links against upstream LLVM/Clang libraries. It acts as a drop-in replacement for clang, adding the ability to disable, replace, and insert custom passes at both the LLVM IR and Machine IR levels.

### Design Principles

- **No fork:** Link against installed LLVM/Clang libraries. No source modifications to LLVM.
- **Default transparency:** Without `--flex-*` flags or config, produces bit-identical output to upstream clang.
- **Drop-in replacement:** Accepts all clang flags. Users set `CXX=flexclang` in their build system.
- **Both IR and MIR:** Unified config covers LLVM IR optimization passes and Machine IR codegen passes.
- **Agent-loop friendly:** YAML config can be generated programmatically by optimization agents.

## 3. Architecture

```
+--------------------------------------------------------------+
|                      flexclang binary                        |
+--------------------------------------------------------------+
|  main()                                                      |
|   +-- Parse --flex-* flags + load YAML config                |
|   +-- Register RegisterTargetPassConfigCallback              |
|   |    +-- Callback receives TargetPassConfig* at codegen    |
|   |    +-- Calls disablePass() / substitutePass() /          |
|   |    |   insertPass() based on config rules                |
|   |    +-- Loads MIR pass plugins via FlexPassLoader         |
|   +-- Register PassBuilderCallbacks for IR pass mods         |
|   +-- Create CompilerInstance (reuse clang's)                |
|   +-- ExecuteCompilerInvocation()                            |
+--------------------------------------------------------------+
|  FlexPassLoader                                              |
|   +-- Loads .so via DynamicLibrary                           |
|   +-- Finds flexclangCreatePass() factory                    |
|   +-- Creates MachineFunctionPass instances on demand        |
+--------------------------------------------------------------+
|  Upstream LLVM/Clang libraries (linked, not modified)        |
|   clangDriver, clangFrontend, clangCodeGen,                  |
|   LLVMCore, LLVMAMDGPUCodeGen, ...                          |
+--------------------------------------------------------------+
```

### 3.1 Key Design Constraint

Both `GCNTargetMachine` and `GCNPassConfig` are declared `final` in the LLVM source. Neither can be subclassed. Therefore, flexclang uses LLVM's `RegisterTargetPassConfigCallback` mechanism (declared in `llvm/Target/RegisterTargetPassConfigCallback.h`) to receive a `TargetPassConfig*` after `createPassConfig()` is called but before `addISelPasses()` and `addMachinePasses()` execute. This callback receives the TargetPassConfig and can call:
- `disablePass(AnalysisID)` -- substitute with invalid pass (skip it)
- `substitutePass(AnalysisID, IdentifyingPassPtr)` -- replace with a different pass
- `insertPass(AnalysisID, IdentifyingPassPtr)` -- insert a pass after a named pass

These are public methods on `TargetPassConfig` that modify the pipeline before it runs.

### 3.2 Components

| Component | File | Responsibility |
|-----------|------|----------------|
| `main.cpp` | `src/main.cpp` | Entry point. Mirrors `clang_main()` but registers flexclang interceptors before calling `ExecuteCompilerInvocation()`. |
| `FlexConfig` | `src/FlexConfig.{h,cpp}` | Parses YAML config and CLI `--flex-*` flags into a unified `FlexConfig` struct. |
| `FlexPassConfigCallback` | `src/FlexPassConfigCallback.{h,cpp}` | Implements the `RegisterTargetPassConfigCallback` that applies MIR pass modifications (disable, replace, insert) using `TargetPassConfig` public API. |
| `FlexPassLoader` | `src/FlexPassLoader.{h,cpp}` | Loads `.so` files via `DynamicLibrary`, calls factory functions to create pass instances. |

### 3.3 Entry Point (main.cpp)

flexclang's `main()` performs these steps:

1. Initialize all LLVM targets (same as clang).
2. Parse command-line args, extracting `--flex-*` flags.
3. If `--flex-config=<path>` or `FLEXCLANG_CONFIG` env var is set, parse the YAML config.
4. Merge CLI flags into the config (CLI takes precedence over YAML).
5. Register MIR pass modifications via `RegisterTargetPassConfigCallback`. This callback fires after `createPassConfig()` returns the `GCNPassConfig` but before `addISelPasses()`/`addMachinePasses()` execute. In the callback, call `disablePass()`, `substitutePass()`, and `insertPass()` on the TargetPassConfig based on the parsed config.
6. Create a `CompilerInstance` + `CompilerInvocation` from remaining args (standard clang path).
7. Register IR pass modifications:
   - For each `ir-passes.disable` entry: register a `shouldRunOptionalPassCallback` that returns false for that pass name.
   - For each `ir-passes.load-plugin` entry: load the `.so` and call `registerPassBuilderCallbacks()` (same as `-fpass-plugin=`).
8. Call `ExecuteCompilerInvocation()` -- standard clang execution.

### 3.4 MIR Pass Interception (RegisterTargetPassConfigCallback)

flexclang uses LLVM's `RegisterTargetPassConfigCallback` mechanism. The callback is registered at startup and fires during `addPassesToEmitFile()` -> `addPassesToGenerateCode()` (CodeGenTargetMachineImpl.cpp:136).

```cpp
// In main.cpp, before ExecuteCompilerInvocation():
static RegisterTargetPassConfigCallback FlexCallback(
  [&Config](TargetMachine &TM, PassManagerBase &PM,
            TargetPassConfig *PassConfig) {
    // Only apply to AMDGPU targets
    if (TM.getTargetTriple().getArch() != Triple::amdgcn)
      return;

    // Apply disable rules
    for (const auto &Rule : Config.getDisabledPasses()) {
      AnalysisID ID = resolvePassID(Rule.target);
      if (ID) {
        if (Config.isCriticalPass(Rule.target))
          errs() << "flexclang: warning: disabling '" << Rule.target
                 << "' may cause incorrect code generation\n";
        PassConfig->disablePass(ID);
      }
    }

    // Apply replace rules (substitute with dynamically loaded pass)
    for (const auto &Rule : Config.getReplacePasses()) {
      AnalysisID ID = resolvePassID(Rule.target);
      Pass *Replacement = FlexPassLoader::load(Rule.plugin);
      if (ID && Replacement)
        PassConfig->substitutePass(ID, Replacement);
    }

    // Apply insert-after rules
    for (const auto &Rule : Config.getInsertAfterPasses()) {
      AnalysisID TargetID = resolvePassID(Rule.target);
      Pass *NewPass = FlexPassLoader::load(Rule.plugin);
      if (TargetID && NewPass)
        PassConfig->insertPass(TargetID, NewPass);
    }
  });
```

**Limitations of this approach:**
- `insertPass()` only supports insert-**after**, not insert-before. For insert-before, we identify the preceding pass in the pipeline and insert after that instead. The `--flex-list-passes` command shows the pipeline order, making it easy for users to find the preceding pass.
- Pass identification uses `AnalysisID` (a `void*` pointer to the pass's static `ID` member). Flexclang needs a registry mapping pass name strings to `AnalysisID` values. This registry is built by scanning LLVM's pass registration tables.

### 3.5 Dynamic Pass Loading (FlexPassLoader)

MIR pass plugins export two C functions:

```cpp
// Required: factory function that creates the pass instance
extern "C" MachineFunctionPass* flexclangCreatePass();

// Optional: pass name for identification
extern "C" const char* flexclangPassName();
```

`FlexPassLoader` uses `sys::DynamicLibrary::getPermanentLibrary()` to load the `.so` and `getAddressOfSymbol()` to find the factory function. Pass instances are created on demand during the `RegisterTargetPassConfigCallback`.

For IR pass plugins, the standard `PassPlugin` API is used (`llvmGetPassPluginInfo()`), compatible with upstream clang's `-fpass-plugin=`.

## 4. Configuration

### 4.1 YAML Config Format

```yaml
# flexclang.yaml

# Machine IR pass modifications (flexclang's core value-add)
mir-passes:
  # Disable a pass (skip it)
  - action: disable
    target: si-form-memory-clauses

  # Replace a pass with a plugin
  - action: replace
    target: machine-scheduler
    plugin: ./my-scheduler.so

  # Insert a plugin after an existing pass
  - action: insert-after
    target: machine-scheduler
    plugin: ./my-post-sched.so

  # Insert at a named hook point (maps to GCNPassConfig virtual methods)
  - action: insert-at
    hook: pre-regalloc     # pre-isel, machine-ssa-opt, ilp-opts,
    plugin: ./my-pass.so   # pre-regalloc, post-regalloc, pre-sched2,
                           # pre-emit, pre-emit2

# LLVM IR pass modifications
ir-passes:
  # Disable a built-in IR pass (not possible in upstream clang)
  - action: disable
    target: instcombine

  # Load a standard PassPlugin (equivalent to -fpass-plugin=)
  # Plugin controls its own insertion point via PassBuilder callbacks.
  # Useful in YAML so the config is self-contained.
  - action: load-plugin
    plugin: ./my-ir-pass.so

# Custom latency model (future work, placeholder)
latency-model:
  file: ./gfx942-latency.yaml
```

### 4.2 CLI Flags

CLI flags mirror YAML config entries. CLI takes precedence when both specify the same pass.

**MIR pass control (flexclang-only, no upstream equivalent):**

| CLI Flag | YAML Equivalent |
|----------|----------------|
| `--flex-disable-pass=<name>` | `mir-passes: [{action: disable, target: <name>}]` |
| `--flex-replace-pass=<name>:<plugin.so>` | `mir-passes: [{action: replace, target: <name>, plugin: <so>}]` |
| `--flex-insert-after=<name>:<plugin.so>` | `mir-passes: [{action: insert-after, target: <name>, plugin: <so>}]` |
| `--flex-insert-at=<hook>:<plugin.so>` | `mir-passes: [{action: insert-at, hook: <hook>, plugin: <so>}]` |

**IR pass control:**

| CLI Flag | YAML Equivalent |
|----------|----------------|
| `--flex-disable-ir-pass=<name>` | `ir-passes: [{action: disable, target: <name>}]` |
| `-fpass-plugin=<plugin.so>` | `ir-passes: [{action: load-plugin, plugin: <so>}]` |

Note: `-fpass-plugin=` is an upstream clang flag that works as-is. The YAML `load-plugin` action is equivalent, provided for self-contained configs.

**Discovery and config:**

| CLI Flag | Purpose |
|----------|---------|
| `--flex-config=<path>` | Load YAML config file |
| `--flex-list-passes` | Dump all MIR and IR pass names in pipeline order |
| `--flex-latency-model=<path>` | Load custom latency model (future work) |

### 4.3 Environment Variables

| Variable | Purpose |
|----------|---------|
| `FLEXCLANG_CONFIG` | Default config file path (overridden by `--flex-config`) |

## 5. Pass Plugin API

### 5.1 MIR Pass Plugin (flexclang-specific)

```cpp
// my-mir-pass.cpp
#include "llvm/CodeGen/MachineFunctionPass.h"

class MyPass : public MachineFunctionPass {
public:
  static char ID;
  MyPass() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    // Custom logic operating on Machine IR
    return false; // return true if MF was modified
  }

  StringRef getPassName() const override { return "my-custom-pass"; }
};
char MyPass::ID = 0;

// Required: factory function
extern "C" llvm::MachineFunctionPass* flexclangCreatePass() {
  return new MyPass();
}

// Optional: pass name metadata
extern "C" const char* flexclangPassName() {
  return "my-custom-pass";
}
```

Build:
```bash
clang++ -shared -fPIC -o my-pass.so my-mir-pass.cpp \
  $(llvm-config --cxxflags --ldflags --libs codegen core support)
```

### 5.2 IR Pass Plugin (upstream-compatible)

```cpp
// my-ir-pass.cpp
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Passes/PassBuilder.h"

class MyIRPass : public llvm::PassInfoMixin<MyIRPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &AM) {
    // Custom IR transformation
    return llvm::PreservedAnalyses::all();
  }
};

extern "C" ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "MyIRPass", "1.0",
    [](llvm::PassBuilder &PB) {
      PB.registerOptimizerLastEPCallback(
        [](llvm::ModulePassManager &MPM, llvm::OptimizationLevel) {
          MPM.addPass(
            llvm::createModuleToFunctionPassAdaptor(MyIRPass()));
        });
    }};
}
```

This plugin works with both flexclang and upstream clang (`clang -fpass-plugin=./my-ir-pass.so`).

## 6. Pass Identification and Discovery

### 6.1 Listing Passes

```bash
# List all MIR and IR passes for a target
flexclang --flex-list-passes -x hip /dev/null --offload-arch=gfx942 -O2
```

Output format:
```
=== IR Optimization Pipeline ===
  [ir.1]  always-inliner
  [ir.2]  amdgpu-printf-runtime-binding
  ...

=== MIR Codegen Pipeline ===
  [mir.1]  amdgpu-isel
  [mir.2]  si-fix-sgpr-copies
  [mir.3]  si-fold-operands
  ...
  [mir.12] machine-scheduler
  ...
  [mir.20] greedy (sgpr)
  [mir.25] greedy (vgpr)
  ...

=== Hook Points (for insert-at) ===
  pre-isel, machine-ssa-opt, ilp-opts, pre-regalloc,
  post-regalloc, pre-sched2, pre-emit, pre-emit2
```

### 6.2 Pass Name Source

MIR pass names come from `Pass::getPassName()`. These are the same strings accepted by LLVM's `-start-before`/`-stop-after` flags. IR pass names are the textual pipeline names from `PassRegistry.def` and `AMDGPUPassRegistry.def`.

### 6.3 Critical Passes

The following passes are marked as critical. Disabling them produces a warning:

| Pass | Reason |
|------|--------|
| `si-lower-control-flow` | Required for correct CFG |
| `si-insert-waitcnts` | Required for memory ordering correctness |
| `prologepilog` | Required for stack frame setup |
| `phi-node-elimination` | Required before register allocation |
| `virtregrewriter` | Required to map virtual to physical registers |
| `si-fix-sgpr-copies` | Required for correct SGPR handling |

## 7. Build System

```cmake
cmake_minimum_required(VERSION 3.20)
project(flexclang VERSION 0.1.0)

# Find installed LLVM and Clang
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Found Clang")

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

# flexclang executable
add_executable(flexclang
  src/main.cpp
  src/FlexConfig.cpp
  src/FlexPassConfigCallback.cpp
  src/FlexPassLoader.cpp
)

target_link_libraries(flexclang PRIVATE
  # Clang libraries
  clangFrontend
  clangFrontendTool
  clangCodeGen
  clangDriver
  clangBasic
  clangSerialization
  clangOptions

  # LLVM libraries
  LLVMAMDGPUCodeGen
  LLVMAMDGPUAsmParser
  LLVMAMDGPUDesc
  LLVMAMDGPUInfo
  LLVMCodeGen
  LLVMCore
  LLVMSupport
  LLVMTarget
  LLVMPasses
  LLVMAnalysis
  LLVMTransformUtils
  LLVMScalarOpts
  LLVMInstCombine
  LLVMMC
  LLVMMCParser
  LLVMOption
  LLVMipo
)

# YAML parsing (use LLVM's built-in YAMLParser)
# No external dependency needed -- LLVM includes yaml::Stream

install(TARGETS flexclang DESTINATION bin)
```

**Build requirements:**
- An installed LLVM/Clang build with AMDGPU target (e.g., ROCm amdclang build, or custom LLVM build)
- CMake 3.20+
- C++17 compiler

## 8. Examples and Validation

### 8.1 Example IR Pass Plugin: Instruction Counter

A simple IR pass that prints the number of LLVM IR instructions per function. Demonstrates the upstream-compatible `llvmGetPassPluginInfo()` API.

```cpp
// examples/ir-pass-counter/IRInstCounter.cpp
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

class IRInstCounter : public PassInfoMixin<IRInstCounter> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    unsigned Count = 0;
    for (auto &BB : F)
      Count += BB.size();
    errs() << "[IRInstCounter] " << F.getName() << ": " << Count
           << " instructions\n";
    return PreservedAnalyses::all();
  }
};

extern "C" ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "IRInstCounter", "0.1",
    [](PassBuilder &PB) {
      PB.registerOptimizerLastEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel) {
          MPM.addPass(createModuleToFunctionPassAdaptor(IRInstCounter()));
        });
    }};
}
```

Build:
```bash
clang++ -shared -fPIC -o ir-inst-counter.so IRInstCounter.cpp \
  $(llvm-config --cxxflags --ldflags --libs core support passes)
```

Usage (works with both flexclang and upstream clang):
```bash
flexclang -fpass-plugin=./ir-inst-counter.so \
  -x hip test_kernel.hip -o test_kernel.o --offload-arch=gfx942
```

### 8.2 Example MIR Pass Plugin: NOP Inserter

A MIR pass that inserts a `s_nop 0` before every MFMA instruction. This serves as a "canary" -- its effect is visible in the assembly output, proving the pass ran and was inserted at the right point.

```cpp
// examples/mir-pass-nop-inserter/MIRNopInserter.cpp
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/MachineFunction.h"

using namespace llvm;

class MIRNopInserter : public MachineFunctionPass {
public:
  static char ID;
  MIRNopInserter() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
    bool Changed = false;

    for (auto &MBB : MF) {
      for (auto MI = MBB.begin(); MI != MBB.end(); ++MI) {
        // Check if this is an MFMA instruction (opcode name contains "MFMA")
        if (TII->getName(MI->getOpcode()).contains("MFMA")) {
          BuildMI(MBB, MI, MI->getDebugLoc(),
                  TII->get(AMDGPU::S_NOP)).addImm(0);
          Changed = true;
        }
      }
    }

    if (Changed)
      errs() << "[MIRNopInserter] Inserted NOPs in " << MF.getName() << "\n";
    return Changed;
  }

  StringRef getPassName() const override { return "mir-nop-inserter"; }
};
char MIRNopInserter::ID = 0;

extern "C" MachineFunctionPass* flexclangCreatePass() {
  return new MIRNopInserter();
}

extern "C" const char* flexclangPassName() {
  return "mir-nop-inserter";
}
```

Build:
```bash
clang++ -shared -fPIC -o mir-nop-inserter.so MIRNopInserter.cpp \
  $(llvm-config --cxxflags --ldflags --libs amdgpucodegen codegen core support)
```

Usage:
```bash
flexclang --flex-insert-after=machine-scheduler:./mir-nop-inserter.so \
  -x hip test_kernel.hip -S -o test_kernel.s --offload-arch=gfx942
```

### 8.3 Test HIP Kernel

A minimal GEMM kernel that exercises MFMA, global memory loads, LDS, and VALU -- all instruction types relevant to the scheduling and latency issues.

```cpp
// examples/test_kernel.hip
#include <hip/hip_runtime.h>

// Simple 16x16 MFMA-based GEMM tile
// Exercises: global_load, ds_read, v_mfma, v_mov, s_waitcnt
__global__ void gemm_16x16(const half* __restrict__ A,
                           const half* __restrict__ B,
                           float* __restrict__ C,
                           int M, int N, int K) {
  // Thread identification
  int tx = threadIdx.x;
  int bx = blockIdx.x;
  int by = blockIdx.y;

  // Shared memory for tiles
  __shared__ half tileA[16][16];
  __shared__ half tileB[16][16];

  float acc = 0.0f;

  for (int k = 0; k < K; k += 16) {
    // Load A tile to shared memory
    tileA[tx / 16][tx % 16] = A[(by * 16 + tx / 16) * K + k + tx % 16];
    // Load B tile to shared memory
    tileB[tx / 16][tx % 16] = B[(k + tx / 16) * N + bx * 16 + tx % 16];

    __syncthreads();

    // Accumulate
    for (int kk = 0; kk < 16; ++kk) {
      acc += __half2float(tileA[tx / 16][kk]) *
             __half2float(tileB[kk][tx % 16]);
    }

    __syncthreads();
  }

  C[(by * 16 + tx / 16) * N + bx * 16 + tx % 16] = acc;
}
```

### 8.4 YAML Config Examples

**Example 1: Disable a pass**
```yaml
# examples/configs/disable-memory-clauses.yaml
mir-passes:
  - action: disable
    target: si-form-memory-clauses
```

**Example 2: Insert custom MIR pass after scheduler**
```yaml
# examples/configs/insert-nop-after-sched.yaml
mir-passes:
  - action: insert-after
    target: machine-scheduler
    plugin: ./mir-nop-inserter.so
```

**Example 3: Combined IR + MIR modifications**
```yaml
# examples/configs/combined.yaml
ir-passes:
  - action: load-plugin
    plugin: ./ir-inst-counter.so

mir-passes:
  - action: disable
    target: si-form-memory-clauses
  - action: insert-after
    target: machine-scheduler
    plugin: ./mir-nop-inserter.so
```

### 8.5 Validation Script

A test script that validates the pass plugin system works end-to-end.

```bash
#!/bin/bash
# examples/validate.sh
# Validates flexclang pass plugin system

set -e
FLEXCLANG=${FLEXCLANG:-./build/flexclang}
ARCH=${ARCH:-gfx942}
KERNEL=examples/test_kernel.hip

echo "=== Test 1: Baseline compilation ==="
$FLEXCLANG -x hip $KERNEL -S -o /tmp/baseline.s --offload-arch=$ARCH -O2
echo "PASS: Baseline compiles"

echo "=== Test 2: --flex-list-passes ==="
$FLEXCLANG --flex-list-passes -x hip /dev/null --offload-arch=$ARCH -O2 \
  > /tmp/pass-list.txt 2>&1
grep -q "machine-scheduler" /tmp/pass-list.txt
echo "PASS: machine-scheduler found in pass list"

echo "=== Test 3: Disable pass ==="
$FLEXCLANG --flex-disable-pass=si-form-memory-clauses \
  -x hip $KERNEL -S -o /tmp/disabled.s --offload-arch=$ARCH -O2
# Assembly should differ from baseline (fewer clause formations)
if diff -q /tmp/baseline.s /tmp/disabled.s > /dev/null 2>&1; then
  echo "WARNING: disabling si-form-memory-clauses produced identical output"
else
  echo "PASS: Disabling pass changed assembly output"
fi

echo "=== Test 4: IR pass plugin ==="
$FLEXCLANG -fpass-plugin=./ir-inst-counter.so \
  -x hip $KERNEL -S -o /tmp/ir-plugin.s --offload-arch=$ARCH -O2 \
  2>/tmp/ir-plugin-stderr.txt
grep -q "\[IRInstCounter\]" /tmp/ir-plugin-stderr.txt
echo "PASS: IR pass plugin ran and produced output"

echo "=== Test 5: MIR pass plugin ==="
$FLEXCLANG --flex-insert-after=machine-scheduler:./mir-nop-inserter.so \
  -x hip $KERNEL -S -o /tmp/mir-plugin.s --offload-arch=$ARCH -O2 \
  2>/tmp/mir-plugin-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/mir-plugin-stderr.txt
# Verify s_nop appears more in plugin output than baseline
BASELINE_NOPS=$(grep -c "s_nop" /tmp/baseline.s || true)
PLUGIN_NOPS=$(grep -c "s_nop" /tmp/mir-plugin.s || true)
if [ "$PLUGIN_NOPS" -gt "$BASELINE_NOPS" ]; then
  echo "PASS: MIR pass plugin inserted NOPs (baseline=$BASELINE_NOPS, plugin=$PLUGIN_NOPS)"
else
  echo "FAIL: Expected more s_nop instructions"
  exit 1
fi

echo "=== Test 6: YAML config ==="
$FLEXCLANG --flex-config=examples/configs/combined.yaml \
  -x hip $KERNEL -S -o /tmp/yaml-config.s --offload-arch=$ARCH -O2 \
  2>/tmp/yaml-stderr.txt
grep -q "\[IRInstCounter\]" /tmp/yaml-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/yaml-stderr.txt
echo "PASS: YAML config loaded both IR and MIR plugins"

echo ""
echo "All tests passed!"
```

### 8.6 Expected Directory Structure

```
examples/
  test_kernel.hip                      # Test HIP kernel
  validate.sh                          # Validation script
  ir-pass-counter/
    IRInstCounter.cpp                  # IR pass plugin source
    CMakeLists.txt                     # Build config for the plugin
  mir-pass-nop-inserter/
    MIRNopInserter.cpp                 # MIR pass plugin source
    CMakeLists.txt                     # Build config for the plugin
  configs/
    disable-memory-clauses.yaml        # Example: disable a pass
    insert-nop-after-sched.yaml        # Example: insert MIR pass
    combined.yaml                      # Example: combined IR + MIR
```

## 9. Custom Scheduler with Corrected Latency Model (Future Work)

This section outlines the planned custom scheduler pass that will use measured latency data. Implementation details will be designed separately.

### 8.1 Concept

A custom MIR pass plugin (`flex-scheduler.so`) that:
1. Reads a per-target latency model file (YAML) with measured instruction latencies
2. Overrides LLVM's built-in `TargetSchedModel` latencies at scheduling time
3. Ships with default latency models for gfx942, gfx950 (to be measured and refined)
4. Allows users to supply their own measurements (e.g., from rocprofv3)

### 8.2 Latency Model Format (preliminary)

```yaml
# gfx942-latency.yaml
target: gfx942
version: 1

instruction-latencies:
  # Memory operations (measured via rocprofv3)
  vmem-load: 120          # global memory load (default: 80)
  vmem-store: 80          # global memory store
  lds-load: 8             # LDS/shared memory load (default: 5)
  lds-store: 4             # LDS/shared memory store
  smem-load: 20           # scalar memory load (default: 5)

  # ALU operations
  valu-32bit: 1            # 32-bit VALU (default: 1)
  valu-64bit: 1            # 64-bit VALU
  valu-trans32: 4          # transcendental 32-bit

  # MFMA operations
  mfma-f16-16x16x16: 8    # MFMA F16 16x16x16
  mfma-f16-32x32x8: 16    # MFMA F16 32x32x8
  # ... etc
```

### 8.3 Integration

```yaml
# flexclang.yaml
mir-passes:
  - action: replace
    target: machine-scheduler
    plugin: ./flex-scheduler.so

latency-model:
  file: ./gfx942-latency.yaml
```

## 10. Agent Team Setup

For iterative refinement of this design, a two-member agent team is defined:

### 10.1 Team Members

**AMDGPU HIP Kernel Developer (Reviewer)**
- Role: Review designs from the kernel developer's perspective
- Focus: Usability, pain points addressed, missing features, real-world workflow fit
- Questions to ask: "Can I do X with this?", "What happens when Y?", "This doesn't address Z"
- Knowledge: AMDGPU ISA, HIP programming, rocprofv3, performance tuning workflows

**Clang System Designer (Proposer)**
- Role: Propose refined designs based on reviewer feedback
- Focus: Architecture, LLVM internals, implementation feasibility, API design
- Questions to ask: "How does this interact with X in LLVM?", "Is this approach maintainable?"
- Knowledge: LLVM pass infrastructure, clang internals, compiler plugin APIs

### 10.2 Agent Definitions

Agent definitions are placed in `.claude/agents/`:

**`.claude/agents/hip-kernel-reviewer.md`:**
```markdown
---
name: hip-kernel-reviewer
description: AMDGPU HIP kernel developer who reviews compiler design proposals
model: opus
---

You are an experienced AMDGPU HIP kernel developer. You write high-performance
GPU kernels for gfx942 and gfx950 targets. You are deeply familiar with:

- AMDGPU ISA (CDNA3/CDNA4)
- HIP programming model
- rocprofv3 for performance measurement
- MFMA instructions, AGPR vs VGPR usage
- IGLP intrinsics for instruction interleaving
- Inline assembly for performance-critical sections
- The pain of working with the compiler's default scheduling

Your job is to REVIEW design proposals for flexclang from the perspective of
a kernel developer who will USE this tool daily. Ask questions like:
- "Can I use this to fix the scheduling problem in my GEMM kernel?"
- "What happens if I disable waitcnt insertion and add my own?"
- "How do I debug when my custom pass produces wrong output?"
- "Will this work with hipcc's build system?"

Be critical but constructive. Point out missing features, usability issues,
and real-world scenarios the design doesn't handle.
```

**`.claude/agents/clang-system-designer.md`:**
```markdown
---
name: clang-system-designer
description: LLVM/Clang system designer who proposes refined compiler designs
model: opus
---

You are a compiler engineer with deep knowledge of LLVM internals. You are
familiar with:

- LLVM's new and legacy pass managers
- TargetPassConfig and its virtual hooks
- PassBuilder extension points and plugin API
- AMDGPU backend architecture (GCNPassConfig, GCNSchedStrategy)
- Dynamic library loading and C ABI for plugin interfaces
- CMake build systems for LLVM-based tools

Your job is to PROPOSE refined designs for flexclang based on feedback from
the HIP kernel developer reviewer. When proposing changes:
- Reference specific LLVM source files and classes
- Explain implementation feasibility
- Identify risks and alternatives
- Keep designs minimal -- solve the stated problem, don't over-engineer

The LLVM source code is at /home/poyechen/workspace/repo/llvm-project.
Read actual source files to verify your proposals are implementable.
```

### 10.3 Review Loop

The agent team operates in a review loop:
1. Clang System Designer reads the current spec and proposes refinements
2. HIP Kernel Developer reviews and provides feedback
3. Repeat until both agree the design addresses key use cases
4. Final spec is written and committed

## 11. Scope and Non-Goals

### In Scope (Phase 1)
- flexclang binary that links against installed LLVM/Clang
- Drop-in clang replacement (all standard flags work, including `-fpass-plugin=`)
- YAML config + CLI flags for MIR pass modifications (disable, replace, insert-after, insert-at)
- MIR pass plugin loading via `.so` + `flexclangCreatePass()` API
- Per-pass disable for IR passes (`--flex-disable-ir-pass`)
- `--flex-list-passes` for pipeline discovery
- Critical pass warnings
- Example plugins (IR + MIR) and validation script

### In Scope (Phase 2, future)
- Custom scheduler pass with corrected latency model
- Default latency models for gfx942, gfx950
- Latency model YAML format and loading
- Integration with rocprofv3 measurement pipeline

### Non-Goals
- Forking or modifying LLVM source code
- Replacing the entire codegen pipeline (partial modifications only)
- Supporting non-AMDGPU targets (AMDGPU-specific features only; generic clang features work for all targets)
- GUI or IDE integration
- Automatic performance tuning (the agent loop is external to flexclang)
