#!/usr/bin/perl
# Smoke test for `return` inside eval{} (D25: perlc compiled a top-level
# `return` inside eval{} to perl_die, corrupting the intended return value
# into a die message and $@ into whatever text was meant to be returned —
# instead of real Perl's actual semantics, where `return` inside eval{}
# exits just that eval block with the given value).
# Fast, narrow coverage — see eval_return.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: return inside eval, catching an inner die.
{
    my $outer = eval {
        eval { die "inner error\n"; };
        return "inner caught: $@" if $@;
        return "no error";
    };
    check('smoke_return_in_eval', index($outer, "inner caught") >= 0);
}

# return inside eval within a sub — execution continues in the sub after
# the eval, it does NOT return from the sub itself.
sub smoke_foo { my $x = eval { return "from eval"; }; return "x=$x"; }

{
    check('smoke_return_in_eval_within_sub', smoke_foo() eq "x=from eval");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "eval_return_smoke_done\n";
