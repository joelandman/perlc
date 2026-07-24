#!/usr/bin/perl
# Deep: D41 — local on hash/array elements.
use strict;

print "=== hash assign ===\n";
{
    my %h = (a => 1, b => 2);
    { local $h{a} = 9; print "in:$h{a}\n"; }
    print "out:$h{a}\n";
}

print "=== array assign ===\n";
{
    my @a = (1, 2, 3);
    { local $a[1] = 99; print "in:$a[1]\n"; }
    print "out:$a[1]\n";
}

print "=== nested ===\n";
{
    my %h = (b => 2);
    { local $h{b} = 7; { local $h{b} = 8; print "n2:$h{b}\n"; } print "n1:$h{b}\n"; }
    print "n0:$h{b}\n";
}

print "=== bare local undefs ===\n";
{
    my %h = (a => 1);
    { local $h{a}; print "cleared:", (defined $h{a} ? "no" : "yes"), "\n"; }
    print "restored:$h{a}\n";
}

print "d41_local_elem_done\n";
