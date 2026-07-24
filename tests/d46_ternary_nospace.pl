#!/usr/bin/perl
# Deep: D46 — no-space ternary
my $x = 1; my $y = 2;
print "spaced=", ($x ? $y : "str"), "\n";
print "nosp1=", ($x?$y:"str"), "\n";
print "nosp2=", (defined($x)?$x:"nope"), "\n";
print "nosp3=", ($x?1:0), "\n";
print "nosp4=", (0?$y:"z"), "\n";
print "nested=", ($x?$y?3:4:5), "\n";
print "d46_ternary_nospace_done\n";
