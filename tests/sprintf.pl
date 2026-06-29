#!/usr/bin/perl
use 5.010;
use strict;
use warnings;

# basic string and integer
printf("Hello, %s! You are %d years old.\n", "Alice", 30);

# floats
printf("Pi is approximately %.4f\n", 3.14159265);
printf("%e\n", 12345.6789);
printf("%g\n", 0.000123);

# width and alignment
printf("[%10s]\n", "right");
printf("[%-10s]\n", "left");
printf("[%05d]\n", 42);

# sprintf returns a string
my $s = sprintf("(%s, %d)", "foo", 99);
say $s;

# hex and octal
printf("%x\n", 255);
printf("%X\n", 255);
printf("%o\n", 8);

# multiple uses
for my $i (1..3) {
    my $line = sprintf("item %02d: %s", $i, "value");
    say $line;
}

# width from argument
printf("%*d\n", 8, 42);

# precision from argument
printf("%.*f\n", 3, 3.14159);

# %% literal
printf("100%%\n");

# sprintf used in concatenation
my $prefix = "Result: ";
my $result = $prefix . sprintf("%d + %d = %d", 3, 4, 7);
say $result;
