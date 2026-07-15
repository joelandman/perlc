#!/usr/bin/perl
# Smoke test for D69: List::Util::uniq was broken on its two most common
# call shapes — an array-variable argument (didn't deduplicate at all,
# since the underlying runtime only stripped *consecutive* duplicates,
# Unix `uniq`-command style, not real Perl's "keep first occurrence
# anywhere in the list" semantics) and the fully-qualified
# List::Util::uniq(...) form (returned an empty list entirely, since the
# qualified name wasn't recognized as the builtin at all).
# Fast, narrow coverage — see d69_uniq.pl for the in-depth suite.
use strict;
use warnings;
use List::Util qw(uniq);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my @arr = (3, 1, 4, 1, 5, 9, 1);
my @u = uniq(@arr);
check('smoke_array_arg_dedup', "@u" eq "3 1 4 5 9");

my @u2 = List::Util::uniq(@arr);
check('smoke_qualified_call', "@u2" eq "3 1 4 5 9");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d69_uniq_smoke_done\n";
