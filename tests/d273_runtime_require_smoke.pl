#!/usr/bin/perl
# D273: a runtime require runs the file once and dies when it is missing.
use strict;
use warnings;
use lib 'tests/lib';

my $f = 'D273' . 'Run.pm';
my $v = require $f;
print "v=$v n=$D273Run::n hi=", D273Run::hi(), "\n";
my $v2 = require $f;
print "v2=$v2 n2=$D273Run::n\n";

my $miss = eval { require No::Such::ModXYZ273 };
print "miss=", (defined $miss ? $miss : "U"),
      " loc=", ($@ =~ /Can't locate No\/Such\/ModXYZ273\.pm/ ? 1 : 0), "\n";

my $ff = 'D273' . 'False.pm';
my $fr = eval { require $ff };
print "false=", (defined $fr ? $fr : "U"),
      " msg=", ($@ =~ /D273False\.pm did not return a true value/ ? 1 : 0), "\n";
