#!/usr/bin/perl
# Smoke test for D28: `sort`/`reduce` comparator blocks always rebound
# `$a`/`$b` to fresh locals, ignoring an outer lexical `my $a`/`my $b`
# that should shadow them — the well-known real-Perl "my $a used in
# sort comparison" footgun. perlc's own (unshadowed) `$a`/`$b` always
# won the lookup, so the comparator's `$a`/`$b` never actually resolved
# to the outer lexical the way real Perl's does.
# Fast, narrow coverage — see sort_reduce_shadow.pl for the in-depth
# suite.
use strict;
use warnings;
use List::Util qw(reduce);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug, via reduce (deterministic regardless of sort
# algorithm choice, unlike sort itself — see sort_reduce_shadow.pl's
# header comment for why).
{
    my $a = 1;    # shadows reduce's own $a for the rest of this scope
    my @data = (1, 2, 3, 4, 5);
    my $result = reduce { $a * $b } @data;
    check('smoke_reduce_a_shadowed', $result == 5);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_reduce_shadow_smoke_done\n";
