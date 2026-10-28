#!/usr/bin/perl
# Regression guard (perlc D191) for a trailing comma with no following
# semicolon as the LAST statement in a block:
#   push @x, "a", "b",     # <- trailing comma, block closes here
#   ...                    # (legal Perl — a block's final statement never
#                           needs a trailing ';')
# Root cause: the D191 `consumeTrailingComma()` helper (src/parser.cpp) was
# returning a bool but never actually advancing past the comma, so the comma
# was left dangling and the next parseStmt choked on it. Output must match
# real perl byte-for-byte.
use strict;
use warnings;

sub check { my ($name, $ok) = @_; print $name, "=", ($ok ? "ok" : "FAIL"), "\n"; }

my @x;
{
    push @x, "a", "b",
}
check('push_trailing_comma', "@x" eq "a b");

my @z;
{
    $z[0] = 1,
    $z[1] = 2,
}
check('elem_assign_trailing_comma', "@z" eq "1 2");

check('d191_trailing_comma_done', 1);
