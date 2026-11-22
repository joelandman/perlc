#!/usr/bin/env perl
# Stage 46 mapdie smoke: eval-caught die / div0 on small N (sequential).
use strict;
use warnings;

my @r;
eval {
    @r = map {
        if ($_ == 5) { die "boom $_" }
        $_
    } (0 .. 10);
};
print "rlen=", scalar(@r), " r=@r\n";
print "boom=", ($@ =~ /boom 5/ ? 1 : 0), "\n";

my @d;
eval {
    @d = map { 1 / ($_ - 3) } (0 .. 6);
};
print "dlen=", scalar(@d), "\n";
print "div=", ($@ =~ /Illegal division by zero/ ? 1 : 0), "\n";

print "done\n";
