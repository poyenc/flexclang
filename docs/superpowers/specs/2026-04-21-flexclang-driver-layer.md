# flexclang Driver Layer Specification

**Date:** 2026-04-21
**Status:** Draft
**Depends on:** flexclang Phase 1 design spec (`2026-04-21-flexclang-design.md`)

## 1. Problem

flexclang Phase 1 operates at the cc1 level only. Users must invoke it with low-level flags like `-triple amdgcn-amd-amdhsa -target-cpu gfx942`. This prevents the primary use case: `CMAKE_CXX_COMPILER=flexclang` for HIP builds. HIP compilation requires the clang driver layer, which orchestrates:

1. Device compilation (cc1 for each `--offload-arch`)
2. Host compilation (cc1 for the host target)
3. Offload bundling (`clang-offload-bundler`)
4. Linking

Without the driver, none of these phases happen automatically.

## 2. Solution

Make flexclang a dual-mode binary that handles both driver mode and cc1 mode, matching clang's own architecture.

### 2.1 Mode Dispatch

```
flexclang [args]
    |
    +-- argv[1] == "-cc1" --> cc1 mode (Phase 1 implementation)
    |
    +-- otherwise ---------> driver mode (new)
```

**cc1 mode** is the existing Phase 1 behavior: strip `--flex-*` flags, register MIR/IR callbacks, run `CompilerInvocation` + `ExecuteCompilerInvocation`.

**Driver mode** uses clang's `Driver` class to build and execute the compilation, injecting `--flex-*` flags into AMDGCN cc1 jobs.

### 2.2 Driver Mode Flow

1. Parse `--flex-*` flags from argv, saving originals for later injection
2. Create `Driver` object with flexclang's own executable path
3. Set `Driver::CC1Main` to flexclang's cc1 function (in-process execution)
4. Call `Driver::BuildCompilation()` with remaining (non-flex) driver args
5. Iterate compiled jobs; inject `--flex-*` flags into AMDGCN cc1 commands
6. Call `Driver::ExecuteCompilation()`

The driver re-invokes flexclang's cc1 function in-process via `CC1Main` for each compilation phase. This is clang's default behavior (`-fintegrated-cc1`).

### 2.3 Flag Injection

Between `BuildCompilation()` and `ExecuteCompilation()`, scan each `Command` in the compilation's job list. For commands targeting AMDGCN (detected by `-triple amdgcn*` in the argument list), append the original `--flex-*` flag strings to the command's argument list via `Command::replaceArguments()`.

Host cc1 commands, linker commands, and bundler commands are left unmodified. This ensures flex modifications only affect device code generation, matching the existing AMDGCN-only guard in `FlexPassConfigCallback`.

The original `--flex-*` strings are saved in `FlexConfig::originalFlexArgs` during `parseFlexArgs()`. These are the raw argv strings (e.g., `"--flex-disable-pass=si-form-memory-clauses"`) that get injected verbatim into cc1 argument lists.

### 2.4 AMDGCN Job Detection

A cc1 job targeting AMDGCN is detected by scanning its argument list for `-triple` followed by a value starting with `amdgcn`. Non-cc1 commands (linker, bundler) do not have `-triple` and are naturally skipped.

## 3. Multi-Job Safety

The driver runs multiple cc1 jobs in sequence within the same process. The following global state must be managed:

| Concern | Strategy |
|---------|----------|
| `RegisterTargetPassConfigCallback` (static) | Re-registered per cc1 run. Old callback destroyed, new one created. AMDGCN guard prevents host impact. |
| LLVM command-line options | `cl::ResetAllOptionOccurrences()` called at start of each cc1 run. |
| Fatal error handler | Installed before `ExecuteCompilerInvocation`, removed after. Per-run lifecycle. |
| `InitLLVM` / target init | Called once in `main()`. Not repeated in cc1 runs. |

## 4. Installation

flexclang must be installed in the same `bin/` directory as the clang it links against. The driver computes critical paths from its own executable location:

- **Resource directory** (`lib/clang/<version>/`): built-in headers, HIP wrapper headers, GPU bitcode
- **Tool paths**: `ld.lld`, `clang-offload-bundler`, `llvm-mc`
- **ROCm device libraries**: found relative to install prefix

Co-installation makes all paths resolve automatically. Install via:
```bash
cp build/flexclang /opt/rocm/llvm/bin/
# or
cmake --install . --prefix /opt/rocm/llvm
```

## 5. Usage

### 5.1 Driver Mode (Primary)

```bash
# Direct invocation
flexclang -x hip kernel.hip --offload-arch=gfx942 -O2 -o kernel.o

# With flex flags
flexclang --flex-disable-pass=si-form-memory-clauses \
  -x hip kernel.hip --offload-arch=gfx942 -O2 -S -o kernel.s

# With YAML config
flexclang --flex-config=my-config.yaml \
  -x hip kernel.hip --offload-arch=gfx942 -O2 -o kernel.o
```

