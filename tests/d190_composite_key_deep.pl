#!/usr/bin/perl
# Deep regression guard (perlc D190) for the composite-key hash subscript,
# exercising the shapes a real program actually hits: the
# /usr/bin/callgrind_annotate `$compressed{$context,$index}` idiom, a write
# through a composite key read back separately, a composite key used as the
# LHS of an indirect write, and two composite keys that must not collide. All
# output must match real perl byte-for-byte.
use strict;
use warnings;

sub check { my ($name, $ok) = @_; print $name, "=", ($ok ? "ok" : "FAIL"), "\n"; }

# callgrind_annotate shape: composite index into a hash.
my %compressed;
for my $ctx (qw(a b)) {
    for my $idx (0 .. 2) {
        $compressed{$ctx, $idx} = $ctx . $idx;
    }
}
check('composite_lookup', $compressed{"a", 1} eq "a1");
check('composite_lookup_b', $compressed{"b", 2} eq "b2");
check('composite_missing', !defined($compressed{"a", 9}));

# two distinct composite keys must not collide even if their parts overlap
my %h;
$h{"ab", 1} = "first";
$h{"a", "b1"} = "second";
check('distinct_keys', $h{"ab", 1} eq "first" && $h{"a", "b1"} eq "second");

# write through the composite key is visible via a fresh read
$h{"m", "n"} = 100;
check('composite_roundtrip', $h{"m", "n"} == 100);

# a composite key that appears twice with the same parts returns the same slot
my %t;
$t{"k", "v"} = "x";
$t{"k", "v"} = "y";
check('composite_overwrite', $t{"k", "v"} eq "y");

print "d190_composite_key_done\n";
