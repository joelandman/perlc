#!/usr/bin/perl
# D225: `return printf STDERR ...` prints and returns the character count.
use strict;
use warnings;
sub w {
    return printf STDERR "ab %s\n", "c";
}
my $n = w();
printf STDERR "n=%d\n", $n;
