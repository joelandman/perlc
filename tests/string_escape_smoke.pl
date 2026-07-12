#!/usr/bin/perl
# Smoke test for D51: `\$` (and `\@`) inside a double-quoted string
# literal didn't produce a literal `$`/`@` correctly — the lexer
# collapsed the escape to a bare `$`/`@` too early, indistinguishable
# from a real interpolation trigger, so the parser's interpolation
# scanner misread the digits/word following it as a capture variable or
# array name instead of literal text.
# Fast, narrow coverage — see string_escape.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug.
{
    my $x = "price: \$100";
    check('smoke_escaped_dollar', $x eq "price: \$100");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "string_escape_smoke_done\n";
