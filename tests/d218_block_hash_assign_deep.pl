#!/usr/bin/perl
# D218: `%name =` block vs `{ k => v }` / `{ %$h }` constructors.
use strict;
use warnings;
my %src;
@src{10, 20, 30} = ("x", "y", "z");
my %out;
{
    %out = ();
    my ($k, $v);
    while (($k, $v) = each %src) {
        $out{$k} = $v . "!";
    }
    my $n = 0;
    $n++ while each %out;
    # each above consumed the iterator; reset by a fresh each-less count
    print "n=", scalar(keys %out), "\n";
}
my @ks = sort { $a <=> $b } keys %out;
print "keys=", join(",", @ks), "\n";
print "vals=", join(",", @out{@ks}), "\n";

my @refs = map { { k => $_ } } (1, 2);
print "mapfat=", ref($refs[0]), " ", $refs[0]{k}, " ", ref($refs[1]), " ", $refs[1]{k}, "\n";

my $h = { a => 7, b => 8 };
my @inner = map { { %$h } } (1);
print "inner=", ref($inner[0]), "\n";
my @ik = sort keys %{$inner[0]};
print "ik=", join(",", @ik), "\n";
print "iv=", join(",", @{$inner[0]}{@ik}), "\n";

my $empty = {};
print "empty=", ref($empty), " ", scalar(keys %$empty), "\n";
print "done\n";
