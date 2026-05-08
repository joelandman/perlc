#!/usr/bin/perl
use strict;
use warnings;

# Basic closure: captures a single outer variable
sub make_counter {
    my $n = 0;
    return sub { $n = $n + 1; return $n; };
}

my $c1 = make_counter();
my $c2 = make_counter();

print $c1->() . "\n";   # 1
print $c1->() . "\n";   # 2
print $c1->() . "\n";   # 3
print $c2->() . "\n";   # 1  (independent counter)
print $c1->() . "\n";   # 4

# Closure capturing multiple variables
sub make_adder {
    my ($base, $step) = @_;
    return sub { $base = $base + $step; return $base; };
}

my $by2 = make_adder(10, 2);
my $by5 = make_adder(0, 5);

print $by2->() . "\n";   # 12
print $by2->() . "\n";   # 14
print $by5->() . "\n";   # 5
print $by5->() . "\n";   # 10

# Closure over loop variable
my @funcs;
my $i = 1;
while ($i <= 3) {
    my $val = $i;
    push @funcs, sub { return $val * 2; };
    $i = $i + 1;
}
print $funcs[0]->() . "\n";  # 2
print $funcs[1]->() . "\n";  # 4
print $funcs[2]->() . "\n";  # 6
