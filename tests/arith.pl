use feature "say";
#!/usr/bin/perl
use 5.010;
use strict;
use warnings;

my $a = 10;
my $b = 3;

say $a + $b;
say $a - $b;
say $a * $b;
say $a / $b;
say $a % $b;

my $x = 1;
$x += 5;
say $x;

my $s = "Hello";
$s .= " World";
say $s;

my $n = 2.5;
say $n * 4;
