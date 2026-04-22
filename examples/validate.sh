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
# Must use a real kernel (not /dev/null) so MIR passes run and get listed
$FLEXCLANG -cc1 --flex-list-passes $CC1_FLAGS -S -o /dev/null $KERNEL \
  > /tmp/pass-list.txt 2>&1
# Verify unified [ir.N] / [mir.N] format
grep -q "\[ir\.[0-9]*\].*instcombine" /tmp/pass-list.txt
grep -q "\[mir\.[0-9]*\].*machine-scheduler" /tmp/pass-list.txt
# Verify IR section appears before MIR section (IR runs first)
IR_LINE=$(grep -n "=== IR Optimization Pipeline ===" /tmp/pass-list.txt | head -1 | cut -d: -f1)
MIR_LINE=$(grep -n "=== MIR Codegen Pipeline ===" /tmp/pass-list.txt | head -1 | cut -d: -f1)
if [ "$IR_LINE" -ge "$MIR_LINE" ]; then
  echo "FAIL: IR section should appear before MIR section"
  exit 1
fi
echo "PASS: --flex-list-passes shows [ir.N]/[mir.N] format, IR before MIR"

echo "=== cc1 Test 3a: Disable non-substitutable pass ==="
# si-form-memory-clauses is added via insertPass() -- disablePass() has no effect.
# flexclang detects this at runtime and warns after compilation.
$FLEXCLANG -cc1 --flex-disable-pass=si-form-memory-clauses \
  $CC1_FLAGS -S -o /tmp/disabled-nosub.s $KERNEL \
  2>/tmp/disabled-nosub-stderr.txt
grep -q "was not disabled" /tmp/disabled-nosub-stderr.txt
diff -q /tmp/baseline.s /tmp/disabled-nosub.s > /dev/null 2>&1
echo "PASS: Non-substitutable pass detected at runtime, assembly unchanged"

echo "=== cc1 Test 3b: Disable substitutable pass ==="
# machine-scheduler is added via addPass(AnalysisID) -- can be disabled.
# No warning, assembly should differ from baseline.
$FLEXCLANG -cc1 --flex-verbose --flex-disable-pass=machine-scheduler \
  $CC1_FLAGS -S -o /tmp/disabled-sub.s $KERNEL \
  2>/tmp/disabled-sub-stderr.txt
grep -q "flexclang: requesting disable of MIR pass 'machine-scheduler'" /tmp/disabled-sub-stderr.txt
! grep -q "was not disabled" /tmp/disabled-sub-stderr.txt
if diff -q /tmp/baseline.s /tmp/disabled-sub.s > /dev/null 2>&1; then
  echo "FAIL: Disabling machine-scheduler should change assembly"
  exit 1
fi
echo "PASS: Substitutable pass disabled, assembly changed"

echo "=== cc1 Test 3c: Replace non-substitutable pass ==="
# si-form-memory-clauses is added via insertPass() -- substitutePass() has no effect.
# flexclang detects this at runtime and warns after compilation.
$FLEXCLANG -cc1 --flex-replace-pass=si-form-memory-clauses:$MIR_PLUGIN \
  $CC1_FLAGS -S -o /tmp/replaced-nosub.s $KERNEL \
  2>/tmp/replaced-nosub-stderr.txt
grep -q "was not replaced" /tmp/replaced-nosub-stderr.txt
echo "PASS: Non-substitutable replace detected at runtime"

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

echo "=== cc1 Test 7: --flex-dry-run ==="
rm -f /tmp/dryrun-out.s
$FLEXCLANG -cc1 --flex-dry-run \
  --flex-disable-pass=machine-scheduler \
  --flex-insert-after=machine-scheduler:$MIR_PLUGIN \
  $CC1_FLAGS -S -o /tmp/dryrun-out.s $KERNEL \
  2>/tmp/dryrun-stderr.txt || true
grep -q "\[dry-run\] MIR disable" /tmp/dryrun-stderr.txt
grep -q "\[dry-run\] MIR insert-after" /tmp/dryrun-stderr.txt
# Dry-run exits before compilation -- output file must not be created
if [ -f /tmp/dryrun-out.s ]; then
  echo "FAIL: --flex-dry-run should not produce output file"
  exit 1
