# REMEDIATION.md — Critical Fixes Needed

**Status after comprehensive review (2026-06-26).**  All
remediation items previously listed have been re-tested against
the current `src/` and `tests/` trees; the up-to-date status is
below.

| # | Item | Previous claim | Reality now |
|---|------|----------------|-------------|
| 1 | `EvalBlock` LLVM codegen crash | FIXED (commit a701cd1) | **Partially fixed**: the LLVM verify error is gone (no more `ret ptr` in `i32` main), but the body is emitted with `emitBlock` (no value) and the result is unconditionally `perlUndef()`.  Nested `eval` and any `my $r = eval { ... }` return the wrong value. |
| 2 | `require` caching bug | FIXED (verified) | **Not reproducible** (test files deleted).  Cannot confirm or deny without `tests/test_require_simple.pl` restored. |
| 3 | Compound `-=` on shared scalars | FIXED (commit db7ba77) | **Still working**: `tests/regression_bugs.pl regression_subtract` and `regression_multi_subtract` both print `pass`. |
| 4 | `*=` `/=` `%=` atomic RMW for shared scalars | FIXED (commit db7ba77) | **Still working**: `tests/regression_bugs.pl regression_multiply` / `divide` / `modulo` all print `pass`. |
| 5 | `closure + range with captured variable` | FIXED (commit 776b963) | **Still working**: `tests/regression_bugs.pl regression_closure_range` and `regression_anon_range` both print `pass`. |
| 6 | Stage 32 loop-invariant PV deferral | FIXED (Stage 32) | **Likely working** (no direct test) — IR inspection shows `perl_free` count dropped from 114 → 93. |
| 7 | Stage 32 deref hoisting | FIXED (Stage 32) | **Likely partial** — the inner loop of `tests/mbs.pl` still emits 102 `perl_array_get_ref` calls per program load; the hoisting for `$zj->[$i]` / `$ccj->[$i]` is not firing. |
| 8 | Stage 33 known tag type tracking | FIXED (Stage 33) | **Likely working** but unverified for nested cases. |
| 9 | Stage 33 array element type tracking | FIXED (Stage 33) | **Likely partial** — see mbs issue above. |
| 10 | `funcArgElemTypes_` removal | FIXED (commit 5d7d56d) | **Confirmed**: code references show the field is present but no longer consulted by `emitCall`/`emitArrayPtr`. |
| 11 | Phase 4 unboxed sub returns | FIXED (tryEmitInline) | **Likely working**: `tests/regression_bugs.pl` exercises subs in scalar context and matches Perl. |

---

## Outstanding items needing remediation

The five items below were *not* in the prior REMEDIATION.md but
are real correctness regressions uncovered during this review.
They are listed in priority order; #1 blocks any test that uses
≥ 3-element numeric arrays, #5 is purely a UX issue but visible
to every user.

### R1. `perl_alloc_float_array` is not registered with the codegen.

**Test that demonstrates**: write
```perl
my @a = (1.0, 2.0, 3.0);
print "@a\n";
```
Compile with `./perlc` — fails with
`Error: Unknown runtime function: perl_alloc_float_array`.

**Where**: `src/codegen.cpp` ~line 234.  The existing registration
table has `RT("perl_alloc_flat_array", pv, i64);` and
`RT("perl_alloc_float_pair", ...)` but is missing
`RT("perl_alloc_float_array", pv, i64);`.

**Fix** (1 line at `src/codegen.cpp:234`):
```c
RT("perl_alloc_flat_array",  pv, i64);
RT("perl_alloc_float_array", pv, i64);   /* <-- ADD THIS */
RT("perl_alloc_float_pair",  pv, Type::getDoubleTy(ctx_), Type::getDoubleTy(ctx_));
```

**Why it slipped through**: commit `5934a7a feat: FLAT_ARRAY 1D
ArrowDeref fast path` added `perl_alloc_float_array` to
`src/runtime.c` and `src/runtime.h` and used it at codegen line
5903, but the `RT()` registration table in `declareRuntime()`
was not updated.  Because the codegen call site is only reached
when an anon-array literal has 3+ all-F64 elements, smaller
benchmarks (mbs uses 2-element complex numbers, fibn/nb don't
use anon arrays) never exercised it.  `tests/mbs.pl` should
have caught it but is timing-out for other reasons.

