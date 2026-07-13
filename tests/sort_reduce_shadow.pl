#!/usr/bin/perl
# In-depth test suite for D28: `sort`/`reduce` comparator blocks always
# rebound `$a`/`$b` to fresh locals, ignoring an outer lexical `my $a`/
# `my $b` that should shadow them.
#
# Root cause: real Perl's `sort`/`reduce` comparator `$a`/`$b` are
# dynamically-aliased *package* variables, not a fresh lexical scope the
# construct introduces itself. If an earlier `my $a`/`my $b` is already
# declared in an enclosing lexical scope, it permanently shadows the
# package variable's name for all later code in that scope — including
# a comparator block compiled after it, since Perl resolves `$a`/`$b`
# references via ordinary compile-time lexical scoping, the same as any
# other variable. The comparator ends up reading whatever the *outer*
# lexical holds (fixed, never updated per-comparison) instead of the
# pair actually being compared. This is a well-known real-Perl footgun
# (real Perl even warns about it: "my $a used in sort comparison").
# perlc instead always declared its own fresh `$a`/`$b` locals for the
# comparator, unconditionally winning any lookup — "sensible" on its
# own, but a real behavioral divergence from actual Perl semantics.
#
# Fixed: both sites that bind `$a`/`$b` for a comparator (sort's, a
# separate LLVM function since it must be passed as a real C function
# pointer to the sorting routine; and reduce's, compiled inline in the
# same function since it's just a loop, no function pointer needed)
# now check whether `$a`/`$b` is already visible from an enclosing
# scope before declaring their own, and skip the declaration if so —
# letting normal variable lookup fall through to the outer shadow
# exactly as real Perl's lexical scoping would.
#
# reduce's check is fully general (`lookupVar()`, since its scope stack
# isn't reset — it can see both file-scope *and* enclosing-sub-scope
# shadows). sort's comparator is compiled as a genuinely separate LLVM
# function with a full scope reset, so its check can only reach a
# file-scope shadow (via fileScalarGlobals_, which that reset doesn't
# touch) — an enclosing *sub-scoped* `my $a` shadowing a sort comparator
# would need closure-capture machinery sort's comparator doesn't have,
# and remains a narrower, separate, open gap.
#
# IMPORTANT — why this file checks *comparator behavior* (via
# push()-ing observations) rather than final sort order for the `sort`
# cases: once `$a` is shadowed to a fixed, non-varying value, the
# resulting comparator is not a valid strict-weak-ordering (it violates
# antisymmetry — cmp(x,y) no longer depends on x at all), which is
# undefined behavior for *any* sorting algorithm. Real Perl's `sort` is
# a stable mergesort; perlc's uses C's qsort() (unstable, pivot-choice-
# dependent). For a degenerate, non-transitive comparator, different
# algorithms legitimately produce different final permutations — that
# divergence is a pre-existing, separate, out-of-scope characteristic of
# perlc's sort implementation choice, not a bug in this fix, and even
# the *number* of comparator invocations differs between algorithms.
# What this fix guarantees, and what's checked here, is that *every*
# individual comparator invocation reads the correct (shadowed) value —
# an algorithm-independent property.
use strict;
use warnings;
use List::Util qw(reduce);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug, via reduce (fully deterministic) — ───────
# ── shadowing $a only ───────────────────────────────────────────────────────
{
    my $a = 1;
    my @data = (1, 2, 3, 4, 5);
    my $result = reduce { $a * $b } @data;
    check('reduce_a_shadowed', $result == 5);
}

# ── Section 2: shadowing $b only — $a keeps working normally (reduce's ────
# ── own internal $a), only $b resolves to the outer lexical ────────────────
{
    my $b = 100;
    my @data = (1, 2, 3, 4, 5);
    my $result = reduce { $a + $b } @data;
    # each step adds the fixed $b=100 instead of the next element;
    # starting accumulator 1, then +100 four times (elements 2..5) = 401
    check('reduce_b_shadowed', $result == 401);
}

# ── Section 3: shadowing both $a and $b ────────────────────────────────────
{
    my $a = 2;
    my $b = 3;
    my @data = (10, 20, 30);
    my $result = reduce { $a + $b } @data;
    # $a and $b are both fixed (2 and 3) for every comparison, so the
    # accumulator becomes 5 after the first reduction step and stays 5
    check('reduce_both_shadowed', $result == 5);
}

# ── Section 4: regression — reduce with no shadowing still works ──────────
# ── correctly (normal internal $a/$b binding) ───────────────────────────────
{
    my @data = (1, 2, 3, 4, 5);
    my $result = reduce { $a * $b } @data;
    check('reduce_no_shadow_regression', $result == 120);
}

# ── Section 6: regression — sort with no shadowing still produces the ─────
# ── exact correct order ──────────────────────────────────────────────────────
{
    my @nums = (5, 3, 8, 1, 4);
    my @sorted_asc = sort { $a <=> $b } @nums;
    my @sorted_desc = sort { $b <=> $a } @nums;
    check('sort_no_shadow_ascending', join(",", @sorted_asc) eq "1,3,4,5,8");
    check('sort_no_shadow_descending', join(",", @sorted_desc) eq "8,5,4,3,1");
}

# ── Section 7: regression — sort by a derived key (string length) with ────
# ── no shadowing still works correctly ──────────────────────────────────────
{
    my @words = ("banana", "apple", "cherry", "date", "fig");
    my @sorted = sort { length($a) <=> length($b) } @words;
    check('sort_no_shadow_by_length', join(",", @sorted) eq "fig,date,apple,banana,cherry");
}

# ── Section 5: sort — verify the comparator's own $a resolves to the ──────
# ── shadowing outer lexical on every single invocation (algorithm- ────────
# ── independent; does not check final sort order — see header comment). ───
# ── Declared at true file scope (not a nested block) and placed *after* ───
# ── the no-shadowing regression checks above: sort's shadow-detection is ──
# ── file-scope-only (see header comment above), so a file-scope `my $a` ───
# ── here would otherwise permanently shadow $a for any *later* sort in ────
# ── this file too. Also sidesteps a separate, pre-existing, unrelated ─────
# ── defect (D61: sort's comparator — a genuinely separate LLVM function ───
# ── with no closure-capture support — can't see/modify a *block*-scoped ───
# ── outer variable at all, only a file-scope one; logged separately, not ──
# ── fixed here since it's orthogonal to D28's $a/$b-shadowing fix). ───────
my $a = "SHADOW";
my @observed_a;
my @shadow_data = (3, 1, 2, 5, 4);
my @shadow_sorted = sort { push @observed_a, $a; $b <=> $b } @shadow_data;
my $all_shadow = 1;
for my $v (@observed_a) { $all_shadow = 0 if $v ne "SHADOW"; }
check('sort_a_shadowed_every_call', $all_shadow);
check('sort_comparator_was_called', scalar(@observed_a) > 0);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_reduce_shadow_tests_done\n";
