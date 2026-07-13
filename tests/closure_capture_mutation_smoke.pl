#!/usr/bin/perl
# Smoke test for D62: closures captured a `my` variable's *value* by
# clone, not by reference — a mutation from inside a closure was never
# visible to the enclosing scope (or vice versa), diverging from real
# Perl's actual by-reference closure semantics.
# Note: uses a string-initialized scalar ("0", not 0) rather than an
# integer literal — a plain `my $x = 0;` can compile to perlc's unboxed
# int fast path, which has its own separate, still-open capture gap
# (D64) unrelated to this fix; forcing a string keeps this test isolated
# to exactly what D62 fixed.
# Fast, narrow coverage — see closure_capture_mutation.pl for the
# in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: $x++ inside a closure never propagated back to the
# enclosing scope's own $x.
{
    my $x = "0";
    my $f = sub { $x++; };
    $f->();
    $f->();
    check('smoke_closure_mutation_visible', $x == 2);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "closure_capture_mutation_smoke_done\n";
