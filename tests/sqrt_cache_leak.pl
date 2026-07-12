#!/usr/bin/perl
# In-depth test suite for D9: the `floatSqrtOf_` compiler-side cache
# (Stage 30's "v*v -> x" optimization, rewriting a squared sqrt()-
# assigned variable back to its original input, avoiding an fmul on the
# sqrt critical path) was keyed only by variable *name* in codegen.cpp,
# with no invalidation. A later `my $name = ...` declaration — whether
# in the very same function (shadowing) or, worse, in a completely
# unrelated sub that happened to reuse the same variable name — left
# the stale association in place, so `$name * $name` silently returned
# the *previous* sub's sqrt() input as an LLVM Value* instead of
# actually computing the new variable's square. (The stale reference
# happened not to crash only because it was always a ConstantFP, which
# LLVM allows referencing across functions; the value was still
# numerically wrong for the referencing function's own variable.)
#
# Fixed with a one-line addition: `floatSqrtOf_.erase(nm)` before the
# conditional re-population, every time a `my $var = <float-RHS>`
# declaration is compiled — a fresh declaration always invalidates any
# previous association for that name, and only re-establishes one when
# *this* declaration's own right-hand side was actually sqrt(x).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — sqrt-assigned var in one sub leaking ────
# ── into an unrelated same-named var's squaring in a different sub ─────────
sub leak_f1 { my $a = sqrt(4); return $a; }
sub leak_f2 { my $a = 10.0; return $a * $a; }

check('cross_sub_sqrt_correct', leak_f1() == 2);
check('cross_sub_unaffected', leak_f2() == 100);

# ── Section 2: order reversed — the non-sqrt sub is compiled/called ───────
# ── *before* any sqrt-using sub touches the same variable name ─────────────
sub leak_g2 { my $b = 7.0; return $b * $b; }
sub leak_g1 { my $b = sqrt(64); return $b; }

check('reversed_order_non_sqrt_first', leak_g2() == 49);
check('reversed_order_sqrt_after', leak_g1() == 8);

# ── Section 3: same-function shadowing — a second `my $name` ──────────────
# ── declaration in the *same* sub (in a nested block, to avoid real ────────
# ── Perl's "masks earlier declaration" warning under use warnings) must ────
# ── also invalidate the earlier one ─────────────────────────────────────────
sub leak_h1 {
    my $c = sqrt(9);
    {
        my $c = 5.0;
        return $c * $c;
    }
}
check('same_function_shadowing', leak_h1() == 25);

# ── Section 4: regression — the legitimate sqrt optimization itself ───────
# ── still works correctly (dist*dist recovers the exact input) — uses ──────
# ── perfect squares so squaring sqrt(x) round-trips exactly in floating ────
# ── point (sqrt(2)*sqrt(2) is not bitwise 2.0, since sqrt(2) is not ────────
# ── exactly representable — that's a floating-point precision fact, not ────
# ── a perlc bug, so deliberately avoided here) ──────────────────────────────
sub dist_squared {
    my ($x) = @_;
    my $d = sqrt($x);
    return $d * $d;
}
check('legitimate_sqrt_opt_16', dist_squared(16) == 16);
check('legitimate_sqrt_opt_81', dist_squared(81) == 81);

# ── Section 5: multiple, unrelated subs each legitimately using sqrt on ───
# ── the same variable name — each must get its own correct value ───────────
sub leak_i1 { my $v = sqrt(25); return $v * $v; }
sub leak_i2 { my $v = sqrt(36); return $v * $v; }
sub leak_i3 { my $v = sqrt(49); return $v * $v; }

check('multi_sub_sqrt_i1', leak_i1() == 25);
check('multi_sub_sqrt_i2', leak_i2() == 36);
check('multi_sub_sqrt_i3', leak_i3() == 49);

# ── Section 6: calling the "leaking" subs multiple times / interleaved ────
# ── — confirms the fix isn't order- or call-count-dependent ────────────────
check('interleaved_call_1', leak_f2() == 100);
check('interleaved_call_2', leak_f1() == 2);
check('interleaved_call_3', leak_f2() == 100);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sqrt_cache_leak_tests_done\n";
