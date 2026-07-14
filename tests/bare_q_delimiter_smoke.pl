#!/usr/bin/perl
# Smoke test for D60: bare `q(...)`/`q[...]`/`q<...>`/`q/.../ ` (single-`q`,
# non-`{`, non-`qq` delimiter) was not recognized as a quote-like
# operator at all — it fell through to ordinary bareword lexing and hit
# a hard parse error on the first token inside that didn't look like a
# valid argument list.
# Fast, narrow coverage — see bare_q_delimiter.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: q(...) with a non-brace delimiter was a hard parse
# error under perlc.
{
    my $x = q(literal $var here);
    check('smoke_bare_q_paren', $x eq 'literal $var here');
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "bare_q_delimiter_smoke_done\n";
