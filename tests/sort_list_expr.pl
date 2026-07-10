#!/usr/bin/perl
# In-depth test suite for `sort` over a general list-producing expression.
#
# Root cause: the parser's `sort` handling only recognized a fixed set of
# argument shapes — `sort keys %h`, `sort values %h`, `sort @arr`,
# `sort (LIST)`, `sort qw(...)` — and anything else (grep{}/map{}/reverse/
# function calls/nested sort chains) fell through with the argument list
# (`elems`) never populated, silently becoming `sort()`, an empty list,
# with no parse error and no warning.
#
# Fixed by adding a final fallback: if none of the specific shapes match,
# parse the argument as a single general expression and store it in
# n->left — the same slot already used for `sort keys %h` / `sort @arr`.
# No codegen changes were needed: emitArrayPtr() already has cases for
# GrepFunc/MapFunc/Call/ReverseFunc/etc. that correctly turn any of those
# into a PerlArray* for sorting.
#
# NOTE: `sort BAREWORD(args)` (e.g. `sort get_nums()`) is deliberately NOT
# exercised here — real Perl itself treats a bareword immediately followed
# by `(` in sort's argument position as the `sort SUBNAME LIST` comparator
# form (a separate, genuinely ambiguous grammar case — TESTS.md D42, not
# yet fixed), not "call it and sort the result." Confirmed by testing
# against real Perl directly: `sort get_nums()` behaves unlike a plain
# function call there too, so it's out of scope for this fix.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — sort of a grep result ────────────────────
{
    my @a = (5, 3, 8, 1, 9, 2);
    my @s = sort grep { $_ > 2 } @a;
    check('sort_grep_result', join(",", @s) eq "3,5,8,9");
}

# ── Section 2: sort of a map result ─────────────────────────────────────────
{
    my @a = (3, 1, 2);
    my @s = sort map { $_ * 2 } @a;
    check('sort_map_result', join(",", @s) eq "2,4,6");
}

# ── Section 3: sort of a reverse result ─────────────────────────────────────
{
    my @a = (1, 2, 3);
    my @s = sort reverse @a;
    check('sort_reverse_result', join(",", @s) eq "1,2,3");
}

# ── Section 4: custom comparator block combined with a grep argument ───────
{
    my @a = (5, 3, 8, 1, 9, 2);
    my @s = sort { $b <=> $a } grep { $_ > 2 } @a;
    check('custom_block_with_grep', join(",", @s) eq "9,8,5,3");
}

# ── Section 5: nested sort/map chain as the argument ────────────────────────
{
    my @a = (3, 1, 2);
    my @s = sort map { $_ + 100 } sort @a;
    check('nested_sort_map_chain', join(",", @s) eq "101,102,103");
}

# ── Section 6: numeric comparator with grep filtering out a value ──────────
{
    my @a = (3, 1, 2, 9, 7);
    my @s = sort { $a <=> $b } grep { $_ != 7 } @a;
    check('numeric_comparator_with_grep', join(",", @s) eq "1,2,3,9");
}

# ── Section 7: sort of a map producing strings ──────────────────────────────
{
    my @s = sort map { "$_!" } (3, 1, 2);
    check('sort_map_strings', join(",", @s) eq "1!,2!,3!");
}

# ── Section 8: regression — plain list literal ──────────────────────────────
{
    my @s = sort (3, 1, 2);
    check('plain_list_literal_regression', join(",", @s) eq "1,2,3");
}

# ── Section 9: regression — sort @arr ───────────────────────────────────────
{
    my @a = (3, 1, 2);
    my @s = sort @a;
    check('sort_array_regression', join(",", @s) eq "1,2,3");
}

# ── Section 10: regression — sort keys %h ───────────────────────────────────
{
    my %h = (b => 1, a => 2, c => 3);
    my @s = sort keys %h;
    check('sort_keys_regression', join(",", @s) eq "a,b,c");
}

# ── Section 11: sort-of-grep used directly inside join/print (no intermediate ─
# ── variable — exercises the expression appearing in argument position) ────
{
    my @a = (5, 3, 8, 1);
    my $out = join(",", sort grep { $_ > 2 } @a);
    check('sort_grep_inline_in_join', $out eq "3,5,8");
}

# ── Section 12: sort of an empty grep result — empty list, not an error ────
{
    my @a = (1, 2, 3);
    my @s = sort grep { $_ > 100 } @a;
    check('sort_empty_grep_result', scalar(@s) == 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_list_expr_tests_done\n";