fi
echo "PASS: --flex-dry-run printed modifications without compiling"

echo "=== cc1 Test 8: --flex-replace-pass ==="
# Replace machine-scheduler with the NOP inserter plugin.
# machine-scheduler is added via addPass(AnalysisID), so substitutePass works.
# The NOP inserter finds MFMA opcodes at this pipeline position and inserts s_nop.
$FLEXCLANG -cc1 --flex-verbose \
  --flex-replace-pass=machine-scheduler:$MIR_PLUGIN \
  $CC1_FLAGS -S -o /tmp/replaced.s $KERNEL \
  2>/tmp/replace-stderr.txt
grep -q "flexclang: requesting replace of 'machine-scheduler'" /tmp/replace-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/replace-stderr.txt
REPLACE_NOPS=$(grep -c "s_nop" /tmp/replaced.s || true)
if [ "$REPLACE_NOPS" -gt "$BASELINE_NOPS" ]; then
  echo "PASS: --flex-replace-pass substituted pass (baseline_nops=$BASELINE_NOPS, replaced_nops=$REPLACE_NOPS)"
else
  echo "FAIL: Expected more s_nop instructions after replace"
  exit 1
fi

echo "=== cc1 Test 9: CLI overrides YAML for same target ==="
# YAML inserts NOP plugin after machine-scheduler.
# CLI disables machine-scheduler (same target).
# CLI takes precedence: YAML rule for machine-scheduler is skipped.
# Verify: scheduler is disabled, NOP plugin does NOT run.
cat > /tmp/flex-test-override.yaml <<'YAMLEOF'
mir-passes:
  - action: insert-after
    target: machine-scheduler
    plugin: examples/mir-pass-nop-inserter/build/mir-nop-inserter.so
YAMLEOF
$FLEXCLANG -cc1 --flex-verbose \
  --flex-config=/tmp/flex-test-override.yaml \
  --flex-disable-pass=machine-scheduler \
  $CC1_FLAGS -S -o /tmp/cli-override.s $KERNEL \
  2>/tmp/cli-override-stderr.txt
# CLI disable should take effect
grep -q "flexclang: requesting disable of MIR pass 'machine-scheduler'" /tmp/cli-override-stderr.txt
# YAML insert-after for same target should be skipped (CLI wins)
if grep -q "\[MIRNopInserter\]" /tmp/cli-override-stderr.txt; then
  echo "FAIL: YAML rule should have been overridden by CLI for same target"
  exit 1
fi
echo "PASS: CLI overrides YAML for same target"

echo "=== cc1 Test 10: --flex-disable-ir-pass ==="
$FLEXCLANG -cc1 --flex-verbose --flex-disable-ir-pass=instcombine \
  $CC1_FLAGS -S -o /tmp/ir-disabled.s $KERNEL \
  2>/tmp/ir-disabled-stderr.txt
grep -q "flexclang: skipping IR pass" /tmp/ir-disabled-stderr.txt
if diff -q /tmp/baseline.s /tmp/ir-disabled.s > /dev/null 2>&1; then
  echo "WARNING: disabling instcombine produced identical output (may be expected at -O2)"
else
  echo "PASS: --flex-disable-ir-pass skipped IR pass, assembly changed"
fi

echo "=== cc1 Test 11a: Unknown IR pass name ==="
$FLEXCLANG -cc1 --flex-disable-ir-pass=nonexistent-pass-name \
  $CC1_FLAGS -S -o /tmp/ir-nomatch.s $KERNEL \
  2>/tmp/ir-nomatch-stderr.txt
grep -q "flexclang: error: unknown IR pass 'nonexistent-pass-name'" /tmp/ir-nomatch-stderr.txt
echo "PASS: Unknown IR pass name produces error"

echo "=== cc1 Test 11b: Required IR pass ==="
$FLEXCLANG -cc1 --flex-disable-ir-pass=verify \
  $CC1_FLAGS -S -o /tmp/ir-required.s $KERNEL \
  2>/tmp/ir-required-stderr.txt
grep -q "is required and cannot be disabled" /tmp/ir-required-stderr.txt
echo "PASS: Required IR pass produces specific warning"

