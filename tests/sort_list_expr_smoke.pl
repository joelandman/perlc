#!/usr/bin/perl
# Smoke test for `sort` over a general list-producing expression (D22:
# perlc's parser only recognized `sort keys %h`, `sort values %h`,
# `sort @arr`, `sort (LIST)`, and `sort qw(...)` as sort arguments — any
# other list expression, e.g. `sort grep{...}@arr`, silently fell through
# with the argument list left empty, i.e. `sort()`, with no error).
# Fast, narrow coverage — see sort_list_expr.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: sort of a grep result.
{
    my @a = (5, 3, 8, 1, 9, 2);
    my @s = sort grep { $_ > 2 } @a;
    check('smoke_sort_grep', join(",", @s) eq "3,5,8,9");
}

# sort of a map result.
{
    my @a = (3, 1, 2);
    my @s = sort map { $_ * 2 } @a;
    check('smoke_sort_map', join(",", @s) eq "2,4,6");
}

# sort @arr still works — regression check.
{
    my @a = (3, 1, 2);
    my @s = sort @a;
    check('smoke_sort_array_regression', join(",", @s) eq "1,2,3");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_list_expr_smoke_done\n";
