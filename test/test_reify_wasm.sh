
#!/bin/bash
#
# A simple test script to run rysmith and rylink with the WASM target on
# a bunch of random programs and make sure they all compile and run correctly.
#
# Usage:
#   bash /test/test_reify_wasm.sh

set -e

cd "$(dirname "$0")/.."

seed="$RANDOM$RANDOM"
NUM_FUNCS=100
NUM_PROGS=$((NUM_FUNCS * 20))
STRUCTURED_LOWERING=random # true|false|random
EXAMPLES=3

# =========================================================================
# Test rysmith
# =========================================================================

rm -rf rysmith_out
./rysmith -n $NUM_FUNCS --target wasm --emit-main --validate --seed "$seed" --structured-lowering "$STRUCTURED_LOWERING" --n-examples $EXAMPLES

# Enter rysmith out to build every .wat and run it
for f in rysmith_out/*.wat; do
    wasmtime $f
done

# =========================================================================
# Test rylink (--no-split-by-source)
# =========================================================================

rm -rf rysmith_out
./rysmith -n $NUM_FUNCS --target wasm --emit-desc --validate --seed "$seed" --structured-lowering "$STRUCTURED_LOWERING" --n-examples $EXAMPLES

rm -rf rylink_out
./rylink -n $NUM_PROGS --target wasm --emit-main --validate --seed "$seed" --no-split-by-source --structured-lowering "$STRUCTURED_LOWERING"

# Enter rylink out to build every .wat and run it
for f in rylink_out/*.wat; do
    wasmtime $f
done
