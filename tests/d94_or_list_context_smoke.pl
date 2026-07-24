#!/usr/bin/perl
# Smoke: D94 — ||/or/&&/and/ // RHS must inherit outer list context.
use strict;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub t { return wantarray() ? (1, 2, 3) : 9 }
sub z { return 0 }

my @a = (z() || t());
check('or_rhs_list', join(",", @a) eq "1,2,3");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d94_or_list_context_smoke_done\n";
