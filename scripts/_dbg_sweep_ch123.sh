#!/bin/bash
# scripts/_dbg_sweep_ch123.sh — 21-test regression sweep for chapter 159.
# Same shape as the ch-121 sweep, but with the chapter-123 /bin/cc test.
LOG=/tmp/osdev_sweep_ch123/sweep.log
mkdir -p "$(dirname "$LOG")"
cd "$(dirname "$0")/.."
TESTS=( test_cc_hello test_cc_vars test_atexit test_bin_as test_bin_ld_ar
  test_libc_stat test_libc_errno test_libc_stdio test_libc_env
  test_gcc_cross_hello test_boot_to_desktop test_userfs_echo
  test_clipboard test_mount_ro test_userfs_timeout test_httpd_forward
  test_browser_proxy test_cow test_fork_exec test_busy_on_mix
  test_clone_files test_directories )
pass=0; fail=0; failed=""
: > "$LOG"
for t in "${TESTS[@]}"; do
  pkill -f qemu-system-aarch64 2>/dev/null; sleep 1
  echo "===== $t =====" >> "$LOG"
  out=$(timeout 240 python3 scripts/$t.py 2>&1 | tail -80)
  echo "$out" >> "$LOG"
  # The real PASS detection uses the post-processing AWK pass on
  # the log because individual tests use a zoo of summary formats.
  # Here we tally with a permissive grep.
  if echo "$out" | grep -qE "0 FAIL|^OK$|^ALL PASS|ALL TESTS PASSED|all checks passed|TEST PASSED|^PASS: chapter"; then
    pass=$((pass+1)); echo "  -> PASS" >> "$LOG"
  else
    fail=$((fail+1)); failed="$failed $t"; echo "  -> FAIL" >> "$LOG"
  fi
done
echo "SWEEP_SUMMARY: PASS=$pass FAIL=$fail FAILED=$failed" >> "$LOG"
pkill -f qemu-system-aarch64 2>/dev/null
