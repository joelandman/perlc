#!/usr/bin/perl
# Smoke test for D30 (Time::HiRes was not implemented at all — `use
# Time::HiRes qw(...)` was silently ignored the way unrecognized pragmas
# are, so time()/sleep() stayed integer-second resolution and
# gettimeofday/usleep/tv_interval didn't exist).
# Fast, narrow coverage — see time_hires.pl for the in-depth suite.
use strict;
use warnings;
use Time::HiRes qw(time sleep gettimeofday);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: imported time() should have sub-second precision.
{
    my $t = time();
    check('smoke_hires_time_has_fraction', $t =~ /\./);
}

# gettimeofday() in list context returns (seconds, microseconds).
{
    my @tv = gettimeofday();
    check('smoke_gettimeofday_list_count', scalar(@tv) == 2);
}

# sleep() with a fractional argument doesn't error.
{
    my $slept = sleep(0.01);
    check('smoke_hires_sleep_nonneg', $slept >= 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "time_hires_smoke_done\n";