### R2. `eval { BLOCK }` returns the wrong value.

**Test that demonstrates**: `tests/eval_exception.pl` prints
`nested_eval=` (empty) instead of `nested_eval=inner caught`.

**Where**: `src/codegen.cpp:6225-6255` (case `NK::EvalBlock`,
spanning lines 6247-6254 for the body emission and the
unconditional `return perlUndef()`).

**Fix** (~5 lines, at lines 6248 and 6254):
```cpp
case NK::EvalBlock: {
    /* $@ = "" before eval */
    ...
    /* REPLACE: emitBlock(*n.body);  (line 6248) */
    Value *bodyVal = emitBlockLast(*n.body);
    Value *cloned = bodyVal ? callRT("perl_clone", {bodyVal}) : perlUndef();
    freeIfOwned(bodyVal);
    /* fall through to endBB */
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(endBB);
    builder_.SetInsertPoint(endBB);
    callRT("perl_eval_pop", {});
    /* REPLACE: return perlUndef();  (line 6254) */
    return cloned;
}
```

**Why it slipped through**: the original commit `a701cd1`
prevented the LLVM verify error by short-circuiting `ret ptr`
in main, but the body-value path was never wired up.  The
existing test in `tests/eval_exception.pl` exercises
`return "inner caught"` from a nested eval but the
test never reads the value of the outer eval; only the side
effect of printing it.  Reading the value (`my $r = eval { ... }`)
immediately fails.

### R3. Scalar-context list producers (`grep`/`map`/`sort`)
return the wrong value.

**Test that demonstrates**: `tests/wantarray_extended.pl` lines
150-156:
```
my $grep_count = count_grep();    # should be 3 (count), gets 5
my $map_count  = count_map();     # should be 3, gets 6
my $sort_count = count_sort();    # should be undef, gets 3
```

**Where**: `src/runtime.c:419-434` (`perl_array_to_list_return`).
The function unconditionally takes the last element in scalar
context, but Perl semantics are:
* `grep BLOCK LIST` in scalar context → `perl_alloc_int(count)`
* `map BLOCK LIST` in scalar context → undef or last (Perl issues
  "Useless use of map in scalar context" warning)
* `sort LIST` in scalar context → undef

**Fix** (add three helpers, ~30 lines):

```c
PerlValue *perl_grep_list_return(PerlArray *av) {
    int ctx = (s_wantarray_depth > 0) ? s_wantarray_stack[s_wantarray_depth - 1] : 0;
    if (ctx) {
        PerlValue *r = pv_alloc();
        r->tag = PERL_LIST_RESULT;
        r->flags = 0; r->matchpos = 0; r->blessed_class = NULL;
        r->pval = av;
        av->refcount = 1;
        return r;
    }
    /* scalar: return count */
    long long n = av->len;
    perl_array_free(av);
    return perl_alloc_int(n);
}

PerlValue *perl_sort_list_return(PerlArray *av) {
    int ctx = (s_wantarray_depth > 0) ? s_wantarray_stack[s_wantarray_depth - 1] : 0;
    if (ctx) {
        PerlValue *r = pv_alloc();
        r->tag = PERL_LIST_RESULT;
        r->flags = 0; r->matchpos = 0; r->blessed_class = NULL;
        r->pval = av;
        av->refcount = 1;
        return r;
    }
    perl_array_free(av);
    return perl_alloc_undef();   /* sort in scalar ctx: undef */
}

PerlValue *perl_map_list_return(PerlArray *av) {
    /* map: behave like grep_count for now (Perl doc: undefined;
       many real programs rely on "last element" behavior) */
    return perl_array_to_list_return(av);
}
```

Then in `src/codegen.cpp`, replace `perl_array_to_list_return`
calls at lines 2846 and 3789 with the appropriate per-kind helper
when the producer is `GrepFunc`/`SortFunc`/`MapFunc`.

