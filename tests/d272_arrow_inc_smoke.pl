#!/usr/bin/perl
# D272: ++/-- on an arrow element mutates that element.
use strict;
use warnings;

my $obj = bless { n => 3 }, "O";
my $post = $obj->{n}++;
print "post=$post n=$obj->{n}\n";
my $pre = ++$obj->{n};
print "pre=$pre n=$obj->{n}\n";

my $h = { a => 2 };
$h->{a}++;
print "ha=$h->{a}\n";

my $row = [9];
$row->[0]++;
my $pre2 = ++$row->[0];
print "row=$row->[0] pre2=$pre2\n";

my %hh = (k => 4);
$hh{k}++;
print "hk=$hh{k}\n";
