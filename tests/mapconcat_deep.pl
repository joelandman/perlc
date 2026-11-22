#!/usr/bin/env perl
# Stage 46 map-concat deep: 13005 inputs, two values each, plus flatten.
use strict;
use warnings;

my $N = 13005;
my @a = (0 .. $N - 1);
my @m = map { ($_, $_ + 1) } @a;
print "mlen=", scalar(@m), " m0=$m[0] m1=$m[1] mmid=$m[13000] mend=$m[-1]\n";
my $msum = 0;
$msum += $m[$_] foreach 0, 1, 100, 101, 13000, 13001, 26008, 26009;
print "msum=$msum\n";

my @aoa = map { [$_, $_ * 2] } @a;
my @flat = map { @$_ } @aoa;
print "flen=", scalar(@flat), " f0=$flat[0] f1=$flat[1] fend=$flat[-1]\n";

print "done\n";
