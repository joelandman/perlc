#!/usr/bin/env perl
# Closed interp/qr match deep: bound 13005 (>= PERLC_AUTOPAR_MIN) takes
# the pool-parallel grep bodies and must stay byte-identical to
# sequential real perl. Do not mention $1 / $& here.
use strict;
use warnings;

my $N = 13005;
my @w;
for my $i (0 .. $N - 1) {
    if ($i % 3 == 0) { push @w, "apple$i" }
    elsif ($i % 3 == 1) { push @w, "pear$i" }
    else { push @w, "plum$i" }
}

my @g = grep { my $p = "app"; /$p/ } @w;
print "glen=", scalar(@g), " g0=$g[0] gend=$g[-1]\n";

my @gi = grep { my $p = "PEAR"; /$p/i } @w;
print "ilen=", scalar(@gi), " i0=$gi[0] iend=$gi[-1]\n";

my @bind = grep { my $p = "plum"; $_ =~ /$p/ } @w;
print "blen=", scalar(@bind), " b0=$bind[0] bend=$bind[-1]\n";

my @qr = grep { my $re = qr/le/; $_ =~ $re } @w;
print "qlen=", scalar(@qr), " q0=$qr[0] qend=$qr[-1]\n";

print "done\n";
