#!/usr/bin/perl
# D272: pre/post decrement, numify, empty slot, scalar deref.
use strict;
use warnings;

my $obj = bless { n => "3" }, "O";
print "num=", $obj->{n}++, " now=$obj->{n}\n";
$obj->{n}--;
print "dec=$obj->{n}\n";
my $pd = $obj->{n}--;
print "pd=$pd now=$obj->{n}\n";

my $h = { a => 5 };
my $old = $h->{a}--;
print "old=$old ha=$h->{a}\n";
print "pre=", --$h->{a}, " ha=$h->{a}\n";

my @a;
$a[0]++;
print "empty=$a[0]\n";

my $x = 1;
my $r = \$x;
$$r++;
print "dr=$$r\n";

my @b = (7);
my $pb = $b[0]--;
print "pb=$pb b=$b[0]\n";

$obj->{n} += 1;
$obj->{n} .= "x";
print "add=$obj->{n}\n";
