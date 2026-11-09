#!/usr/bin/perl
# D224: `our $x = INIT unless COND` declares always and assigns only
# when the modifier says so.
use strict;
use warnings;
our $NL = "X" unless defined $NL;
print "[$NL]\n";
$NL = "Y" unless defined $NL;
print "[$NL]\n";
