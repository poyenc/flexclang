# flexclang

A drop-in replacement for clang that lets AMDGPU HIP kernel developers disable, replace, and insert LLVM MIR and IR optimization passes via CLI flags and YAML configuration. Links against installed LLVM/Clang libraries without forking or modifying LLVM source code.

## Motivation

AMDGPU HIP kernel developers face several pain points with upstream LLVM/Clang:

- **Scheduler defaults to maximize occupancy** instead of ILP, often at the cost of instruction-level parallelism.
- **Inaccurate instruction latency model.** CDNA/GFX9 hardcodes static latencies that don't reflect real measured values.
- **IGLP ordering undone by rescheduling.** High register pressure triggers reschedules that undo user-specified instruction interleaving.
- **No per-pass enable/disable for Machine IR passes.** The only controls are `-start-before`/`-stop-after`.
- **Long upstream lead time.** Patches require review by the compiler team; some are rejected on policy grounds.

flexclang addresses these by giving developers direct control over the compiler pipeline without waiting for upstream changes.

## How It Works

flexclang mirrors clang's compilation flow but intercepts the pass pipeline at two levels:

- **MIR passes:** Uses LLVM's `RegisterTargetPassConfigCallback` to receive the `TargetPassConfig` after `createPassConfig()` but before passes execute. Calls `disablePass()`, `substitutePass()`, and `insertPass()` based on configuration.
- **IR passes:** Uses `shouldRunOptionalPassCallback` via `PassInstrumentationCallbacks` to skip IR optimization passes by name.

Both `GCNTargetMachine` and `GCNPassConfig` are `final` in LLVM and cannot be subclassed. The callback-based approach works around this constraint.

```
flexclang [args]
    |
    +-- argv[1] == "-cc1" --> cc1 mode (direct compilation)
    |
    +-- otherwise ---------> driver mode (orchestrates host + device)
```

Without any `--flex-*` flags, flexclang produces bit-identical output to upstream clang.

## Requirements

- LLVM 21+ (amd-staging branch supported)
- CMake 3.20+
- C++17 compiler

## Building

```bash
# Build flexclang
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/llvm/build
cmake --build build -j$(nproc)

# Build example plugins
LLVM_BUILD=/path/to/llvm/build

cmake -B examples/ir-pass-counter/build \
  -DLLVM_DIR=$LLVM_BUILD/lib/cmake/llvm \
  examples/ir-pass-counter
cmake --build examples/ir-pass-counter/build

cmake -B examples/mir-pass-nop-inserter/build \
  -DLLVM_DIR=$LLVM_BUILD/lib/cmake/llvm \
  -DLLVM_BUILD_DIR=$LLVM_BUILD \
  -DLLVM_MAIN_SRC_DIR=/path/to/llvm/source/llvm \
  examples/mir-pass-nop-inserter
cmake --build examples/mir-pass-nop-inserter/build
```

## Installation

flexclang must be installed in the same `bin/` directory as the clang it links against (the driver resolves resource directories and tools relative to its own path):

```bash
cp build/flexclang /opt/rocm/llvm/bin/
```

## Usage

### Driver Mode (primary)

```bash
# Drop-in replacement for clang
flexclang -x hip kernel.hip --offload-arch=gfx942 -O2 -o kernel.o

# With flex flags
flexclang --flex-disable-pass=machine-scheduler \
  -x hip kernel.hip --offload-arch=gfx942 -O2 -S -o kernel.s

# CMake integration
cmake -DCMAKE_CXX_COMPILER=/opt/rocm/llvm/bin/flexclang \
      -DCMAKE_HIP_ARCHITECTURES=gfx942 ..
```

Flex flags are injected only into AMDGCN device cc1 jobs. Host compilation is unaffected.

### cc1 Mode (advanced)

```bash
flexclang -cc1 -x hip -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  --flex-disable-pass=machine-scheduler -O2 -S -o kernel.s kernel.hip
```

### Kernel Iteration Workflow

