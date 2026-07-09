#!/usr/bin/perl
# In-depth test suite for list-assignment arity mismatch (my (...) = (...)
# where the LHS and RHS element counts differ).
#
# Root cause of the original crash: `my ($x, $y) = (10);` — a single-element
# parenthesized RHS with no comma parses down to a bare scalar node (not an
# ArrayLit, since the parser treats parens with no comma as plain grouping).
# The list-assignment codegen's `emitArrayPtr(RHS)` then returned null (no
# array to produce for a bare scalar), and the fallback path passed the
# resulting PerlValue* directly to perl_array_get_ref() as if it were a
# PerlArray* — a raw type confusion that read garbage memory and segfaulted.
# Fixed by wrapping the lone scalar in a real one-element PerlArray first,
# matching the pattern already used by `@arr = RHS` and lvalue-slice
# assignment elsewhere in the same function.
#
# NOTE: two unrelated, separately-tracked defects are avoided here:
#   - `defined $x` without parens fails to parse (TESTS.md D34) — every
#     `defined` call below uses parens.
#   - a named `sub` declared *inside* a bare block that follows another bare
#     block resolves to the wrong value at its call site (a distinct bug
#     found while writing this suite, not yet in the registry as of this
#     commit) — so `myfunc` below is declared at file scope, not nested.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub myfunc { return 77; }

sub arity_in_sub {
    my ($x, $y) = (@_);
    return defined($y) ? "$x:$y" : "$x:undef";
}

# ── Section 1: the original crash — single-scalar RHS, more LHS slots ──────
{
    my ($x, $y) = (10);
    check('fewer_rhs_single_scalar', $x == 10 && !defined($y));
}

# ── Section 2: multi-element RHS, one extra LHS slot ────────────────────────
{
    my ($a, $b, $c) = (1, 2);
    check('fewer_rhs_multi_elem', $a == 1 && $b == 2 && !defined($c));
}

# ── Section 3: exact match (regression) ─────────────────────────────────────
{
    my ($p, $q) = (5, 6);
    check('exact_match', $p == 5 && $q == 6);
}

# ── Section 4: more RHS values than LHS scalars — extras discarded ─────────
{
    my ($a, $b) = (1, 2, 3, 4);
    check('more_rhs_than_lhs', $a == 1 && $b == 2);
}

# ── Section 5: single LHS scalar, multi-element RHS ─────────────────────────
{
    my ($x) = (1, 2, 3);
    check('single_lhs_multi_rhs', $x == 1);
}

# ── Section 6: RHS is a bare scalar variable, not a literal ─────────────────
{
    my $v = 99;
    my ($p, $q) = ($v);
    check('rhs_scalar_var', $p == 99 && !defined($q));
}

# ── Section 7: RHS is a single-value function call ──────────────────────────
{
    my ($m, $n) = (myfunc());
    check('rhs_function_call', $m == 77 && !defined($n));
}

# ── Section 8: RHS is a ternary producing a single scalar ──────────────────
{
    my $cond = 1;
    my ($x, $y) = ($cond ? 100 : 200);
    check('rhs_ternary', $x == 100 && !defined($y));
}

# ── Section 9: RHS is totally empty list ────────────────────────────────────
{
    my ($a, $b) = ();
    check('empty_rhs', !defined($a) && !defined($b));
}

# ── Section 10: RHS is a single explicit undef ──────────────────────────────
{
    my ($x, $y) = (undef);
    check('rhs_explicit_undef', !defined($x) && !defined($y));
}

# ── Section 11: RHS is an array variable flattened inside parens ───────────
{
    my @one = (99);
    my ($x, $y) = (@one);
    check('rhs_flattened_array_var', $x == 99 && !defined($y));
}

# ── Section 12: RHS has a trailing comma, single element ───────────────────
{
    my ($x, $y) = (5,);
    check('rhs_trailing_comma', $x == 5 && !defined($y));
}

# ── Section 13: trailing @rest array with a single-scalar RHS ──────────────
# (a multi-element-RHS variant of this, `my ($a,@rest)=(1,2,3,4)` collecting
# the remainder into @rest, is a SEPARATE, already-tracked bug — TESTS.md
# D39 — not exercised here since it's unrelated to this fix.)
{
    my ($a, @rest) = (10);
    check('trailing_rest_single_rhs', $a == 10 && scalar(@rest) == 0);
}
{
    my ($a, $b, @rest) = (10);
    check('trailing_rest_scalar_gap', $a == 10 && !defined($b) && scalar(@rest) == 0);
}

# ── Section 14: fewer-RHS inside a sub (via @_, not just file scope) ───────
{
    check('fewer_rhs_inside_sub', arity_in_sub(42) eq "42:undef");
    check('exact_rhs_inside_sub', arity_in_sub(1, 2) eq "1:2");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "list_assign_arity_tests_done\n";
