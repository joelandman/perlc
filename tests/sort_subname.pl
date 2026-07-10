#!/usr/bin/perl
# In-depth test suite for `sort SUBNAME LIST` (named comparator sub, no
# braces).
#
# Root cause: the parser's `sort` handling had no notion of a bareword
# comparator sub name at all. `sort by_name @words` — a bareword IDENT
# immediately followed by `@words`, with no `{ }`/`(`/`keys`/`values`
# preceding it — matched none of the recognized argument shapes and
# silently produced an empty result (before the D22 fix) or misparsed the
# bareword as a plain string/expression, leaving @words disconnected from
# the sort entirely (after D22, before this fix).
#
# Fixed with two changes: the parser now detects a leading bareword IDENT
# (not immediately followed by `,`/`=>`/`;`/EOF) as SUBNAME, consumes it,
# and lets the existing keys/values/@arr/(list)/qw/general-expression
# dispatch parse whatever follows as the LIST. Codegen gained a new
# "subname" sort mode: it generates a comparator wrapper function (same
# shape as the `sort { BLOCK }` case) that, instead of inlining a block
# body, assigns its two arguments into file-scope global $a/$b cells and
# calls the named sub with no arguments — matching how a separately-
# compiled named sub can only see $a/$b as Perl's actual package globals,
# not as locals the way an inlined block comparator can.
#
# Important disambiguation, confirmed directly against real Perl (not
# guessed): `sort BAREWORD(...)` is SUBNAME + a parenthesized LIST, not
# "call the bareword as a function and sort its result" — `sort
# get_nums()` sorts an EMPTY list in real Perl (SUBNAME "get_nums" + the
# empty list `()`), while `sort get_nums(9,9)` sorts the 2-element list
# `(9,9)` using get_nums as the comparator. Both are exercised below.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub by_name       { $a cmp $b }
sub by_name_desc  { $b cmp $a }
sub by_num        { $a <=> $b }
sub by_num_desc   { $b <=> $a }
sub get_four       { return (5, 2, 8, 1); }

# ── Section 1: the original bug — string comparator over an array ──────────
{
    my @w = ("banana", "apple", "cherry");
    my @s = sort by_name @w;
    check('subname_string_ascending', join(",", @s) eq "apple,banana,cherry");
}

# ── Section 2: descending string comparator ─────────────────────────────────
{
    my @w = ("banana", "apple", "cherry");
    my @s = sort by_name_desc @w;
    check('subname_string_descending', join(",", @s) eq "cherry,banana,apple");
}

# ── Section 3: numeric comparator ───────────────────────────────────────────
{
    my @n = (5, 2, 8, 1);
    my @s = sort by_num @n;
    check('subname_numeric_ascending', join(",", @s) eq "1,2,5,8");
}

# ── Section 4: descending numeric comparator ────────────────────────────────
{
    my @n = (5, 2, 8, 1);
    my @s = sort by_num_desc @n;
    check('subname_numeric_descending', join(",", @s) eq "8,5,2,1");
}

# ── Section 5: SUBNAME with a parenthesized, non-empty list ─────────────────
{
    my @s = sort by_num (5, 2, 8, 1);
    check('subname_parenthesized_list', join(",", @s) eq "1,2,5,8");
}

# ── Section 6: SUBNAME with an empty parenthesized list — SUBNAME + (), NOT ─
# ── "call the bareword as a function" (confirmed against real Perl) ────────
{
    my @s = sort get_four();
    check('subname_empty_parens_is_empty_list', scalar(@s) == 0);
}

# ── Section 7: SUBNAME with keys %h ─────────────────────────────────────────
{
    my %h = (b => 1, a => 2, c => 3);
    my @s = sort by_name keys %h;
    check('subname_with_keys', join(",", @s) eq "a,b,c");
}

# ── Section 8: SUBNAME over an empty array — empty result, no crash ────────
{
    my @e = ();
    my @s = sort by_num @e;
    check('subname_empty_array', scalar(@s) == 0);
}

# ── Section 9: SUBNAME over a single-element array ──────────────────────────
{
    my @o = (42);
    my @s = sort by_num @o;
    check('subname_single_element', join(",", @s) eq "42");
}

# ── Section 10: regressions — block comparator and default sort unaffected ─
{
    my @n = (5, 2, 8, 1);
    my @s = sort { $a <=> $b } @n;
    check('block_comparator_regression', join(",", @s) eq "1,2,5,8");
}
{
    my @w = ("banana", "apple", "cherry");
    my @s = sort @w;
    check('default_sort_regression', join(",", @s) eq "apple,banana,cherry");
}

# ── Section 11: regression — sort of a grep result (D22) still works ───────
{
    my @a = (5, 3, 8, 1, 9, 2);
    my @s = sort grep { $_ > 2 } @a;
    check('sort_grep_regression', join(",", @s) eq "3,5,8,9");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_subname_tests_done\n";
