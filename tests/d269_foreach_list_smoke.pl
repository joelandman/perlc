#!/usr/bin/perl
# D269: foreach over `@{ $ref }, [0]` is list context. The deref flattens
# and the trailing anon array stays one element.
use strict;
use warnings;
my $to = [[1, "T", "T"]];
my @got;
for my $h (@{ $to }, [0]) {
    my $r = ref($h) || "noref";
    my $z = defined $h->[0] ? $h->[0] : "undef";
    push @got, "$r:$z";
}
print join(",", @got), "\n";
my @a = ("A");
my @b = ("B");
for my $x (@a, @b) { $x .= "1"; }
print "$a[0] $b[0]\n";
