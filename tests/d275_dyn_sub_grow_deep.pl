#!/usr/bin/perl
# D275: a slot past the old cap, and a later install replaces the closure.
use strict;
use warnings;
no strict 'refs';
no strict 'subs';
no warnings 'redefine';

for my $i (1 .. 300) {
    my $k = $i;
    *{"d$k"} = sub { return $k };
}
print "d256=", d256(), "\n";
print "d299=", d299(), "\n";
*{"d1"} = sub { return 7 };
print "repl=", d1(), "\n";
print "d300=", d300(), "\n";
