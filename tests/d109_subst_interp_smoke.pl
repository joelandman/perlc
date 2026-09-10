#!/usr/bin/perl
# Smoke test for D109: s/PATTERN/REPLACEMENT/'s REPLACEMENT text must
# support arbitrary variable interpolation ($name, @arr), not just
# $1-$9/$& capture refs.
use strict;
use warnings;

my $name = "World";
my $s = "hello X";
$s =~ s/X/$name/;
print "$s\n";
