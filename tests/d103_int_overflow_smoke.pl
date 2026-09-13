#!/usr/bin/perl
# Smoke test for D103: integer arithmetic overflowing 64-bit signed range
# must auto-promote (exact UV, or NV beyond that) instead of silently
# wrapping.
use strict;
use warnings;

my $big = 9223372036854775807;
print $big + 1, "\n";
