#!/usr/bin/perl
# Smoke test for regex-literal backslash handling (D23: perlc's regex-
# literal lexer collapsed `\\` (escaped backslash) into a single `\` before
# handing the pattern to PCRE2 — so /a\\d/ (literal backslash then "d")
# silently became /a\d/ (digit metaclass), changing match semantics with
# no error).
# Fast, narrow coverage — see regex_backslash.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: literal backslash in the subject should match \\ in
# the pattern.
{
    my $s = "a\\b";
    check('smoke_literal_backslash_match', $s =~ /a\\b/ ? 1 : 0);
}

# \\d must NOT behave like \d (digit class) — it's a literal backslash
# followed by a literal "d".
{
    my $s = "a3b";
    check('smoke_double_backslash_not_digit_class', !($s =~ /a\\d/));
}

# Real \d (single backslash) still works — regression check.
{
    my $s = "a3b";
    check('smoke_single_backslash_digit_class_regression', $s =~ /a\d/ ? 1 : 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "regex_backslash_smoke_done\n";