echo "=== cc1 Test 12: FLEXCLANG_CONFIG env var ==="
FLEXCLANG_CONFIG=examples/configs/combined.yaml \
  $FLEXCLANG -cc1 \
  $CC1_FLAGS -S -o /tmp/envvar-config.s $KERNEL \
  2>/tmp/envvar-stderr.txt
grep -q "\[IRInstCounter\]" /tmp/envvar-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/envvar-stderr.txt
echo "PASS: FLEXCLANG_CONFIG env var loads config"

echo "=== cc1 Test 13: Critical pass warning ==="
# Disabling a critical pass like si-insert-waitcnts should warn
$FLEXCLANG -cc1 --flex-disable-pass=si-insert-waitcnts \
  $CC1_FLAGS -S -o /tmp/critical-pass.s $KERNEL \
  2>/tmp/critical-pass-stderr.txt || true
grep -q "may cause incorrect code generation" /tmp/critical-pass-stderr.txt
echo "PASS: Critical pass warning emitted"

echo "=== cc1 Test 14: --flex-config overrides FLEXCLANG_CONFIG ==="
# FLEXCLANG_CONFIG points to combined.yaml (loads IR+MIR plugins).
# --flex-config points to disable-scheduler.yaml (just disables scheduler).
# --flex-config should win: no IR plugin output, scheduler disabled.
FLEXCLANG_CONFIG=examples/configs/combined.yaml \
  $FLEXCLANG -cc1 --flex-verbose \
  --flex-config=examples/configs/disable-scheduler.yaml \
  $CC1_FLAGS -S -o /tmp/config-override.s $KERNEL \
  2>/tmp/config-override-stderr.txt
# --flex-config's rule should apply
grep -q "flexclang: requesting disable of MIR pass 'machine-scheduler'" /tmp/config-override-stderr.txt
# FLEXCLANG_CONFIG's IR plugin should NOT have loaded
if grep -q "\[IRInstCounter\]" /tmp/config-override-stderr.txt; then
  echo "FAIL: FLEXCLANG_CONFIG should be overridden by --flex-config"
  exit 1
fi
echo "PASS: --flex-config overrides FLEXCLANG_CONFIG"

echo "=== cc1 Test 15: Unknown MIR pass name ==="
$FLEXCLANG -cc1 --flex-disable-pass=nonexistent-mir-pass \
  $CC1_FLAGS -S -o /tmp/unknown-mir.s $KERNEL \
  2>/tmp/unknown-mir-stderr.txt
grep -q "flexclang: error: unknown MIR pass 'nonexistent-mir-pass'.*--flex-list-passes" /tmp/unknown-mir-stderr.txt
echo "PASS: Unknown MIR pass name produces error"

echo "=== cc1 Test 16: --flex-verify-plugins ==="
$FLEXCLANG -cc1 --flex-verify-plugins \
  --flex-insert-after=machine-scheduler:$MIR_PLUGIN \
  $CC1_FLAGS -S -o /tmp/verify-plugin.s $KERNEL \
  2>/tmp/verify-plugin-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/verify-plugin-stderr.txt
# Compilation should succeed (verifier should not flag the NOP inserter)
echo "PASS: --flex-verify-plugins runs without verification failure"

echo ""
echo "========================================="
echo "  Driver Mode Tests"
echo "========================================="
echo "(Require flexclang co-installed with clang)"

# Driver mode needs flexclang in the same bin/ as clang
FLEXCLANG_REAL=$(readlink -f $FLEXCLANG)
FLEXCLANG_DIR=$(dirname "$FLEXCLANG_REAL")
DRIVER_KERNEL=examples/test_kernel_driver.hip
if [ ! -f "$FLEXCLANG_DIR/clang" ] && [ ! -L "$FLEXCLANG_DIR/clang" ]; then
  echo "SKIP: Driver mode tests require flexclang in same dir as clang"
  echo ""
  echo "cc1 tests passed!"
  exit 0
fi

echo "=== Driver Test 1: Baseline C compilation ==="
echo 'int main() { return 0; }' > /tmp/test.c
$FLEXCLANG /tmp/test.c -c -o /tmp/driver-test.o 2>/dev/null
echo "PASS: Driver mode compiles C"

