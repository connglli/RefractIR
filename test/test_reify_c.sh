
#!/bin/bash
#
# A simple test script to run rysmith and rylink with the C target on
# a bunch of random programs and make sure they all compile and run correctly.
#
# Usage:
#   bash /test/test_reify_c.sh

set -e
export LSAN_OPTIONS="${LSAN_OPTIONS:-detect_leaks=0}"

cd "$(dirname "$0")/.."

seed="$RANDOM$RANDOM"
NUM_FUNCS=100
NUM_PROGS=$((NUM_FUNCS * 20))
EXAMPLES=3
STRUCTURED_LOWERING=random # true|false|random
# CFLAGS="-Wunused-variable -Werror=unused-variable -Wincompatible-pointer-types -Werror=incompatible-pointer-types -fsanitize=address,undefined"
CFLAGS="-Wincompatible-pointer-types -Werror=incompatible-pointer-types -fsanitize=address,undefined"
LDFLAGS="-lm"

# =========================================================================
# Test rysmith
# =========================================================================

rm -rf rysmith_out
./rysmith -n $NUM_FUNCS --target c --emit-main --validate --seed "$seed" --structured-lowering "$STRUCTURED_LOWERING" --n-examples $EXAMPLES

# Enter rysmith out to build every .c and run it
for f in rysmith_out/*.c; do
    gcc $CFLAGS "$f" $LDFLAGS
    ./a.out
    ./rytwin -o twin.sir --target c --emit-main --validate "${f/.c/.sir}" || true
    [[ -f twin.sir ]] && ./symiri -w twin.sir && rm -rf twin.sir
    [[ -f twin.c ]] && gcc $CFLAGS twin.c $LDFLAGS && ./a.out && rm -rf twin.c
done

# =========================================================================
# Test rylink (split-by-source)
# =========================================================================

rm -rf rysmith_out
./rysmith -n $NUM_FUNCS --target c --emit-desc --validate --seed "$seed" --structured-lowering "$STRUCTURED_LOWERING" --n-examples $EXAMPLES

rm -rf rylink_out
./rylink -n $NUM_PROGS --target c --emit-main --validate --seed "$seed" --structured-lowering "$STRUCTURED_LOWERING"

# Enter rylink to build every program and run it
for d in rylink_out/prog_*; do
    pushd $d
    gcc $CFLAGS *.c $LDFLAGS
    ./a.out
    ../../rytwin -o twin.sir --target c --emit-main --validate $(ls *.sir) || true
    [[ -f twin.sir ]] && ../../symiri -w twin.sir && rm -rf twin.sir
    [[ -f twin.c ]] && gcc $CFLAGS twin.c $LDFLAGS && ./a.out && rm -rf twin.c
    popd
done

# =========================================================================
# Test rylink (--no-split-by-source)
# =========================================================================

rm -rf rylink_out
./rylink -n $NUM_PROGS --target c --emit-main --validate --seed "$seed" --no-split-by-source --structured-lowering "$STRUCTURED_LOWERING"

# Enter rylink out to build every .c and run it
for f in rylink_out/*.c; do
    gcc $CFLAGS "$f" $LDFLAGS
    ./a.out
    ./rytwin -o twin.sir --target c --emit-main --validate "${f/.c/.sir}" || true
    [[ -f twin.sir ]] && ./symiri -w twin.sir && rm -rf twin.sir
    [[ -f twin.c ]] && gcc $CFLAGS twin.c $LDFLAGS && ./a.out && rm -rf twin.c
done