### 5.2 CMake Integration

```bash
cmake -DCMAKE_CXX_COMPILER=/opt/rocm/llvm/bin/flexclang \
      -DCMAKE_HIP_ARCHITECTURES=gfx942 ..
```

Or via `CMAKE_HIP_COMPILER`:
```bash
cmake -DCMAKE_HIP_COMPILER=/opt/rocm/llvm/bin/flexclang \
      -DCMAKE_HIP_ARCHITECTURES=gfx942 ..
```

### 5.3 cc1 Mode (Advanced)

For direct cc1 usage, the `-cc1` prefix is now required:
```bash
flexclang -cc1 -x hip -triple amdgcn-amd-amdhsa -target-cpu gfx942 \
  --flex-disable-pass=si-form-memory-clauses -O2 -S -o kernel.s kernel.hip
```

### 5.4 Dry-Run in Driver Mode

`--flex-dry-run` in driver mode prints the planned flex modifications and exits without compiling, same as cc1 mode.

## 6. API Details

### 6.1 Driver Class Usage

```cpp
#include "clang/Driver/Driver.h"
#include "clang/Driver/Compilation.h"

// Constructor
Driver(StringRef ClangExecutable, StringRef TargetTriple,
       DiagnosticsEngine &Diags, std::string Title,
       IntrusiveRefCntPtr<vfs::FileSystem> VFS = nullptr);

// In-process cc1 callback
using CC1ToolFunc = function_ref<int(SmallVectorImpl<const char *> &)>;
CC1ToolFunc CC1Main = nullptr;

// Build and execute
Compilation *BuildCompilation(ArrayRef<const char *> Args);
int ExecuteCompilation(Compilation &C,
    SmallVectorImpl<std::pair<int, const Command *>> &FailingCommands);
```

### 6.2 Command Modification

```cpp
// Read current arguments
const ArgStringList &Command::getArguments() const;

// Replace all arguments
void Command::replaceArguments(ArgStringList List);

// Allocate stable string in compilation arena
const char *Compilation::getArgs().MakeArgString(StringRef);
```

### 6.3 FlexConfig Extension

```cpp
struct FlexConfig {
  // ... existing fields ...
  std::vector<std::string> originalFlexArgs; // Raw --flex-* strings from argv
};
```

## 7. Files Changed

| File | Change |
|------|--------|
| `src/main.cpp` | Restructure: extract `flexclang_cc1_main()`, add `flexclang_driver_main()`, new dispatch `main()` |
| `src/FlexConfig.h` | Add `originalFlexArgs` field |
| `src/FlexConfig.cpp` | Populate `originalFlexArgs` during parsing |
| `CMakeLists.txt` | Add libraries if needed (clangDriver already linked) |
| `examples/validate.sh` | Add `-cc1` to existing tests, add driver-mode tests |
| `examples/test_kernel.hip` | Restore full HIP syntax for driver-mode testing |

## 8. Validation

1. **Driver-mode compilation**: `flexclang -x hip kernel.hip --offload-arch=gfx942 -O2 -S -o kernel.s` succeeds
2. **Driver + flex flags**: `flexclang --flex-disable-pass=si-form-memory-clauses -x hip kernel.hip --offload-arch=gfx942 -O2 -S -o kernel.s` produces modified assembly
3. **cc1 mode**: `flexclang -cc1 ...` works as before (with `-cc1` prefix)
4. **Bit-identity**: Driver-mode flexclang without flex flags produces identical output to clang
5. **MIR plugin in driver mode**: `flexclang --flex-insert-after=machine-scheduler:./plugin.so -x hip kernel.hip --offload-arch=gfx942` works
6. **YAML config in driver mode**: `flexclang --flex-config=config.yaml -x hip kernel.hip --offload-arch=gfx942` works
7. **Host code unaffected**: Flex flags do not modify host (x86) compilation
8. **All validate.sh tests pass**: Both cc1-mode and driver-mode tests

## 9. Scope

### In Scope
- Driver mode using clang's `Driver` class
- In-process cc1 execution via `CC1Main`
- `--flex-*` flag injection into AMDGCN cc1 jobs only
- Co-installation requirement (same bin/ as clang)
- Updated validate.sh with driver-mode tests

### Out of Scope
- Separate installation support (auto-detection of clang paths)
- `--flex-clang-path` or `--flex-resource-dir` flags
- Out-of-process cc1 execution (`-fno-integrated-cc1`)
- Multi-arch flex configs (different flex flags per `--offload-arch`)
