#!/usr/bin/perl
# Smoke test for named subs declared inside a bare `{ }` block (D45: a
# named sub declared inside any bare block, at any nesting depth, was
# silently uncallable — the call site resolved to nothing and silently
# returned an empty/undef value instead of running the sub's body).
# Fast, narrow coverage — see nested_block_sub.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: a sub declared and called inside the same bare block.
{
    sub add_one { my ($x) = @_; return $x + 1; }
    check('smoke_sub_in_single_block', add_one(4) == 5);
}

# A preceding sibling block, then a sub declared in a second block.
{
    my $unused = 1;
}
{
    sub double_it { my ($x) = @_; return $x * 2; }
    check('smoke_sub_after_sibling_block', double_it(5) == 10);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "nested_block_sub_smoke_done\n";
