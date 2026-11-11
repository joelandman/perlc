#!/usr/bin/perl
# D270: element refs stay aliased through a sub, an arrow, and appends.
use strict;
use warnings;

sub take {
    my ($self) = @_;
    my $r = \$self->{output};
    $$r = "" unless defined $$r;
    $$r .= "Hello";
    $$r .= " ";
    $$r .= "page";
    return $r;
}

my $obj = bless {}, "Q";
my $got = take($obj);
print "via=$$got obj=$obj->{output}\n";

my $h = { row => [10] };
my $er = \$h->{row}->[0];
$$er += 5;
print "row=$h->{row}->[0]\n";

my @xs = (1, 2);
my $xr = \$xs[1];
$$xr = 9;
print "xs=$xs[0],$xs[1]\n";

my $missing = bless {}, "M";
my $mr = \$missing->{k};
$$mr = "born";
print "born=$missing->{k}\n";
