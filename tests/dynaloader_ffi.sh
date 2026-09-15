#!/bin/bash
# Self-verifying test for the DynaLoader-compatible FFI (phase 2).
# Like d128_module_error_location.sh: the fixtures exercise behavior real
# perl cannot replicate byte-for-byte (perlc-compiled bootstrap modules,
# perlc's XSUB convention), so this runs OUTSIDE the stdout-diff harness
# and checks its own assertions. Run: bash tests/dynaloader_ffi.sh
set -u
cd "$(dirname "$0")/.."
PERLC=./perlc
pass=0; fail=0
check() { # name expected actual
  if [ "$2" == "$3" ]; then pass=$((pass+1)); else
    echo "FAIL: $1 (expected '$2' got '$3')"; fail=$((fail+1));
  fi
}

# ── 1. dl_load_file + dl_find_symbol on a real C .so ──
$PERLC tests/dynaloader_ffi_smoke.pltxt -o /tmp/_dl_sm.$$ 2>/dev/null
check dl_smoke_libref_defined 1 "$(/tmp/_dl_sm.$$ | grep libref_defined | cut -d= -f2)"
check dl_smoke_symref_defined 1 "$(/tmp/_dl_sm.$$ | grep symref_defined | cut -d= -f2)"
rm -f /tmp/_dl_sm.$$

# ── 2. bootstrap a perlc-compiled module (compile-on-demand) ──
$PERLC tests/dynaloader_ffi_deep.pltxt -o /tmp/_dl_dp.$$ 2>/dev/null
out=$(/tmp/_dl_dp.$$ 2>&1)
check dl_deep_boot_ok 1 "$(echo "$out" | grep boot_ok | cut -d= -f2)"
check dl_deep_double 42 "$(echo "$out" | grep '^double:' | cut -d' ' -f2)"
check dl_deep_greet "pxs-hello: world" "$(echo "$out" | grep '^greet:' | cut -d' ' -f2-)"
check dl_deep_sym_ok 1 "$(echo "$out" | grep sym_ok | cut -d= -f2)"
check dl_deep_missing_undef 1 "$(echo "$out" | grep missing_undef | cut -d= -f2)"
check dl_deep_err_set 1 "$(echo "$out" | grep err_set | cut -d= -f2)"
check dl_deep_add 42 "$(echo "$out" | grep '^add:' | cut -d' ' -f2)"
rm -f /tmp/_dl_dp.$$

echo "=== dynaloader_ffi: PASS=$pass FAIL=$fail ==="
[ $fail -eq 0 ] && echo "=== dynaloader_ffi: ALL PASS ===" || echo "=== dynaloader_ffi: FAILURES ==="
exit $fail
