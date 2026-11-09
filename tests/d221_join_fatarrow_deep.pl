#!/usr/bin/perl
# D221: join with a fatarrow after the separator, including an empty separator.
use strict;
use warnings;
my $e = "enc";
my $suggestion = " (maybe)";
my $ver = "3.48";
my @supported = ("ascii", "utf8");
my $msg = join '' =>
    "doc ",
    "\"=encoding $e\" ",
    $suggestion,
    "  [$ver\'s supported encodings are: @supported]";
print $msg, "\n";
my $one = join '' => "only";
print "[$one]\n";
my $plain = join ":", "a", "b", "c";
print "$plain\n";
my $mixed = join "|" => "p", "q";
print "$mixed\n";
sub whine { print join("|", @_), "\n"; }
whine(sprintf "x%s:%s", "y", "z",);
print "done\n";
