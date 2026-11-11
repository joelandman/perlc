use strict;
use warnings;
print "paren=", (1 .5), "\n";
print "spaced=", (1 . 5), "\n";
print "add=", (1 + .5), "\n";
print "mul=", (2 * .5), "\n";
print "exp=", .5e1, "\n";
print "us=", .5_0, "\n";
my $r = sub { return .5 };
print "ret=", $r->(), "\n";
print "sayish=", sprintf("%.1f", .2), "\n";
my @pad = (
    0, 200, 133.3, 90.9, 74.3, 66.7, 50, 33.3,
    16.7, 8.3, 5.5, 4.1, 2, 1, .5, .2
);
print "last=", $pad[-1], " prev=", $pad[-2], " n=", scalar(@pad), "\n";
