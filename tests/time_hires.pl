#!/usr/bin/perl
# In-depth test suite for Time::HiRes (D30: not implemented at all — `use
# Time::HiRes qw(...)` fell through the same silent-ignore path as any
# other unrecognized pragma, since no Time/HiRes.pm file exists on disk
# and the module wasn't in perlc's PRAGMAS set; time()/sleep() therefore
# always stayed integer-second resolution, and gettimeofday/usleep/
# tv_interval didn't exist as callable names at all).
#
# Implemented as a built-in (no real .pm file), matching how POSIX/
# Scalar::Util/Carp already work in perlc. The one architectural wrinkle
# specific to Time::HiRes: `time` and `sleep` are perlc *lexer keywords*
# with pre-existing (integer-only) builtin behavior, unlike POSIX::floor
# etc. (which were never keywords in perlc or real Perl — always ordinary
# sub calls). Overriding a keyword's behavior must be opt-in, matching
# real Perl exactly: a bare `use Time::HiRes;` with no import list does
# NOT override time()/sleep() at all (confirmed against real Perl); only
# `use Time::HiRes qw(time)` / `qw(sleep)` does. This is implemented by
# threading the explicit import list into main.cpp's importMap the same
# way POSIX/Scalar::Util imports already work, and having the parser
# consult importMap_ specifically at its KW_TIME/KW_SLEEP sites (emitting
# a qualified Time::HiRes::time/sleep Call node instead of the normal
# TimeFunc/SleepFunc node when explicitly imported). gettimeofday/
# usleep/tv_interval have no pre-existing keyword meaning, so — like
# POSIX::floor — they're always available unqualified with no import
# gating needed.
#
# Non-determinism note: every check below verifies a *structural*
# property (contains a decimal point, element count, non-negative,
# strictly positive) rather than an exact timing value, so the output is
# stable and byte-for-byte comparable against real Perl despite actual
# wall-clock timings differing between runs/processes.
use strict;
use warnings;
use Time::HiRes qw(time sleep usleep gettimeofday tv_interval);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — imported time() has sub-second ──────────
# ── precision (a fractional part), not just integer seconds ────────────────
{
    my $t = time();
    check('hires_time_has_fraction', $t =~ /\./);
    check('hires_time_looks_like_epoch', $t > 1_000_000_000);
}

# ── Section 2: gettimeofday() — list context returns (seconds, ────────────
# ── microseconds); scalar context returns fractional seconds ───────────────
{
    my @tv = gettimeofday();
    check('gettimeofday_list_count', scalar(@tv) == 2);
    check('gettimeofday_list_sec_is_epoch', $tv[0] > 1_000_000_000);
    check('gettimeofday_list_usec_in_range', $tv[1] >= 0 && $tv[1] < 1_000_000);

    my $scalar_tv = gettimeofday();
    check('gettimeofday_scalar_has_fraction', $scalar_tv =~ /\./);
}

# ── Section 3: usleep(MICROSECONDS) sleeps and returns actual usecs slept ─
{
    my $u = usleep(1000);
    check('usleep_returns_positive', $u > 0);
}

# ── Section 4: sleep(SECONDS) accepts a fractional argument and returns ───
# ── the actual (non-negative) number of seconds slept ──────────────────────
{
    my $s = sleep(0.01);
    check('hires_sleep_nonneg', $s >= 0);
    check('hires_sleep_zero_arg_ok', sleep(0) >= 0);
}

# ── Section 5: tv_interval computes elapsed time between two ──────────────
# ── gettimeofday-style array refs ───────────────────────────────────────────
{
    my $t0 = [gettimeofday()];
    usleep(1000);
    my $t1 = [gettimeofday()];
    my $elapsed = tv_interval($t0, $t1);
    check('tv_interval_positive', $elapsed > 0);

    # single-arg form: interval from $t0 to now
    my $t2 = [gettimeofday()];
    usleep(500);
    my $elapsed2 = tv_interval($t2);
    check('tv_interval_single_arg_positive', $elapsed2 > 0);
}

# ── Section 6: fully-qualified access works too (Time::HiRes::time) ───────
{
    my $t = Time::HiRes::time();
    check('qualified_time_has_fraction', $t =~ /\./);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "time_hires_tests_done\n";
