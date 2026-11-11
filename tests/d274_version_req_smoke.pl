#!/usr/bin/perl
# D274: $class->VERSION($need) checks $class::VERSION.
use strict;
use warnings;

package F;
our $VERSION = 1.2;
print "noarg=", F->VERSION, "\n";
my $eq = eval { F->VERSION(1.2) };
print "eq=$eq\n";
my $lo = eval { F->VERSION(1) };
print "lo=$lo\n";
eval { F->VERSION(2) };
my $hi = $@;
$hi =~ s/ at .*//;
print "hi=$hi";

package H;
eval { H->VERSION(1) };
my $none = $@;
$none =~ s/ at .*//;
print "none=$none";
