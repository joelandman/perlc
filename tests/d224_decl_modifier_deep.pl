#!/usr/bin/perl
# D224: conditional my inside a sub stays in scope when the modifier
# is false; array form; unless/if both directions.
use strict;
use warnings;
our $NL = "X" unless defined $NL;
print "nl=$NL\n";
sub f {
    my $a = 1 if 0;
    print defined $a ? "sa=$a\n" : "sa-undef\n";
    my $b = 2 if 1;
    print "sb=$b\n";
    my $c = 3 unless 0;
    print "sc=$c\n";
    my @xs = (4, 5) if 1;
    print "xs=@xs\n";
}
f();
$NL = "Z" unless defined $NL;
print "nl2=$NL\n";
