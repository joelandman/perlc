#!/usr/bin/perl
# D234: require EXPR loads a computed filename.
use strict;
use warnings;
my $f = "/tmp/pt_d234_req_$$.pm";
open my $fh, ">", $f or die "open: $!\n";
print $fh "1;\n";
close $fh;
my $r = require $f;
print "r=$r\n";
unlink $f;
