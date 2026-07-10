#!/usr/bin/perl
# Smoke test for 3+ level chained hash/array autovivification (D40: perlc's
# codegen only recognized a base that was exactly one HashElem/ArrayElem
# level for ArrowDeref autoviv — a base that was itself another ArrowDeref
# (2+ levels) fell to a plain, non-autovivifying deref that silently
# produced an orphaned, discarded container).
# Fast, narrow coverage — see autoviv_chain.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: 3-level implicit-arrow chain.
{
    my %h;
    $h{a}{b}{c} = 1;
    check('smoke_three_level_implicit', $h{a}{b}{c} == 1);
}

# 3-level explicit-arrow chain (same underlying AST shape).
{
    my %h;
    $h{a}->{b}->{c} = 1;
    check('smoke_three_level_explicit', $h{a}->{b}->{c} == 1);
}

# 2-level chain still works — regression check.
{
    my %h;
    $h{a}{b} = 1;
    check('smoke_two_level_regression', $h{a}{b} == 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "autoviv_chain_smoke_done\n";
