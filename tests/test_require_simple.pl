#!/usr/bin/perl
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

require "tests/lib/RequireValue.pm";
check('require_loads_named_sub', RequireValue::value() == 42);

require "tests/lib/RequireValue.pm";
check('require_repeat_keeps_sub_available', RequireValue::value() == 42);

require "missing_require_for_perlc.pm";
check('require_missing_sets_error', length($@) > 0);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "test_require_simple_done\n";
