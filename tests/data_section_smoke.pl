#!/usr/bin/perl
# Smoke: __DATA__ / <DATA>
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $first = <DATA>;
chomp $first;
check('first', $first eq "alpha");

my @rest = <DATA>;
chomp @rest;
check('rest', join(",", @rest) eq "beta,gamma");

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "data_section_smoke_done\n";
__DATA__
alpha
beta
gamma
