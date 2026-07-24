#!/usr/bin/perl
# Deep: D80 — $+{name} string interpolation + bare form regression.
use strict;

print "=== basic ===\n";
{
    "hello world" =~ /(?<w1>\w+) (?<w2>\w+)/;
    print "both:$+{w1}/$+{w2}\n";
    print "one:$+{w1}\n";
    print "bare:", $+{w2}, "\n";
}

print "=== keys %+ ===\n";
{
    "ab" =~ /(?<a>a)(?<b>b)/;
    print "keys=", join(",", sort keys %+), "\n";
    print "vals=$+{a}$+{b}\n";
}

print "=== mid-string ===\n";
{
    "9" =~ /(?<n>\d)/;
    print "x$+{n}y\n";
}

print "d80_plus_hash_interp_done\n";
