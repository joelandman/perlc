#!/usr/bin/perl
# In-depth test suite for D60: bare `q(...)`/`q[...]`/`q<...>`/`q/.../ `
# (single-`q`, non-`{`, non-`qq` delimiter) was not recognized as a
# quote-like operator by the lexer at all.
#
# Root cause: the lexer's `q{}`/`qq{}` recognition (lexer.cpp) only
# triggered for `c=='q' && (peek(1)=='{' || peek(1)=='q')` — i.e. a bare
# `q` was only recognized when immediately followed by `{` (brace) or
# another `q` (the `qq` prefix, itself then handling any delimiter via
# the already-D51-fixed `qopen`/`qclose` fallback). A bare single-`q`
# followed directly by any *other* delimiter character (`(`, `[`, `<`,
# `/`, etc.) fell through entirely to ordinary bareword lexing, which
# then choked on the first token inside that didn't look like a valid
# call-argument expression (`q(literal $var here)` → parsed as a call to
# a bareword sub named `q` with `literal` as one argument, then hit `$var`
# with no operator/comma separating it from `literal` first).
#
# Fixed by widening the recognition condition to also match `q`/`qq`
# followed by a small, deliberately conservative set of bracket/slash
# delimiters (`( [ < /`), matching this defect's own reported examples.
# NOT widened to "any non-alphanumeric punctuation": `q`, being a single
# short letter, legitimately appears immediately before many *other*
# punctuation characters in entirely unrelated contexts — most
# importantly a hash-subscript bareword key (`$h{q}`, where the
# character after `q` is `}`) — and treating those as quote-delimiter
# starts would silently break such existing, unrelated code (confirmed
# empirically before choosing the final, narrower set; see Section 9
# below, which is a direct regression check for exactly this).
#
# **A second, more severe, previously-undiscovered bug was found and
# fixed in the same investigation, same code block**: plain `q{...}`
# (not `qq{...}`) was *itself* already broken, independent of any
# delimiter-recognition gap — `q{hello world}` silently produced no
# output at all under perlc. Root cause: the shared `pos_ += 2;` after
# recognizing the `q{`/`qq{`/`qq<delim>` pair unconditionally skipped two
# characters, which is correct for genuine `qq` (both letters of the
# prefix) but wrong for plain `q` immediately followed by a real
# delimiter — skipping 2 there consumed the delimiter *itself* along
# with the `q`, leaving the balanced-brace-scan's own `if (peek() == '{')`
# check looking at the first *content* character instead of the actual
# delimiter, so it silently fell through to the wrong branch entirely.
# Fixed by skipping only 1 character (`q` itself) for the non-`qq` case,
# leaving the delimiter in place for that check and the existing
# `qopen`/`qclose` fallback to see correctly, exactly as they already do
# for `qq`.
#
# NOT fixed here (confirmed, via a temporary git-worktree bisection
# against a commit that predates this entire session's work, to be a
# long-standing, completely unrelated, pre-existing defect — not
# introduced by or related to this fix in any way): a double-quoted
# string interpolating a scalar variable whose name happens to *start*
# with "qq" (e.g. `$qqfoo`), followed later in the same string by a
# literal `]`, `)`, or `}` character, produces a spurious parse error
# (`my $qqfoo = "a"; print "$qqfoo]\n";` fails to compile). This is
# unrelated to quote-like-operator delimiter recognition (that code path
# is never even reached — the failure is inside string interpolation of
# an ordinary variable reference) and was not investigated further here.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — bare q(...) with parens ─────────────────
{
    my $x = q(literal $var here);
    check('bare_q_paren', $x eq 'literal $var here');
}

# ── Section 2: bare q[...] with brackets ───────────────────────────────────
{
    my $x = q[bracket delimited];
    check('bare_q_bracket', $x eq 'bracket delimited');
}

# ── Section 3: bare q<...> with angle brackets ─────────────────────────────
{
    my $x = q<angle delimited>;
    check('bare_q_angle', $x eq 'angle delimited');
}

# ── Section 4: bare q/.../ with slashes ────────────────────────────────────
{
    my $x = q/slash delimited/;
    check('bare_q_slash', $x eq 'slash delimited');
}

# ── Section 5: empty content ────────────────────────────────────────────────
{
    my $x = q();
    check('bare_q_empty', $x eq '');
}

# ── Section 6: the second, more severe bug found alongside this one — ─────
# ── plain q{...} (not qq{...}) previously produced no output at all ───────
{
    my $x = q{brace delimited};
    check('plain_q_brace_regression', $x eq 'brace delimited');
}

# ── Section 7: regression — qq(...) etc. (D51) is unaffected ──────────────
{
    my $answer = 42;
    my $x = qq(the answer is $answer);
    check('qq_paren_regression', $x eq 'the answer is 42');
}

# ── Section 8: regression — qw(...) is unaffected ──────────────────────────
{
    my @words = qw(alpha beta gamma);
    check('qw_regression', join(",", @words) eq 'alpha,beta,gamma');
}

# ── Section 9: regression — a hash key bareword "q" is not swallowed as a ─
# ── quote-operator delimiter start (the reason the widened delimiter set ──
# ── was kept deliberately narrow — '}' is excluded) ─────────────────────────
{
    my %h = (q => "bareword_q_works");
    check('hash_key_bareword_q_regression', $h{q} eq 'bareword_q_works');
}

# ── Section 10: bare q(...) does not interpolate (unlike qq(...)) ─────────
{
    my $notvar = "should not appear";
    my $x = q($notvar literal);
    check('bare_q_no_interpolation', $x eq '$notvar literal');
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "bare_q_delimiter_tests_done\n";
