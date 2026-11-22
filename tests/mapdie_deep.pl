#!/usr/bin/env perl
# Stage 46 mapdie deep: bound 13005 so the pool path records the lowest
# index die and rethrows after join. Assignment must not complete.
use strict;
use warnings;

my $N = 13005;
my @a = (0 .. $N - 1);

my @r;
eval {
    @r = map {
        if ($_ == 9000) { die "boom" }
        $_ * 2
    } @a;
};
print "rlen=", scalar(@r), "\n";
print "boom=", ($@ =~ /boom/ ? 1 : 0), "\n";

my @d;
eval {
    @d = map { 1 / ($_ - 9000) } @a;
};
print "dlen=", scalar(@d), "\n";
print "div=", ($@ =~ /Illegal division by zero/ ? 1 : 0), "\n";

my @g;
eval {
    @g = grep {
        if ($_ == 9000) { die "gboom" }
        ($_ & 1) == 0
    } @a;
};
print "glen=", scalar(@g), "\n";
print "gboom=", ($@ =~ /gboom/ ? 1 : 0), "\n";

print "done\n";
