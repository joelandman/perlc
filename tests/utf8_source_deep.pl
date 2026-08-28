#!/usr/bin/perl
# Deep: use utf8 on interpolation, qw, and substr.
use strict;
use warnings;
use utf8;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $e = "é";
check('var_len', length($e) == 1);
check('interp_var', length("[$e]") == 3);
my @w = qw(café);
check('qw_len', length($w[0]) == 4);
check('substr', substr("aéiou", 1, 1) eq "é");

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "utf8_source_deep_done\n";
