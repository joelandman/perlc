#!/usr/bin/perl
# In-depth test suite for D63: `$$ref[idx]`/`$$ref{key}` — real Perl's
# shorthand for `$ref->[idx]`/`$ref->{key}` — produced the wrong
# (empty/undef) result.
#
# Root cause: found while scoping D59's fix (string interpolation of
# `$$`). Confirmed this reproduces identically as a completely bare
# expression with no string interpolation involved at all, so it's an
# unrelated bug from D59's own root cause (the string-interpolation raw-
# text scanner) despite the superficial `$$...[...]` similarity.
#
# The parser already correctly builds a `DerefScalar` AST node for a bare
# `$$ref` (real Perl: `${$ref}`, "dereference $ref as a scalar"). The bug
# was in `parseSubscript`'s *adjacent-subscript* handling (parser.cpp,
# used for `EXPR[idx]`/`EXPR{key}` immediately following any of a list of
# "subscriptable" expression kinds, `DerefScalar` among them): it took
# whatever `DerefScalar` node had just been built and used it *directly*
# as the base of a further `ArrowDeref`, which compiles as "dereference
# the base value (already itself the result of dereferencing $ref!) as an
# array/hash, then index it" — an extra, incorrect second dereference.
# Real Perl's actual rule for `$$ref[idx]` is that it's *one* level of
# indirection total: `$ref` itself (not `${$ref}`) is the array/hash
# reference to index — exactly equivalent to `$ref->[idx]`.
#
# Fixed by special-casing exactly this situation in `parseSubscript`:
# when the base about to receive an *adjacent* `[idx]`/`{key}` subscript
# is a bare `DerefScalar`, unwrap it back to its own inner scalar-variable
# node (i.e. `$ref` itself) before building the `ArrowDeref`, rather than
# using the `DerefScalar` wrapper as-is. This only fires for the specific
# adjacent-bracket grammar production `$$name[...]`/`$$name{...}` with no
# intervening `->` — a genuinely different, already-correctly-working
# double dereference, `$$ref->[idx]` (real Perl: dereference $ref as a
# scalar to get a *second* reference, then arrow-deref *that* as an
# array/hash), goes through a separate branch of the same function and is
# completely untouched by this fix (tested as a regression below). A
# subsequent *chained* subscript after the first one (`$$aref[0][1]`)
# is also unaffected, since by then the base is already an `ArrowDeref`,
# not a `DerefScalar` — the unwrap only ever fires once, for the base
# case.
#
# NOT fixed here (a separate, deeper, pre-existing gap, already
# documented as out of scope by D59's own notes): a subscripted
# dereference *inside string interpolation* (`"$$ref[0]"`) still doesn't
# work — `parseStringInterp`'s raw-text scanner is an entirely separate
# code path from the token-level parser this fix touches, and doesn't
# attempt to recognize a subscript following `$$word` at all.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — reading an array element via $$aref[i] ──
{
    my $aref = [10, 20, 30];
    check('array_deref_subscript_read', $$aref[1] == 20);
}

# ── Section 2: the hash equivalent — $$href{key} ───────────────────────────
{
    my $href = {a => 1, b => 2};
    check('hash_deref_subscript_read', $$href{a} == 1);
}

# ── Section 3: $$aref[i] as an lvalue — assignment through the shorthand ──
{
    my $aref = [10, 20, 30];
    $$aref[1] = 99;
    check('array_deref_subscript_write', join(",", @$aref) eq "10,99,30");
}

# ── Section 4: $$href{key} as an lvalue ────────────────────────────────────
# (built via concatenation, not string interpolation of $$href{$k} — that
# combination is a separate, still-open, deeper gap; see D59's notes)
{
    my $href = {a => 1};
    $$href{b} = 2;
    my @pairs;
    for my $k (sort keys %$href) { push @pairs, $k . "=" . $$href{$k}; }
    check('hash_deref_subscript_write', join(",", @pairs) eq "a=1,b=2");
}

# ── Section 5: nested chained subscripts — $$aoa[i][j] (array of arrays) ──
{
    my $aoa = [[1, 2], [3, 4]];
    check('nested_array_deref_subscript', $$aoa[1][0] == 3);
}

# ── Section 6: nested chained subscripts — $$hoh{k1}{k2} (hash of hashes) ─
{
    my $hoh = {x => {y => 5}};
    check('nested_hash_deref_subscript', $$hoh{x}{y} == 5);
}

# ── Section 7: regression — the genuinely different double-deref form, ────
# ── $$ref->[idx] (explicit arrow), is unaffected by this fix ──────────────
{
    my $inner = [100, 200];
    my $refref = \$inner;
    check('explicit_arrow_double_deref_regression', $$refref->[0] == 100);
}

# ── Section 8: regression — a plain, un-subscripted $$ref deref still ─────
# ── works correctly ──────────────────────────────────────────────────────────
{
    my $x = 42;
    my $ref = \$x;
    check('plain_deref_no_subscript_regression', $$ref == 42);
}

# ── Section 9: regression — ordinary $name[idx]/$name{key} (no $$ at all) ─
# ── is unaffected ────────────────────────────────────────────────────────────
{
    my @arr = (7, 8, 9);
    my %h = (k => "v");
    check('plain_array_index_regression', $arr[1] == 8);
    check('plain_hash_key_regression', $h{k} eq "v");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "deref_subscript_tests_done\n";
