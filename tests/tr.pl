#!/usr/bin/perl
use strict;
use warnings;

# ── basic translation ────────────────────────────────────────────────────────
my $s = "hello";
$s =~ tr/a-z/A-Z/;
print "$s\n";                    # HELLO

$s = "WORLD";
$s =~ tr/A-Z/a-z/;
print "$s\n";                    # world

# single char
$s = "banana";
$s =~ tr/a/o/;
print "$s\n";                    # bonono

# ── y/// alias ──────────────────────────────────────────────────────────────
$s = "hello";
$s =~ y/a-z/A-Z/;
print "$s\n";                    # HELLO

# ── return value (count of chars translated) ────────────────────────────────
$s = "hello world";
my $count = ($s =~ tr/a-z//);
print "$count\n";                # 10  (all lowercase letters, space untouched)

$s = "aabbcc";
$count = ($s =~ tr/a/a/);
print "$count\n";                # 2

# ── delete flag /d ──────────────────────────────────────────────────────────
$s = "hello world";
$s =~ tr/aeiou//d;
print "$s\n";                    # hll wrld

$s = "abc123def";
$s =~ tr/0-9//d;
print "$s\n";                    # abcdef

# ── squeeze flag /s ─────────────────────────────────────────────────────────
$s = "aaabbbccc";
$s =~ tr/a-z/a-z/s;
print "$s\n";                    # abc

$s = "bookkeeper";
$s =~ tr/a-z//s;
print "$s\n";                    # bokeper

# ── complement flag /c ──────────────────────────────────────────────────────
$s = "abc123";
$s =~ tr/a-z/*/c;               # replace non-lowercase with *
print "$s\n";                    # abc***

# ── complement + delete /cd ─────────────────────────────────────────────────
$s = "Hello, World! 123";
$s =~ tr/a-zA-Z//cd;            # delete non-alpha
print "$s\n";                    # HelloWorld

# ── complement + squeeze /cs ────────────────────────────────────────────────
$s = "abc123def456ghi";
$s =~ tr/a-z/ /cs;              # replace non-lowercase runs with single space
print "$s\n";                    # abc def ghi

# ── escape sequences in tr ──────────────────────────────────────────────────
$s = "line1\nline2\nline3";
$count = ($s =~ tr/\n//);
print "$count\n";                # 2

$s = "col1\tcol2\tcol3";
$s =~ tr/\t/|/;
print "$s\n";                    # col1|col2|col3

# ── operating on $_ ─────────────────────────────────────────────────────────
$_ = "Hello World";
tr/a-z/A-Z/;
print "$_\n";                    # HELLO WORLD

# ── no replace list (count only, no mutation) ───────────────────────────────
$s = "the cat sat on the mat";
$count = ($s =~ tr/aeiou//);
print "$count\n";                # 6

# ── replicate last replacement char when replace list is shorter ─────────────
$s = "abcdef";
$s =~ tr/a-f/xy/;               # a→x, b→y, c-f→y (last char replicated)
print "$s\n";                    # xyyyyy

# ── ROT13 ───────────────────────────────────────────────────────────────────
$s = "Hello, World!";
$s =~ tr/A-Za-z/N-ZA-Mn-za-m/;
print "$s\n";                    # Uryyb, Jbeyq!

# re-apply ROT13 to decode
$s =~ tr/A-Za-z/N-ZA-Mn-za-m/;
print "$s\n";                    # Hello, World!

# ── tr inside a loop ────────────────────────────────────────────────────────
my @words = ("hello", "world", "perl");
for my $w (@words) {
    $w =~ tr/a-z/A-Z/;
    print "$w\n";               # HELLO / WORLD / PERL
}

# ── combined delete + squeeze (/ds) ─────────────────────────────────────────
$s = "aaa1bbb2ccc";
$s =~ tr/0-9//ds;              # delete digits (squeeze redundant but harmless)
print "$s\n";                   # aaabbbccc

print "done\n";
