#!/usr/bin/perl
use strict;
use warnings;

# ── @{expr} interpolation ───────────────────────────────────────────────────
my @arr = (1, 2, 3);
my $ref = \@arr;

print "@{$ref}\n";                          # 1 2 3

my %h = (list => [4, 5, 6]);
print "@{$h{list}}\n";                      # 4 5 6

# anonymous array ref inline
print "@{[7, 8, 9]}\n";                     # 7 8 9

# expression result as array
my @words = qw(hello world);
print "words: @{[reverse @words]}\n";       # words: world hello

# nested: array ref returned by sub
sub get_list { [10, 20, 30] }
print "@{get_list()}\n";                    # 10 20 30

# ── @$ref interpolation ─────────────────────────────────────────────────────
my $aref = [qw(a b c)];
print "@$aref\n";                           # a b c

my $aref2 = \@arr;
print "ref2: @$aref2\n";                    # ref2: 1 2 3

# ── ${\expr} interpolation ──────────────────────────────────────────────────
my $x = 21;
print "${\ ($x * 2)}\n";                    # 42

print "uc: ${\ uc('hello')}\n";             # uc: HELLO

my @nums = (1..5);
print "count: ${\ scalar(@nums)}\n";        # count: 5

print "calc: ${\(3 + 4)}\n";               # calc: 7

# combined in a single string
my $name = "world";
my @items = qw(foo bar baz);
print "Hello $name, items: @{[scalar @items]}, list: @items\n";
# Hello world, items: 3, list: foo bar baz

# @{[expr]} as the canonical "expression in string" idiom
my $pi = 3.14159;
print "pi rounded: @{[ int($pi * 100) / 100 ]}\n";   # pi rounded: 3.14

print "done\n";
