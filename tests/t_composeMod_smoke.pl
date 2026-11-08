#!/usr/bin/perl
use strict;
use warnings;
use lib 'tests/lib';
use ComposeMod qw(process);

print process(20), "\n";
print process(30), "\n";
print ComposeMod::process(10), "\n";
print Helper::doubler(4), "\n";
