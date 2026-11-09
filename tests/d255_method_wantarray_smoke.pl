use strict;
use warnings;
# D255: a method's return() in list context is an empty list.
package P255;
sub empty { return() }
sub pair { return (1, 2) }
sub bare { return }
package main;
my $o = bless {}, "P255";
my @e = $o->empty();
print "empty=", scalar(@e), "\n";
my @p = $o->pair();
print "pair=", join(",", @p), " n=", scalar(@p), "\n";
my @b = $o->bare();
print "bare=", scalar(@b), "\n";
my $s = $o->empty();
print "scalar=", defined($s) ? "D" : "U", "\n";
my $last = $o->pair();
print "last=$last\n";
