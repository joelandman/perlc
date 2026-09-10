#!/usr/bin/perl
# Smoke test for D112: a module's file-scope `my` variable must not
# collide with the main script's same-named `my` variable.
use strict;
use warnings;
use lib 'tests/lib';
use D112Leaky;

my $counter = 7;
print "leaky_get=", D112Leaky::get(), "\n";
print "main_counter=", $counter, "\n";
