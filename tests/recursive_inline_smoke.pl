#!/usr/bin/perl
# Smoke test for D57: a directly self-recursive sub matching the
# AST-level inliner's shape (`my (@params) = @_; return expr`) used to
# hang/crash the *compiler itself* at compile time — tryEmitInline() had
# no self-reference guard, so inlining a call whose body contains
# another call to the same sub recursed the compiler forever on the
# same, unchanging AST node. This test only needs to *compile and run*
# to prove the fix — if the compiler still hung, this file would never
# produce output at all (see harness.sh's timeout).
# Fast, narrow coverage — see recursive_inline.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: direct self-recursion matching the inlinable shape.
sub factorial { my ($n) = @_; return $n <= 1 ? 1 : $n * factorial($n - 1); }
check('smoke_direct_self_recursion', factorial(5) == 120);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "recursive_inline_smoke_done\n";
