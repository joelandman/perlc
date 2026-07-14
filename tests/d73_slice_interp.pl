#!/usr/bin/perl
# In-depth test suite for D73: array/hash slice interpolation inside
# double-quoted strings didn't work at all.
#
# Root cause: Parser::parseStringInterp's "@arr" scanning block (parser.cpp)
# unconditionally built a whole-array join as soon as it saw "@name" — it
# never checked whether a "[" or "{" immediately followed the name. So
# "@arr[1,2]" interpolated the *entire* array (space-joined) and then
# appended the literal, never-parsed text "[1,2]" right after it; "@h{'a',
# 'b'}" was worse still, since "@h" isn't a valid array name by itself —
# it silently interpolated as an empty array followed by literal
# "{'a','b'}" text.
#
# Fixed by extending the same scanning block: after the variable name is
# read, check for an immediately-following "[" or "{". If present, extract
# the bracketed content (tracking nesting depth of the same bracket type),
# tokenize and parse it as a comma-separated expression list (a small new
# Parser::parseExprListFromTokens helper, added alongside the pre-existing
# single-expression parseExprFromTokens), and build an ArraySlice/HashSlice
# AST node — the same node kinds the token-based parser already produces
# for non-interpolated "@arr[LIST]"/"@hash{LIST}" slice syntax. Either the
# slice or the pre-existing whole-array node is then wrapped in the same
# space-joining JoinFunc as before, since real Perl interpolates a slice's
# result the same way it interpolates a whole array.
#
# Deliberately does NOT `use warnings`: real Perl emits a stylistic
# "Scalar value @arr[1] better written as $arr[1]" warning on any
# single-index/key slice, which several sections below intentionally
# exercise (the fix must interpolate a single-element slice correctly too,
# even though it's a discouraged style) — perlc doesn't emit `use warnings`
# diagnostics at all (D56) and has no `no warnings` support yet to suppress
# just that one (D48), so the warning would otherwise land on stderr and
# break this file's exact-match harness comparison. Same precedent as
# other test files in this suite (e.g. sort_scalar_context.pl for D29).
use strict;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repros ──────────────────────────────────────────
{
    my @arr = (10, 20, 30, 40);
    check('array_slice_basic', "@arr[1,2]" eq "20 30");
}
{
    my %h = (a => 1, b => 2);
    check('hash_slice_basic', "@h{'a','b'}" eq "1 2");
}

# ── Section 2: whole-array/whole-context regression (no slice syntax) ─────
{
    my @arr = (10, 20, 30, 40);
    check('whole_array_regression', "@arr" eq "10 20 30 40");
}

# ── Section 3: single-element and non-contiguous slice indices ────────────
{
    my @arr = (10, 20, 30, 40);
    check('single_element_slice', "@arr[1]" eq "20");
    check('noncontiguous_slice', "@arr[0,3]" eq "10 40");
}

# ── Section 4: variable index/key inside the slice ──────────────────────────
{
    my @arr = (10, 20, 30, 40);
    my $i = 2;
    check('variable_index_slice', "@arr[$i]" eq "30");
}
{
    my %h = (a => 1, b => 2, c => 3);
    my $k = "b";
    check('variable_key_slice', "@h{$k}" eq "2");
}

# ── Section 5: 3+ element slices, both array and hash ───────────────────────
{
    my @arr = (10, 20, 30, 40, 50);
    check('three_elem_array_slice', "@arr[0,2,4]" eq "10 30 50");
}
{
    my %h = (a => 1, b => 2, c => 3, d => 4);
    check('three_elem_hash_slice', "@h{'a','c','d'}" eq "1 3 4");
}

# ── Section 6: slice embedded within surrounding literal text ──────────────
{
    my @arr = (10, 20, 30);
    check('slice_with_surrounding_text', "prefix @arr[0,1] suffix" eq "prefix 10 20 suffix");
}

# ── Section 7: negative index inside an array slice ─────────────────────────
{
    my @arr = (10, 20, 30, 40);
    check('negative_index_slice', "@arr[-1,-2]" eq "40 30");
}

# ── Section 8: regressions — plain scalar and hash-element interpolation,
#    and array-of-arrays subscript (not slice syntax, must not be confused
#    with it) all unaffected ─────────────────────────────────────────────────
{
    my $x = 5;
    check('plain_scalar_regression', "scalar: $x" eq "scalar: 5");
}
{
    my %h = (a => 1);
    check('plain_hash_elem_regression', "helem: $h{a}" eq "helem: 1");
}
{
    my @aoa = ([1, 2], [3, 4]);
    check('array_of_arrays_regression', "aoa: $aoa[0][1]" eq "aoa: 2");
}
{
    my @arr = (1, 2, 3);
    check('plain_array_elem_regression', "elem: $arr[1]" eq "elem: 2");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d73_slice_interp_done\n";
