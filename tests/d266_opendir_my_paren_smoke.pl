# D266: opendir my ($dh), DIR declares the handle.
use strict;
use warnings;
my $dir = ".";
opendir my ($dh), $dir or die "open: $!";
my $n = 0;
while (readdir $dh) { $n++; }
closedir $dh;
print "n=", ($n > 0 ? 1 : 0), "\n";
