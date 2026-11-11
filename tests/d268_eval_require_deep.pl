#!/usr/bin/perl
# D268: require-in-eval loads the module once, and a missing module
# still fails the eval. A constant eval with no require stays inline.
use strict;
use warnings;
my $ok = eval "require HTML::Entities; 1";
print "ok=", (defined $ok ? $ok : "undef"), " err=", ($@ eq "" ? "empty" : "set"), "\n";
my $a = HTML::Entities::encode_entities("x<y>");
my $b = HTML::Entities::encode_entities("x<y>", '<>&');
print "a=$a\n";
print "b=$b\n";
print "plain=", eval "40+2", "\n";
print "one=", eval "1", "\n";
