#!/usr/bin/perl
# D208 (2026-11-07): write-back aliasing coherence for packed rows.
# Push/OOB-growth/list-view mutation paths converge on one canonical boxed
# array so every alias observes every write (the perl ref-alias contract).
use strict; use warnings;

sub mk { my @r = (1, 2, 3, 4); return \@r; }

# shared OOB growth: alias sees the grow + the undef gaps + the value
my $r2 = mk();
my $a2 = $r2;
$r2->[7] = 77;
print "shared-oob: ", scalar(@$a2), " last=", $a2->[7],
      " gap=", (defined $a2->[5] ? "DEF" : "U"), "\n";

# in-bounds write-back through the alias
$r2->[1] = 42;
print "wb: a21=$a2->[1]\n";

# push through an alias: BOTH handles see the append
my $r3 = mk();
my $b3 = $r3;
push @$b3, 9;
print "push: r3len=", scalar(@$r3), " r3last=", $r3->[4],
      " b3len=", scalar(@$b3), "\n";

# list view AFTER a push stays one identity
my @snap = @$r3;
print "view-after-push: ", scalar(@snap), " last=", $snap[4], "\n";

# aliased rows through a container literal + 2D numeric store coherence
my @rows = (mk(), mk());
my $alias0 = $rows[0];
$alias0->[0] = 500;
print "container: rows00=$rows[0][0] alias=$alias0->[0]\n";

# exclusively-owned row keeps in-place conversion byte-faithfully
my $solo = mk();
$solo->[7] = 99;
print "solo-grow: ", scalar(@$solo), " last=", $solo->[7],
      " gap=", (defined $solo->[5] ? "DEF" : "U"), "\n";

# dclone of a packed row is INDEPENDENT + value-faithful
use Storable qw(dclone);
my $orig = mk();
my $cp   = dclone($orig);
$cp->[2] = 303;
print "dclone: orig=$orig->[2] cp=$cp->[2] len=", scalar(@$cp), "\n";

# hash-cell row + autoviv push coherence
my %h;
$h{k} = mk();
my $hr = $h{k};
push @$hr, 50;
print "hash-cell: hlen=", scalar(@{$h{k}}), " last=", $h{k}[4], "\n";
