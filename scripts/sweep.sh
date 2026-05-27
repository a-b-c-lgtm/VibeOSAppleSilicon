#!/usr/bin/env bash
# scripts/sweep.sh — full regression sweep.
#
# Runs every scripts/test_*.py with a 240s per-test timeout (with
# a longer override for tests that exercise the in-guest toolchain
# end-to-end), prints a one-line PASS/FAIL per test, then prints
# the final "N/M PASS" summary and the tail of every FAIL log.
#
# Naming convention reminder (see /memories/debug-scripts-policy):
#   test_*  — regression tests, run by this script
#   _snoop_* / _dbg_* — ad-hoc tools, NEVER run here (leading
#                       underscore intentionally excludes them
#                       from the test_*.py glob)
#
# Exit code: 0 if every test passed, 1 otherwise.

set -u
cd "$(dirname "$0")/.."

failed_tests=""
total=0
passed=0

for f in scripts/test_*.py; do
    name=$(basename "$f")
    # Per-test timeout override.  A few regressions exercise the
    # in-guest toolchain end-to-end (compile gmp/mpfr/mpc; rebuild
    # all of DoomGeneric from source) and legitimately run for
    # 15-30 minutes.  Everything else gets the default 240s budget.
    case "$name" in
        test_doom_full.py)  per_test_timeout=1800 ;;
        test_guest_gcc.py)  per_test_timeout=1800 ;;
        *)                  per_test_timeout=240  ;;
    esac
    echo -n "Running $name... "
    if timeout "$per_test_timeout" python3 "$f" > "/tmp/$name.log" 2>&1; then
        echo "PASS"
        passed=$((passed + 1))
    else
        echo "FAIL"
        failed_tests="$failed_tests $name"
    fi
    total=$((total + 1))
    # Defensive cleanup: when `timeout` kills a test, the python
    # process dies but any QEMU subprocess it spawned survives as
    # an orphan, holds build/{disk,data}.img open, and poisons
    # every subsequent test ("no serial socket" cascades).  Reap
    # any stray qemu-system-aarch64 between tests so each test
    # starts from a clean slate.
    pkill -9 -f qemu-system-aarch64 >/dev/null 2>&1 || true
done

echo ""
echo "$passed/$total PASS"

for f in $failed_tests; do
    echo "--- FAIL: $f ---"
    tail -n 30 "/tmp/$f.log"
done

# Non-zero exit if any test failed, so CI / shell-chained
# invocations can short-circuit.
[ -z "$failed_tests" ]
