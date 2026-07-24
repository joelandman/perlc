#!/usr/bin/perl
# Deep: D81 — delete slice forms.
use strict;

print "=== hash qw ===\n";
{
    my %h = (a => 1, b => 2, c => 3, d => 4);
    my @del = delete @h{qw(a c)};
    print "del=", join(",", @del), " keys=", join(",", sort keys %h), "\n";
}

print "=== hash scalar last ===\n";
{
    my %h = (a => 1, b => 2, c => 3);
    my $last = delete @h{'a', 'b'};
    print "last=$last keys=", join(",", sort keys %h), "\n";
}

print "=== array slice ===\n";
{
    my @a = (10, 20, 30, 40, 50);
    my @d = delete @a[1, 4];
    print "del=", join(",", @d), " n=", scalar(@a), "\n";
    print "a0=$a[0] a2=$a[2] a3=$a[3]\n";
}

print "=== single array trailing shrink ===\n";
{
    my @b = (1, 2, 3);
    delete $b[2];
    print "b=", join(",", @b), " n=", scalar(@b), "\n";
}

print "=== paren form ===\n";
{
    my %g = (x => 1, y => 2, z => 3);
    delete(@g{qw(x y)});
    print "gkeys=", join(",", sort keys %g), "\n";
}

print "=== single hash still works ===\n";
{
    my %h = (a => 1, b => 2);
    my $v = delete $h{a};
    print "v=$v keys=", join(",", sort keys %h), "\n";
}

print "d81_delete_slice_done\n";
