use strict;
use warnings;
# D256: scalar splice returns the last removed element.
my @d = (4, 5, 6);
my $last = splice @d, 1, 1;
print "last=$last dn=", scalar(@d), " d=", join(",", @d), "\n";
my @all = (1, 2, 3);
my $end = splice @all;
print "end=", defined($end) ? $end : "U", " n=", scalar(@all), "\n";
my @none = ();
my $z = splice @none, 0, 1;
print "z=", defined($z) ? "D" : "U", "\n";
