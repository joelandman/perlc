#!/usr/bin/perl
# D235: s///r returns the copy and does not modify the original.
use strict;
use warnings;
my $layer = "Foo::Bar::Baz";
my $p = ($layer =~ s{::}{/}gr) . ".pm";
print "p=$p\n";
print "layer=$layer\n";
my $s = "ababa";
my $t = $s =~ s/a/X/gr;
print "t=$t s=$s\n";
my $none = $s =~ s/z/Q/r;
print "none=$none s=$s\n";
