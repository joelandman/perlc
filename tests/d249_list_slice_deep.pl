use strict;
use warnings;

sub trio { return (7, 8, 9, 10); }

my ($a, $b) = (trio())[2, 0];
print "call=$a,$b\n";

my ($sec, $min) = (localtime(0))[0, 1];
print "lm=$sec,$min\n";

my $tail = (10, 20, 30)[0, 2];
print "tail=$tail\n";

my @st = (stat("/etc/passwd"))[1, 2, 7];
print "statn=", scalar(@st), " mode=", $st[1], " size=", $st[2], "\n";

my ($p, $q) = (gmtime(0))[5,];
print "trail=$p commaq=", defined($q) ? $q : "U", "\n";

my $empty = ()[0, 1];
print "empty=", defined($empty) ? $empty : "U", "\n";

my @side;
sub once { push @side, 1; return (4, 5, 6); }
my ($x, $z) = (once())[0, 2];
print "once=$x,$z calls=", scalar(@side), "\n";
