#!/usr/bin/perl
# Regression check for D30: `time`/`sleep` are perlc *lexer keywords*
# with pre-existing (integer-only) builtin behavior — a program that
# never `use`s Time::HiRes at all, or that `use`s it with no import
# list, must see plain, unmodified time()/sleep() (matching real Perl's
# own behavior exactly: Time::HiRes never overrides these unless a
# name is explicitly requested via qw(...)).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# No `use Time::HiRes` anywhere in this file at all.
{
    my $t = time();
    check('no_module_time_stays_integer', $t =~ /^\d+$/);
}

{
    my $rc = sleep(0);
    check('no_module_sleep_returns_defined', defined($rc));
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "time_hires_no_import_regression_done\n";
