#!/usr/bin/perl
# Smoke: D41 — local $h{key} / local $arr[i]
use strict;

my %h = (a => 1, b => 2);
{ local $h{a} = 9; print "in:$h{a}\n"; }
print "out:$h{a}\n";
my @a = (1, 2, 3);
{ local $a[1] = 99; print "in:$a[1]\n"; }
print "out:$a[1]\n";
print "d41_local_elem_smoke_done\n";
