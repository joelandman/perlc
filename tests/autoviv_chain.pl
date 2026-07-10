#!/usr/bin/perl
# In-depth test suite for 3+ level chained hash/array autovivification.
#
# Root cause: the ArrowDeref-assignment codegen (`$ref->[i] = val` /
# `$ref->{k} = val`, with autovivification) only special-cased a base that
# was exactly one HashElem or ArrayElem level ($h{k}[i]=val, $a[i]{k}=val).
# A base that was itself another ArrowDeref — exactly what a 3+ level chain
# like $h{a}{b}{c} produces (parsed as ArrowDeref(ArrowDeref(HashElem(h,a),
# "b"), "c")) — fell through to the generic "$ref->[i]=val" fallback, which
# does a plain, non-autovivifying emitExpr+perl_deref_hash/array of the
# base. Since the middle level ($h{a}{b}) didn't exist yet, that produced a
# fresh, disconnected, immediately-discarded container — silently doing
# nothing, with no error.
#
# Fixed with a new recursive emitAutovivContainer() helper (codegen.cpp)
# that walks an arbitrary-depth HashElem/ArrayElem/ArrowDeref chain,
# autovivifying every missing intermediate level via the existing
# perl_(hash|array)_autoviv_(hash|array)[_sv] runtime primitives.
#
# IMPORTANT scoping note (see TESTS.md D40): the fix is deliberately scoped
# to chains rooted in a %hash/@array element (isElemRootedChain() in
# codegen.cpp). A chain rooted in a bare scalar/ref variable instead (e.g.
# $ref->[0][1] where $ref holds an arrayref) is NOT routed through the new
# recursive autoviv path — it keeps using the pre-existing FLAT_ARRAY-aware
# fallback, which is required for correctness on numeric benchmarks
# (nb.pl's $bodies->[i][j] pattern) where the inner arrays are FLAT_ARRAY-
# tagged, not plain REF_ARRAY; the autoviv_* runtime helpers only recognize
# REF_ARRAY/REF_HASH tags and would silently destroy FLAT_ARRAY data if
# used indiscriminately. A SEPARATE, still-open gap (not fixed here, logged
# as D50) is `$ref->{a}{b} = val` starting from an existing-but-empty
# scalar ref — that's a different code path (plain deref, not autoviv) and
# out of scope for this fix.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — 3-level implicit-arrow chain ─────────────
{
    my %h;
    $h{a}{b}{c} = 1;
    check('three_level_implicit', $h{a}{b}{c} == 1);
}

# ── Section 2: 3-level explicit-arrow chain ─────────────────────────────────
{
    my %h;
    $h{a}->{b}->{c} = 1;
    check('three_level_explicit', $h{a}->{b}->{c} == 1);
}

# ── Section 3: 2-level chain (regression) ───────────────────────────────────
{
    my %h;
    $h{a}{b} = 1;
    check('two_level_regression', $h{a}{b} == 1);
}

# ── Section 4: 4 and 5-level chains ─────────────────────────────────────────
{
    my %h;
    $h{a}{b}{c}{d} = 1;
    check('four_level', $h{a}{b}{c}{d} == 1);
}
{
    my %h;
    $h{a}{b}{c}{d}{e} = 2;
    check('five_level', $h{a}{b}{c}{d}{e} == 2);
}

# ── Section 5: mixed array/hash chains, hash-array-hash ─────────────────────
{
    my %h;
    $h{a}[0]{b} = 3;
    check('hash_array_hash', $h{a}[0]{b} == 3);
}

# ── Section 6: mixed array/hash chains, hash-hash-array ─────────────────────
{
    my %h;
    $h{a}{b}[0] = 4;
    check('hash_hash_array', $h{a}{b}[0] == 4);
}

# ── Section 7: array-rooted chain, array-hash-hash ──────────────────────────
{
    my @a;
    $a[0]{b}{c} = 5;
    check('array_hash_hash', $a[0]{b}{c} == 5);
}

# ── Section 8: array-rooted chain, array-array-array ────────────────────────
{
    my @a;
    $a[0][1][2] = 6;
    check('array_array_array', $a[0][1][2] == 6);
}

# ── Section 9: hash-array-array ─────────────────────────────────────────────
{
    my %h;
    $h{x}[0][1] = 7;
    check('hash_array_array', $h{x}[0][1] == 7);
}

# ── Section 10: multiple distinct branches under the same root ─────────────
# (must not clobber each other — each autoviv step must only create the
# specific missing level, not reset siblings)
{
    my %h;
    $h{a}{b}{c} = 1;
    $h{a}{b}{d} = 2;
    $h{a}{x}{y} = 3;
    check('sibling_branches_independent',
          $h{a}{b}{c} == 1 && $h{a}{b}{d} == 2 && $h{a}{x}{y} == 3);
}

# ── Section 11: reading a non-existent deep path returns undef, no crash ───
{
    my %h;
    my $v = $h{a}{b}{c};
    check('read_missing_deep_path', !defined($v));
}

# ── Section 12: ref() at each level of a deep chain ─────────────────────────
{
    my %h;
    $h{a}{b}{c} = 1;
    check('ref_at_each_level', ref($h{a}) eq "HASH" && ref($h{a}{b}) eq "HASH");
}

# ── Section 13: array of hashes of arrays, built incrementally ─────────────
{
    my @a;
    $a[0]{items}[0] = "x";
    $a[0]{items}[1] = "y";
    check('array_hash_array_incremental', join(",", @{ $a[0]{items} }) eq "x,y");
}

# ── Section 14: nb.pl-style scalar-ref-rooted 2D array — must still use the ─
# ── FLAT_ARRAY-aware fast path, not the new autoviv path (regression) ──────
{
    my $bodies = [[0, 0, 0], [1, 1, 1]];
    $bodies->[0][1] = 99;
    check('scalar_ref_2d_array_regression',
          $bodies->[0][0] == 0 && $bodies->[0][1] == 99 && $bodies->[1][2] == 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "autoviv_chain_tests_done\n";