# Driver-mode HIP flags: --cuda-device-only -nogpulib avoids dependency on
# ROCm system headers and device libraries, which may not match this LLVM build.
DRIVER_HIP_FLAGS="-x hip --offload-arch=$ARCH -O2 --cuda-device-only -nogpulib"

echo "=== Driver Test 2: HIP kernel compilation ==="
$FLEXCLANG $DRIVER_HIP_FLAGS -S -o /tmp/driver-baseline.s $DRIVER_KERNEL
grep -q "mfma" /tmp/driver-baseline.s
echo "PASS: Driver mode compiles HIP kernel with MFMA"

echo "=== Driver Test 3: Flex flags change assembly ==="
$FLEXCLANG --flex-verbose --flex-disable-pass=machine-scheduler \
  $DRIVER_HIP_FLAGS -S -o /tmp/driver-disabled.s $DRIVER_KERNEL \
  2>/tmp/driver-stderr.txt
grep -q "flexclang: requesting disable of MIR pass 'machine-scheduler'" /tmp/driver-stderr.txt
if diff -q /tmp/driver-baseline.s /tmp/driver-disabled.s > /dev/null 2>&1; then
  echo "WARNING: disabling machine-scheduler produced identical driver output"
else
  echo "PASS: Flex flags change assembly in driver mode"
fi

echo "=== Driver Test 4: MIR plugin in driver mode ==="
$FLEXCLANG --flex-insert-after=machine-scheduler:$MIR_PLUGIN \
  $DRIVER_HIP_FLAGS -S -o /tmp/driver-mir-plugin.s $DRIVER_KERNEL \
  2>/tmp/driver-mir-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/driver-mir-stderr.txt
DRIVER_BASELINE_NOPS=$(grep -c "s_nop" /tmp/driver-baseline.s || true)
DRIVER_PLUGIN_NOPS=$(grep -c "s_nop" /tmp/driver-mir-plugin.s || true)
if [ "$DRIVER_PLUGIN_NOPS" -gt "$DRIVER_BASELINE_NOPS" ]; then
  echo "PASS: MIR plugin works in driver mode (baseline=$DRIVER_BASELINE_NOPS, plugin=$DRIVER_PLUGIN_NOPS)"
else
  echo "FAIL: Expected more s_nop in driver mode MIR plugin output"
  exit 1
fi

echo "=== Driver Test 5: YAML config in driver mode ==="
$FLEXCLANG --flex-config=examples/configs/combined.yaml \
  $DRIVER_HIP_FLAGS -S -o /tmp/driver-yaml.s $DRIVER_KERNEL \
  2>/tmp/driver-yaml-stderr.txt
grep -q "\[IRInstCounter\]" /tmp/driver-yaml-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/driver-yaml-stderr.txt
echo "PASS: YAML config works in driver mode"

echo "=== Driver Test 6: --flex-dry-run in driver mode ==="
rm -f /tmp/driver-dryrun-out.s
$FLEXCLANG --flex-dry-run \
  --flex-disable-pass=machine-scheduler \
  $DRIVER_HIP_FLAGS -S -o /tmp/driver-dryrun-out.s $DRIVER_KERNEL \
  2>/tmp/driver-dryrun-stderr.txt || true
grep -q "\[dry-run\] MIR disable" /tmp/driver-dryrun-stderr.txt
if [ -f /tmp/driver-dryrun-out.s ]; then
  echo "FAIL: --flex-dry-run in driver mode should not produce output file"
  exit 1
fi
echo "PASS: --flex-dry-run works in driver mode"

echo "=== Driver Test 7: Host code unaffected by flex flags ==="
# Compile a plain C file with flex flags -- host compilation should succeed
# and flex flags should not interfere (AMDGCN-only guard)
echo 'int main() { return 42; }' > /tmp/driver-host-test.c
$FLEXCLANG --flex-verbose --flex-disable-pass=machine-scheduler \
  /tmp/driver-host-test.c -c -o /tmp/driver-host-flex.o \
  2>/tmp/driver-host-stderr.txt
# No flex messages should appear for host-only compilation (no AMDGCN jobs)
if grep -q "flexclang: requesting disable" /tmp/driver-host-stderr.txt; then
  echo "FAIL: Flex flags should not affect host-only compilation"
  exit 1
fi
echo "PASS: Host code unaffected by flex flags"

echo ""
echo "All tests passed!"
