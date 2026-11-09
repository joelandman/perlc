#!/usr/bin/perl
# D237: can() prefers a dyn closure over a static sub, and a child
# without the method finds the parent's closure.
use strict;
use warnings;
package P;
sub greet { return "static" }
no warnings 'redefine';
*greet = sub { return "dyn" };
package C;
our @ISA = ('P');
sub other { return "own" }
package main;
my $c = bless {}, 'C';
my $p = bless {}, 'P';
print "c=", $c->can('greet') ? $c->greet : "none", "\n";
print "p=", $p->can('greet') ? $p->greet : "none", "\n";
print "o=", $c->can('other') ? $c->other : "none", "\n";
print defined($p->can('other')) ? "pother\n" : "pno\n";
