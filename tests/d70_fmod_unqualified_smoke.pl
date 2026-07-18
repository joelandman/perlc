# D70: POSIX::fmod unqualified import smoke test
# (no use warnings to avoid diagnostic differences, D56)
use strict;

# fmod should work both qualified and unqualified
my $r1 = POSIX::fmod(10, 3);
use POSIX qw(fmod);
my $r2 = fmod(10, 3);
die "D70: qualified wrong" unless $r1 == 1;
die "D70: unqualified wrong" unless $r2 == 1;
print "ok\n";
