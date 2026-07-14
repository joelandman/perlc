#!/usr/bin/perl
# Smoke test for D64: a captured scalar that compiles to perlc's unboxed
# int/float fast path (a plain `my $x = 0;` at block scope, not file
# scope) got a boxed *snapshot* of its value at capture time instead of
# sharing the real stable-pointer storage — so a mutation from inside
# the closure never propagated back, even after D62 fixed the general
# (PV-boxed) case. Confirmed to affect both AnonSub and sort{}'s
# comparator identically.
# Fast, narrow coverage — see closure_unboxed_capture.pl for the
# in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: an int-literal-initialized $x++ inside a closure
# never propagated back to the enclosing scope's own $x.
{
    my $x = 0;
    my $f = sub { $x++; };
    $f->();
    $f->();
    check('smoke_unboxed_int_capture_visible', $x == 2);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "closure_unboxed_capture_smoke_done\n";
