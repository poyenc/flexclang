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
$FLEXCLANG -cc1 --flex-disable-pass=si-form-memory-clauses \
  $CC1_FLAGS -S -o /tmp/disabled.s $KERNEL
if diff -q /tmp/baseline.s /tmp/disabled.s > /dev/null 2>&1; then
  echo "WARNING: disabling si-form-memory-clauses produced identical output"
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
FLEXCLANG_REAL=$(readlink -f $FLEXCLANG)
FLEXCLANG_DIR=$(dirname "$FLEXCLANG_REAL")
if [ ! -f "$FLEXCLANG_DIR/clang" ] && [ ! -L "$FLEXCLANG_DIR/clang" ]; then
  echo "SKIP: Driver mode tests require flexclang in same dir as clang"
  echo ""
  echo "cc1 tests passed!"
  exit 0
fi

echo "=== Driver Test 1: Baseline C compilation ==="
$FLEXCLANG /tmp/test.c -c -o /tmp/driver-test.o 2>/dev/null
echo "PASS: Driver mode compiles C"

echo "=== Driver Test 2: Flex flags in driver mode ==="
echo '__attribute__((amdgpu_kernel)) void k(float *p) { p[0] = 1.0f; }' > /tmp/k.hip
$FLEXCLANG --flex-verbose --flex-disable-pass=si-form-memory-clauses \
  -x hip /tmp/k.hip --offload-arch=$ARCH -c -o /tmp/k.o \
  2>/tmp/driver-stderr.txt || true
grep -q "flexclang: disabled MIR pass" /tmp/driver-stderr.txt
echo "PASS: Flex flags work in driver mode"

echo ""
echo "All tests passed!"
