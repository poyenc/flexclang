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
   - For each `ir-passes.load-plugin` entry: load the `.so` and call `registerPassBuilderCallbacks()`.
   - For each `ir-passes.add` entry: register at the named extension point.
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

# Machine IR pass modifications
mir-passes:
  # Disable a pass (skip it)
  - action: disable
    target: si-form-memory-clauses

  # Replace a pass with a plugin
  - action: replace
    target: machine-scheduler
    plugin: ./my-scheduler.so

  # Insert a plugin before an existing pass
  - action: insert-before
    target: machine-scheduler
    plugin: ./my-analysis.so

  # Insert a plugin after an existing pass
  - action: insert-after
    target: machine-scheduler
    plugin: ./my-post-sched.so

  # Insert at a named hook point
  - action: insert-at
    hook: pre-regalloc     # pre-isel, machine-ssa-opt, ilp-opts,
    plugin: ./my-pass.so   # pre-regalloc, post-regalloc, pre-sched2,
                           # pre-emit, pre-emit2

# LLVM IR pass modifications
ir-passes:
  # Disable a built-in IR pass
  - action: disable
    target: instcombine

  # Add plugin at an extension point
  - action: add
    extension-point: optimizer-last  # peephole, pipeline-start,
    plugin: ./my-ir-pass.so          # vectorizer-start, vectorizer-end,
                                     # scalar-optimizer-late, cgscc-late

  # Load a standard PassPlugin (.so with llvmGetPassPluginInfo)
  - action: load-plugin
    plugin: ./my-ir-pass.so

  # Replace entire opt pipeline with textual pipeline string
  - action: set-pipeline
    pipeline: "module(function(instcombine,sroa),dce)"

# Custom latency model (future work, placeholder)
latency-model:
  file: ./gfx942-latency.yaml
```

### 4.2 CLI Flags

All YAML config entries have CLI equivalents:

| CLI Flag | YAML Equivalent |
|----------|----------------|
| `--flex-config=<path>` | Load YAML config file |
| `--flex-disable-pass=<name>` | `mir-passes: [{action: disable, target: <name>}]` |
| `--flex-replace-pass=<name>:<plugin.so>` | `mir-passes: [{action: replace, target: <name>, plugin: <so>}]` |
| `--flex-insert-before=<name>:<plugin.so>` | `mir-passes: [{action: insert-before, ...}]` |
| `--flex-insert-after=<name>:<plugin.so>` | `mir-passes: [{action: insert-after, ...}]` |
| `--flex-insert-at=<hook>:<plugin.so>` | `mir-passes: [{action: insert-at, ...}]` |
| `--flex-disable-ir-pass=<name>` | `ir-passes: [{action: disable, target: <name>}]` |
| `--flex-ir-plugin=<plugin.so>` | `ir-passes: [{action: load-plugin, plugin: <so>}]` |
| `--flex-ir-plugin-at=<ep>:<plugin.so>` | `ir-passes: [{action: add, extension-point: <ep>, ...}]` |
| `--flex-list-passes` | Dump pipeline (MIR and IR pass names) |
| `--flex-latency-model=<path>` | `latency-model: {file: <path>}` |

CLI flags are merged with YAML config. CLI takes precedence for conflicts.

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

## 8. Custom Scheduler with Corrected Latency Model (Future Work)

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

## 9. Agent Team Setup

For iterative refinement of this design, a two-member agent team is defined:

### 9.1 Team Members

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

### 9.2 Agent Definitions

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

### 9.3 Review Loop

The agent team operates in a review loop:
1. Clang System Designer reads the current spec and proposes refinements
2. HIP Kernel Developer reviews and provides feedback
3. Repeat until both agree the design addresses key use cases
4. Final spec is written and committed

## 10. Scope and Non-Goals

### In Scope (Phase 1)
- flexclang binary that links against installed LLVM/Clang
- Drop-in clang replacement (all standard flags work)
- YAML config + CLI flags for pass modifications
- MIR pass plugin loading via `.so` + `flexclangCreatePass()` API
- IR pass plugin support (wrapping upstream `-fpass-plugin`)
- Per-pass disable/replace/insert-before/insert-after for MIR passes
- Per-pass disable for IR passes
- `--flex-list-passes` for pipeline discovery
- Critical pass warnings

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
