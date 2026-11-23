#!/usr/bin/env perl
# Nested-lexical capture deep: bound 13005 (>= PERLC_AUTOPAR_MIN)
# takes the pool-parallel map/grep bodies and must stay byte-identical
# to sequential real perl. Do not mention $1 / $& here.
use strict;
use warnings;

my $N = 13005;
my @w;
for my $i (0 .. $N - 1) {
    if ($i % 3 == 0) { push @w, "apple$i" }
    elsif ($i % 3 == 1) { push @w, "pear$i" }
    else { push @w, "plum$i" }
}

sub gpat {
    my $p = "app";
    return grep { /$p/ } @_;
}
my @g = gpat(@w);
print "glen=", scalar(@g), " g0=$g[0] gend=$g[-1]\n";

sub mapk {
    my $k = 1;
    return map { length($_) + $k } @_;
}
my @m = mapk(@w);
print "m0=$m[0] m1=$m[1] mend=$m[-1]\n";

sub ufmt {
    my $fmt = "A*";
    return map { unpack($fmt, $_) } @_;
}
my @u = ufmt(@w);
print "ulen=", scalar(@u), " u0=$u[0] uend=$u[-1]\n";

sub qre {
    my $re = qr/plum/;
    return grep { $_ =~ $re } @_;
}
my @qr = qre(@w);
print "qlen=", scalar(@qr), " q0=$qr[0] qend=$qr[-1]\n";

print "done\n";
