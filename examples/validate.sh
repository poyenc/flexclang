#!/bin/bash
# examples/validate.sh
# Validates flexclang pass plugin system
#
# flexclang is a cc1-level tool, so we use cc1-style flags:
#   -x hip -triple amdgcn-amd-amdhsa -target-cpu <arch> -O2

set -e
FLEXCLANG=${FLEXCLANG:-./build/flexclang}
ARCH=${ARCH:-gfx942}
KERNEL=examples/test_kernel.hip

# cc1-style flags (no --offload-arch, no driver flags)
CC1_FLAGS="-x hip -triple amdgcn-amd-amdhsa -target-cpu $ARCH -O2"

# Plugin paths (relative to repo root where this script is run from)
IR_PLUGIN=examples/ir-pass-counter/build/ir-inst-counter.so
MIR_PLUGIN=examples/mir-pass-nop-inserter/build/mir-nop-inserter.so

echo "=== Test 1: Baseline compilation ==="
$FLEXCLANG $CC1_FLAGS -S -o /tmp/baseline.s $KERNEL
echo "PASS: Baseline compiles"

echo "=== Test 2: --flex-list-passes ==="
$FLEXCLANG --flex-list-passes $CC1_FLAGS -S -o /dev/null /dev/null \
  > /tmp/pass-list.txt 2>&1
grep -q "machine-scheduler" /tmp/pass-list.txt
echo "PASS: machine-scheduler found in pass list"

echo "=== Test 3: Disable pass ==="
$FLEXCLANG --flex-disable-pass=si-form-memory-clauses \
  $CC1_FLAGS -S -o /tmp/disabled.s $KERNEL
# Assembly should differ from baseline (fewer clause formations)
if diff -q /tmp/baseline.s /tmp/disabled.s > /dev/null 2>&1; then
  echo "WARNING: disabling si-form-memory-clauses produced identical output"
else
  echo "PASS: Disabling pass changed assembly output"
fi

echo "=== Test 4: IR pass plugin ==="
$FLEXCLANG -fpass-plugin=$IR_PLUGIN \
  $CC1_FLAGS -S -o /tmp/ir-plugin.s $KERNEL \
  2>/tmp/ir-plugin-stderr.txt
grep -q "\[IRInstCounter\]" /tmp/ir-plugin-stderr.txt
echo "PASS: IR pass plugin ran and produced output"

echo "=== Test 5: MIR pass plugin ==="
$FLEXCLANG --flex-insert-after=machine-scheduler:$MIR_PLUGIN \
  $CC1_FLAGS -S -o /tmp/mir-plugin.s $KERNEL \
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
  $CC1_FLAGS -S -o /tmp/yaml-config.s $KERNEL \
  2>/tmp/yaml-stderr.txt
grep -q "\[IRInstCounter\]" /tmp/yaml-stderr.txt
grep -q "\[MIRNopInserter\]" /tmp/yaml-stderr.txt
echo "PASS: YAML config loaded both IR and MIR plugins"

echo ""
echo "All tests passed!"
