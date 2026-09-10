#!/usr/bin/perl
# Smoke test for D99: `my @b = @a;` aliased @a's storage instead of copying
# it — mutating @b (or pushing/popping it) silently corrupted @a too.
use strict;
use warnings;

my @a = (1, 2, 3);
my @b = @a;
$b[0] = 99;
print "a=@a b=@b\n";
print(($a[0] == 1 && $b[0] == 99) ? "ok\n" : "FAIL\n");
