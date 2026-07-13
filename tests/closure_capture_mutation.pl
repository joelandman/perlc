#!/usr/bin/perl
# In-depth test suite for D62: closures captured a `my` variable's
# *value* by clone (`perl_array_push_capture()`, runtime.c) instead of
# storing the original `PerlValue*` — so a mutation from inside a
# closure was never visible to the enclosing scope, and vice versa,
# diverging from real Perl's actual by-reference closure semantics.
#
# The clone looked like it should just be deleted, but wasn't dead code:
# `CodeGen::popScope()` (codegen.cpp) unconditionally `perl_free()`s
# every `my` variable's tracked stable `PerlValue*` when its lexical
# scope exits normally. A destructive local experiment (patch out the
# clone, rebuild, run an adversarial repro, revert — not shipped)
# confirmed removing the clone naively causes real heap corruption
# (`free(): invalid size`) the moment a captured variable's declaring
# block exits normally and its freed slab slot gets reused before the
# closure is called again.
#
# Fixed with reference counting instead: `perl_array_push_capture` now
# stores the original pointer and bumps a capture-refcount packed into
# spare `PerlValue.flags` bits; `perl_free()` defers the real free until
# both the declaring scope AND every capturing closure have released
# their share (`PV_FLAG_CAPTURE_RELEASED` records that the scope's side
# already happened). `PerlClosure` itself gained a `refcount` (mirroring
# the existing `PerlArray`/`PerlHash` pattern) so `perl_clone`/`perl_assign`
# aliasing a code ref doesn't tear a closure down while another reference
# still exists, and `perl_assign` was fixed to stop clobbering a captured
# variable's capture-count bits on every plain reassignment (a gap found
# during review, not in the original design draft). `sort { BLOCK }`'s
# comparator (D61) also captures via the same function but has no
# `PerlClosure`/teardown event of its own, so `perl_sort_custom` gained a
# symmetric release step — without it, every scalar any `sort{}`
# comparator captures would be incremented once and never decremented,
# permanently leaking it.
#
# NOTE on test design: every scalar below that's mutated via `++`/`+`
# from inside a closure or comparator is initialized as a *string*
# ("0", not 0). A plain `my $x = 0;` at block scope can compile to
# perlc's unboxed-int fast path (`intScopes_`), which captures a boxed
# *snapshot* of the current value rather than the real stable pointer —
# a separate, deeper, still-open codegen limitation (logged as D64,
# confirmed to affect both AnonSub and sort's comparator identically)
# that this runtime-level fix cannot address. Forcing a string
# initializer keeps every section here isolated to exactly what D62 was
# about.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — mutation from inside a closure is ───────
# ── visible to the enclosing scope afterward ────────────────────────────────
{
    my $x = "0";
    my $f = sub { $x++; };
    $f->();
    $f->();
    check('mutation_visible_after_calls', $x == 2);
}

# ── Section 2: two-way visibility — mutating from outside after closure ───
# ── creation is visible inside the closure too ─────────────────────────────
{
    my $x = "5";
    my $f = sub { return $x; };
    $x = "9";
    check('outside_mutation_visible_inside', $f->() == 9);
}

# ── Section 3: the adversarial escaping-closure crash repro — the real ────
# ── memory-safety proof, not just a correctness check. A closure captured ─
# ── into an outer variable inside a block that exits *normally* (no ───────
# ── explicit return — popScope's free loop actually fires here, unlike ────
# ── the classic `return sub {...}` idiom), called once inside the block, ──
# ── then heavy intervening allocation churn to force the freed slab slot ──
# ── back into reuse, then called again from outside — must produce the ────
# ── correctly mutated value, not garbage or a crash. ───────────────────────
{
    my $outer_f;
    {
        my $n = "0";
        $outer_f = sub { $n++; return $n; };
        check('escaping_closure_call_inside_block', $outer_f->() == 1);
    }
    for (1..50000) { my $tmp = "churn" . $_; }
    check('escaping_closure_call_after_churn_1', $outer_f->() == 2);
    check('escaping_closure_call_after_churn_2', $outer_f->() == 3);
}

# ── Section 4: multiple independent closures capturing the same variable ──
# ── — each sees the shared variable, and freeing one early doesn't affect ─
# ── the other's continued correct operation ─────────────────────────────────
{
    my $shared = "0";
    my $f = sub { $shared = $shared + 1; return $shared; };
    my $g = sub { $shared = $shared + 10; return $shared; };
    check('multi_closure_f_first_call', $f->() == 1);
    check('multi_closure_g_first_call', $g->() == 11);
    undef $g;
    check('multi_closure_f_after_g_freed', $f->() == 12);
}

# ── Section 5: sort{} comparator scalar mutation — D61's own test suite ───
# ── deliberately avoided this (array/hash only); now directly exercises ───
# ── perl_sort_custom's capture-release step ─────────────────────────────────
{
    my $calls = "0";
    my @data = (5, 3, 8, 1, 4);
    my @sorted = sort { $calls++; $a <=> $b } @data;
    check('sort_scalar_mutation_order', join(",", @sorted) eq "1,3,4,5,8");
    check('sort_scalar_mutation_visible', $calls > 0);
}

# ── Section 6: repeated sorts reusing the same captured scalar — no leak ──
# ── or premature free across many perl_sort_custom calls. Checks every ────
# ── iteration's sort result rather than the comparator's total invocation ─
# ── count: qsort()'s exact number of comparisons is an implementation ─────
# ── detail (glibc's qsort tries an internal mergesort with a malloc'd temp ─
# ── buffer, falling back to quicksort if that allocation fails or behaves ─
# ── differently under a given allocator — confirmed varying between a ─────
# ── plain build and an ASan-instrumented one in the same environment, with ─
# ── the sort result unaffected either way) — matching the same algorithm- ──
# ── independent-verification approach already established for D28/D61. ────
{
    my $total = "0";
    my $all_correct = 1;
    for my $i (1..100) {
        my @d = (3, 1, 2);
        my @s = sort { $total++; $a <=> $b } @d;
        $all_correct = 0 unless join(",", @s) eq "1,2,3";
    }
    check('sort_repeated_capture_reuse_correct', $all_correct);
    check('sort_repeated_capture_reuse_invoked', $total > 0);
}

# ── Section 7: regression — a closure that captures nothing mutable ───────
# ── (the overwhelmingly common case) is completely unaffected ─────────────
{
    my @data = (9, 1, 6, 3);
    my $f = sub { return scalar(@data); };
    check('no_mutation_capture_regression', $f->() == 4);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "closure_capture_mutation_tests_done\n";
