#!/usr/bin/perl
use strict;
use warnings;

my $v = 20;
sub halver { my ($v) = @_; return $v / 2; }
sub doubler { my ($v) = @_; return $v * 2; }

print "fs-same-name doubler(halver(\$v))=", doubler(halver($v)), "\n";
