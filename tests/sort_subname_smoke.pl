#!/usr/bin/perl
# Smoke test for `sort SUBNAME LIST` (D42: perlc silently produced an
# empty/wrong result — the parser had no notion of a bareword sort
# comparator at all, so `sort by_name @words` either dropped @words
# entirely or, after the D22 fix, misparsed `by_name` as an ordinary
# expression instead of a comparator sub name).
# Fast, narrow coverage — see sort_subname.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub by_name { $a cmp $b }
sub by_num  { $a <=> $b }

# The original bug: named string comparator over an array.
{
    my @w = ("banana", "apple", "cherry");
    my @s = sort by_name @w;
    check('smoke_sort_subname_string', join(",", @s) eq "apple,banana,cherry");
}

# Named numeric comparator.
{
    my @n = (5, 2, 8, 1);
    my @s = sort by_num @n;
    check('smoke_sort_subname_numeric', join(",", @s) eq "1,2,5,8");
}

# Block comparator still works — regression check.
{
    my @n = (5, 2, 8, 1);
    my @s = sort { $a <=> $b } @n;
    check('smoke_block_comparator_regression', join(",", @s) eq "1,2,5,8");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_subname_smoke_done\n";
