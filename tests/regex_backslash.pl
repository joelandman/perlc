#!/usr/bin/perl
# In-depth test suite for regex-literal backslash handling.
#
# Root cause: the regex-literal lexer (readRegex() in lexer.cpp) special-
# cased `\\` in the source and collapsed it to a single `\` in the pattern
# string handed to PCRE2, while passing every OTHER backslash-escaped pair
# (\d, \s, \/, etc.) through unchanged (both characters). This was
# inconsistent and wrong: PCRE2 needs to see \\ exactly as written (two
# characters) to interpret it as "match one literal backslash" — collapsing
# it to a single \ before PCRE2 ever sees the pattern turns the NEXT
# character into part of a (possibly unintended) escape sequence. E.g.
# /a\\d/ (literal backslash, then "d") silently became /a\d/ (digit
# metaclass) with no error — a silent, wrong-match correctness bug for any
# pattern containing a literal backslash.
#
# Fixed by removing the special case entirely: every backslash-escaped
# pair is now passed through to PCRE2 unchanged, exactly as the general
# (non-\\) branch already did. PCRE2 does its own escape interpretation,
# including tolerating a redundant escape of the delimiter character
# (\/ is harmless — PCRE2 allows backslash-escaping any non-alphanumeric
# character to mean "match that literal character").
#
# NOTE: while testing this fix, a SEPARATE, unrelated bug was found and is
# deliberately NOT exercised here: `\$` inside a DOUBLE-QUOTED STRING
# literal (not a regex pattern) does not produce a literal `$` correctly
# — `"price: \$100"` prints as "price: 00" under perlc instead of
# "price: $100" (see TESTS.md D51). Tests below that need a literal `$`
# in the subject string use single quotes to sidestep that unrelated bug.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — literal backslash match ──────────────────
{
    my $s = "a\\b";
    check('literal_backslash_match', $s =~ /a\\b/ ? 1 : 0);
}

# ── Section 2: \\d must NOT act like the \d digit class ─────────────────────
{
    my $s = "a3b";
    check('double_backslash_not_digit_class', !($s =~ /a\\d/));
}
{
    my $s = "a\\3b";
    check('double_backslash_literal_then_digit', $s =~ /a\\3b/ ? 1 : 0);
}

# ── Section 3: single \d (regression) still matches digits ─────────────────
{
    my $s = "a3b";
    check('single_backslash_digit_class_regression', $s =~ /a\d/ ? 1 : 0);
}

# ── Section 4: multiple consecutive literal backslashes ─────────────────────
{
    my $s = "a\\\\b"; # subject contains: a \ \ b
    check('multiple_literal_backslashes', $s =~ /a\\\\b/ ? 1 : 0);
}

# ── Section 5: escaped delimiter (redundant-but-harmless for PCRE2) ────────
{
    my $s = "a/b";
    check('escaped_delimiter_slash', $s =~ /a\/b/ ? 1 : 0);
}

# ── Section 6: capture groups combined with literal-backslash separators ───
{
    my $s = "path\\to\\file";
    my $matched = ($s =~ /(\w+)\\(\w+)\\(\w+)/) ? "$1-$2-$3" : "no match";
    check('captures_with_backslash_separators', $matched eq "path-to-file");
}

# ── Section 7: substitution with a literal-backslash pattern (regression) ──
{
    my $s = "a\\b";
    $s =~ s/\\/-/;
    check('substitution_backslash_pattern_regression', $s eq "a-b");
}

# ── Section 8: split() on a literal-backslash pattern (shares the same ─────
# ── regex-literal lexer path as bare /pat/ matches) ─────────────────────────
{
    my $s = "C:\\Users\\test";
    my @parts = split(/\\/, $s);
    check('split_on_literal_backslash', join(",", @parts) eq "C:,Users,test");
}

# ── Section 9: escaped dollar sign in a pattern still anchors correctly ────
# ── (sourced via single-quotes to sidestep the separate D51 string bug) ────
{
    my $s = 'price: $100';
    my $matched = ($s =~ /\$(\d+)/) ? $1 : undef;
    check('escaped_dollar_in_pattern', defined($matched) && $matched == 100);
}

# ── Section 10: no backslash at all in the pattern (regression) ────────────
{
    my $s = "hello world";
    check('no_backslash_regression', $s =~ /world/ ? 1 : 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "regex_backslash_tests_done\n";
