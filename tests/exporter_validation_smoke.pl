#!/usr/bin/perl
# Smoke test for the D26 follow-up: perlc now validates that an
# explicitly-`qw()`-requested import name actually appears in the
# target module's @EXPORT/@EXPORT_OK, rejecting a bogus request with a
# compile-time error matching real Perl's Exporter ("NAME" is not
# exported by the Module module) instead of silently allowing it
# through. Since a *rejected* import must fail to compile — which
# tests/harness.sh always treats as a test failure, with no support for
# "expected to fail compilation" cases — this suite only covers the
# *positive* path (valid explicit imports still work, no false-positive
# rejections). The negative path (a bad import is correctly rejected,
# with real Perl's exact error message) is verified manually; see
# REMEDIATION.md's writeup for the exact commands/output compared.
# Fast, narrow coverage — see exporter_validation.pl for the in-depth
# suite.
use strict;
use warnings;
use lib "tests/lib";
use MathOps qw(subtract);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

check('smoke_valid_export_ok_import', subtract(10, 3) == 7);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "exporter_validation_smoke_done\n";
