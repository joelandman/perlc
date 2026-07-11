#!/usr/bin/perl
# Smoke test for system()'s return value (D27: perl_system() already
# unwrapped the wait(2) status via WEXITSTATUS() before returning it,
# instead of returning the raw status word real Perl does — silently
# breaking the documented `$rc >> 8` idiom for recovering the exit code).
# Fast, narrow coverage — see system_exit_code.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: system() of a failing command, recovered via >> 8.
{
    my $rc = system("false");
    check('smoke_system_false_shifted', ($rc >> 8) == 1);
}

# system() of a successful command.
{
    my $rc = system("true");
    check('smoke_system_true_shifted', ($rc >> 8) == 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "system_exit_code_smoke_done\n";