```bash
# 1. Discover passes
flexclang --flex-list-passes -x hip /dev/null --offload-arch=gfx942 -O2

# 2. Compile baseline
flexclang -x hip kernel.hip --offload-arch=gfx942 -O2 -S -o baseline.s

# 3. Modify pipeline
flexclang --flex-disable-pass=machine-scheduler \
  -x hip kernel.hip --offload-arch=gfx942 -O2 -S -o modified.s

# 4. Compare
diff baseline.s modified.s
```

## CLI Flags

### MIR Pass Control

| Flag | Description |
|------|-------------|
| `--flex-disable-pass=<name>` | Disable a MIR pass |
| `--flex-replace-pass=<name>:<plugin.so>` | Replace a MIR pass with a plugin |
| `--flex-insert-after=<name>:<plugin.so>` | Insert a plugin after a MIR pass |

### IR Pass Control

| Flag | Description |
|------|-------------|
| `--flex-disable-ir-pass=<name>` | Disable an IR optimization pass |
| `-fpass-plugin=<plugin.so>` | Load an IR pass plugin (standard clang flag) |

### Discovery and Debugging

| Flag | Description |
|------|-------------|
| `--flex-config=<path>` | Load YAML config file |
| `--flex-list-passes` | List all IR and MIR passes in pipeline order |
| `--flex-verbose` | Print all pass modifications applied |
| `--flex-verify-plugins` | Insert MachineVerifier after each plugin pass |
| `--flex-dry-run` | Print what modifications would apply, then exit |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `FLEXCLANG_CONFIG` | Default config file path (overridden by `--flex-config`) |

## YAML Configuration

```yaml
# flexclang.yaml

mir-passes:
  # Disable a pass
  - action: disable
    target: machine-scheduler

  # Replace a pass with a plugin
  - action: replace
    target: machine-scheduler
    plugin: ./my-scheduler.so
    config: ./latency-model.yaml   # optional: passed to plugin

  # Insert a plugin after an existing pass
  - action: insert-after
    target: machine-scheduler
    plugin: ./my-post-sched.so

ir-passes:
  # Disable a built-in IR pass
  - action: disable
    target: instcombine

  # Load a PassPlugin (equivalent to -fpass-plugin=)
  - action: load-plugin
    plugin: ./my-ir-pass.so
```

CLI flags take precedence over YAML when both target the same pass.

## Pass Discovery

```bash
flexclang --flex-list-passes -x hip /dev/null --offload-arch=gfx942 -O2
```

Output:
```
=== IR Optimization Pipeline ===
  [ir.1]  verify
  [ir.2]  annotation2metadata
  ...
  [ir.21] instcombine
  ...

Use --flex-disable-ir-pass=<pass-name> to disable any IR pass listed above.

=== MIR Codegen Pipeline ===
  [mir.1]  amdgpu-isel
  [mir.2]  si-fix-sgpr-copies
  ...
  [mir.71] machine-scheduler
  ...

Use --flex-disable-pass=<name> or --flex-insert-after=<name>:<plugin.so>
```

## Writing Plugins

### MIR Pass Plugin

```cpp
#include "llvm/CodeGen/MachineFunctionPass.h"

class MyPass : public llvm::MachineFunctionPass {
public:
  static char ID;
  MyPass() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(llvm::MachineFunction &MF) override {
    // Custom MIR transformation
    return false;
  }

  llvm::StringRef getPassName() const override { return "my-pass"; }
};
char MyPass::ID = 0;

// Required
extern "C" llvm::MachineFunctionPass* flexclangCreatePass() {
  return new MyPass();
}

// Optional: pass name for --flex-verbose output
extern "C" const char* flexclangPassName() {
  return "my-pass";
}

// Optional: receive plugin-specific config file contents
extern "C" llvm::MachineFunctionPass*
flexclangCreatePassWithConfig(const char *configContents) {
  return new MyPass(configContents);
}
```

Build:
```bash
clang++ -shared -fPIC -fno-rtti -o my-pass.so my-pass.cpp \
  $(llvm-config --cxxflags --ldflags --libs codegen core support)
```

### IR Pass Plugin

Uses the standard upstream `llvmGetPassPluginInfo()` API, compatible with both flexclang and `clang -fpass-plugin=`:

