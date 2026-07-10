#!/usr/bin/perl
# Smoke test for trailing @rest/%rest in list assignment (D39: perlc's
# parser stripped the sigil off every LHS variable in `my (...) = ...`,
# so a trailing @rest was silently treated as a same-named scalar that was
# never declared — the real @rest array stayed empty).
# Fast, narrow coverage — see list_assign_rest.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: trailing @rest should collect the remainder.
{
    my ($a, @rest) = (1, 2, 3, 4, 5);
    check('smoke_array_rest_collects', $a == 1 && join(",", @rest) eq "2,3,4,5");
}

# Trailing %rest should collect the remainder as key/value pairs.
{
    my ($a, %rest) = (1, "k1", "v1", "k2", "v2");
    check('smoke_hash_rest_collects',
          $a == 1 && $rest{k1} eq "v1" && $rest{k2} eq "v2");
}

# No leftover elements — @rest must be empty, not error.
{
    my ($a, $b, @rest) = (1, 2);
    check('smoke_empty_rest', $a == 1 && $b == 2 && scalar(@rest) == 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "list_assign_rest_smoke_done\n";
