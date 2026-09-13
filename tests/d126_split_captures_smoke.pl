#!/usr/bin/perl
# Smoke test for D126: a split() pattern with a capturing group must
# interleave each capture's text between the surrounding fields the way
# real Perl does — split(/(,)/, "a,b,c") is ("a", ",", "b", ",", "c"),
# 5 elements, not ("a","b","c").
use strict;
use warnings;

my @p = split(/(,)/, "a,b,c");
print scalar(@p), "\n";
print join("|", @p), "\n";