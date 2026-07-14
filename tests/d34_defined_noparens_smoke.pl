#!/usr/bin/perl
# Smoke test for D34: `defined EXPR` without surrounding parens was a hard
# parse error — one of the single most common Perl idioms
# (`if (defined $x) {...}`) failed to compile at all under perlc.
# Fast, narrow coverage — see d34_defined_noparens.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $x = 5;
check('smoke_defined_true', defined $x);

my $y;
check('smoke_defined_false', !defined $y);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d34_defined_noparens_smoke_done\n";
