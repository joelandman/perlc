#!/usr/bin/perl
# In-depth test suite for trailing @rest/%rest in list assignment
# (my ($a, $b, @rest) = LIST / my ($a, %rest) = LIST).
#
# Root cause of the original bug: the parser's `my (LIST) = RHS` handling
# (parser.cpp, parseMy's LPAREN branch) correctly tracked each LHS
# variable's sigil when emitting its `my` declaration, but when building
# the assignment-target list it unconditionally called makeScalar() on
# every variable name — stripping the sigil and turning `@rest` into a
# bare ScalarVar node named "rest". Codegen's list-assignment loop then
# treated every target uniformly as a single scalar slot, so the *array*
# `@rest` (correctly declared, but never targeted by the assignment) stayed
# empty, while a same-named-but-unrelated scalar silently absorbed one
# element of the RHS list and went nowhere useful.
#
# Fixed in two places: the parser now preserves the sigil (emitting
# ArrayVar/HashVar nodes for trailing @rest/%rest instead of ScalarVar),
# and codegen's list-assignment loop now recognizes an ArrayVar/HashVar
# target and slurps every remaining RHS element into it (via the new
# perl_array_extend_from runtime helper, plus perl_hash_from_list for the
# hash case) instead of doing a single per-index scalar assign.
#
# NOTE: `sub`s below are declared at file scope, not inside a nested bare
# block — a named sub declared inside a bare block that follows another
# bare block resolves to the wrong value at its call site (TESTS.md D45,
# a separate, already-tracked bug unrelated to this fix).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub with_rest      { my ($first, @rest) = @_; return "$first:" . join(",", @rest); }
sub whole_list      { my (@all) = @_; return scalar(@all); }
sub two_then_sum    { my ($a, $b, @rest) = @_; my $sum = 0; $sum += $_ for @rest; return "$a-$b-$sum"; }

# ── Section 1: the original bug — one scalar, trailing array collects rest ──
{
    my ($a, @rest) = (1, 2, 3, 4, 5);
    check('array_rest_collects', $a == 1 && join(",", @rest) eq "2,3,4,5");
}

# ── Section 2: two scalars, trailing array ──────────────────────────────────
{
    my ($a, $b, @rest) = (1, 2, 3, 4, 5);
    check('two_scalars_then_rest', $a == 1 && $b == 2 && join(",", @rest) eq "3,4,5");
}

# ── Section 3: exact match — @rest ends up empty, not undef/error ──────────
{
    my ($a, $b, @rest) = (1, 2);
    check('no_leftover_rest_empty', $a == 1 && $b == 2 && scalar(@rest) == 0);
}

# ── Section 4: single leftover element ──────────────────────────────────────
{
    my ($a, @rest) = (1, 2);
    check('single_leftover', $a == 1 && join(",", @rest) eq "2");
}

# ── Section 5: whole list slurped — array is the only LHS element ──────────
{
    my (@all) = (1, 2, 3, 4);
    check('whole_list_slurp', join(",", @all) eq "1,2,3,4");
}

# ── Section 6: trailing %rest collects remaining key/value pairs ───────────
{
    my ($a, %rest) = (1, "k1", "v1", "k2", "v2");
    check('hash_rest_collects',
          $a == 1 && $rest{k1} eq "v1" && $rest{k2} eq "v2" && scalar(keys %rest) == 2);
}

# ── Section 7: trailing %rest with zero remaining pairs ─────────────────────
{
    my ($a, %rest) = (1);
    check('hash_rest_empty', $a == 1 && scalar(keys %rest) == 0);
}

# Section 8 (trailing %rest with an odd trailing element, e.g.
# `my ($a,%rest) = (1,"k1","v1","k2")` → $rest{k2} should be undef, matching
# real Perl) is deliberately NOT included here: real Perl emits "Odd number
# of elements in hash assignment" to stderr for that input under `use
# warnings`, which perlc has no way to suppress (no `no warnings 'misc'`
# support — that pragma form is a hard parse error — and no %SIG support to
# install a __WARN__ handler either), so it can never byte-match real Perl's
# combined stdout+stderr in this harness. Manually verified correct instead
# (perl_hash_from_list, runtime.c: a trailing unpaired key now gets an undef
# value instead of being silently dropped, matching real Perl's assignment
# behavior even though the accompanying warning isn't reproduced) — see
# REMEDIATION.md item 19.

# ── Section 9: repeated assignment overwrites @rest instead of accumulating ─
{
    my ($a, @rest) = (1, 2, 3);
    ($a, @rest) = (9, 8);
    check('repeated_assign_overwrites', $a == 9 && join(",", @rest) eq "8");
}

# ── Section 10: our (...) form with a trailing array ────────────────────────
{
    our ($g, @grest);
    ($g, @grest) = (10, 20, 30);
    check('our_form_trailing_array', $g == 10 && join(",", @grest) eq "20,30");
}

# ── Section 11: @rest sourced from @_ inside a sub ──────────────────────────
{
    check('sub_at_with_rest_multi', with_rest(1, 2, 3, 4) eq "1:2,3,4");
    check('sub_at_with_rest_single', with_rest(1) eq "1:");
}

# ── Section 12: whole-@_-slurp inside a sub ─────────────────────────────────
{
    check('sub_whole_list_slurp', whole_list(10, 20, 30) == 3);
    check('sub_whole_list_empty', whole_list() == 0);
}

# ── Section 13: two scalars + rest sourced from @_, used numerically ───────
{
    check('sub_two_then_sum_multi', two_then_sum(1, 2, 3, 4, 5) eq "1-2-12");
    check('sub_two_then_sum_none', two_then_sum(1, 2) eq "1-2-0");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "list_assign_rest_tests_done\n";
