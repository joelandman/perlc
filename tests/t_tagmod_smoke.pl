#!/usr/bin/perl
use strict;
use warnings;
use lib 'tests/lib';
use TagMod qw(:colors :math);

print join(",", (red, green, blue)), "\n";
print scale(21), "\n";
