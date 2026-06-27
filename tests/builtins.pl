use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

# --- shift / unshift ---
my @a = (1, 2, 3, 4, 5);
my $first = shift @a;
say $first;         # 1
say scalar @a;      # 4

unshift @a, 10, 20;
say $a[0];          # 10
say $a[1];          # 20
say scalar @a;      # 6

# --- chomp ---
my $line = "hello\n";
chomp $line;
say $line;          # hello
say length($line);  # 5

# --- length ---
my $s = "abcdef";
say length($s);     # 6
say length("xy");   # 2

# --- substr ---
say substr($s, 2);      # cdef
say substr($s, 1, 3);   # bcd
say substr($s, -2);     # ef
say substr($s, 1, 2);   # bc

# --- join ---
my @words = ("one", "two", "three");
say join(", ", @words);         # one, two, three
say join("-", "a", "b", "c");   # a-b-c
say join("", @words);           # onetwothree

# --- split ---
my $csv = "a,b,c,d";
my @parts = split(",", $csv);
say scalar @parts;   # 4
say $parts[0];       # a
say $parts[3];       # d

my $sentence = "  hello   world  foo  ";
my @ws = split(" ", $sentence);
say scalar @ws;   # 3
say $ws[0];       # hello
say $ws[2];       # foo

# split with regex-style pattern
my $path = "usr/local/bin";
my @dirs = split(/\//, $path);
say scalar @dirs;  # 3
say $dirs[1];      # local

# --- round-trip: split then join ---
my $orig = "one:two:three";
my @bits = split(":", $orig);
my $rejoined = join(":", @bits);
say $rejoined;     # one:two:three

# --- chomp in a loop ---
my @lines = ("foo\n", "bar\n", "baz\n");
foreach my $l (@lines) {
    chomp $l;
    say $l;        # foo, bar, baz
}
