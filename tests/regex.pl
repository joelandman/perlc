#!/usr/bin/perl
use 5.010;
use strict;
use warnings;

# --- basic match ---
my $s = "Hello, World!";
if ($s =~ /World/) { say "match"; }       # match
if ($s !~ /xyz/)   { say "no xyz"; }      # no xyz

# --- case-insensitive ---
if ($s =~ /hello/i) { say "icase"; }      # icase

# --- capture groups ---
my $date = "2024-03-15";
if ($date =~ /(\d{4})-(\d{2})-(\d{2})/) {
    say $1;   # 2024
    say $2;   # 03
    say $3;   # 15
}

# --- substitution ---
my $str = "foo bar foo";
$str =~ s/foo/baz/;
say $str;      # baz bar foo

# --- global substitution ---
my $str2 = "aabbcc";
$str2 =~ s/b/x/g;
say $str2;     # aaxxcc

# --- substitution with capture ---
my $str3 = "hello world";
$str3 =~ s/(\w+)/[$1]/g;
say $str3;     # [hello] [world]

# --- split with regex ---
my $csv = "one,,two,,three";
my @parts = split(/,+/, $csv);
say $parts[0];   # one
say $parts[1];   # two
say $parts[2];   # three

# --- match negation ---
my $num = "12345";
if ($num !~ /[a-z]/) { say "digits only"; }   # digits only
