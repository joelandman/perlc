#!/usr/bin/perl
# Smoke test for D9: the `floatSqrtOf_` compiler-side optimization cache
# (Stage 30's "v*v -> x" rewrite for a variable assigned from sqrt(x))
# was keyed only by variable *name*, with no invalidation when the same
# name got a later, unrelated `my` declaration — including in a
# completely different sub. `$a*$a` then silently returned a stale
# sqrt() input from an earlier, unrelated sub instead of the actual
# squared value.
# Fast, narrow coverage — see sqrt_cache_leak.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: a sqrt()-assigned variable in one sub leaking into
# an unrelated same-named variable's squaring in a different sub.
sub f1 { my $a = sqrt(4); return $a; }
sub f2 { my $a = 10.0; return $a * $a; }

check('smoke_f1_sqrt_correct', f1() == 2);
check('smoke_f2_unaffected_by_f1', f2() == 100);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sqrt_cache_leak_smoke_done\n";
