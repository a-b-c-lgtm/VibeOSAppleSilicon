#!/usr/bin/env bash
# scripts/sweep.sh — full regression sweep.
#
# Runs every scripts/test_*.py with a 240s per-test timeout, prints
# a one-line PASS/FAIL per test, then prints the final "N/M PASS"
# summary and the tail of every FAIL log.
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
    echo -n "Running $name... "
    if timeout 240 python3 "$f" > "/tmp/$name.log" 2>&1; then
        echo "PASS"
        passed=$((passed + 1))
    else
        echo "FAIL"
        failed_tests="$failed_tests $name"
    fi
    total=$((total + 1))
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
