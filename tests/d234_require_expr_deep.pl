#!/usr/bin/perl
# D234: require of a concatenation. A missing file is false inside eval.
use strict;
use warnings;
my $dir = "/tmp/pt_d234_$$";
mkdir $dir;
my $base = "$dir/Mod.pm";
open my $fh, ">", $base or die "open: $!\n";
print $fh "1;\n";
close $fh;
my $name = $dir . "/Mod" . ".pm";
my $r = require $name;
print "r=$r\n";
my $miss = eval { require $dir . "/NoSuch.pm" };
print defined($miss) ? "miss=set\n" : "miss=undef\n";
unlink $base;
rmdir $dir;
