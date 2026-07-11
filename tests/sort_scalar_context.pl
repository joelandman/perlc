#!/usr/bin/perl
# In-depth test suite for `sort` in scalar context.
#
# Root cause (D29): perlc treated SortFunc identically to MapFunc/GrepFunc
# in every scalar-context codegen path — always returning the element
# count via perl_array_len(). Real Perl documents/implements sort()'s
# scalar-context return value as undef (unlike grep/map, which do return
# a count in scalar context). Four codegen sites shared this logic:
#   - emitExpr's direct case for SortFunc/MapFunc/GrepFunc (hit by e.g.
#     `my $x = sort @a;`, a plain scalar declaration)
#   - emitBlockLast's implicit-last-expression-of-block path (hit when a
#     sub's body ends in a bare `sort ...` and the sub's context depends
#     on the caller, i.e. currentSubNeedsWantarray_)
#   - emitBlockLast's captured-`return`-without-emitting-ret path
#   - the main `case NK::Return:` path (explicit `return sort ...;`)
#
# Fixed by splitting SortFunc out of the shared MapFunc/GrepFunc handling
# in all four sites: the first (unconditionally-scalar-context) site now
# returns perlUndef() directly, without even evaluating the list/block —
# matching real Perl's behavior of not evaluating sort's arguments at all
# in scalar context. The three wantarray-dependent sites (which pick
# between list/scalar results at runtime via a `select`, since the
# context isn't known until the caller is reached) now select perlUndef()
# for the scalar branch specifically when the node is a SortFunc, while
# grep/map keep returning perl_array_len() in that branch.
#
# NOT exercised here (accepted, narrow divergence from real Perl, out of
# scope for D29): the three wantarray-dependent sites still eagerly
# evaluate the sort list/comparator block via emitArrayPtr() before
# branching on context, since the runtime `select` needs a value for both
# branches. Real Perl skips evaluating sort's arguments entirely in
# scalar context (confirmed: a sub call used as sort's list arg is never
# invoked, and a sort block's comparator body is never run, when the
# result is discarded into scalar context). Under perlc, in the rare case
# where a sub whose implicit-last-expression or explicit `return` is a
# bare `sort` is called from both list and scalar context call sites (the
# only case that reaches these three sites, since a statically-known
# scalar context already resolves via the first site), the list/block is
# still evaluated for its side effects even when the final return value
# is correctly undef. This does not affect the returned value's
# correctness, only misses a real Perl micro-optimization around
# suppressing evaluation.
# Deliberately no `use warnings` — real Perl's "Useless use of sort in
# scalar context" warning under -w would land on stderr and break the
# byte-for-byte harness comparison; the semantics under test (the return
# value) are unaffected by the warnings pragma.
use strict;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# Named subs are declared at file scope (not nested inside the `{ }` blocks
# below) — a block-scoped named sub declared after a preceding sibling block
# hits an unrelated, pre-existing perlc bug (list-context calls to it return
# an empty list; logged separately as D55, not fixed here to keep this
# defect's fix/test tightly scoped).
sub by_num { $a <=> $b }
sub last_expr_sort { sort @_ }
sub explicit_return_sort { return sort @_; }
sub block_last_expr_sort { sort { $a <=> $b } @_ }

# ── Section 1: the original bug — plain sort assigned to a scalar ──────────
{
    my @a = (3, 1, 2);
    my $x = sort @a;
    check('plain_sort_scalar_is_undef', !defined($x));
}

# ── Section 2: block comparator in scalar context ───────────────────────────
{
    my @a = (3, 1, 2);
    my $x = sort { $a <=> $b } @a;
    check('block_sort_scalar_is_undef', !defined($x));
}

# ── Section 3: named-sub comparator in scalar context ───────────────────────
{
    my @a = (5, 2, 8, 1);
    my $x = sort by_num @a;
    check('subname_sort_scalar_is_undef', !defined($x));
}

# ── Section 4: sort scalar context is falsy in boolean context ─────────────
{
    my @a = (3, 1, 2);
    my $truthy = sort(@a) ? "true" : "false";
    check('sort_scalar_is_falsy', $truthy eq "false");
}

# ── Section 5: implicit last-expression of a sub, called in both list and ──
# ── scalar context (forces runtime wantarray dispatch) ─────────────────────
{
    my @list = last_expr_sort(3, 1, 2);
    my $sc   = last_expr_sort(3, 1, 2);
    check('wantarray_implicit_last_list', join(",", @list) eq "1,2,3");
    check('wantarray_implicit_last_scalar_undef', !defined($sc));
}

# ── Section 6: explicit `return sort ...`, called in both contexts ─────────
{
    my @list = explicit_return_sort(3, 1, 2);
    my $sc   = explicit_return_sort(3, 1, 2);
    check('wantarray_explicit_return_list', join(",", @list) eq "1,2,3");
    check('wantarray_explicit_return_scalar_undef', !defined($sc));
}

# ── Section 7: block comparator, implicit last-expression, both contexts ──
{
    my @list = block_last_expr_sort(5, 2, 8);
    my $sc   = block_last_expr_sort(5, 2, 8);
    check('wantarray_block_last_list', join(",", @list) eq "2,5,8");
    check('wantarray_block_last_scalar_undef', !defined($sc));
}

# ── Section 8: regression — sort in list context is unaffected ─────────────
{
    my @a = (3, 1, 2);
    my @s = sort @a;
    check('sort_list_context_regression', join(",", @s) eq "1,2,3");
}

# ── Section 9: regression — grep/map still return a count in scalar ────────
# ── context (only sort's scalar-context behavior changed) ──────────────────
{
    my @a = (1, 2, 3, 4);
    my $gc = grep { $_ > 2 } @a;
    check('grep_scalar_count_regression', $gc == 2);
    my $mc = map { $_ * 2 } @a;
    check('map_scalar_count_regression', $mc == 4);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_scalar_context_tests_done\n";
