#!/usr/bin/perl
# Smoke test: File::Basename was completely unimplemented — basename()
# and dirname() were unrecognized bareword calls, silently returning
# undef (empty string) instead of the path component.
use strict;
use warnings;
use File::Basename;

my $b = basename("/usr/local/bin/foo.txt");
my $d = dirname("/usr/local/bin/foo.txt");
print "$b $d\n";
print(($b eq "foo.txt" && $d eq "/usr/local/bin") ? "ok\n" : "FAIL\n");
