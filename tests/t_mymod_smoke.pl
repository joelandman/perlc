#!/usr/bin/perl
use strict;
use warnings;
use lib 'tests/lib';
use MyMod qw(add greet counter);

print add(2, 3), "\n";
print greet("World"), "\n";
print counter(), "\n";
print counter(), "\n";

# also test fully-qualified call
print MyMod::add(10, 20), "\n";
