#!/usr/bin/perl
# Smoke test for D108: plain double-quoted string literals didn't
# recognize \f, \a, \e, \b.
use strict;
use warnings;

print "a\fb\ac\ed\be\n";
