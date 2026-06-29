use feature "say";
#!/usr/bin/perl
use 5.010;
use strict;
use warnings;

# basic foreach with range
foreach my $i (1..5) {
    print "$i ";
}
print "\n";                    # 1 2 3 4 5

# for modifier with range
say $_ for 1..3;               # 1 2 3

# array assignment from range
my @r = (1..5);
say scalar @r;                 # 5
say $r[0];                     # 1
say $r[4];                     # 5

# range in expression context
my @a = (10..15);
say join(", ", @a);            # 10, 11, 12, 13, 14, 15

# range with variables
my $lo = 3;
my $hi = 7;
my @b = ($lo..$hi);
say scalar @b;                 # 5
say $b[0];                     # 3
say $b[4];                     # 7

# empty range (lo > hi)
my @empty = (5..3);
say scalar @empty;             # 0

# range used in join
say join("-", 1..4);           # 1-2-3-4

# C-style for with range variable
my $sum = 0;
foreach my $n (1..10) {
    $sum += $n;
}
say $sum;                      # 55
