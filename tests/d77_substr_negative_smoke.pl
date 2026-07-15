#!/usr/bin/perl
# Smoke test for D77: substr() mishandled negative LENGTH (treated as "no
# truncation" instead of real Perl's documented "stop N chars before the
# end") and a far-out-of-range negative OFFSET (silently clamped instead
# of correctly returning undef).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $s = "Hello, World!";
check('smoke_negative_length', substr($s, 2, -3) eq "llo, Wor");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d77_substr_negative_smoke_done\n";
