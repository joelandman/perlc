#!/usr/bin/perl
# D232: delete $$ref keeps other keys, returns the value, and the
# arrow form $$ref->{key} is the extra scalar deref.
use strict;
use warnings;
my $h = { opt_stderr => "E", opt_utf8 => "U", errors => "X", keep => "K" };
print "s=", delete $$h{opt_stderr}, "\n";
print "u=", delete $$h{opt_utf8}, "\n";
my $gone = delete $$h{errors};
print defined($gone) ? "e=$gone\n" : "eundef\n";
print "keep=$$h{keep}\n";
print defined($$h{opt_stderr}) ? "still\n" : "cleared\n";
my $inner = { b => 9 };
my $ref = \$inner;
print "b=", delete $$ref->{b}, "\n";
print defined($inner->{b}) ? "inner\n" : "innergone\n";
my $arr = ["p", "q", "r"];
print "q=", delete $$arr[1], "\n";
my $n = 0;
$n++ for @$arr;
print "n=$n\n";
print join(",", map { defined($_) ? $_ : "U" } @$arr), "\n";
my $none = delete $$h{absent};
print defined($none) ? "bad\n" : "ok\n";
