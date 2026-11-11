#!/usr/bin/perl
# D268: a constant eval that requires a module must define that module's subs.
use strict;
use warnings;
my $ok = eval "require HTML::Entities; 1";
print "ok=", (defined $ok ? $ok : "undef"), " err=", ($@ eq "" ? "empty" : "set"), "\n";
print "def=", (defined &HTML::Entities::encode_entities ? 1 : 0), "\n";
my $s = HTML::Entities::encode_entities(q{A<B>&"'});
print "s=$s\n";
print "sum=", eval "2+2", "\n";
