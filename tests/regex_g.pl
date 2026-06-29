#!/usr/bin/perl
use 5.010;
use strict;
use warnings;

# --- while with /g, no captures ---
my $str = "cat bat hat";
while ($str =~ /\w+at/g) {
    say "match";   # match x3
}

# --- while with /g and captures ---
my $data = "x=1 y=2 z=3";
while ($data =~ /(\w+)=(\d+)/g) {
    say $1;   # x, y, z
    say $2;   # 1, 2, 3
}

# --- loop reuse: same var, fresh loop ---
my $s2 = "aa bb cc";
while ($s2 =~ /(\w+)/g) {
    say $1;   # aa, bb, cc
}

# --- for/foreach with /g (list context) ---
my @words = ("hello world foo" =~ /(\w+)/g);
say $words[0];         # hello
say $words[1];         # world
say $words[2];         # foo
say scalar @words;     # 3

# --- foreach loop variable ---
foreach my $m ("one1 two2 three3" =~ /([a-z]+)/g) {
    say $m;   # one, two, three
}
