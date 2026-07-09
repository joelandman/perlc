#!/usr/bin/perl
# Smoke test for list-assignment arity mismatch (my ($x,$y) = (10) used to
# segfault perlc: perl_array_get_ref was handed a scalar PerlValue* where it
# expected a PerlArray*, because a single-element parenthesized RHS with no
# comma parses down to a bare scalar node, not an ArrayLit).
# Fast, narrow coverage — see list_assign_arity.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original crashing repro: fewer RHS values than LHS scalars.
{
    my ($x, $y) = (10);
    check('smoke_fewer_rhs_no_crash', $x == 10 && !defined($y));
}

# Trailing extra LHS scalars beyond a multi-element RHS.
{
    my ($a, $b, $c) = (1, 2);
    check('smoke_trailing_undef', $a == 1 && $b == 2 && !defined($c));
}

# Exact match still works (regression check).
{
    my ($p, $q) = (5, 6);
    check('smoke_exact_match', $p == 5 && $q == 6);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "list_assign_arity_smoke_done\n";
