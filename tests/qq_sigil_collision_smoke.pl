#!/usr/bin/perl
# Smoke test for D65: a variable name that happens to start with "qq"
# (e.g. `$qqfoo`), or a variable literally named "q"/"qw" used for
# hash-element access (`$q{key}`, `$qw{key}`), was silently
# misinterpreted by the lexer as a quote-like operator (`qq`/`q`/`qw`)
# instead of a plain sigil-prefixed identifier — either producing a
# spurious parse error or silently evaluating to the wrong value.
# Fast, narrow coverage — see qq_sigil_collision.pl for the in-depth
# suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: a "qq"-prefixed variable name interpolated in a
# string, followed by a literal ']', caused a spurious parse error.
{
    my $qqfoo = "hello";
    my $s = "1:$qqfoo]";
    check('smoke_qq_prefixed_var_interp', $s eq '1:hello]');
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "qq_sigil_collision_smoke_done\n";
