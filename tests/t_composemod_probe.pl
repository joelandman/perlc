#!/usr/bin/perl
use strict;
use warnings;
use lib 'tests/lib';
use ComposeMod qw(process);
use Helper qw(halver);

print "compose process(20)=", process(20), "\n";
print "direct halver(20)=", halver(20), "\n";
