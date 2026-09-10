#!/usr/bin/perl
# Smoke test: Getopt::Long was completely unimplemented — GetOptions()
# silently resolved to nothing and always returned false, so every
# real-world script using it (the most common way Perl CLI tools parse
# @ARGV) fell into its own "or die"/"or usage()" path no matter what
# flags were passed.
use strict;
use warnings;
use Getopt::Long;

@ARGV = ("--verbose");
my $verbose = 0;
GetOptions("verbose" => \$verbose) or die "bad opts\n";
print "verbose=$verbose\n";
print(($verbose == 1) ? "ok\n" : "FAIL\n");
