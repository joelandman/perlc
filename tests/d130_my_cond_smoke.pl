#!/usr/bin/perl
# Smoke test for D130: `if (my @arr = EXPR)` / `if (my %h = EXPR)` —
# a single array/hash variable declared inline (no parens) as an
# if/while condition was a hard parser error.
use strict;
use warnings;

sub pair { return (1, 2); }

if (my @r = pair()) {
    print "yes:@r\n";
} else {
    print "no\n";
}
