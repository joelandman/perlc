use strict;
use warnings;

my ($y, $m, $d) = (gmtime(0))[5, 4, 3];
print "ymd=$y,$m,$d\n";

my $one = (gmtime(0))[5];
print "one=$one\n";

my $last = (gmtime(0))[5, 4, 3];
print "last=$last\n";

my @n = (10, 20, 30, 40)[2, 0];
print "n=", join(",", @n), "\n";

my $r = [10, 20, 30, 40];
my $n = 0;
sub bump { $n++; 3 }
my $s = $r->[bump(), 1];
print "s=$s n=$n\n";
