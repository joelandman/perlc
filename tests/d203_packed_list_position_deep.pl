#!/usr/bin/perl
# D203/D207 deep: packed rows crossing serialization/dumper/json walkers
# must (a) freeze/thaw REF-faithfully with alias-sharing preserved, (b) encode
# their VALUES (element views) without fork-converting shared row PVs in
# place, and (c) grow exclusively-owned rows OOB byte-faithfully (undef gaps).
use strict;
use warnings;
use Storable qw(freeze thaw dclone);
use Data::Dumper;
use JSON::PP qw(encode_json);
$Data::Dumper::Terse = 1;

sub mk { my @r = (1, 2, 3, 4); return \@r; }   # packflat hook -> packed row

# (a) freeze/thaw through a shared alias pair — thawed aliases stay shared refs
my $a = mk();
my $b = $a;                                    # share the backing
my $t = thaw(freeze([$a, $b]));
print "thaw_len=", scalar(@$t), " t0=", join(",", @{$t->[0]}), "\n";
print "thaw_shared=", (ref($t->[0]) eq 'ARRAY' && ref($t->[1]) eq 'ARRAY' ? 'ARRAY' : 'BAD'), "\n";

# dclone of a packed row keeps its values
my $dc = dclone($a);
print "dclone=", join(",", @$dc), "\n";

# (b) dumper/json encode the VALUES (digits only — no addresses, no order)
my $dumped = Dumper($b);
my @digits = $dumped =~ /(\d+)/g;
print "dumper_digits=", join("", @digits), "\n";
my $j = encode_json([$a, $b]);
print "json=$j\n";

# walkers must NOT fork the shared pair: an encode before a write stays coherent
my $j2 = encode_json([$a]);
$b->[0] = 500;
print "after_encode_write: j2=$j2 a0=$a->[0]\n";

# (c) exclusively-owned row grows OOB faithfully (undef gaps)
my $r1 = mk();
my @img = @$r1;
print "grow_before=", scalar(@img), "\n";
$r1->[7] = 99;
my @img2 = @$r1;
print "grow_after=", scalar(@img2), " last=", $r1->[7],
      " gap=", (defined $r1->[5] ? 'DEF' : 'U'), "\n";

# freeze round-trip of the grown row
my $g = thaw(freeze($r1));
print "frozen_grown_len=", scalar(@$g), " gap=", (defined $g->[5] ? 'DEF' : 'U'), "\n";
