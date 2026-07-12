#!/usr/bin/perl
# In-depth test suite for D51: `\$`/`\@` inside a double-quoted string
# literal didn't produce a literal `$`/`@` — and, found while fixing it,
# single-quoted strings incorrectly resolved *every* backslash escape
# (`\n`, `\t`, etc.) the same way double-quoted strings do, and
# `qq(...)`/`q{...}`-with-a-non-brace-delimiter used the wrong closing
# delimiter entirely.
#
# Root cause (the core D51 bug): the lexer's `readString()` collapsed
# `\$`/`\@` to a bare `$`/`@` character while scanning a double-quoted
# string's raw content. The *parser's* interpolation scanner
# (`parseStringInterp`) then re-scans that same raw text looking for a
# bare `$`/`@` to trigger variable interpolation — and by the time it
# runs, an escaped-literal `$` is byte-for-byte indistinguishable from a
# real interpolation trigger. `"price: \$100"` therefore had its
# collapsed `$100` misread as the `$1` capture variable followed by
# literal `00`, giving `"price: 00"` instead of `"price: $100"`.
#
# Fixed by having `readString()` emit a `\x02` marker byte (never
# otherwise producible in this buffer) immediately before an
# escaped-literal `$`/`@`, instead of collapsing straight to a bare
# character. A plain backslash was considered and rejected as the
# marker: `"a\\$x"` (an escaped backslash immediately followed by a
# genuine, unescaped `$x`) already collapses `\\` to one literal `\`
# one step earlier in the same escape switch, leaving that `\`
# indistinguishable from an escaped `\$` if a plain backslash were used
# as the signal — `\x02` avoids the ambiguity entirely.
# `parseStringInterp` was given a new, first-checked case recognizing
# `\x02` and emitting just the following character as a literal.
#
# Found and fixed alongside (same function, same investigation):
#   - Single-quoted strings previously ran through the exact same
#     escape-resolution switch as double-quoted ones, so `'a\nb'`
#     incorrectly became `a`, newline, `b` (4 raw characters) instead of
#     the 4 literal characters `a`, `\`, `n`, `b` real Perl produces
#     (single-quoted strings only ever recognize `\\` and `\'` as
#     escapes). `readString()` now takes an explicit `interpolates`
#     flag from each of its three call sites instead of inferring
#     escape behavior from the delimiter character.
#   - `qq(...)`/`q(...)`-with-a-non-brace-delimiter (e.g. `qq(...)`,
#     `qq[...]`, `qq/.../`) hardcoded `'}'` as the closing delimiter
#     regardless of the actual one used, so `qq(hello world)` silently
#     produced no output at all (the scan for a literal `}` that never
#     appears ran past the intended end). Fixed by computing the
#     correct close delimiter from the actual open one, mirroring the
#     mapping `qw()` already used a few lines above in the same file.
#
# NOT fixed here (confirmed pre-existing, unrelated, reproduces
# identically with this fix's changes reverted — logged as new D60):
# bare `q(...)`/`q[...]` (single-`q`, non-brace, non-`qq` delimiter)
# isn't recognized as a quote-like operator at all and is a hard parse
# error. Avoided in this file by using `qq()` instead of `q()` wherever
# a non-brace delimiter is needed.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — \$ followed by digits in a ──────────────
# ── double-quoted string ────────────────────────────────────────────────────
{
    my $x = "price: \$100";
    check('escaped_dollar_before_digits', $x eq "price: \$100");
}

# ── Section 2: \@ followed by a word (the array-interpolation-trigger ─────
# ── equivalent of Section 1) ─────────────────────────────────────────────────
{
    my $y = "email\@example.com";
    check('escaped_at_before_word', $y eq "email\@example.com");
}

# ── Section 3: multiple escaped $/@ in the same string ─────────────────────
{
    my $z = "\$5 \@ \$10 each";
    check('multiple_escapes_in_one_string', $z eq "\$5 \@ \$10 each");
}

# ── Section 4: the ambiguous case — an escaped backslash (\\) immediately ─
# ── followed by a genuine, unescaped interpolation trigger must still ──────
# ── interpolate correctly, not be swallowed as if it were \$ ───────────────
{
    my $v = "value";
    my $w = "a\\$v";
    check('escaped_backslash_then_real_interp', $w eq "a\\value");
}

# ── Section 5: escaped $/@ alongside a genuine interpolation in the same ──
# ── string ────────────────────────────────────────────────────────────────
{
    my $n = 5;
    my $s = "\$$n is the price";
    check('escaped_dollar_then_real_var', $s eq "\$5 is the price");
}

# ── Section 6: regression — plain, unescaped interpolation still works ────
{
    my $name = "World";
    my $greeting = "Hello, $name!";
    check('plain_interpolation_regression', $greeting eq "Hello, World!");
}

# ── Section 7: single-quoted strings only recognize \\ and \' as escapes ──
# ── — every other backslash sequence stays as two literal characters ───────
{
    my $s1 = 'a\nb\tc\$d';
    check('single_quoted_backslash_n_literal', $s1 eq 'a\nb\tc\$d');
    check('single_quoted_length', length($s1) == 10);

    my $s2 = 'it\'s here';
    check('single_quoted_escaped_quote', $s2 eq "it's here");

    my $s3 = 'a\\b';
    check('single_quoted_escaped_backslash', $s3 eq 'a\b');
    check('single_quoted_escaped_backslash_length', length($s3) == 3);
}

# ── Section 8: qq(...) with a non-brace delimiter now uses the correct ────
# ── matching closing delimiter, and still interpolates ─────────────────────
{
    my $q1 = qq(hello world);
    check('qq_paren_delimiter', $q1 eq "hello world");

    my $answer = 42;
    my $q2 = qq(the answer is $answer);
    check('qq_paren_delimiter_interpolates', $q2 eq "the answer is 42");

    my $q3 = qq[bracket delimited];
    check('qq_bracket_delimiter', $q3 eq "bracket delimited");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "string_escape_tests_done\n";
