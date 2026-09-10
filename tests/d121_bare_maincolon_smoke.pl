#!/usr/bin/perl
# Smoke test for D121: $::name (bare :: prefix, shorthand for
# $main::name) must not be a parse error.
use strict;
use warnings;

$::x = 5;
print $::x, "\n";
