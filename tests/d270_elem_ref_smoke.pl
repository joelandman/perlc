#!/usr/bin/perl
# D270: a scalar ref to a hash or array element aliases that element.
use strict;
use warnings;

my $self = bless {}, "P";
$self->{output} = "OLD";
my $r = \$self->{output};
$$r = "NEW";
print "slot=$self->{output}\n";

my %h = (k => "A");
my $hr = \$h{k};
$$hr .= "B";
print "h=$h{k}\n";

my @a = ("x");
my $ar = \$a[0];
$$ar = "y";
print "a=$a[0]\n";