```cpp
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Passes/PassBuilder.h"

class MyIRPass : public llvm::PassInfoMixin<MyIRPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &AM) {
    return llvm::PreservedAnalyses::all();
  }
};

extern "C" ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "MyIRPass", "1.0",
    [](llvm::PassBuilder &PB) {
      PB.registerOptimizerLastEPCallback(
        [](llvm::ModulePassManager &MPM, llvm::OptimizationLevel,
           llvm::ThinOrFullLTOPhase) {
          MPM.addPass(
            llvm::createModuleToFunctionPassAdaptor(MyIRPass()));
        });
    }};
}
```

### Plugin Requirements

- Build with `-fno-rtti` (matching LLVM)
- Use `$(llvm-config --cxxflags)` to inherit correct flags
- Plugin LLVM version must match the LLVM flexclang links against
- MIR plugins that use `AMDGPU::` opcodes need the AMDGPU target include path

## Diagnostics

flexclang provides specific error messages for common issues:

```
# Unknown pass name
flexclang: error: unknown MIR pass 'bogus' (use --flex-list-passes to see available passes)
flexclang: error: unknown IR pass 'bogus' (use --flex-list-passes to see available passes)

# Required IR pass (cannot be disabled)
flexclang: warning: IR pass 'verify' is required and cannot be disabled

# Critical MIR pass (dangerous to disable)
flexclang: warning: disabling 'si-insert-waitcnts' may cause incorrect code generation

# Non-substitutable MIR pass (added via insertPass(), not addPass())
flexclang: warning: MIR pass 'si-form-memory-clauses' was not disabled
(pass may be added via insertPass() and cannot be disabled via disablePass())
```

### Debugging a Custom Plugin

```bash
# 1. Verify the pass runs
flexclang --flex-verbose --flex-insert-after=machine-scheduler:./my-pass.so ...

# 2. Dump MIR before/after
flexclang -mllvm -print-before=my-pass -mllvm -print-after=my-pass ...

# 3. Run verifier after plugin
flexclang --flex-verify-plugins --flex-insert-after=machine-scheduler:./my-pass.so ...

# 4. MIR round-trip (isolate your pass)
flexclang -mllvm -stop-before=my-pass -x hip kernel.hip -o pre.mir ...
llc -run-pass=my-pass -march=amdgcn -mcpu=gfx942 pre.mir -o post.mir
diff pre.mir post.mir
```

## Validation

```bash
# Run the test suite (from repo root)
FLEXCLANG=./build/flexclang ./examples/validate.sh
```

The test suite covers 17 cc1 tests and 7 driver-mode tests including pass disable/replace/insert, plugin loading, YAML config, dry-run, CLI precedence, and diagnostic messages.

## Project Structure

```
src/
  main.cpp                    # Entry point: dual-mode dispatch (cc1 + driver)
  FlexConfig.{h,cpp}          # CLI + YAML parsing, config struct
  FlexPassNameRegistry.{h,cpp} # Pass name -> AnalysisID resolution
  FlexPassLoader.{h,cpp}      # Dynamic .so plugin loading
  FlexPassConfigCallback.{h,cpp} # MIR pipeline interception + pass listing
examples/
  test_kernel.hip             # Minimal MFMA kernel (cc1 mode)
  test_kernel_driver.hip      # MFMA kernel (driver mode)
  validate.sh                 # End-to-end test suite
  ir-pass-counter/            # Example IR pass plugin
  mir-pass-nop-inserter/      # Example MIR pass plugin
  configs/                    # Example YAML configs
docs/
  superpowers/specs/          # Design specifications
  superpowers/plans/          # Implementation plans
```

## Scope

### Phase 1 (implemented)
- Drop-in clang replacement with driver and cc1 modes
- MIR pass disable/replace/insert via `RegisterTargetPassConfigCallback`
- IR pass disable via `shouldRunOptionalPassCallback`
- YAML config + CLI flags, plugin loading, pass discovery
- Runtime detection of non-substitutable passes
- Critical pass warnings, plugin verification

### Phase 2 (planned)
- Custom scheduler with corrected latency model for gfx942/gfx950
- IGLP-preserving scheduler
- Per-target YAML latency models with measured values

## License

See LLVM license terms. flexclang links against LLVM/Clang libraries without modification.
