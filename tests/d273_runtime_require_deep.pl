#!/usr/bin/perl
# D273: an inlined require runs once, a dying file fails the eval,
# and a string eval of a missing require sets $@.
use strict;
use warnings;
use lib 'tests/lib';

require D273Twice;
print "twice=$D273Twice::n\n";
require D273Twice;
print "twice2=$D273Twice::n\n";

my $fd = 'D273' . 'Die.pm';
my $dr = eval { require $fd };
print "die=", (defined $dr ? "set" : "U"),
      " boom=", ($@ =~ /boom273/ ? 1 : 0), "\n";

my $ev = eval "require No::Such::ModXYZ273; 1";
print "ev=", (defined $ev ? $ev : "undef"),
      " at=", ($@ =~ /Can't locate No\/Such\/ModXYZ273\.pm/ ? 1 : 0), "\n";

my $f = 'D273' . 'Run.pm';
my $v = require $f;
print "run=$v n=$D273Run::n\n";
my $v2 = require $f;
print "run2=$v2 n2=$D273Run::n\n";
