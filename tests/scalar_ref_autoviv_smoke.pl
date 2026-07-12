#!/usr/bin/perl
# Smoke test for D50: `$ref->{a}{b} = val` (chained autoviv starting
# from an *existing* scalar ref, not a hash/array element) silently
# failed — the intermediate `$ref->{a}` level was created as a
# disconnected temporary rather than being properly linked back into
# $ref's own hash, so the write was silently lost.
# Fast, narrow coverage — see scalar_ref_autoviv.pl for the in-depth
# suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug.
{
    my $ref = {};
    $ref->{a}{b} = 1;
    check('smoke_two_level_scalar_ref_autoviv', $ref->{a}{b} == 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "scalar_ref_autoviv_smoke_done\n";
