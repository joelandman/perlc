#!/usr/bin/perl
# In-depth test suite for system()'s return value.
#
# Root cause (D27): perl_system() (runtime.c) already unwrapped the raw
# wait(2) status via WEXITSTATUS() before returning it to Perl code,
# instead of returning the raw status word the way real Perl's system()
# does. Real Perl's documented idiom for recovering the plain exit code is
# `$rc >> 8` (the same convention $? uses) — with the unwrapped value,
# that idiom silently computed a nonsense result (right-shifting an
# already-plain 0-255 exit code by 8 gives 0 almost always).
#
# Fixed by returning the raw `ret` from the C system(3) call directly
# (still returning -1 as-is when system(3) itself fails to launch a
# child), matching real Perl's actual return-value contract.
#
# NOT exercised here (found while testing, out of scope for D27, a
# separate/narrower gap): perlc's system() always shells out via
# `/bin/sh -c COMMAND` (through the C system(3) call), while real Perl
# optimizes simple single-word commands with no shell metacharacters by
# exec'ing them directly and returning -1 if the exec itself fails.
# Under perlc, a nonexistent command instead gets a shell-reported "not
# found" (exit 127) rather than -1. Also not exercised: the multi-
# argument LIST form of system() (`system($prog, @args)`, which bypasses
# the shell entirely in real Perl) — perlc's parser only accepts a single
# string argument.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — failing command, recovered via >> 8 ──────
{
    my $rc = system("false");
    check('system_false_shifted_exit_code', ($rc >> 8) == 1);
}

# ── Section 2: successful command ───────────────────────────────────────────
{
    my $rc = system("true");
    check('system_true_shifted_exit_code', ($rc >> 8) == 0);
}

# ── Section 3: shell-invoked command with a specific nonzero exit code ─────
{
    my $rc = system("sh -c 'exit 7'");
    check('system_shell_specific_exit_code', ($rc >> 8) == 7);
}

# ── Section 4: raw status is nonzero for a failing command (not the plain ──
# ── exit code — 1 << 8 = 256, not 1) ────────────────────────────────────────
{
    my $rc = system("false");
    check('system_false_raw_status_is_256', $rc == 256);
}

# ── Section 5: raw status is exactly zero for success ───────────────────────
{
    my $rc = system("true");
    check('system_true_raw_status_is_zero', $rc == 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "system_exit_code_tests_done\n";
