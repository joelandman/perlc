#!/usr/bin/perl
# Smoke test for D117: implicit string->number coercion used a
# hand-rolled parser (manual digit accumulation, repeated-multiply
# exponent loop) instead of strtod, accumulating rounding error.
use strict;
use warnings;

print "2.2250738585072014e-300"+0, "\n";
