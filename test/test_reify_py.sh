
#!/bin/bash
#
# A simple test script to run rysmith and rylink with the Python target on
# a bunch of random programs and make sure they all compile and run correctly.
#
# Usage:
#   bash /test/test_reify_py.sh

set -e

cd "$(dirname "$0")/.."

seed="$RANDOM$RANDOM"
NUM_FUNCS=100
NUM_PROGS=$((NUM_FUNCS * 20))
EXAMPLES=3

# =========================================================================
# Test rysmith
# =========================================================================

rm -rf rysmith_out
./rysmith -n $NUM_FUNCS --target python --emit-main --validate --seed "$seed" --n-examples $EXAMPLES

# Enter rysmith out to build every .py and run it
for f in rysmith_out/*.py; do
    python $f
done

# =========================================================================
# Test rylink (--no-split-by-source)
# =========================================================================

rm -rf rysmith_out
./rysmith -n $NUM_FUNCS --target python --emit-desc --validate --seed "$seed" --n-examples $EXAMPLES

rm -rf rylink_out
./rylink -n $NUM_PROGS --target python --emit-main --validate --seed "$seed" --no-split-by-source

# Enter rylink out to build every .py and run it
for f in rylink_out/*.py; do
    python $f
done
