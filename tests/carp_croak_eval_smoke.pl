#!/usr/bin/perl
# Smoke test for Carp::croak catchability (D35: perl_carp_croak() called
# exit(1) directly instead of routing through the die/eval mechanism, so
# the standard `eval { croak(...) }` pattern for turning a library error
# into a catchable exception killed the whole process instead).
#
# NOTE: only the catchable-inside-eval behavior is exercised here via the
# self-checking harness. "croak() outside any eval terminates the process"
# is inherently untestable from within a self-checking .pl test (a real
# process exit would abort the rest of the checks) — it was verified
# manually instead; see carp_croak_eval.pl's header comment and TESTS.md
# D35 for the verification transcript.
use strict;
use warnings;
use Carp;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub bad { croak("boom"); }

# The original bug: croak inside eval must be caught, not kill the process.
{
    my $caught = 0;
    my $msg = "";
    eval { bad(); };
    if ($@) { $caught = 1; $msg = $@; }
    check('smoke_croak_caught_by_eval', $caught);
    check('smoke_croak_message_content', index($msg, "boom") >= 0);
}

# Execution continues normally after the eval — process is still alive.
{
    my $x = 1 + 1;
    check('smoke_execution_continues_after_croak', $x == 2);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "carp_croak_eval_smoke_done\n";