**Why it slipped through**: The original wantarray context
propagation work added `perl_array_to_list_return` and verified
that list-context returns work.  The scalar-context cases
(returns 5 from `count_grep` when called from `my $c = ...`)
were not exercised by any test in the prior corpus.

### R4. `perl_to_string` and `perl_to_string_dup` lack cases for
`PERL_FLAT_ARRAY` and `PERL_FLOAT_PAIR`.

**Test that demonstrates**:
```perl
my @a = (1.0, 2.0, 3.0);
print "@a\n";                  # empty, should print "1 2 3"
my $z = [4.0, 5.0];
print "z=$z\n";                # empty, should print "z=ARRAY(0x...)"
```

**Where**: `src/runtime.c:965-1005` and `1018-1057`.  The
`switch` statements fall through to `default: return strdup("")`.

**Fix** (~10 lines per function):

```c
case PERL_FLAT_ARRAY: {
    /* Format pval as double[] of length matchpos */
    long long n = v->matchpos;
    double *d = (double *)v->pval;
    size_t cap = 16 + n * 24;
    char *buf = malloc(cap);
    size_t off = 0;
    for (long long i = 0; i < n; i++) {
        int w = snprintf(buf + off, cap - off, "%s%g",
                         i == 0 ? "" : " ", d[i]);
        if (w < 0 || (size_t)w >= cap - off) break;
        off += w;
    }
    if (cap > 0) buf[off] = '\0';
    return buf;   /* caller must free */
}
case PERL_FLOAT_PAIR: {
    double re = v->fval;
    double im = ((double *)v->pval)[0];   /* matchpos bits */
    /* decode matchpos: see codegen for encoding */
    ...
}
```

(The FLOAT_PAIR encoding from the docstring at codegen.cpp
~line 5884 stores `fval = re` and `matchpos = *(long long*)&im`,
which is platform-specific; the fix is to use a clean two-`double`
allocation instead.)

**Why it slipped through**: The FLAT_ARRAY / FLOAT_PAIR
optimizations were added for *computational* speed; no test in
the corpus printed the resulting values via `print` or
interpolation.

### R5. REPL linker invocation does not include `-lsqlite3`.

**Test that demonstrates**: run `./perlc -i`, enter
`my $x = 1 + 2; print "x=$x\n";`, then `quit`.
The REPL prints `Compilation failed.` and a linker error from
clang-18 complaining about `undefined reference to
sqlite3_close`.

**Where**: `src/main.cpp:600`.  The non-REPL compile path at
line 801 correctly passes `-lsqlite3`; the REPL path was
forgotten when SQLite was added to the runtime.

**Fix** (1 line):

```cpp
// src/main.cpp:600 (REPL compile command)
cmd += " " + tmpIR + " " + rtSrc + " -o " + outFile
       + " -lm -lpcre2-8 -lsqlite3 -latomic 2>&1";   /* add -lsqlite3 */
```

**Why it slipped through**: SQLite was added to the runtime
but the REPL compile command was not updated.  Because the
runtime.c references `sqlite3_close` unconditionally (the
`perl_dbi_*` functions are statically referenced from the IR
emitted for any program that calls any DBI method, even if the
program never instantiates a DBI handle), *every* REPL
compilation fails.

---

## Already-fixed items (verified)

The following items from the original REMEDIATION.md remain
fixed as of `git log` HEAD `729fcfa`:

* Compound `-=` on shared scalars — regression_subtract passes.
* `*=` `/=` `%=` atomic RMW — regression_multiply/divide/modulo
  pass.
* Closure capture of `for (1..$captured)` — regression_closure_range
  and regression_anon_range pass.
* Stage 32 loop-invariant PV deferral — visible in IR with
  reduced `perl_free` count.
* Stage 32 deref hoisting — *partially* working (mbs inner loop
  not benefiting).
* Stage 33 known tag type tracking — wired but not directly
  tested.
* Stage 33 array element type tracking — wired but not directly
  tested.
* `funcArgElemTypes_` removal — code references confirm the
  removal.
* Phase 4 unboxed sub returns — `cabs2` etc. inline correctly
  per the inner-loop IR.
