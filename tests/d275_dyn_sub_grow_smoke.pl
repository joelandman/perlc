#!/usr/bin/perl
# D275: more than 256 computed-glob subs stay callable.
use strict;
use warnings;
no strict 'refs';
no strict 'subs';

for my $i (1 .. 300) {
    my $k = $i;
    *{"d$k"} = sub { return $k };
}
print "d1=", d1(), "\n";
print "d300=", d300(), "\n";
