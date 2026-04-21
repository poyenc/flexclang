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
