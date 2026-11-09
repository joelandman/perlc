#!/usr/bin/perl
# D225: stdout printf, parenthesized printf, and a false short-circuit
# that must not print.
use strict;
use warnings;
sub plain {
    return printf "xy %d\n", 7;
}
sub paren {
    return printf("z\n");
}
my $a = plain();
my $b = paren();
print "a=$a b=$b\n";
my $c = 0;
$c and printf "nope\n";
print "end\n";
