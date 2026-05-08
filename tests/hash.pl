#!/usr/bin/perl
use strict;
use warnings;

# --- basic creation and access ---
my %colors = (red => 1, green => 2, blue => 3);
say $colors{red};       # 1
say $colors{green};     # 2
say $colors{blue};      # 3

# --- insertion and update ---
$colors{yellow} = 4;
say $colors{yellow};    # 4
$colors{red} = 10;
say $colors{red};       # 10

# --- exists ---
if (exists $colors{green}) {
    say "green exists";
}
if (!exists $colors{purple}) {
    say "purple missing";
}

# --- delete ---
delete $colors{green};
if (!exists $colors{green}) {
    say "green deleted";
}

# --- sorted keys iteration ---
foreach my $k (sort keys %colors) {
    say $k;
}

# --- count keys ---
my $n = scalar keys %colors;
say $n;

# --- hash as sub argument ---
sub sum_hash {
    my (%h) = @_;
    my $total = 0;
    foreach my $k (sort keys %h) {
        $total += $h{$k};
    }
    return $total;
}

my %nums = (a => 3, b => 7, c => 2);
say sum_hash(%nums);    # 12
