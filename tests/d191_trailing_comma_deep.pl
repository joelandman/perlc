#!/usr/bin/perl
# Deep regression guard (perlc D191) for a trailing comma with no following
# semicolon as the LAST statement in a block, exercising the shapes that
# byte-match real perl in perlc: a trailing comma after a list operator
# (the last statement of a sub and inside a nested bare block), after element
# assignments, and after a bareword print. The trailing comma must be the sole
# last statement (followed by `}`, `)`, `]`, or EOF). Root cause was D191's
# `consumeTrailingComma()` not advancing past the comma, and `parsePrint`/
# `parseSay` never dropping the trailing-comma guard that `parsePush`/
# `parseUnshift` have; every shape here must match real perl byte-for-byte.
use strict;
use warnings;

sub check { my ($name, $ok) = @_; print $name, "=", ($ok ? "ok" : "FAIL"), "\n"; }

my @items;

# trailing comma after a list operator that is the last statement of a sub
sub push_last {
    push @items, 1, 2, 3,
}
push_last();
check('push_last_stmt_trailing', scalar(@items) == 3);

# trailing comma after a list operator in a nested bare block, before more code
{
    push @items, 4,
}
check('push_nested_trailing', scalar(@items) == 4);

# element assignments, each with a trailing comma, last in a bare block
{
    $items[4] = 5,
    $items[5] = 6,
}
check('elem_assign_trailing', "@items" eq "1 2 3 4 5 6");

# trailing comma after a bareword print (D191: parsePrint arg loop guard)
sub print_last {
    print "tail",
}
print_last();
check('print_trailing_comma', 1);

# a trailing comma before the closing `]`/`)` of a closing delimiter
my @row;
{
    push @row, 10, 20, 30,
}
check('push_nested_delim_trailing', "@row" eq "10 20 30");

check('d191_trailing_comma_done', 1);
