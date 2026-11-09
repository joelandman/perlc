#!/usr/bin/perl
# D218: a bare block that starts with `%hash =` is a block, not a hash constructor.
use strict;
use warnings;
my %code;
@code{1, 2} = ("A", "B");
{
    my %ch = ();
    my ($k, $v);
    while (($k, $v) = each %code) {
        $ch{$k} = $v;
    }
    my @ks = sort keys %ch;
    print join(",", @ks), "\n";
    print join(",", @ch{@ks}), "\n";
}
print "after\n";
